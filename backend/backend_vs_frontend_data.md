# Backend vs Frontend Data — Code-Verified Analysis

I've read every C++ source file in the repository. This document is now verified against the actual code, not assumptions.

---

## How the Data Flows (Verified)

1. [EngineLoop.hpp](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/EngineLoop.hpp) builds an `EngineSnapshot` struct every N ticks (default: every 1000 ticks, line 46).
2. [Exporter.hpp](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/Exporter.hpp) serializes that struct into a JSON string via `snapshot_to_json()` (line 54).
3. [WebSocketServer.hpp](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/WebSocketServer.hpp) broadcasts that JSON string to all connected browser clients (line 202).

---

## Exact JSON the Backend Currently Sends

This is the **actual** output from [snapshot_to_json()](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/Exporter.hpp#L54-L89):

```json
{
  "timestamp_ns": 1719470400000000000,
  "microstructure": {
    "ofi": 1234.500000,
    "ofi_normalized": 0.120000,
    "vwap": 15002.400000,
    "best_bid": 15000,
    "best_ask": 15002,
    "spread_ticks": 2.000000
  },
  "hawkes": {
    "intensity": 4.500000,
    "p_next_event_1s": 0.310000,
    "critical": false
  },
  "pin": {
    "score": 0.150000,
    "level": "LOW"
  },
  "market_impact": {
    "avg_fill_price": 0.000000,
    "slippage_bps": 0.000000,
    "fully_filled": false
  },
  "risk": {
    "flash_crash_probability": 0.000000,
    "arbitrage_opportunity": false,
    "arbitrage_profit_factor": 1.000000,
    "spoofing_detected": false,
    "spoofing_cancel_ratio": 0.000000
  }
}
```

That's it. That's everything the frontend currently receives. Now let's compare this against what each graph actually needs.

---

## Gap Analysis by Visualization

### ✅ 1. OFI vs Price Action — READY (No changes needed)

| Field | In Backend? | Source |
|-------|-------------|--------|
| `ofi` | ✅ Yes | [AnalyticsEngine.hpp L53](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/AnalyticsEngine.hpp#L53) |
| `ofi_normalized` | ✅ Yes | [AnalyticsEngine.hpp L59](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/AnalyticsEngine.hpp#L59) |
| `vwap` | ✅ Yes | [AnalyticsEngine.hpp L87](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/AnalyticsEngine.hpp#L87) |
| `best_bid` | ✅ Yes | [LimitOrderBook.hpp L103](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/LimitOrderBook.hpp#L103) |
| `best_ask` | ✅ Yes | [LimitOrderBook.hpp L104](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/LimitOrderBook.hpp#L104) |
| `spread_ticks` | ✅ Yes | [AnalyticsEngine.hpp L95](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/AnalyticsEngine.hpp#L95) |
| `mid_price` | Compute on frontend | `(best_bid + best_ask) / 2` |

**Verdict:** This chart is fully supported. Frontend computes `mid_price` locally.

---

### ✅ 2. KPI Strip & Flash Crash Risk Panel — READY (No changes needed)

| Field | In Backend? | Source |
|-------|-------------|--------|
| `hawkes.intensity` | ✅ Yes | [HawkesPIN.hpp L55](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/HawkesPIN.hpp#L55) |
| `hawkes.critical` | ✅ Yes | [HawkesPIN.hpp L64](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/HawkesPIN.hpp#L64) |
| `pin.score` | ✅ Yes | [HawkesPIN.hpp L122](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/HawkesPIN.hpp#L122) |
| `pin.level` | ✅ Yes | [HawkesPIN.hpp L144](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/HawkesPIN.hpp#L144) |
| `flash_crash_probability` | ⚠️ Broken | See bug below |
| `spoofing_detected` | ✅ Yes | [EngineLoop.hpp L246](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/EngineLoop.hpp#L246) |
| `spoofing_cancel_ratio` | ✅ Yes | [EngineLoop.hpp L247](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/EngineLoop.hpp#L247) |

> [!WARNING]
> **Bug Found:** `flash_crash_probability` is **always 0.0** because [EngineLoop.hpp line 242-243](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/EngineLoop.hpp#L242-L243) passes an **empty vector `{}`** instead of the actual ask levels from the LOB:
> ```cpp
> snap.flash_crash_probability = graph_.compute_flash_crash_probability(
>     {}, lob_.get_best_ask(), 20);  // <-- {} is empty!
> ```
> The LOB has `bids_` and `asks_` vectors internally, but they are **private** with no getter. The backend team needs to add a public accessor like `const std::vector<PriceLevel>& get_asks() const` to `LimitOrderBook` and pass it here.

---

### ❌ 3. 3D Limit Order Book Depth Surface — NOT READY

| What's needed | In Backend? | Details |
|---------------|-------------|---------|
| Array of bid levels `[[price, volume], ...]` | ❌ No | `LimitOrderBook` stores full depth in `bids_` and `asks_` vectors ([LimitOrderBook.hpp L169-170](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/LimitOrderBook.hpp#L169-L170)), but they are **private** and not serialized. |
| Array of ask levels `[[price, volume], ...]` | ❌ No | Same — the data exists internally but is never exported to JSON. |

**What the backend team needs to do:**
1. Add public getters to [LimitOrderBook.hpp](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/LimitOrderBook.hpp):
   ```cpp
   const std::vector<PriceLevel>& get_bids() const { return bids_; }
   const std::vector<PriceLevel>& get_asks() const { return asks_; }
   ```
2. In [Exporter.hpp](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/Exporter.hpp), iterate the top N non-empty levels and serialize them:
   ```json
   "book_depth": {
     "bids": [[15000, 12000], [14999, 9000], ...],
     "asks": [[15002, 11000], [15003, 8000], ...]
   }
   ```

---

### ❌ 4. Spoofing & Layering Detector (Bubble Plot) — NOT READY

| What's needed | In Backend? | Details |
|---------------|-------------|---------|
| `spoofing_detected` (boolean) | ✅ Yes | Works |
| `spoofing_cancel_ratio` (number) | ✅ Yes | Works |
| Array of suspicious orders with `price`, `quantity`, `lifetime_ms`, `score` | ❌ No | The `SpoofingDetector` class ([GraphAlgos.hpp L228](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/GraphAlgos.hpp#L228)) returns individual `SpoofingAlert` structs per cancel event, but these are **never collected or serialized**. They are only printed to stderr in verbose mode ([EngineLoop.hpp L196-199](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/EngineLoop.hpp#L196-L199)). |

**What the backend team needs to do:**
1. In [EngineLoop.hpp](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/EngineLoop.hpp), collect `SpoofingAlert` structs into a `std::vector<SpoofingAlert>` during the snapshot window (instead of just incrementing a counter).
2. Serialize the vector in [Exporter.hpp](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/Exporter.hpp):
   ```json
   "spoofing": {
     "is_active": true,
     "cancel_ratio": 0.93,
     "suspicious_orders": [
       {"price": 15020, "quantity": 50000, "lifetime_ms": 120, "score": 0.91}
     ]
   }
   ```

> [!NOTE]
> The `SpoofingAlert` struct already exists at [GraphAlgos.hpp L220-226](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/GraphAlgos.hpp#L220-L226) with `price`, `quantity`, `cancel_ratio`, and `is_suspected_spoof`. The only missing field is `lifetime_ms` (time between ADD and CANCEL), which the `SpoofingDetector` already tracks internally via `OrderRecord.timestamp_ns` ([GraphAlgos.hpp L270-273](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/GraphAlgos.hpp#L270-L273)).

---

### ❌ 5. Arbitrage Opportunity Network Graph — NOT READY

| What's needed | In Backend? | Details |
|---------------|-------------|---------|
| `arbitrage_opportunity` (boolean) | ⚠️ Hardcoded `false` | [EngineLoop.hpp L244](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/EngineLoop.hpp#L244): `snap.arbitrage_opportunity = false;` with comment "computed on-demand, not per-tick" |
| `arbitrage_profit_factor` (number) | ⚠️ Hardcoded `1.0` | [EngineLoop.hpp L245](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/EngineLoop.hpp#L245): `snap.arbitrage_profit_factor = 1.0;` |
| `cycle_path` (array of asset names) | ❌ No | The `ArbitrageResult` struct has `std::vector<int> cycle` ([GraphAlgos.hpp L135](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/GraphAlgos.hpp#L135)), but these are integer indices, not asset names. And they are never serialized. |
| `edges` (array of from/to/rate) | ❌ No | The `detect_arbitrage()` function takes `exchange_rates` as input ([GraphAlgos.hpp L140](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/GraphAlgos.hpp#L140)), but edges are never exported. |

> [!CAUTION]
> **Arbitrage is completely disabled.** The `detect_arbitrage()` function exists and works ([GraphAlgos.hpp L139-210](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/GraphAlgos.hpp#L139-L210)), but it is **never called** from `emit_snapshot()`. The backend team needs to:
> 1. Decide on a set of assets/exchanges and feed exchange rates to `detect_arbitrage()` periodically.
> 2. Add the cycle path and edges to the `EngineSnapshot` struct.
> 3. Serialize them in `snapshot_to_json()`.

---

### ❌ 6. Market Impact & Slippage Simulator — NOT READY

| What's needed | In Backend? | Details |
|---------------|-------------|---------|
| `avg_fill_price` | ⚠️ Always `0.0` | [EngineLoop.hpp L251](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/EngineLoop.hpp#L251): hardcoded to `0.0` |
| `slippage_bps` | ⚠️ Always `0.0` | [EngineLoop.hpp L252](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/EngineLoop.hpp#L252): hardcoded to `0.0` |
| `fully_filled` | ⚠️ Always `false` | [EngineLoop.hpp L253](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/EngineLoop.hpp#L253): hardcoded to `false` |
| `levels_consumed` | ❌ Not in JSON | Exists in `MarketImpactResult` struct ([GraphAlgos.hpp L27](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/GraphAlgos.hpp#L27)) but not in `EngineSnapshot` |
| `fills` array `[{price, quantity}]` | ❌ Not computed | `simulate_market_impact()` doesn't store per-level fills; it only sums them ([GraphAlgos.hpp L67-76](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/GraphAlgos.hpp#L67-L76)) |

> [!WARNING]
> **Market impact is completely disabled.** The `simulate_market_impact()` function exists ([GraphAlgos.hpp L51-89](file:///c:/Users/MANTHAN%20KHETADE/Desktop/cs661%20project/MarketXray/src/GraphAlgos.hpp#L51-L89)) and works, but `emit_snapshot()` never calls it. Same root cause as the LOB depth issue: `bids_` and `asks_` are private, so the engine loop can't pass them to `simulate_market_impact()`.

---

## Summary Scoreboard

| Visualization | Status | Blocking Issue |
|:---|:---:|:---|
| OFI vs Price Action | ✅ Ready | None |
| KPI Strip (Hawkes, PIN, Spread) | ✅ Ready | None |
| Flash Crash Risk Panel | ⚠️ Broken | Empty vector passed — always returns 0.0 |
| 3D Limit Order Book | ❌ Missing | `bids_`/`asks_` are private, not serialized |
| Spoofing Bubble Plot | ❌ Missing | Individual alerts not collected or serialized |
| Arbitrage Network Graph | ❌ Missing | `detect_arbitrage()` never called, hardcoded false |
| Market Impact Simulator | ❌ Missing | `simulate_market_impact()` never called, hardcoded 0.0 |

---

## What to Tell Your Backend Team

Share this checklist:

- [ ] Add `get_bids()` and `get_asks()` public getters to `LimitOrderBook.hpp`
- [ ] Fix `flash_crash_probability` — pass actual ask levels instead of `{}`
- [ ] Call `simulate_market_impact()` in `emit_snapshot()` and serialize the result
- [ ] Collect `SpoofingAlert` structs into a vector and serialize them
- [ ] Call `detect_arbitrage()` periodically and serialize cycle + edges
- [ ] Add `levels_consumed` and per-level `fills` array to market impact JSON
- [ ] Add `book_depth.bids` and `book_depth.asks` arrays (top 50 levels) to JSON
