// OFI vs Price Action — dual-axis canvas chart. OFI as green/red bars around a
// zero baseline + net line; mid price as a white line on the right axis.
// Hover/focus time is synchronized with the rest of the dashboard.

import { scaleLinear } from "d3-scale";
import { useMemo } from "react";
import type { FeedFrame } from "../feed";
import { useCanvas } from "../hooks";
import { fmtPrice, midOf } from "../types";
import { useSyncState, WINDOW_PRESETS_MS } from "../sync";

export default function OFIPriceChart({ frame }: { frame: FeedFrame }) {
  const { windowMs, setWindowMs, focusTime, setFocusTime } = useSyncState();

  const view = useMemo(() => {
    if (frame.snaps.length === 0 || frame.times.length === 0) return null;
    const end = frame.times[frame.times.length - 1];
    const start = end - windowMs;
    let first = 0;
    while (first < frame.times.length && frame.times[first] < start) first++;
    const snaps = frame.snaps.slice(first);
    const times = frame.times.slice(first);
    return snaps.length > 1 ? { snaps, times, start, end } : null;
  // Feed history is a capped ring buffer and is updated in place. Revision is
  // therefore the reliable signal for every incoming snapshot.
  }, [frame.revision, windowMs]);

  const n = view?.snaps.length ?? 0;

  const ref = useCanvas((ctx, w, h) => {
    if (!view || n < 2) return;
    const padL = 40, padR = 48, padT = 8, padB = 16;
    const iw = w - padL - padR, ih = h - padT - padB;

    let ofiMax = 1;
    let pMin = Infinity, pMax = -Infinity;
    for (const s of view.snaps) {
      ofiMax = Math.max(ofiMax, Math.abs(s.microstructure.ofi));
      const m = midOf(s);
      if (m < pMin) pMin = m;
      if (m > pMax) pMax = m;
    }
    const pSpan = Math.max(1, pMax - pMin);
    const x = scaleLinear().domain([view.start, view.end]).range([padL, padL + iw]);
    const yOfi = scaleLinear().domain([-ofiMax, ofiMax]).range([padT + ih * 0.96, padT + ih * 0.04]);
    const yPx = scaleLinear().domain([pMin, pMin + pSpan]).range([padT + ih * 0.95, padT + ih * 0.05]);
    const barW = Math.max(1, iw / Math.max(1, n));

    // gridlines + zero line
    ctx.strokeStyle = "rgba(148,163,184,0.08)";
    ctx.lineWidth = 1;
    for (let g = 0; g <= 4; g++) {
      const gy = padT + (g / 4) * ih;
      ctx.beginPath(); ctx.moveTo(padL, gy); ctx.lineTo(padL + iw, gy); ctx.stroke();
    }
    ctx.strokeStyle = "rgba(148,163,184,0.25)";
    ctx.beginPath(); ctx.moveTo(padL, yOfi(0)); ctx.lineTo(padL + iw, yOfi(0)); ctx.stroke();

    // OFI bars
    for (let i = 0; i < n; i++) {
      const v = view.snaps[i].microstructure.ofi;
      ctx.fillStyle = v >= 0 ? "rgba(34,197,94,0.45)" : "rgba(239,68,68,0.45)";
      const y0 = yOfi(0);
      const y1 = yOfi(v);
      const cx = x(view.times[i]);
      ctx.fillRect(cx - barW * 0.28, Math.min(y0, y1), barW * 0.56, Math.abs(y1 - y0));
    }

    // OFI net line
    ctx.strokeStyle = "#22c55e";
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    for (let i = 0; i < n; i++) {
      const yy = yOfi(view.snaps[i].microstructure.ofi);
      const xx = x(view.times[i]);
      i === 0 ? ctx.moveTo(xx, yy) : ctx.lineTo(xx, yy);
    }
    ctx.stroke();

    // mid price line
    ctx.strokeStyle = "rgba(255,255,255,0.85)";
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    for (let i = 0; i < n; i++) {
      const yy = yPx(midOf(view.snaps[i]));
      const xx = x(view.times[i]);
      i === 0 ? ctx.moveTo(xx, yy) : ctx.lineTo(xx, yy);
    }
    ctx.stroke();

    // axes labels
    ctx.fillStyle = "#8b98a9";
    ctx.font = "10px JetBrains Mono";
    ctx.textAlign = "right";
    ctx.fillText(`+${(ofiMax / 1000).toFixed(1)}K`, padL - 4, padT + 8);
    ctx.fillText("0", padL - 4, yOfi(0) + 3);
    ctx.fillText(`−${(ofiMax / 1000).toFixed(1)}K`, padL - 4, padT + ih);
    ctx.textAlign = "left";
    ctx.fillText(`$${fmtPrice(pMax)}`, padL + iw + 4, padT + 8);
    ctx.fillText(`$${fmtPrice(pMin)}`, padL + iw + 4, padT + ih);

    // crosshair
    if (focusTime !== null && focusTime >= view.start && focusTime <= view.end) {
      let i = 0;
      while (i < view.times.length - 1 && view.times[i] < focusTime) i++;
      i = Math.max(0, Math.min(n - 1, i));
      const s = view.snaps[i];
      const cx = x(view.times[i]);
      ctx.strokeStyle = "rgba(251,191,36,0.7)";
      ctx.setLineDash([4, 4]);
      ctx.beginPath(); ctx.moveTo(cx, padT); ctx.lineTo(cx, padT + ih); ctx.stroke();
      ctx.setLineDash([]);
      ctx.fillStyle = "#fbbf24";
      ctx.font = "11px JetBrains Mono";
      ctx.textAlign = cx > w / 2 ? "right" : "left";
      const tx = cx > w / 2 ? cx - 8 : cx + 8;
      ctx.fillText(`OFI ${s.microstructure.ofi >= 0 ? "+" : ""}${s.microstructure.ofi.toFixed(0)}`, tx, padT + 14);
      ctx.fillStyle = "#e6edf3";
      ctx.fillText(`Mid $${fmtPrice(midOf(s))}`, tx, padT + 28);
    }
  }, `${frame.revision}-${windowMs}-${focusTime}`);

  const handleMove = (offsetX: number, width: number) => {
    if (!view) return;
    const padL = 40;
    const padR = 48;
    const iw = width - padL - padR;
    const clamped = Math.max(padL, Math.min(padL + iw, offsetX));
    const x = scaleLinear().domain([view.start, view.end]).range([padL, padL + iw]);
    setFocusTime(x.invert(clamped));
  };

  return (
    <section className="panel" style={{ flex: 1.15 }}>
      <div className="panel-head">
        <div className="panel-title">
          <span className="material-symbols-outlined">show_chart</span>
          OFI vs Price Action
        </div>
        <div className="panel-controls">
          <div className="seg">
            {WINDOW_PRESETS_MS.map((ms) => (
              <button key={ms} className={windowMs === ms ? "on" : ""} onClick={() => setWindowMs(ms)}>
                {(ms / 1000).toFixed(0)}s
              </button>
            ))}
          </div>
        </div>
      </div>
      <div className="panel-body">
        <canvas
          ref={ref}
          className="chart-abs"
          onMouseMove={(e) => handleMove(e.nativeEvent.offsetX, e.currentTarget.clientWidth)}
          onMouseLeave={() => setFocusTime(null)}
        />
        <div className="overlay-tl legend overlay-chip">
          <span><span className="sw" style={{ background: "#22c55e" }} />OFI (net)</span>
          <span><span className="sw" style={{ background: "#fff" }} />Mid price</span>
        </div>
      </div>
    </section>
  );
}
