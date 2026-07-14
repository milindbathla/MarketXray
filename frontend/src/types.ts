// ============================================================================
// Backend contract — mirrors src/Exporter.hpp snapshot_to_json() EXACTLY.
// One JSON object per WebSocket message from ws://localhost:9001.
// ============================================================================

export type BookLevel = [price: number, volume: number];

export interface SuspiciousOrder {
  price: number;
  quantity: number;
  lifetime_ms: number; // time between ADD and CANCEL
  score: number;       // cancel_ratio at time of alert (0..1)
}

export interface Execution {
  timestamp_ns: number;
  price: number;
  quantity: number;
  side: "BUY" | "SELL";
}

export interface Snapshot {
  timestamp_ns: number;
  source_timestamp_ns: number;
  microstructure: {
    ofi: number;
    ofi_normalized: number;
    vwap: number;
    best_bid: number;   // integer price ticks
    best_ask: number;
    spread_ticks: number;
  };
  hawkes: {
    intensity: number;
    p_next_event_1s: number;
    critical: boolean;
  };
  pin: {
    score: number;
    level: string; // "LOW" | "MODERATE" | "HIGH" ...
  };
  market_impact: {
    avg_fill_price: number;
    slippage_bps: number;
    fully_filled: boolean;
    levels_consumed: number;
  };
  risk: {
    flash_crash_probability: number;
    arbitrage_opportunity: boolean;
    arbitrage_profit_factor: number;
    arbitrage_cycle: number[];
    spoofing_detected: boolean;
    spoofing_cancel_ratio: number;
  };
  spoofing: {
    is_active: boolean;
    cancel_ratio: number;
    suspicious_orders: SuspiciousOrder[];
  };
  executions: Execution[];
  book_depth: {
    bids: BookLevel[]; // top 50 non-empty, descending price
    asks: BookLevel[]; // top 50 non-empty, ascending price
  };
}

// ============================================================================
// Frontend-local derived types
// ============================================================================

/** One time & sales print. Live mode uses backend execution messages; SIM mode
 *  generates equivalent prints locally. */
export interface Trade {
  t: number;      // ms epoch (frontend receive-time base)
  price: number;  // ticks
  size: number;
  side: "BUY" | "SELL";
}

/** Suspicious order tagged with arrival time for the scatter's time decay. */
export interface SpoofPoint extends SuspiciousOrder {
  t: number; // ms received
}

export type FeedStatus = "connecting" | "live" | "sim";

/** Prices from the engine are integer ticks; 1 tick = $0.01. */
export const TICK = 0.01;

export const fmtPrice = (ticks: number): string => (ticks * TICK).toFixed(2);

export const fmtClock = (ms: number): string => {
  if (ms < 86_400_000 * 10) {
    const total = Math.max(0, Math.floor(ms));
    const hh = Math.floor(total / 3_600_000);
    const mm = Math.floor((total % 3_600_000) / 60_000);
    const ss = Math.floor((total % 60_000) / 1000);
    const mmm = total % 1000;
    const p = (n: number, l = 2) => String(n).padStart(l, "0");
    return `${p(hh)}:${p(mm)}:${p(ss)}.${p(mmm, 3)}`;
  }
  const d = new Date(ms);
  const p = (n: number, l = 2) => String(n).padStart(l, "0");
  return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}.${p(d.getMilliseconds(), 3)}`;
};

export const fmtNum = (v: number): string =>
  Math.abs(v) >= 1000 ? v.toLocaleString("en-US", { maximumFractionDigits: 0 }) : v.toFixed(0);

export const midOf = (s: Snapshot): number =>
  (s.microstructure.best_bid + s.microstructure.best_ask) / 2;
