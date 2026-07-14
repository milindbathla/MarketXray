# MarketXray Architecture

## Live ITCH Replay

```text
                         per selected symbol
+-------------------+    ITCH messages     +-----------------------------+
| NASDAQ ITCH .gz   | -------------------> | Rust ingestor/orchestrator  |
| historical input  |                      | - parses binary ITCH        |
+-------------------+                      | - maps stock_locate -> symbol|
                                             | - filters target symbols     |
                                             | - replays from 09:30 ET      |
                                             +--------------+--------------+
                                                            |
                                                   64-byte Tick records
                                                            |
                                                            v
                                             +-----------------------------+
                                             | POSIX shared memory          |
                                             | one SPSC queue per symbol    |
                                             +--------------+--------------+
                                                            |
                                                            v
                                             +-----------------------------+
                                             | C++ market_xray --daemon     |
                                             | - limit order book            |
                                             | - OFI / VWAP / PIN / Hawkes  |
                                             | - risk, spoofing, depth       |
                                             +--------------+--------------+
                                                            |
                                                       JSON snapshot
                                                            |
                                                            v
                                             +-----------------------------+
                                             | Rust snapshot map + gateway  |
                                             | - latest snapshot per symbol |
                                             | - changed market_batch only  |
                                             | - WebSocket :9001            |
                                             +--------------+--------------+
                                                            |
                                                  ws://localhost:9001
                                                            |
                                                            v
                                             +-----------------------------+
                                             | React / Vite dashboard       |
                                             | FeedStore -> charts/panels   |
                                             +-----------------------------+
```

## Multi-Stock Parsing And Routing

```text
Configured symbols: AAPL, MSFT, NVDA, TSLA, AMZN
                         |
                         v
              +-----------------------+
              | Read one ITCH record  |
              +-----------+-----------+
                          |
          +---------------+----------------+
          |                                |
          v                                v
  Stock Directory (R)                 Order / trade record
  stock_locate=14, symbol=AAPL        stock_locate=14, type=A/X/E/...
          |                                |
          v                                v
  target symbol?                    locate known and targeted?
          |                                |
          v                                v
  locate_to_symbol[14] = AAPL       +-------------------------------+
                                    | No  -> discard this record     |
                                    | Yes -> resolve symbol = AAPL   |
                                    +---------------+---------------+
                                                    |
                                                    v
                              +-------------------------------------+
                              | Preserve order state with key:      |
                              | (stock_locate, order_id)            |
                              |                                     |
                              | A/F: store side and price            |
                              | E/C/X/D/U: look up original order    |
                              +------------------+------------------+
                                                 |
                                                 v
                              +-------------------------------------+
                              | Convert to a normalized 64-byte Tick |
                              | timestamp, order_id, price, qty,    |
                              | side, ADD/CANCEL/EXECUTE             |
                              +------------------+------------------+
                                                 |
                 +-------------------------------+-------------------------------+
                 |                               |                               |
                 v                               v                               v
        /mxray_AAPL queue               /mxray_MSFT queue               /mxray_NVDA queue
                 |                               |                               |
                 v                               v                               v
         C++ AAPL book/metrics           C++ MSFT book/metrics           C++ NVDA book/metrics
```

Records for symbols outside the configured list never enter a queue, so each
C++ order book and its metrics remain specific to one stock.

## Live Replay Components

```text
The symbol-specific workers above feed the snapshot gateway:

Rust snapshot map -> changed `market_batch` frames -> WebSocket :9001 -> browser
```


## Browser Fallback

```text
WebSocket unavailable
        |
        v
FeedStore starts one local Simulator per symbol
        |
        v
Same snapshot schema -> dashboard panels
```

## Direct Adapter

```text
shared-memory producer -> market_xray_ws -> WebSocket :9001 -> dashboard
```

`market_xray_ws` is a separate direct broadcaster. Do not run it with the Rust
orchestrator because both own WebSocket port `9001`.
