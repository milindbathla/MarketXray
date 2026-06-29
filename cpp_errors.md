# MarketXray C++ Engine — Bug Report
> Source: `build/cpp.log` (6,475 lines) + kernel journal + source code inspection

---

## Fixed Bugs (previous session)

| # | Field | Was | Fix applied |
|---|---|---|---|
| 1 | `spread_ticks` | ~4.29 billion (uint32 underflow) | int64_t subtraction, `-1.0` sentinel |
| 2 | `spoofing_detected` | permanently `true` | per-snapshot window counter |
| 3 | `vwap` | dragged toward 0 by price=0 executes | Rust stores price in order map |
| 4 | `hawkes_intensity` | always `0.500000` (μ baseline) | ITCH timestamp → Unix epoch offset added in Rust |

---

## Remaining Bugs

---

### BUG-1 [CRITICAL] — Segmentation Fault in EngineLoop::dispatch()

**Evidence from kernel journal:**
```
market_xray[141442]: segfault at 18 ip ...419d4296 error 6
  #0  _ZN5mxray10EngineLoop8dispatchERKNS_4TickE  (dispatch)
  #1  run_daemon
```

**Root cause — pool recycling corrupts linked-list pointers in remove_from_level():**

OrderNode layout: order_id(8) + price(4) + quantity(4) + timestamp_ns(8) + next*(8) + prev*(8)
Offset 0x18 = 24 bytes = node->next — exactly what the crash dereferences.

Sequence that causes the crash:
1. Order A is added at price P → pool slot #42 allocated, inserted into asks_[P] linked list
2. Execute b'E' with qty >= node->quantity → execute_order() calls cancel_order()
3. cancel_order() erases order A from order_map_ and calls deallocate(node) → slot #42 returned to free list
4. Order B is added → pool slot #42 is immediately reused for a DIFFERENT order
5. asks_[P] linked list still has the old OrderNode* pointer (now pointing at order B's data)
6. Later cancel/execute of order B's neighbour in the list calls remove_from_level() which
   writes through the stale next/prev pointers → SIGSEGV

**Fix needed (LimitOrderBook.hpp):**
In cancel_order(), after erasing from order_map_ but BEFORE calling deallocate, nullify
the node's linked-list pointers. Also add a guard in remove_from_level() to handle
nodes whose prev/next do not belong to this level.

---

### BUG-2 [CRITICAL] — best_ask stuck at 1/6/11 (ghost entries from malformed ticks)

**Evidence:**
```
"best_ask":1   → 3008 snapshots  (46%)
"best_ask":11  → 2958 snapshots  (46%)
"best_ask":6   →  478 snapshots   (7%)
```
Price 1/6/11 in fixed-point = $0.01/$0.06/$0.11 — impossible for any NASDAQ stock.

**Root cause:**
Early ITCH messages (before the legitimate order book is populated) insert near-zero-price
asks through add_order(). The current guard only rejects price==0 or price>=MAX_PRICE_TICKS.
Prices 1–99 ($0.01–$0.99) slip through. These ghost levels are never cancelled (no matching
cancel messages exist for them), so update_best_ask() latches onto them permanently once
the real ask side drains.

**Fix needed (LimitOrderBook.hpp):**
```cpp
static constexpr uint32_t MIN_VALID_PRICE = 100; // $1.00 minimum
if (tick.price < MIN_VALID_PRICE || tick.price >= MAX_PRICE_TICKS) return;
```

---

### BUG-3 [HIGH] — vwap grows without bound (cumulative not rolling)

**Evidence:**
```
Snapshot    1: vwap = 1338  ($13.38)
Snapshot  500: vwap =  387  ($3.87)    <- wrong
Snapshot 6475: vwap = 6758  ($67.58)  <- drifting, should be ~$175
```

**Root cause:**
AnalyticsEngine::update_vwap() accumulates vwap_notional_ and vwap_volume_ for the
entire session lifetime. The window_ parameter (500) is used only for OFI, never VWAP.
Early executes with bad prices (before other fixes) heavily skew the cumulative average,
and it never recovers.

**Fix needed (AnalyticsEngine.hpp):**
Use the same rolling deque pattern as OFI — store (price, qty) pairs, evict oldest
when deque exceeds window size, maintain running sums.

---

### BUG-4 [HIGH] — hawkes_intensity still always 0.500000 after epoch fix

**Evidence:** All 6,475 snapshots show intensity=0.500000 (= mu baseline).

**Root cause:**
The Rust fix correctly shifted ITCH timestamps to Unix-epoch nanoseconds (Aug 2019).
But emit_snapshot() still calls:
```cpp
double ts_now = static_cast<double>(system_clock::now().time_since_epoch().count());
// ts_now = June 2026 = ~1.782e18 ns
// stored events = August 2019 = ~1.567e18 ns
// delta_t = 6.8 years in nanoseconds
// exp(-beta * 6.8_years_in_ns) = 0  →  intensity collapses to mu = 0.5
```
The data is historical (2019) but the engine runs in 2026. The Hawkes kernel decays
completely across the 6.8-year gap.

**Fix needed (EngineLoop.hpp):**
Store the last dispatched tick's timestamp_ns as last_tick_ts_ns_. Use it in
emit_snapshot() instead of system_clock::now():
```cpp
snap.hawkes_intensity = hawkes_.compute_intensity(
    static_cast<double>(last_tick_ts_ns_));  // use data time, not wall time
```

---

### BUG-5 [MEDIUM] — spoofing_cancel_ratio wrong denominator

**Evidence:**
```
"spoofing_cancel_ratio":0.182000
"spoofing_cancel_ratio":0.211000
```
After the per-window fix, ratio = spoof_alerts_in_window / snapshot_every_n (1000).
This is alerts-per-tick, not a cancel ratio. It can exceed 1.0 and has no intuitive meaning.

**Fix needed (EngineLoop.hpp):**
Track total_cancels_in_window alongside spoof_alerts_in_window and divide by that.

---

### BUG-6 [LOW] — market_impact always zero (not implemented)

**Evidence:** avg_fill_price=0, slippage_bps=0, fully_filled=false in every snapshot.

**Root cause:** emit_snapshot() hardcodes zeros. The walk-the-book simulation was
planned in the proposal but never coded. The LOB has all the depth data needed.

---

## Summary Table

| # | Severity | Component | Status |
|---|---|---|---|
| 1 | CRITICAL | Segfault — pool recycling + stale linked-list pointer | Not fixed |
| 2 | CRITICAL | best_ask — ghost entries from sub-$1 malformed ticks | Not fixed |
| 3 | HIGH | vwap — cumulative session VWAP, not rolling window | Not fixed |
| 4 | HIGH | hawkes_intensity — wall clock vs 2019 data (6.8yr gap) | Not fixed |
| 5 | MEDIUM | spoofing_cancel_ratio — wrong denominator | Not fixed |
| 6 | LOW | market_impact — hardcoded zeros, never implemented | Not fixed |
