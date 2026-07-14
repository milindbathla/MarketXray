# MarketXray Visualization Plan

## Goal

MarketXray should not look like a normal stock-price dashboard. The main idea is to expose the hidden market microstructure behind price movement: liquidity, cancellations, order-flow pressure, spoofing patterns, arbitrage paths, and slippage.

The frontend should answer one question:

> What was happening inside the limit order book before, during, and after a market event?

The dashboard should use synchronized charts. When the user zooms into a time range in one view, all other views should follow the same time window.

---

## System Flow

```text
NASDAQ ITCH / L2 data
        |
        v
Rust ingestor
        |
        v
Shared memory queue
        |
        v
C++ analytics engine
        |
        v
JSON snapshots / WebSocket
        |
        v
React + D3 + WebGL frontend
```

The frontend consumes backend snapshots containing:

- best bid
- best ask
- spread
- OFI
- normalized OFI
- VWAP
- Hawkes intensity
- PIN score
- spoofing signal
- spoofing cancel ratio
- flash crash probability
- arbitrage flag
- arbitrage profit factor
- market impact output

---

## Main Dashboard Layout

The first screen should be the actual analytics dashboard, not a landing page.

Recommended layout:

```text
+--------------------------------------------------------------------------------+
| Top Bar: symbol, current time, replay controls, speed, connection status         |
+--------------------------------------------------------------------------------+
| KPI Strip: spread | OFI | VWAP | Hawkes | PIN | spoofing | flash crash risk      |
+--------------------------------------------------------------------------------+
| 3D Limit Order Book Depth Surface                                                |
| Main visual, full width, interactive rotation/zoom                               |
+-------------------------------------+------------------------------------------+
| OFI vs Price Action                 | Spoofing & Layering Detector             |
+-------------------------------------+------------------------------------------+
| Market Impact Simulator             | Arbitrage Network Graph                   |
+-------------------------------------+------------------------------------------+
| Event Timeline / Alerts / Snapshot Table                                         |
+--------------------------------------------------------------------------------+
```

The visual style should feel like a professional market analysis tool:

- dark or neutral background,
- strong contrast for bid/ask sides,
- green/blue for buy-side liquidity,
- red/orange for sell-side liquidity,
- yellow/amber for warnings,
- compact controls,
- minimal explanatory text inside the UI.

---

## 1. 3D Limit Order Book Depth Surface

### Purpose

This is the primary visualization. It shows how liquidity changes across time and price.

### Axes

```text
X-axis: time
Y-axis: price level
Z-axis: volume at that price level
Color: liquidity intensity
```

### What It Shows

- liquidity walls,
- sudden liquidity removal,
- bid-side and ask-side depth,
- order-book imbalance,
- possible spoofing patterns,
- pre-crash liquidity thinning.

### User Interactions

- rotate the 3D surface,
- zoom into a time window,
- hover a point to show timestamp, price, side, and volume,
- toggle bid/ask/both,
- change depth range, for example top 10, 25, or 50 levels,
- pause replay at a suspicious market moment.

### Backend Data Needed

For each snapshot:

```json
{
  "timestamp_ns": 1719470400000000000,
  "book_depth": {
    "bids": [{"price": 14999, "volume": 12000}],
    "asks": [{"price": 15001, "volume": 9000}]
  }
}
```

Current backend has best bid/ask, but full depth export should be added for this visualization.

---

## 2. Spoofing & Layering Detector

### Purpose

This view highlights suspicious large orders that are cancelled quickly.

### Chart Type

Scatter plot or bubble plot.

### Axes

```text
X-axis: time
Y-axis: time to cancellation
Bubble size: order quantity
Color: suspicion score / cancel ratio
```

### What It Shows

A suspicious spoofing pattern is:

```text
large order appears
        |
        v
market reacts
        |
        v
order is cancelled quickly
        |
        v
same behavior repeats near nearby price levels
```

### User Interactions

- brush-select suspicious clusters,
- filter by minimum order size,
- filter by cancellation time,
- click a point to inspect order lifecycle,
- jump the 3D order book to the same timestamp.

### Backend Data Needed

```json
{
  "timestamp_ns": 1719470400000000000,
  "spoofing_detected": true,
  "spoofing_cancel_ratio": 0.93,
  "suspicious_orders": [
    {
      "order_id": 12345,
      "price": 15020,
      "quantity": 50000,
      "lifetime_ns": 2000000,
      "side": "SELL",
      "score": 0.91
    }
  ]
}
```

