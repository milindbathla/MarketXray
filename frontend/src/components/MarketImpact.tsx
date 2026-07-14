// Market Impact + Volume — walks the CURRENT ask book with a user-selected
// order size (client-side recompute of the engine's simulate_market_impact):
// red cumulative-slippage line, blue fill-size bars per level, mid marker,
// slippage callout, and footer stats.

import { useMemo, useState } from "react";
import type { FeedFrame } from "../feed";
import { useCanvas } from "../hooks";
import { walkBook } from "../sim";
import { fmtPrice } from "../types";

const SIZES = [1000, 5000, 10000, 25000, 50000];

export default function MarketImpact({ frame }: { frame: FeedFrame }) {
  const [qty, setQty] = useState(10000);
  const s = frame.snaps.length > 0 ? frame.snaps[frame.snaps.length - 1] : null;

  const walk = useMemo(() => {
    if (!s) return null;
    const asks = s.book_depth.asks;
    const ref = asks.length > 0 ? asks[0][0] : 0;
    let remaining = qty, cost = 0;
    const fills: { price: number; take: number; cumSlip: number }[] = [];
    for (const [price, vol] of asks) {
      if (remaining <= 0) break;
      const take = Math.min(remaining, vol);
      cost += take * price;
      remaining -= take;
      const filled = qty - remaining;
      const avg = cost / filled;
      fills.push({ price, take, cumSlip: ref > 0 ? ((avg - ref) / ref) * 10000 : 0 });
    }
    return { fills, summary: walkBook(asks, qty), ref };
  }, [s, qty]);

  const ref = useCanvas((ctx, w, h) => {
    if (!walk || walk.fills.length === 0) return;
    const { fills } = walk;
    const padL = 34, padR = 40, padT = 10, padB = 18;
    const iw = w - padL - padR, ih = h - padT - padB;
    const n = fills.length;

    let maxTake = 1, maxSlip = 0.1;
    for (const f of fills) { maxTake = Math.max(maxTake, f.take); maxSlip = Math.max(maxSlip, f.cumSlip); }
    const x = (i: number) => n === 1 ? padL + iw / 2 : padL + (i / (n - 1)) * iw;
    const yBar = (v: number) => (v / maxTake) * ih * 0.55;
    const ySlip = (v: number) => padT + ih - (v / maxSlip) * ih * 0.88;

    // fill-size bars
    const bw = Math.max(2, (iw / n) * 0.6);
    ctx.fillStyle = "rgba(47,217,244,0.30)";
    for (let i = 0; i < n; i++) {
      const bh = yBar(fills[i].take);
      ctx.fillRect(x(i) - bw / 2, padT + ih - bh, bw, bh);
    }

    // cumulative slippage line
    ctx.strokeStyle = "#ef4444";
    ctx.lineWidth = 2;
    ctx.beginPath();
    for (let i = 0; i < n; i++) {
      const yy = ySlip(fills[i].cumSlip);
      i === 0 ? ctx.moveTo(x(i), yy) : ctx.lineTo(x(i), yy);
      ctx.fillStyle = "#ef4444";
    }
    ctx.stroke();
    for (let i = 0; i < n; i += Math.max(1, Math.floor(n / 12))) {
      ctx.beginPath(); ctx.arc(x(i), ySlip(fills[i].cumSlip), 2.2, 0, Math.PI * 2); ctx.fill();
    }

    // axes
    ctx.fillStyle = "#5b6675";
    ctx.font = "9px JetBrains Mono";
    ctx.textAlign = "left";
    ctx.fillText(`$${fmtPrice(fills[0].price)}`, padL, h - 5);
    ctx.textAlign = "right";
    ctx.fillText(`$${fmtPrice(fills[n - 1].price)}`, padL + iw, h - 5);
    ctx.fillText(`${maxSlip.toFixed(1)} bps`, padL - 4, padT + 8);
    ctx.textAlign = "left";
    ctx.fillText(`${(maxTake / 1000).toFixed(1)}K`, padL + iw + 4, padT + ih - yBar(maxTake) + 8);

    // slippage callout at the end of the walk
    const lastF = fills[n - 1];
    const cy = ySlip(lastF.cumSlip);
    ctx.strokeStyle = "rgba(239,68,68,0.8)";
    ctx.fillStyle = "rgba(239,68,68,0.12)";
    const bx = Math.min(x(n - 1) + 6, w - 112), by = Math.max(padT, cy - 26);
    ctx.beginPath();
    if (typeof ctx.roundRect === "function") ctx.roundRect(bx, by, 104, 24, 6);
    else ctx.rect(bx, by, 104, 24);
    ctx.fill(); ctx.stroke();
    ctx.fillStyle = "#f87171";
    ctx.font = "10px JetBrains Mono";
    ctx.fillText(`slip ${lastF.cumSlip.toFixed(2)} bps`, bx + 8, by + 15);
  }, `${frame.revision}-${qty}`);

  const sum = walk?.summary;

  return (
    <section className="panel cell-impact">
      <div className="panel-head">
        <div className="panel-title">
          <span className="material-symbols-outlined">bar_chart</span>
          Market Impact + Volume
        </div>
        <div className="panel-controls">
          <span>Order Size</span>
          <select className="select" value={qty} onChange={(e) => setQty(Number(e.target.value))}>
            {SIZES.map((v) => <option key={v} value={v}>{v.toLocaleString()}</option>)}
          </select>
        </div>
      </div>
      <div className="panel-body">
        <canvas ref={ref} className="chart-abs" />
        <div className="overlay-tl legend">
          <span><span className="sw" style={{ background: "#ef4444" }} />Cum. slippage (bps)</span>
          <span><span className="sw" style={{ background: "rgba(47,217,244,0.7)" }} />Fill size / level</span>
        </div>
      </div>
      <div className="impact-stats">
        <span><span className="c-muted">VWAP </span><span className="c-cyan">${s ? fmtPrice(s.microstructure.vwap) : "—"}</span></span>
        <span><span className="c-muted">Avg fill </span><span className="c-text">${sum ? fmtPrice(sum.avg_fill_price) : "—"}</span></span>
        <span><span className="c-muted">Impact </span><span className="c-red">{sum ? sum.slippage_bps.toFixed(2) : "—"} bps</span></span>
        <span><span className="c-muted">Levels </span><span className="c-text">{sum ? sum.levels_consumed : "—"}</span></span>
        <span><span className="c-muted">Filled </span><span className={sum?.fully_filled ? "c-green" : "c-amber"}>{sum ? (sum.fully_filled ? "100%" : "PARTIAL") : "—"}</span></span>
      </div>
    </section>
  );
}
