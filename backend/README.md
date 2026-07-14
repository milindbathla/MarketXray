# MarketXray Backend — README

This directory contains:

- the C++ analytics engine
- the WebSocket broadcaster consumed by the frontend
- the Rust shared-memory ingestor in `rust_ingestor/`

## Project Structure

```text
backend/
├── CMakeLists.txt
├── src/
│   ├── main.cpp
│   ├── main_ws.cpp
│   └── ...
└── rust_ingestor/
    ├── Cargo.toml
    └── src/
```

## How to Build

### Prerequisites
- CMake >= 3.20
- A C++20 compiler: GCC 12+, Clang 15+, or MSVC 2022+

### Build Commands

```bash
# From the backend/ directory:
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

# Run the benchmark
./build/market_xray        # Linux / Mac (using GCC/Clang)
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

## Live Frontend Path

```bash
# terminal 1
cd backend
./build/market_xray_ws

# terminal 2
cd backend/rust_ingestor
cargo run --release

# terminal 3
cd frontend
npm install
npm run dev
```