Current backend exposes spoofing boolean and cancel ratio. For richer visualization, export suspicious order details.

---

## 3. OFI vs Price Action

### Purpose

This chart explains whether order flow pressure leads price movement.

OFI means Order Flow Imbalance:

```text
positive OFI = buy pressure
negative OFI = sell pressure
```

### Chart Type

Dual-axis synchronized line chart.

### Lines

- mid price,
- OFI,
- normalized OFI,
- VWAP.

### What It Shows

- whether buyers or sellers dominate,
- whether OFI changes before price moves,
- divergence between price and order flow,
- moments where VWAP acts as a reference price.

### User Interactions

- choose OFI lookback window,
- hover to inspect exact values,
- zoom into milliseconds,
- highlight regions where OFI and price diverge.

### Backend Data Needed

Already mostly available:

```json
{
  "timestamp_ns": 1719470400000000000,
  "microstructure": {
    "ofi": 1234.5,
    "ofi_normalized": 0.12,
    "vwap": 15002.4,
    "best_bid": 15000,
    "best_ask": 15002,
    "spread_ticks": 2
  }
}
```

Frontend can compute:

```text
mid_price = (best_bid + best_ask) / 2
```

---

## 4. Arbitrage Opportunity Network Graph

### Purpose

This view shows market inefficiencies across assets or exchanges.

### Chart Type

Dynamic network graph.

### Graph Meaning

```text
nodes = assets or exchanges
edges = exchange rates / spreads
cycle = possible arbitrage path
```

### What It Shows

- triangular arbitrage opportunities,
- profit factor,
- which cycle creates the opportunity,
- how quickly the opportunity disappears.

### Example

```text
USD -> BTC -> ETH -> USD
Profit factor: 1.038x
```

If a profitable cycle exists, the cycle edges should flash red or amber.

### User Interactions

- pause graph when arbitrage appears,
- click edge to see exchange rate and transformed log weight,
- click opportunity to show the full cycle calculation,
- replay arbitrage events over time.

### Backend Data Needed

```json
{
  "risk": {
    "arbitrage_opportunity": true,
    "arbitrage_profit_factor": 1.038
  },
  "arbitrage": {
    "cycle": ["USD", "BTC", "ETH", "USD"],
    "edges": [
      {"from": "USD", "to": "BTC", "rate": 0.000016},
      {"from": "BTC", "to": "ETH", "rate": 17.2},
      {"from": "ETH", "to": "USD", "rate": 3700}
    ]
  }
}
```

Current backend has Bellman-Ford detection and profit factor. Exporting the cycle path would make this visualization stronger.

---

## 5. Market Impact & Slippage Simulator

### Purpose

This is an interactive what-if tool. It answers:

> If I submit a large market order right now, how much worse will my average fill price be?

### User Input

- side: buy or sell,
- quantity,
- timestamp,
- depth range.

### Output

- average fill price,
- slippage in basis points,
- whether the order fully filled,
- number of price levels consumed,
- fill breakdown by price level.

### Chart Type

Bar chart of consumed price levels.

Example:

```text
Input: Buy 50,000 shares
Best ask: $150.01
Average fill: $150.42
Slippage: 27.3 bps
Levels consumed: 8
Fully filled: yes
```

### User Interactions

- quantity slider,
- buy/sell toggle,
- timestamp scrubber,
- hover each bar to see quantity filled at that price,
- compare slippage at different timestamps.

### Backend Data Needed

```json
{
  "market_impact": {
    "avg_fill_price": 15042,
    "slippage_bps": 27.3,
    "fully_filled": true,
    "levels_consumed": 8,
    "fills": [
      {"price": 15001, "quantity": 8000},
      {"price": 15002, "quantity": 12000}
    ]
  }
}
```

Current backend has a market impact structure, but snapshot output currently needs richer fill-level data for the frontend.

---

## 6. Flash Crash Risk Panel

### Purpose

This panel gives a quick risk reading for the current market state.

### Inputs

- flash crash probability,
- spread,
- top-of-book liquidity,
- Hawkes intensity,
- spoofing signal,
- PIN score.

### Display

Use compact risk cards:

```text
Flash Crash Risk: 72%
Hawkes Intensity: High
PIN: Medium
Spoofing: Active
Spread: 4 ticks
```

### Visual Behavior

- green: calm,
- amber: unstable,
- red: high risk.

If risk jumps quickly, add an alert in the event timeline.

---

## 7. Event Timeline

### Purpose

