import { useMemo } from "react";
import type { FeedFrame } from "../feed";
import { fmtPrice } from "../types";

function Spark({ values, color }: { values: number[]; color: string }) {
  const path = useMemo(() => {
    if (values.length < 2) return "";
    let min = Infinity, max = -Infinity;
    for (const v of values) { if (v < min) min = v; if (v > max) max = v; }
    const span = max - min || 1;
    const n = Math.max(1, values.length - 1);
    return values
      .map((v, i) => `${i === 0 ? "M" : "L"}${((i / n) * 100).toFixed(1)},${(30 - ((v - min) / span) * 26 - 2).toFixed(1)}`)
      .join("");
  }, [values]);

  return (
    <svg className="kpi-spark" viewBox="0 0 100 30" preserveAspectRatio="none">
      <path d={path} fill="none" stroke={color} strokeWidth={1.4} vectorEffect="non-scaling-stroke" />
    </svg>
  );
}

const CY = "#2fd9f4", GN = "#22c55e", RD = "#ef4444", AM = "#fbbf24";

export default function KpiStrip({ frame }: { frame: FeedFrame }) {
  const { snaps } = frame;
  const s = snaps.length > 0 ? snaps[snaps.length - 1] : null;
  const window60 = snaps.slice(-60);

  const series = (f: (i: number) => number) => window60.map((_, i) => f(i));
  const spread = series((i) => window60[i].microstructure.spread_ticks);
  const ofi = series((i) => window60[i].microstructure.ofi);
  const vwap = series((i) => window60[i].microstructure.vwap);
  const pin = series((i) => window60[i].pin.score);
  const spoof = series((i) => window60[i].spoofing.cancel_ratio);
  const flash = series((i) => window60[i].risk.flash_crash_probability);

  if (!s) return <section className="kpis" />;

  const m = s.microstructure;
  const ofiPos = m.ofi >= 0;
  const flashPct = s.risk.flash_crash_probability * 100;
  const flashColor = flashPct > 55 ? RD : flashPct > 30 ? AM : GN;
  const spoofActive = s.spoofing.is_active;

  return (
    <section className="kpis">
      <div className="kpi">
        <div className="kpi-label">Spread</div>
        <div className="kpi-row">
          <span className="kpi-value c-cyan">{m.spread_ticks.toFixed(1)}t</span>
          <Spark values={spread} color={CY} />
        </div>
      </div>
      <div className="kpi">
        <div className="kpi-label">OFI</div>
        <div className="kpi-row">
          <span className={`kpi-value ${ofiPos ? "c-green" : "c-red"}`}>
            {ofiPos ? "+" : "−"}{Math.abs(m.ofi).toLocaleString("en-US", { maximumFractionDigits: 0 })}
          </span>
          <Spark values={ofi} color={ofiPos ? GN : RD} />
        </div>
      </div>
      <div className="kpi">
        <div className="kpi-label">VWAP</div>
        <div className="kpi-row">
          <span className="kpi-value c-cyan">${fmtPrice(m.vwap)}</span>
          <Spark values={vwap} color={CY} />
        </div>
      </div>
      <div className="kpi">
        <div className="kpi-label">PIN · {s.pin.level}</div>
        <div className="kpi-row">
          <span className="kpi-value c-amber">{s.pin.score.toFixed(2)}</span>
          <Spark values={pin} color={AM} />
        </div>
      </div>
      <div className={`kpi ${spoofActive ? "alert" : ""}`}>
        <div className="kpi-label">Spoofing {spoofActive && <span className="dot red pulse" />}</div>
        <div className="kpi-row">
          <span className={`kpi-value ${spoofActive ? "c-red" : "c-green"}`}>
            {spoofActive ? "ACTIVE" : "CLEAR"}
          </span>
          <Spark values={spoof} color={spoofActive ? RD : GN} />
        </div>
      </div>
      <div className={`kpi ${flashPct > 55 ? "alert" : ""}`}>
        <div className="kpi-label">Flash Crash</div>
        <div className="kpi-row">
          <span className="kpi-value" style={{ color: flashColor }}>{flashPct.toFixed(0)}%</span>
          <Spark values={flash} color={flashColor} />
        </div>
      </div>
    </section>
  );
}
