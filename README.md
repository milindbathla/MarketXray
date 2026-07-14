# MarketXray

This repository contains:

- `frontend/` - React + TypeScript + Vite dashboard
- `backend/` - C++ analytics engine and Rust shared-memory ingestor

See [ARCHITECTURE.md](ARCHITECTURE.md) for the current live replay and browser
fallback data flows.

## Prerequisites

- Node.js 20+
- npm 10+
- CMake 3.20+
- GCC/Clang with C++20 support
- Rust toolchain

## Setup

The frontend `node_modules` directory may be stale or copied from another OS.
Refresh it on the current machine before running anything:

```bash
cd frontend
npm install
```

Build the backend components:

```bash
cd ../backend
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

cd rust_ingestor
cargo build --release
```

## Run

Start the frontend:

```bash
cd frontend
npm run dev
```

Start the complete backend from Rust in another terminal. It starts the C++
engine workers and owns the frontend WebSocket on port `9001`:

```bash
cd backend/rust_ingestor
cargo run --release --bin rust_ingestor -- \
  --symbols AAPL,MSFT,NVDA,TSLA,AMZN --replay-speed 200
```

The frontend connects to `ws://localhost:9001` and falls back to simulated
data when the backend is unavailable.

## Verified Commands

These commands were validated in this workspace:

```bash
cd frontend && npm install && npm run build
cd backend && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
cd backend/rust_ingestor && cargo build --release
```