This is the story layer of the dashboard. It explains what happened and when.

### Events To Show

- large OFI spike,
- spread widening,
- spoofing alert,
- arbitrage detected,
- flash crash probability jump,
- liquidity wall formed,
- liquidity wall disappeared.

### Example Timeline

```text
10:31:02.410 - OFI turns sharply negative
10:31:02.427 - Ask-side spoofing cluster detected
10:31:02.450 - Bid depth collapses
10:31:02.470 - Flash crash risk rises to 78%
10:31:02.510 - Mid price drops 14 ticks
```

This makes the dashboard easier to present because it turns raw metrics into a market narrative.

---

## 8. Replay Controls

The frontend should support replay because the dataset is historical.

Controls:

- play / pause,
- speed: 0.25x, 1x, 5x, 10x,
- scrubber timeline,
- jump to next alert,
- reset zoom,
- select time range.

Replay is important because high-frequency events happen too quickly to understand in real time.

---

## 9. Presentation Story

During the demo, show one market event and explain it across all charts:

1. The 3D order book shows liquidity thinning.
2. OFI turns strongly negative.
3. Spoofing detector highlights large cancelled orders.
4. Spread widens.
5. Flash crash risk increases.
6. Market impact simulator shows that a large order now causes more slippage.

This tells a complete story:

> The market looked normal on a price chart, but MarketXray revealed hidden liquidity stress before the visible price move.

---

## Implementation Priority

### Phase 1: Minimum Demo

- KPI strip
- OFI vs price line chart
- spoofing indicator
- event timeline
- WebSocket connection to C++ snapshots

### Phase 2: Strong Demo

- 3D limit order book surface
- market impact simulator
- arbitrage graph
- replay controls

### Phase 3: Full Marks Polish

- synchronized zoom across all charts
- alert-driven replay
- suspicious order drilldown
- fill-level market impact bars
- export screenshot/report button

---

## Smooth High-Performance Frontend Plan

The frontend must be designed as a real-time visualization engine, not as a normal React dashboard. The main rule is:

```text
React controls UI.
Canvas/WebGL renders data.
Web Workers process streams.
Typed arrays store chart buffers.
```

React should never re-render thousands of points every tick.

---

## Frontend Performance Architecture

```text
C++ WebSocket stream
        |
        v
Web Worker
  - parse JSON
  - validate fields
  - aggregate snapshots
  - write to ring buffers
        |
        v
Shared chart buffers
  - Float32Array
  - Uint32Array
  - circular indexes
        |
        v
Render loop
  - requestAnimationFrame
  - draw at 30-60 FPS
  - batch many backend messages into one visual frame
        |
        v
React UI shell
  - filters
  - controls
  - selected timestamp
  - panels
```

The backend may process millions of events, but the browser should only render at screen speed.

---

## Rendering Choices

### 3D LOB Surface

Use:

```text
Three.js / WebGL
```

Do not render the order book as HTML or SVG.

The 3D surface should use a fixed-size rolling matrix:

```text
time steps: last 300-800 snapshots
price levels: top 50 bids + top 50 asks
volume matrix: Float32Array
```

Update only the newest column of the surface when a new snapshot arrives. Avoid rebuilding the entire geometry every frame.

### OFI vs Price

Use:

```text
Canvas 2D or WebGL line renderer
D3 only for scales and axes
```

Keep the latest N points in a ring buffer:

```text
N = 10,000 to 100,000 depending on browser performance
```

When zoomed out, draw downsampled data. When zoomed in, draw exact data.

### Spoofing Scatter Plot

Use:

```text
Canvas/WebGL points
```

Render:

- all hard alerts,
- recent medium/high-risk points,
- faded older low-risk points.

This keeps the chart alive even when `spoofing_detected` is false.

### Arbitrage Graph

Use:

```text
SVG for small graph
Canvas for large graph
```

Since arbitrage nodes are usually few, SVG is acceptable and gives clean labels. If many assets/exchanges are added later, switch to Canvas.

### Market Impact Simulator

Use:

```text
React + Canvas/SVG bar chart
```

This chart only updates when the user changes quantity, side, or timestamp, so it does not need heavy WebGL.

---

## Data Strategy For Smoothness

Do not send raw every-order data to the browser for every tick. Send compact snapshots.

Good snapshot shape:

