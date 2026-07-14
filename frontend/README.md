# MarketXray Frontend — HFT Microstructure Terminal

React 18 + TypeScript (strict) + Vite + Three.js. Dark terminal dashboard
implementing the Stitch design (`../GOOGLE_STITCH_DESIGN.md`) 1:1, plus the
Hawkes Intensity (λ) Self-Excitation Monitor.

## Run

```bash
npm install
npm run dev        # http://localhost:5173
npm run typecheck  # tsc --noEmit (clean)
npm run build      # production bundle (clean)
```

If this repo was copied from another machine or OS, run `npm install` again on
the current machine before using the frontend. The checked-in `node_modules`
contents can be stale or miss platform-specific binaries required by Vite and
Rollup.

## Data modes

| Mode | Badge | When |
|---|---|---|
| **LIVE** | `● NASDAQ ITCH` (green) | C++ engine broadcasting on `ws://localhost:9001` |
| **SIM** | `● SIMULATED FEED` (amber) | Backend unreachable — auto-fallback, retries every 5 s |

The simulator emits the **exact** `Exporter.hpp` JSON schema with financially
coherent dynamics (self-exciting Hawkes staircase/decay, OFI-led price moves,
liquidity walls, spoof bursts). Toggle "Force simulator" in ⚙ Settings.

To run the live backend (POSIX-only → WSL/Linux):
```bash
# terminal 1: build + run the WS broadcaster
cd ../backend
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/market_xray_ws                # ws://localhost:9001

# terminal 2: rust ingestor
cd ../backend/rust_ingestor
cargo run --release

# terminal 3: this frontend
npm run dev
```

## Panels → backend fields

| Panel | Data |
|---|---|
| KPI strip (7) | `microstructure`, `hawkes`, `pin`, `spoofing`, `risk` |
| 3D LOB Depth Surface | `book_depth.bids/asks` buffered over time |
| OFI vs Price Action | `microstructure.ofi`, mid from `best_bid/ask` |
| Hawkes λ Monitor | `hawkes.intensity / p_next_event_1s / critical` |
| Trade Tape | sim prints; LIVE derives prints from snapshot deltas (engine doesn't export executions yet) |
| Market Impact + Volume | client-side walk of `book_depth.asks` for any order size (mirrors `simulate_market_impact`) |
| Spoofing Detector | `spoofing.suspicious_orders[]` (quantity × lifetime_ms × score) |
| Event Volume | tape prints bucketed per interval |

## Architecture

- `src/feed.ts` — external store (`useSyncExternalStore`): WS client + sim
  fallback + ring buffers (900 snaps / 400 trades / 500 spoof points). Data
  never flows through React state at tick rate.
- `src/sim.ts` — simulator + `walkBook()` (shared with the impact panel).
- `src/hooks.ts` — `useFeed()` + `useCanvas()` (DPR-aware, ResizeObserver).
- `src/components/` — one file per panel; all 2D charts are Canvas, the depth
  surface is Three.js with OrbitControls + raycast hover tooltips.
- 3D interactions: drag rotate · scroll zoom · right-drag pan · hover for
  price/volume/side/time · Bids/Asks/Both · depth 10/25/50 · Reset View.
