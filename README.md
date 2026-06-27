# MarketXray C++ Engine — README

## Project Structure

```
cpp_engine/
├── CMakeLists.txt          # Build config with HFT optimization flags
└── src/
    ├── Types.hpp           # Core structs (Tick, OrderNode) — 64-byte cache-aligned
    ├── MemoryPool.hpp      # Custom block allocator — zero heap allocation during trading
    ├── SPSCQueue.hpp       # Lock-free ring buffer — for IPC with Rust (no mutexes)
    ├── LimitOrderBook.hpp  # Ultra-fast LOB using flat arrays and O(1) hash maps
    ├── AnalyticsEngine.hpp # OFI (Order Flow Imbalance) and VWAP rolling-window calculators
    ├── HawkesPIN.hpp       # Hawkes Process + PIN (Probability of Informed Trading)
    ├── GraphAlgos.hpp      # Market Impact, Flash Crash Probability, Bellman-Ford Arbitrage, Spoofing Detector
    ├── Exporter.hpp        # JSON serializer for streaming metrics to the React frontend
    └── main.cpp            # Full integration test: pumps 1M ticks and reports all metrics
```

## How to Build

### Prerequisites
- CMake >= 3.20
- A C++20 compiler: GCC 12+, Clang 15+, or MSVC 2022+

### Build Commands

```bash
# From the cpp_engine/ directory:
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release

# Run the benchmark
./market_xray        # Linux / Mac (using GCC/Clang)
.\Release\market_xray.exe  # Windows with MSVC
```

## What It Computes (All Engines Active)

| Engine                  | Algorithm                          | What It Tells You                                   |
|-------------------------|------------------------------------|-----------------------------------------------------|
| Limit Order Book        | Flat-array + O(1) HashMap          | Real-time bid/ask, price levels, volume             |
| OFI                     | Rolling circular window            | Net order pressure → next price direction           |
| VWAP                    | Incremental weighted average       | True institutional transaction price                |
| Hawkes Process          | Self-exciting point process        | Probability of an incoming burst of aggressive trades|
| PIN                     | Bayesian order-flow decomposition  | Probability that "informed" whales are trading      |
| Market Impact Simulator | Walk-the-book scan                 | Slippage cost for a hypothetical large order        |
| Flash Crash Predictor   | Liquidity Contagion graph score    | Probability of imminent liquidity collapse          |
| Bellman-Ford Arbitrage  | Negative cycle detection           | Detects risk-free cross-asset profit opportunity    |
| Spoofing Detector       | Cluster-based cancel-ratio tracker | Flags potential market manipulation                 |

## Integration with Rust Ingestor

To connect to the Rust data pipeline, replace the mock tick generator in `main.cpp` with the `SPSCQueue<Tick, 65536>` reader.

The Rust side must write `Tick` structs (see `Types.hpp`) into the shared memory ring buffer.
The `Tick` struct uses `#pragma pack(1)` with `alignas(64)` — make sure the Rust side mirrors this with `#[repr(C, align(64))]`.

## Output Format

All metrics are serialized via `Exporter.hpp` into a JSON payload:

```json
{
  "timestamp_ns": 1719470400000000000,
  "microstructure": { "ofi": 1234.5, "ofi_normalized": 0.123, "vwap": 15000.5, ... },
  "hawkes": { "intensity": 2.45, "p_next_event_1s": 0.91, "critical": true },
  "pin": { "score": 0.38, "level": "MEDIUM" },
  "market_impact": { "avg_fill_price": 15100.0, "slippage_bps": 6.67, "fully_filled": true },
  "risk": { "flash_crash_probability": 0.72, "arbitrage_opportunity": true, "spoofing_detected": false }
}
```

This JSON payload can be directly consumed by the React/D3.js frontend over a WebSocket connection.
