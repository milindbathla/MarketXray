// ============================================================================
// Simulated feed — produces Snapshots in the EXACT Exporter.hpp schema.
// Used automatically when the C++ engine (ws://localhost:9001) is offline,
// e.g. on native Windows where the POSIX backend cannot run. Clearly labelled
// "SIM" in the UI. The dynamics are made financially coherent:
//   • mid follows a mean-reverting random walk, OFI leads price moves
//   • Hawkes λ is a true self-exciting process: jumps on event clusters,
//     exponential decay e^{-β·Δt} between them (the staircase-then-glide)
//   • book depth decays away from touch, with occasional liquidity walls
//   • spoofing bursts inject large fast-cancelled orders
// ============================================================================

import type { BookLevel, Snapshot, SuspiciousOrder, Trade } from "./types";

// Hawkes parameters (match backend defaults: mu=0.5, alpha=0.8, beta=1.0)
const MU = 0.5;
const ALPHA = 0.8;
const BETA = 1.0;
const CRITICAL_LAMBDA = 2.6;

export interface SimTick {
  snapshot: Snapshot;
  trades: Trade[];
}

export class Simulator {
  private mid: number;
  private vwapPV = 0;
  private vwapV = 0;
  private lambda = MU;
  private lastT = performance.now();
  private pin = 0.42;
  private ofiState = 0;
  private frenzy = 0;           // 0..1 regime driver
  private wallBid: { off: number; vol: number; ttl: number } | null = null;
  private wallAsk: { off: number; vol: number; ttl: number } | null = null;

  constructor(seedOffset = 0) {
    this.mid = 15002 + seedOffset;
  }

