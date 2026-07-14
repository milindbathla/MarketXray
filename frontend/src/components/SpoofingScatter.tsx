// Spoofing Detector — log-log scatter of suspicious cancels.
// X = time-to-cancellation (ms, log), Y = order size (lots, log).
// Risk color from the engine's per-order score vs the threshold slider;
// dashed anomaly box outlines the danger region (big size, fast cancel).

import { useState } from "react";
import type { FeedFrame } from "../feed";
import { useCanvas } from "../hooks";
import { useSyncState } from "../sync";

const X_MIN = 1, X_MAX = 100000;   // ms
const Y_MIN = 10, Y_MAX = 100000;  // lots

export default function SpoofingScatter({ frame }: { frame: FeedFrame }) {
  const [threshold, setThreshold] = useState(0.85);
  const { windowMs, focusTime } = useSyncState();
  const end = frame.times.length > 0 ? frame.times[frame.times.length - 1] : Date.now();
  const start = end - windowMs;
  const pts = frame.spoofs.filter((p) => p.t >= start && p.t <= end);
  const now = end;

  const ref = useCanvas((ctx, w, h) => {
    const padL = 40, padR = 12, padT = 10, padB = 28;
    const iw = w - padL - padR, ih = h - padT - padB;
    const lx = (v: number) => padL + ((Math.log10(Math.max(X_MIN, Math.min(X_MAX, v))) - Math.log10(X_MIN)) / (Math.log10(X_MAX) - Math.log10(X_MIN))) * iw;
    const ly = (v: number) => padT + ih - ((Math.log10(Math.max(Y_MIN, Math.min(Y_MAX, v))) - Math.log10(Y_MIN)) / (Math.log10(Y_MAX) - Math.log10(Y_MIN))) * ih;

    // log grid + tick labels
    ctx.font = "9px JetBrains Mono";
    ctx.strokeStyle = "rgba(148,163,184,0.08)";
    ctx.fillStyle = "#5b6675";
    for (let e = 0; e <= 5; e++) {
      const gx = lx(Math.pow(10, e));
      ctx.beginPath(); ctx.moveTo(gx, padT); ctx.lineTo(gx, padT + ih); ctx.stroke();
      ctx.textAlign = "center";
      ctx.fillText(e === 0 ? "1" : `10${superscript(e)}`, gx, h - 14);
    }
    for (let e = 1; e <= 5; e++) {
      const gy = ly(Math.pow(10, e));
      ctx.beginPath(); ctx.moveTo(padL, gy); ctx.lineTo(padL + iw, gy); ctx.stroke();
      ctx.textAlign = "right";
      ctx.fillText(`10${superscript(e)}`, padL - 4, gy + 3);
    }
    ctx.textAlign = "center";
    ctx.fillText("Time to Cancellation (ms) — log", padL + iw / 2, h - 3);
    ctx.save();
    ctx.translate(10, padT + ih / 2);
    ctx.rotate(-Math.PI / 2);
    ctx.fillText("Order Size (lots) — log", 0, 0);
    ctx.restore();

    // anomaly region: fast cancel (<1s) AND large (>1k lots)
    const ax0 = lx(X_MIN), ax1 = lx(1000), ay0 = ly(Y_MAX), ay1 = ly(1000);
    ctx.strokeStyle = "rgba(239,68,68,0.55)";
    ctx.fillStyle = "rgba(239,68,68,0.05)";
    ctx.setLineDash([5, 4]);
    ctx.fillRect(ax0, ay0, ax1 - ax0, ay1 - ay0);
    ctx.strokeRect(ax0, ay0, ax1 - ax0, ay1 - ay0);
    ctx.setLineDash([]);
    ctx.fillStyle = "rgba(248,113,113,0.9)";
    ctx.font = "8px Inter";
    ctx.textAlign = "left";
    ctx.fillText("ANOMALY ZONE", ax0 + 6, ay0 + 12);

    // points — newest brighter; risk class = score vs slider threshold
    for (const p of pts) {
      const age = Math.min(1, (now - p.t) / 30000);
      const alpha = 0.9 - age * 0.65;
      const nearFocus = focusTime !== null && Math.abs(p.t - focusTime) <= 1000;
      const r = 2 + Math.min(4, Math.log10(Math.max(10, p.quantity)) - 1) + (nearFocus ? 1.5 : 0);
      const high = p.score >= threshold;
      const med = !high && p.score >= threshold - 0.25;
      ctx.fillStyle = high
        ? `rgba(239,68,68,${alpha})`
        : med
          ? `rgba(251,191,36,${alpha})`
          : `rgba(148,163,184,${alpha * 0.8})`;
      ctx.beginPath();
      ctx.arc(lx(p.lifetime_ms), ly(p.quantity), r, 0, Math.PI * 2);
      ctx.fill();
      if (nearFocus) {
        ctx.strokeStyle = "rgba(251,191,36,0.9)";
        ctx.lineWidth = 1.1;
        ctx.beginPath();
        ctx.arc(lx(p.lifetime_ms), ly(p.quantity), r + 2.2, 0, Math.PI * 2);
        ctx.stroke();
      }
    }
  }, `${frame.revision}-${threshold}-${windowMs}-${focusTime}`);

  return (
    <section className="panel cell-spoof">
      <div className="panel-head">
        <div className="panel-title danger">
          <span className="material-symbols-outlined">track_changes</span>
          Spoofing Detector
        </div>
        <div className="panel-controls">
          <span>Threshold</span>
          <input
            className="range"
            type="range" min={0.5} max={0.99} step={0.01}
            value={threshold}
            onChange={(e) => setThreshold(Number(e.target.value))}
          />
          <b className="c-text mono">{(threshold * 100).toFixed(0)}%</b>
        </div>
      </div>
      <div className="panel-body">
        <canvas ref={ref} className="chart-abs" />
        <div className="overlay-bc legend">
          <span><span className="sw dotpt" style={{ background: "#ef4444" }} />High risk</span>
          <span><span className="sw dotpt" style={{ background: "#fbbf24" }} />Medium</span>
          <span><span className="sw dotpt" style={{ background: "#94a3b8" }} />Low</span>
          <span className="c-muted mono">{pts.length} cancels</span>
        </div>
      </div>
    </section>
  );
}

function superscript(e: number): string {
  const map: Record<string, string> = { "1": "¹", "2": "²", "3": "³", "4": "⁴", "5": "⁵" };
  return map[String(e)] ?? "";
}
