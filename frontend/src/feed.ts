// ============================================================================
// FeedStore — external store for the market scanner and selected-symbol drilldown.
//
// LIVE : WebSocket to the multi-symbol hub through Vite in development.
// SIM  : local per-symbol simulators producing the same snapshot schema.
//
// Components re-render from one external store snapshot, while each symbol keeps
// its own history buffers for charts and drilldown panels.
// ============================================================================

import { Simulator } from "./sim";
import type { FeedStatus, Snapshot, SpoofPoint, Trade } from "./types";

const websocketProtocol = window.location.protocol === "https:" ? "wss:" : "ws:";
const configuredBackendUrl = import.meta.env.VITE_BACKEND_URL;
export const BACKEND_URL = configuredBackendUrl
  ?? (import.meta.env.DEV
    ? `${websocketProtocol}//${window.location.host}/marketxray-ws`
    : `${websocketProtocol}//${window.location.hostname}:9001`);
const SIM_SYMBOLS = ["AAPL", "MSFT", "NVDA", "TSLA", "AMZN"];

// Retain by market event time, not an arbitrary sample count. At a 10 ms
// cadence, the old 900-snapshot cap discarded a 12 s chart's left edge.
const HISTORY_WINDOW_MS = 30_000;
const MAX_SNAPS = 5_000;
const MAX_TRADES = 5_000;
const MAX_SPOOFS = 5_000;
const SIM_INTERVAL_MS = 100;
const RECONNECT_MS = 5000;

export interface MarketEntry {
  symbol: string;
  latest: Snapshot | null;
  snaps: Snapshot[];
  times: number[];
}

export interface FeedFrame {
  /** Increments for every accepted stream update, including after history wraps. */
  revision: number;
  clockMs: number;
  snaps: Snapshot[];
  times: number[];
  trades: Trade[];
  spoofs: SpoofPoint[];
  market: MarketEntry[];
  selectedSymbol: string;
  status: FeedStatus;
  msgRate: number;
  latencyMs: number;
  historyMs: number;
}

interface SymbolState {
  snaps: Snapshot[];
  times: number[];
  trades: Trade[];
  spoofs: SpoofPoint[];
}

type Listener = () => void;

class FeedStore {
  private market = new Map<string, SymbolState>();
  private marketOrder: string[] = [];
  private selectedSymbol = "AAPL";
  private status: FeedStatus = "connecting";
  private version = 0;
  private listeners = new Set<Listener>();
  private cached: FeedFrame | null = null;

  private ws: WebSocket | null = null;
  private sims = new Map<string, Simulator>();
  private simTimer: number | null = null;
  private retryTimer: number | null = null;
  private recvStamps: number[] = [];
  private lastRecv = 0;
  private latency = 0.4;
  private preferSim = false;

  start(): void {
    if (this.preferSim) this.startSim();
    else this.connect();
  }

  setPreferSim(v: boolean): void {
    this.preferSim = v;
    this.teardown();
    if (v) this.startSim();
    else {
      this.status = "connecting";
      this.bump();
      this.connect();
    }
  }

  getPreferSim(): boolean { return this.preferSim; }

  setSelectedSymbol(symbol: string): void {
    if (!this.market.has(symbol)) return;
    if (this.selectedSymbol === symbol) return;
    this.selectedSymbol = symbol;
    this.bump(); // trigger re-render so all panels switch to new symbol
  }

  getSelectedSymbol(): string { return this.selectedSymbol; }

  subscribe = (fn: Listener): (() => void) => {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  };

  getVersion = (): number => this.version;

  frame(): FeedFrame {
    if (!this.cached) {
      const now = performance.now();
      this.recvStamps = this.recvStamps.filter((t) => now - t < 2000);

      const market = this.marketOrder.map((symbol) => {
        const state = this.market.get(symbol) ?? this.emptyState();
        return {
          symbol,
          latest: state.snaps[state.snaps.length - 1] ?? null,
          snaps: state.snaps,
          times: state.times,
        };
      });

      const selectedState = this.market.get(this.selectedSymbol)
        ?? (market.length > 0 ? this.market.get(market[0].symbol) : null)
        ?? this.emptyState();

      this.cached = {
        revision: this.version,
        snaps: selectedState.snaps,
        times: selectedState.times,
        trades: selectedState.trades,
        spoofs: selectedState.spoofs,
        market,
        selectedSymbol: this.selectedSymbol,
        status: this.status,
        msgRate: this.recvStamps.length / 2,
        latencyMs: this.latency,
        historyMs: Math.max(0, (selectedState.times.at(-1) ?? 0) - (selectedState.times[0] ?? 0)),
        clockMs: selectedState.times.at(-1) ?? 0,
      };
    }
    return this.cached;
  }

  private emptyState(): SymbolState {
    return { snaps: [], times: [], trades: [], spoofs: [] };
  }

  private getState(symbol: string): SymbolState {
    const existing = this.market.get(symbol);
    if (existing) return existing;
    const created = this.emptyState();
    this.market.set(symbol, created);
    this.marketOrder.push(symbol);
    if (!this.selectedSymbol) this.selectedSymbol = symbol;
    return created;
  }

  private bump(): void {
    this.version++;
    this.cached = null;
    for (const fn of this.listeners) fn();
  }

  private push(symbol: string, snapshot: Snapshot, trades: Trade[]): void {
    // Live frames are already ordered by the gateway. Rendering them directly
    // keeps every panel on the newest snapshot instead of replaying a queue.
    this.pushImmediate(symbol, snapshot, trades);
  }

