# MarketXray Bug Notes

This file tracks important backend bugs found while running the live NASDAQ ITCH pipeline.

---

## BUG-1: ITCH `stock_locate` Was Ignored

**Status:** Fixed

**Severity:** Critical

**Component:** Rust ingestor

### Symptom

When running the backend on the uploaded NASDAQ ITCH dataset, the output showed incorrect market values:

```text
best_bid: wrong
best_ask: wrong
spread_ticks: wrong
```

The spread sometimes looked unrealistic because the C++ limit order book was not representing one actual stock.

### Root Cause

NASDAQ ITCH files contain messages for the whole exchange, not just one symbol.

The dataset includes many symbols:

```text
AAPL, MSFT, NVDA, ETFs, small-cap stocks, etc.
```

ITCH messages include a `stock_locate` field that identifies which symbol each message belongs to. The Rust parser was ignoring this field and sending every order message into one shared C++ `LimitOrderBook`.

That created a mixed-symbol book:

```text
best bid could come from AAPL
best ask could come from another stock
spread = unrelated ask - unrelated bid
```

So the C++ logic was working on bad input. The order book itself was not the main problem; the stream needed symbol filtering.

### Fix

The Rust ingestor now:

- parses ITCH Stock Directory `R` messages,
- maps `stock_locate` to stock symbol,
- resolves a target symbol, defaulting to `AAPL`,
- filters all add, execute, cancel, delete, replace, and trade messages by that `stock_locate`,
- skips execute/cancel messages if their original order was not tracked.

Run example:

```powershell
cargo run --release -- --symbol AAPL
```

Another symbol:

```powershell
cargo run --release -- --symbol MSFT
```

### Verification

Live run after the fix:

```text
[Rust] Target symbol: AAPL
[Rust] Resolved symbol AAPL to stock_locate 14
```

C++ snapshots then showed realistic AAPL-like values:

```json
{
  "best_bid": 21013,
  "best_ask": 21018,
  "spread_ticks": 5.0
}
```

Meaning:

```text
Best bid: $210.13
Best ask: $210.18
Spread: 5 ticks
```

This confirms the Rust -> shared memory -> C++ pipeline is now streaming one symbol into one order book.

---

## BUG-2: Full Exchange Data Was Polluting VWAP And OFI

**Status:** Fixed by the same `stock_locate` filtering change

**Severity:** High

**Component:** Rust ingestor / C++ analytics

### Symptom

VWAP and OFI could look unstable or misleading when the ITCH dataset was streamed.

### Root Cause

Because all symbols were mixed into one book, VWAP and OFI were aggregating unrelated trades from different instruments.

For example, a trade from one symbol and an order-book update from another symbol could both contribute to the same analytics window.

### Fix

Filtering the Rust stream by one symbol prevents cross-symbol analytics pollution.

---

## BUG-3: Missing Order Lookup Created Fake Execute/Cancel Ticks

**Status:** Fixed

**Severity:** High

**Component:** Rust ingestor

### Symptom

Some execute/cancel messages were emitted with default fallback values:

```text
side = BUY
price = 0
```

This could corrupt VWAP or confuse downstream order-book logic.

### Root Cause

ITCH execute/cancel messages often reference an existing order id instead of carrying all original order details. If the Rust-side `order_side_map` did not contain the order id, the code previously used fallback values.

### Fix

The ingestor now skips execute/cancel/delete/replace messages if their original order was not tracked:

```rust
let Some((side, orig_price)) = order_side_map.get(&order_id).copied() else {
    continue;
};
```

This avoids creating fake price-zero events.

---

## BUG-4: Rust Ingestor Could Not Find Uploaded Dataset

**Status:** Fixed

**Severity:** Medium

**Component:** Rust ingestor

### Symptom

The uploaded ITCH dataset was at:

```text
C:\Users\karti\OneDrive\Desktop\CS661\08302019.NASDAQ_ITCH50.gz
```

But the Rust ingestor only checked `res/` paths.

### Fix

The ingestor now also checks:

```text
../08302019.NASDAQ_ITCH50.gz
../../08302019.NASDAQ_ITCH50.gz
```

This lets it find the uploaded dataset from the current project layout.

---

## BUG-5: Market Impact Is Still Not Wired

**Status:** Open

**Severity:** Medium

**Component:** C++ snapshot/export path

### Symptom

Live JSON snapshots still show:

```json
"market_impact": {
  "avg_fill_price": 0.0,
  "slippage_bps": 0.0,
  "fully_filled": false
}
```

### Root Cause

The C++ engine has a market impact result structure, but `EngineLoop::emit_snapshot()` still hardcodes market impact fields to zero.

### Needed Fix

Expose depth levels from the limit order book and call the walk-the-book simulator during snapshot generation or on demand from the frontend.

---

## BUG-6: Flash Crash Probability Is Still Zero

**Status:** Open

**Severity:** Medium

**Component:** C++ graph/risk snapshot path

### Symptom

Live JSON snapshots show:

```json
"flash_crash_probability": 0.0
```

### Root Cause

`EngineLoop::emit_snapshot()` calls the graph engine with an empty depth vector:

```cpp
graph_.compute_flash_crash_probability({}, lob_.get_best_ask(), 20)
```

### Needed Fix

Expose ask-side depth from the live order book and pass real price levels into the graph risk model.

---

## Current Live Backend Status

Working:

- Rust finds uploaded ITCH dataset.
- Rust resolves target symbol using Stock Directory messages.
- Rust streams one symbol only.
- C++ daemon receives live ticks through shared memory.
- C++ emits live JSON snapshots.
- Best bid, best ask, spread, OFI, VWAP, PIN, Hawkes, and spoofing fields stream.

Still pending:

- full market impact simulation in snapshots,
- real depth export for frontend 3D visualization,
- real depth input for flash crash probability,
- frontend dashboard.

