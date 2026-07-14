import { feed, type FeedFrame, type MarketEntry } from "../feed";
import { fmtPrice } from "../types";

export default function MarketScanner(
  { frame, compact = false }: { frame: FeedFrame; compact?: boolean }
) {
  const active = frame.market.find((entry) => entry.symbol === frame.selectedSymbol) ?? frame.market[0] ?? null;
  const others = frame.market.filter((entry) => entry.symbol !== active?.symbol);

  if (compact) {
    return (
      <div className="scanner-strip">
        {frame.market.map((entry) => {
          const selected = entry.symbol === frame.selectedSymbol;
          return (
            <button
              key={entry.symbol}
              className={`scanner-strip-card ${selected ? "selected" : ""}`}
              onClick={() => feed.setSelectedSymbol(entry.symbol)}
            >
              <div className="scanner-strip-head">
                <span className="scanner-strip-symbol">{entry.symbol}</span>
                <span className="scanner-strip-ofi">{signed(entry.latest?.microstructure.ofi)}</span>
                <span className={`scanner-dot ${toneOfi(entry)}`} />
              </div>
            </button>
          );
        })}
      </div>
    );
  }

  return (
    <section className="scanner panel">
      <div className="panel-head">
        <div className="panel-title">
          <span className="material-symbols-outlined">dashboard</span>
          Market Scanner
        </div>
        <div className="panel-controls">
          <span>{frame.market.length} symbols</span>
          <span className="mono c-muted">click to drill down</span>
        </div>
      </div>
      <div className="scanner-body">
        {active && (
          <button className="scanner-focus" onClick={() => feed.setSelectedSymbol(active.symbol)}>
            <div className="scanner-focus-head">
              <span className="scanner-symbol">{active.symbol}</span>
              <span className="scanner-chip">{signalLabel(active)}</span>
            </div>
            <div className="scanner-focus-grid">
              <Metric label="Spread" value={`${active.latest?.microstructure.spread_ticks.toFixed(1) ?? "—"}t`} tone="cyan" />
              <Metric label="OFI" value={signed(active.latest?.microstructure.ofi)} tone={toneOfi(active)} />
              <Metric label="Flash Risk" value={`${((active.latest?.risk.flash_crash_probability ?? 0) * 100).toFixed(0)}%`} tone="amber" />
              <Metric label="Spoof" value={`${((active.latest?.spoofing.cancel_ratio ?? 0) * 100).toFixed(0)}%`} tone="red" />
              <Metric label="Best Bid" value={active.latest ? `$${fmtPrice(active.latest.microstructure.best_bid)}` : "—"} tone="text" />
              <Metric label="Best Ask" value={active.latest ? `$${fmtPrice(active.latest.microstructure.best_ask)}` : "—"} tone="text" />
            </div>
            <HeatStrip entry={active} active />
          </button>
        )}

        <div className="scanner-stack">
          {others.map((entry) => (
            <button key={entry.symbol} className="scanner-mini" onClick={() => feed.setSelectedSymbol(entry.symbol)}>
              <div className="scanner-mini-head">
                <span className="scanner-mini-symbol">{entry.symbol}</span>
                <span className={`scanner-dot ${toneOfi(entry)}`} />
              </div>
              <div className="scanner-mini-metrics">
                <span>{signed(entry.latest?.microstructure.ofi)}</span>
                <span>risk {((entry.latest?.risk.flash_crash_probability ?? 0) * 100).toFixed(0)}%</span>
                <span>spr {entry.latest?.microstructure.spread_ticks.toFixed(1) ?? "—"}t</span>
              </div>
              <HeatStrip entry={entry} />
            </button>
          ))}
        </div>
      </div>
    </section>
  );
}

function Metric(
  { label, value, tone }: { label: string; value: string; tone: "cyan" | "green" | "red" | "amber" | "text" }
) {
  return (
    <div className="scanner-metric">
      <span className="scanner-metric-label">{label}</span>
      <span className={`scanner-metric-value tone-${tone}`}>{value}</span>
    </div>
  );
}

function HeatStrip({ entry, active = false }: { entry: MarketEntry; active?: boolean }) {
  const series = entry.snaps.slice(-18).map((snap) => {
    const ofi = Math.abs(snap.microstructure.ofi);
    const hawkes = snap.hawkes.intensity;
    return Math.min(1, ofi / 4000 + hawkes / 6);
  });
  return (
    <div className={`scanner-heat ${active ? "active" : ""}`}>
      {series.map((v, i) => (
        <span
          key={`${entry.symbol}-${i}`}
          style={{
            height: `${18 + v * (active ? 30 : 18)}px`,
            opacity: 0.35 + v * 0.65,
            background: colorFor(v),
          }}
        />
      ))}
    </div>
  );
}

function colorFor(v: number): string {
  if (v > 0.8) return "linear-gradient(180deg, #f87171, #ef4444)";
  if (v > 0.55) return "linear-gradient(180deg, #fbbf24, #f59e0b)";
  return "linear-gradient(180deg, #8aebff, #2fd9f4)";
}

function signalLabel(entry: MarketEntry): string {
  const latest = entry.latest;
  if (!latest) return "NO DATA";
  if (latest.hawkes.intensity > 2.4 || latest.spoofing.cancel_ratio > 0.3) return "ANOMALY";
  if (Math.abs(latest.microstructure.ofi) > 1800) return "PRESSURE";
  return "STABLE";
}

function signed(value: number | undefined): string {
  if (value === undefined) return "—";
  return `${value >= 0 ? "+" : ""}${value.toFixed(0)}`;
}

function toneOfi(entry: MarketEntry): "green" | "red" {
  return (entry.latest?.microstructure.ofi ?? 0) >= 0 ? "green" : "red";
}