  step(): SimTick {
    const now = performance.now();
    const nowEpochMs = Date.now();
    const nowSourceMs = nowEpochMs % 86_400_000;
    const dt = Math.min(1, (now - this.lastT) / 1000);
    this.lastT = now;

    // regime: occasionally wind the market up into a frenzy, then relax
    if (Math.random() < 0.006) this.frenzy = Math.min(1, this.frenzy + 0.5 + Math.random() * 0.5);
    this.frenzy *= 0.995;

    // Hawkes: decay, then self-excite on a Poisson-ish event count that rises
    // with frenzy — clustered events compound α (staircase effect).
    this.lambda = MU + (this.lambda - MU) * Math.exp(-BETA * dt);
    const nEvents = Math.random() < 0.25 + this.frenzy * 0.6 ? Math.floor(Math.random() * (1 + this.frenzy * 5)) + 1 : 0;
    this.lambda += ALPHA * nEvents * (0.25 + Math.random() * 0.3);
    const critical = this.lambda > CRITICAL_LAMBDA;

    // OFI leads price: persistent order-flow imbalance state
    this.ofiState = this.ofiState * 0.92 + (Math.random() - 0.5) * 900 + this.frenzy * (Math.random() - 0.35) * 1200;
    const ofi = this.ofiState;

    // mid follows OFI + noise, mean-reverts to 15000
    this.mid += ofi * 0.0004 + (Math.random() - 0.5) * (0.8 + this.frenzy * 4) + (15000 - this.mid) * 0.001;
    const spread = Math.max(1, Math.round(1 + this.frenzy * 3 + Math.random() * 1.6));
    const bb = Math.round(this.mid - spread / 2);
    const ba = bb + spread;

    // trades (aggressor side biased by OFI sign)
    const trades: Trade[] = [];
    const nTr = nEvents + (Math.random() < 0.5 ? 1 : 0);
    for (let i = 0; i < nTr; i++) {
      const buy = Math.random() < 0.5 + Math.max(-0.35, Math.min(0.35, ofi / 4000));
      const size = Math.round(25 * Math.exp(Math.random() * 3.2));
      const price = buy ? ba + Math.floor(Math.random() * 2) : bb - Math.floor(Math.random() * 2);
      trades.push({ t: nowSourceMs - Math.floor(Math.random() * 90), price, size, side: buy ? "BUY" : "SELL" });
      this.vwapPV += price * size;
      this.vwapV += size;
    }
    const vwap = this.vwapV > 0 ? this.vwapPV / this.vwapV : this.mid;

    // liquidity walls appear/expire
    const rollWall = (w: typeof this.wallBid): typeof this.wallBid => {
      if (w) { w.ttl -= 1; return w.ttl > 0 ? w : null; }
      return Math.random() < 0.01
        ? { off: 3 + Math.floor(Math.random() * 14), vol: 12000 + Math.random() * 9000, ttl: 40 + Math.random() * 120 }
        : null;
    };
    this.wallBid = rollWall(this.wallBid);
    this.wallAsk = rollWall(this.wallAsk);

    // book depth: 50 levels/side, exponential decay from touch + noise + walls
    const mkSide = (touch: number, dir: 1 | -1, wall: typeof this.wallBid): BookLevel[] => {
      const out: BookLevel[] = [];
      for (let i = 0; i < 50; i++) {
        let vol = 900 + 5200 * Math.exp(-i / 13) * (0.55 + Math.random() * 0.9);
        if (wall && i === wall.off) vol += wall.vol;
        if (critical) vol *= 0.45 + Math.random() * 0.2; // pre-crash thinning
        out.push([touch + dir * i, Math.round(vol)]);
      }
      return out;
    };
    const bids = mkSide(bb, -1, this.wallBid);
    const asks = mkSide(ba, 1, this.wallAsk);

    // market impact: walk the ask book with a 10,000-share buy (backend default)
    const impact = walkBook(asks, 10000);

    // spoofing bursts
    const spoofing = this.frenzy > 0.35 && Math.random() < 0.35;
    const sus: SuspiciousOrder[] = [];
    if (spoofing) {
      const n = 1 + Math.floor(Math.random() * 4);
      for (let i = 0; i < n; i++) {
        sus.push({
          price: (Math.random() < 0.5 ? bb : ba) + Math.round((Math.random() - 0.5) * 20),
          quantity: Math.round(800 * Math.exp(Math.random() * 3.5)),
          lifetime_ms: Math.round(10 * Math.exp(Math.random() * 4.6)), // 10ms..1s
          score: 0.55 + Math.random() * 0.44,
        });
      }
    }

    // slow PIN drift, flash-crash prob from λ + thinness
    this.pin = Math.max(0.05, Math.min(0.92, this.pin + (Math.random() - 0.5) * 0.015 + this.frenzy * 0.004));
    const flash = Math.max(0, Math.min(0.99, (this.lambda - MU) / (CRITICAL_LAMBDA * 1.4) + this.frenzy * 0.35));

    const snapshot: Snapshot = {
      timestamp_ns: nowEpochMs * 1e6,
      source_timestamp_ns: nowSourceMs * 1e6,
      microstructure: {
        ofi,
        ofi_normalized: Math.max(-1, Math.min(1, ofi / 3000)),
        vwap,
        best_bid: bb,
        best_ask: ba,
        spread_ticks: spread,
      },
      hawkes: {
        intensity: this.lambda,
        p_next_event_1s: 1 - Math.exp(-this.lambda),
        critical,
      },
      pin: {
        score: this.pin,
        level: this.pin < 0.3 ? "LOW" : this.pin < 0.6 ? "MODERATE" : "HIGH",
      },
      market_impact: impact,
      risk: {
        flash_crash_probability: flash,
        arbitrage_opportunity: Math.random() < 0.04,
        arbitrage_profit_factor: 1 + Math.random() * 0.002,
        arbitrage_cycle: [],
        spoofing_detected: spoofing,
        spoofing_cancel_ratio: spoofing ? 0.5 + Math.random() * 0.45 : Math.random() * 0.2,
      },
      spoofing: {
        is_active: spoofing,
        cancel_ratio: spoofing ? 0.5 + Math.random() * 0.45 : Math.random() * 0.2,
        suspicious_orders: sus,
      },
      executions: trades.map((trade) => ({
        timestamp_ns: trade.t * 1e6,
        price: trade.price,
        quantity: trade.size,
        side: trade.side,
      })),
      book_depth: { bids, asks },
    };

    return { snapshot, trades };
  }
}

/** Walk one side of the book with a market order; also used by the Market
 *  Impact panel to recompute for user-selected order sizes. */
export function walkBook(
  levels: BookLevel[],
  qty: number
): { avg_fill_price: number; slippage_bps: number; fully_filled: boolean; levels_consumed: number } {
  let remaining = qty;
  let cost = 0;
  let consumed = 0;
  const ref = levels.length > 0 ? levels[0][0] : 0;
  for (const [price, vol] of levels) {
    if (remaining <= 0) break;
    const take = Math.min(remaining, vol);
    cost += take * price;
    remaining -= take;
    consumed++;
  }
  const filled = qty - remaining;
  const avg = filled > 0 ? cost / filled : 0;
  const slip = ref > 0 && filled > 0 ? ((avg - ref) / ref) * 10000 : 0;
  return {
    avg_fill_price: avg,
    slippage_bps: Math.abs(slip),
    fully_filled: remaining <= 0,
    levels_consumed: consumed,
  };
}