  private pushImmediate(symbol: string, snapshot: Snapshot, trades: Trade[]): void {
    const now = performance.now();
    if (this.lastRecv > 0) {
      const gap = now - this.lastRecv;
      this.latency = this.latency * 0.95 + Math.min(9.9, Math.abs(gap - SIM_INTERVAL_MS) / 20) * 0.05;
    }
    this.lastRecv = now;
    this.recvStamps.push(now);

    const state = this.getState(symbol);
    const marketMs = (snapshot.source_timestamp_ns ?? snapshot.timestamp_ns) / 1e6;

    state.snaps.push(snapshot);
    state.times.push(marketMs);
    this.pruneSnapshotHistory(state, marketMs);

    for (const tr of trades) state.trades.push(tr);
    this.pruneTimedHistory(state.trades, marketMs, MAX_TRADES);

    for (const o of snapshot.spoofing.suspicious_orders) {
      state.spoofs.push({ ...o, t: marketMs });
    }
    this.pruneTimedHistory(state.spoofs, marketMs, MAX_SPOOFS);

    if (!this.market.has(this.selectedSymbol)) this.selectedSymbol = symbol;
  }

  private pruneSnapshotHistory(state: SymbolState, newestMs: number): void {
    const cutoff = newestMs - HISTORY_WINDOW_MS;
    let removeCount = 0;
    while (removeCount < state.times.length && state.times[removeCount] < cutoff) removeCount++;
    removeCount = Math.max(removeCount, state.snaps.length - MAX_SNAPS);
    if (removeCount > 0) {
      state.snaps.splice(0, removeCount);
      state.times.splice(0, removeCount);
    }
  }

  private pruneTimedHistory<T extends { t: number }>(items: T[], newestMs: number, maxItems: number): void {
    const cutoff = newestMs - HISTORY_WINDOW_MS;
    let removeCount = 0;
    while (removeCount < items.length && items[removeCount].t < cutoff) removeCount++;
    removeCount = Math.max(removeCount, items.length - maxItems);
    if (removeCount > 0) items.splice(0, removeCount);
  }

  private executionTrades(snapshot: Snapshot): Trade[] {
    // Accept snapshots from an engine restarted before the executions field
    // exists instead of dropping the entire live frame.
    const executions = Array.isArray(snapshot.executions) ? snapshot.executions : [];
    // Snapshots are displayed on the ITCH time-of-day timeline, whereas each
    // execution carries the source epoch timestamp. Normalize them before the
    // tape/window filters see the trade.
    const sourceMs = (snapshot.source_timestamp_ns ?? snapshot.timestamp_ns) / 1e6;
    const epochOffsetMs = snapshot.timestamp_ns / 1e6 - sourceMs;
    return executions.map((execution) => ({
      t: execution.timestamp_ns / 1e6 - epochOffsetMs,
      price: execution.price,
      size: execution.quantity,
      side: execution.side,
    }));
  }

  private connect(): void {
    try {
      this.ws = new WebSocket(BACKEND_URL);
    } catch {
      this.fallback();
      return;
    }

    const ws = this.ws;
    const guard = window.setTimeout(() => {
      if (this.status !== "live") ws.close();
    }, 3000);

    ws.onopen = () => {
      window.clearTimeout(guard);
      this.stopSim();
      this.status = "live";
      this.bump();
    };

    ws.onmessage = (ev: MessageEvent<string>) => {
      try {
        const payload = JSON.parse(ev.data) as
          | Snapshot
          | { kind?: string; symbols?: Array<{ symbol: string; snapshot: Snapshot }> };

        if ("kind" in payload && payload.kind === "market_batch" && Array.isArray(payload.symbols)) {
          let updated = false;
          for (const entry of payload.symbols) {
            if (entry.snapshot?.microstructure && entry.snapshot.book_depth) {
              this.push(entry.symbol, entry.snapshot, this.executionTrades(entry.snapshot));
              updated = true;
            }
          }
          if (payload.symbols.length > 0 && !this.market.has(this.selectedSymbol)) {
            this.selectedSymbol = payload.symbols[0].symbol;
          }
          if (updated) this.bump();
          return;
        }

        const snapshot = payload as Snapshot;
        if (snapshot?.microstructure && snapshot.book_depth) {
          this.push("AAPL", snapshot, this.executionTrades(snapshot));
          this.bump();
        }
      } catch {
        // skip malformed frame
      }
    };

    ws.onerror = () => {};
    ws.onclose = () => {
      window.clearTimeout(guard);
      this.ws = null;
      if (!this.preferSim) this.fallback();
    };
  }

  private fallback(): void {
    this.startSim();
    if (this.retryTimer === null && !this.preferSim) {
      this.retryTimer = window.setInterval(() => {
        if (this.status !== "live" && this.ws === null && !this.preferSim) this.connect();
      }, RECONNECT_MS);
    }
  }

  private startSim(): void {
    if (this.simTimer !== null) return;
    this.market.clear();
    this.marketOrder = [];
    this.sims = new Map(SIM_SYMBOLS.map((symbol, i) => [symbol, new Simulator(i * 180)]));
    this.selectedSymbol = SIM_SYMBOLS[0];
    this.status = "sim";
    this.bump();
    this.simTimer = window.setInterval(() => {
      for (const [symbol, sim] of this.sims) {
        const { snapshot, trades } = sim.step();
        this.push(symbol, snapshot, trades);
      }
      this.bump();
    }, SIM_INTERVAL_MS);
  }

  private stopSim(): void {
    if (this.simTimer !== null) {
      window.clearInterval(this.simTimer);
      this.simTimer = null;
    }
  }

  private teardown(): void {
    this.stopSim();
    if (this.retryTimer !== null) {
      window.clearInterval(this.retryTimer);
      this.retryTimer = null;
    }
    if (this.ws) {
      this.ws.onclose = null;
      this.ws.close();
      this.ws = null;
    }
  }
}

export const feed = new FeedStore();