```json
{
  "timestamp_ns": 1719470400000000000,
  "microstructure": {
    "ofi": 1234.5,
    "ofi_normalized": 0.12,
    "vwap": 15002.4,
    "best_bid": 15000,
    "best_ask": 15002,
    "spread_ticks": 2
  },
  "depth": {
    "bids": [[15000, 12000], [14999, 9000]],
    "asks": [[15002, 11000], [15003, 8000]]
  },
  "risk": {
    "flash_crash_probability": 0.41,
    "spoofing_detected": false,
    "spoofing_cancel_ratio": 0.72
  }
}
```

Compact arrays are faster than verbose object arrays:

```json
[[price, volume], [price, volume]]
```

instead of:

```json
[{"price": 15000, "volume": 12000}]
```

---

## Ring Buffer Design

Every time-series chart should use a fixed-size ring buffer.

Example:

```text
timestamps: Float64Array
midPrices: Float32Array
ofiValues: Float32Array
vwapValues: Float32Array
writeIndex: integer
capacity: 50000
```

When the buffer fills, overwrite the oldest values.

Benefits:

- no memory growth,
- no expensive array shifting,
- stable performance during long demos,
- instant replay of recent history.

---

## Animation Rules

Use `requestAnimationFrame`.

Render loop:

```text
1. WebSocket receives many snapshots.
2. Worker writes them into buffers.
3. Renderer wakes at next animation frame.
4. Renderer draws latest available state.
5. UI stays smooth.
```

Target:

```text
60 FPS for normal charts
30 FPS minimum for 3D surface
under 16 ms per frame for smooth interaction
```

Never call React `setState()` for every WebSocket message.

---

## Downsampling Strategy

When showing a large time range, the frontend should not draw every point.

Use:

```text
Largest Triangle Three Buckets (LTTB)
or min/max bucket downsampling
```

For example:

```text
50,000 OFI points
screen width = 1,400 pixels
draw about 1,400-3,000 representative points
```

When the user zooms in, show the exact raw points for that smaller time window.

This gives both:

- speed when zoomed out,
- accuracy when zoomed in.

---

## Visual Polish For Premium Feel

### Motion

Use smooth transitions, but keep them short:

```text
100-180 ms for chart updates
200-300 ms for panel transitions
```

Avoid slow animations. The product should feel fast and analytical.

### Hover

Hover interactions should be instant:

- vertical crosshair across charts,
- synchronized tooltip,
- highlighted timestamp in all views,
- nearest data point marker.

### Alerts

Use clear but restrained alert styling:

- amber pulse for medium risk,
- red flash for hard spoofing/arbitrage/flash-crash alert,
- event automatically added to timeline.

### Replay

Replay should feel like a market playback system:

- play/pause,
- speed selector,
- scrubber,
- jump to next alert,
- selected event marker.

### Empty States

Charts should never look broken if data is missing.

Show:

```text
Waiting for stream...
No spoofing alerts in selected window
No arbitrage cycle detected
```

Use small, quiet text inside chart panels.

---

## What To Avoid

Avoid:

- storing all data in React state,
- rendering thousands of SVG circles,
- appending to normal arrays forever,
- recomputing scales on every message,
- parsing huge JSON on the main thread,
- sending full raw order-level data to browser every tick,
- rebuilding Three.js geometry from scratch every frame,
- using heavy table components for live tick data.

These are the main causes of lag.

---

## Best Frontend Stack

Recommended:

```text
Vite
React
TypeScript
Three.js
D3-scale / D3-axis / D3-brush
Canvas 2D for dense charts
Web Workers
Zustand or useSyncExternalStore for lightweight state
```

Use D3 for math and interaction, not for rendering every high-frequency point.

---

## Smoothness Checklist

- [ ] WebSocket data handled in a worker
- [ ] Charts use ring buffers
- [ ] Heavy charts use Canvas/WebGL
- [ ] React state only stores UI controls
- [ ] Render loop uses `requestAnimationFrame`
- [ ] Chart updates are batched
- [ ] Time-series charts use downsampling
- [ ] 3D surface updates incrementally
- [ ] Old points are faded or overwritten
- [ ] Replay mode works without reloading data
- [ ] Crosshair and tooltip are synchronized
- [ ] Dashboard stays responsive during full-speed stream

---

## Why This Visualization Set Deserves High Marks

This frontend directly matches the project proposal and demonstrates:

- high-frequency data handling,
- real-time analytics,
- order-book reconstruction,
- financial microstructure understanding,
- graph algorithms,
- interactive visual analytics,
- systems integration across Rust, C++, and web frontend.

The key strength is that the visualizations are connected. They do not just show separate charts. Together, they explain why a market event happened.
