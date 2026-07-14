import { useMemo, useRef, useState } from "react";
import { scaleLinear } from "d3-scale";
import type { FeedFrame } from "../feed";
import { useCanvas } from "../hooks";
import { fmtPrice, midOf } from "../types";

const LOOKBACKS = [60, 120, 240] as const;
const DEPTHS = [10, 25, 50] as const;

type Hover = { x: number; y: number; index: number; price: number };
type Bounds = { min: number; max: number; left: number; top: number; width: number; height: number };

const mix = (from: string, to: string, amount: number): string => {
  const parse = (value: string) => [1, 3, 5].map((offset) => Number.parseInt(value.slice(offset, offset + 2), 16));
  const [fr, fg, fb] = parse(from);
  const [tr, tg, tb] = parse(to);
  const blend = (a: number, b: number) => Math.round(a + (b - a) * amount);
  return `rgb(${blend(fr, tr)}, ${blend(fg, tg)}, ${blend(fb, tb)})`;
};

/** Time-by-price atlas of the active bid and ask liquidity. */
export default function LiquidityHeatmap({ frame }: { frame: FeedFrame }) {
  const [lookback, setLookback] = useState<(typeof LOOKBACKS)[number]>(120);
  const [depth, setDepth] = useState<(typeof DEPTHS)[number]>(25);
  const [hover, setHover] = useState<Hover | null>(null);
  const bounds = useRef<Bounds | null>(null);
  const snaps = useMemo(() => frame.snaps.slice(-lookback), [frame.revision, lookback]);

  const stats = useMemo(() => {
    const latest = snaps.at(-1);
    if (!latest) return null;
    const bids = latest.book_depth.bids.slice(0, depth);
    const asks = latest.book_depth.asks.slice(0, depth);
    const bidVolume = bids.reduce((total, [, volume]) => total + volume, 0);
    const askVolume = asks.reduce((total, [, volume]) => total + volume, 0);
    const levels = [...bids.map(([price, volume]) => ({ side: "BID", price, volume })), ...asks.map(([price, volume]) => ({ side: "ASK", price, volume }))];
    return {
      imbalance: bidVolume + askVolume ? (bidVolume - askVolume) / (bidVolume + askVolume) : 0,
      wall: levels.sort((a, b) => b.volume - a.volume)[0],
    };
  }, [snaps, depth]);

  const ref = useCanvas((ctx, width, height) => {
    if (snaps.length < 2) return;
    const pad = { left: 48, right: 10, top: 10, bottom: 24 };
    const innerWidth = width - pad.left - pad.right;
    const innerHeight = height - pad.top - pad.bottom;
    const mid = midOf(snaps.at(-1)!);
    const prices = snaps.flatMap((snapshot) => [...snapshot.book_depth.bids.slice(0, depth), ...snapshot.book_depth.asks.slice(0, depth)].map(([price]) => price));
    const minPrice = Math.min(...prices, mid - depth);
    const maxPrice = Math.max(...prices, mid + depth);
    const priceSpan = Math.max(1, maxPrice - minPrice);
    const x = scaleLinear().domain([0, snaps.length - 1]).range([pad.left, pad.left + innerWidth]);
    const y = scaleLinear().domain([minPrice, maxPrice]).range([pad.top + innerHeight, pad.top]);
    const cellWidth = Math.max(1, innerWidth / snaps.length + 0.5);
    const cellHeight = Math.max(2, innerHeight / Math.min(64, priceSpan + 1));
    const volumes = snaps.flatMap((snapshot) => [...snapshot.book_depth.bids.slice(0, depth), ...snapshot.book_depth.asks.slice(0, depth)].map(([, volume]) => volume)).sort((a, b) => a - b);
    const cap = volumes[Math.floor(volumes.length * 0.95)] || 1;
    bounds.current = { min: minPrice, max: maxPrice, left: pad.left, top: pad.top, width: innerWidth, height: innerHeight };

    ctx.fillStyle = "#081421";
    ctx.fillRect(pad.left, pad.top, innerWidth, innerHeight);
    for (let line = 0; line <= 4; line++) {
      const lineY = pad.top + (line / 4) * innerHeight;
      ctx.strokeStyle = "rgba(148,163,184,0.12)";
      ctx.beginPath(); ctx.moveTo(pad.left, lineY); ctx.lineTo(pad.left + innerWidth, lineY); ctx.stroke();
    }

    for (let index = 0; index < snaps.length; index++) {
      const snapshot = snaps[index];
      const paint = (levels: typeof snapshot.book_depth.bids, low: string, high: string) => {
        for (const [price, volume] of levels.slice(0, depth)) {
          const intensity = Math.pow(Math.min(1, volume / cap), 0.62);
          ctx.globalAlpha = 0.22 + intensity * 0.76;
          ctx.fillStyle = mix(low, high, intensity);
          ctx.fillRect(x(index) - cellWidth / 2, y(price) - cellHeight / 2, cellWidth, cellHeight);
        }
      };
      paint(snapshot.book_depth.bids, "#0a3023", "#55f09b");
      paint(snapshot.book_depth.asks, "#3a1118", "#ff7078");
    }
    ctx.globalAlpha = 1;

    ctx.strokeStyle = "#f4c542";
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    snaps.forEach((snapshot, index) => index ? ctx.lineTo(x(index), y(midOf(snapshot))) : ctx.moveTo(x(index), y(midOf(snapshot))));
    ctx.stroke();

    if (stats?.wall) {
      ctx.strokeStyle = stats.wall.side === "BID" ? "rgba(85,240,155,0.8)" : "rgba(255,112,120,0.8)";
      ctx.setLineDash([3, 3]);
      ctx.beginPath(); ctx.moveTo(pad.left, y(stats.wall.price)); ctx.lineTo(pad.left + innerWidth, y(stats.wall.price)); ctx.stroke();
      ctx.setLineDash([]);
    }

    ctx.fillStyle = "#8b98a9";
    ctx.font = "9px JetBrains Mono";
    ctx.textAlign = "right";
    for (let line = 0; line <= 4; line++) ctx.fillText(`$${fmtPrice(maxPrice - (line / 4) * priceSpan)}`, pad.left - 5, pad.top + (line / 4) * innerHeight + 3);
    ctx.textAlign = "left"; ctx.fillText(`-${Math.round(snaps.length / 10)}s`, pad.left, height - 7);
    ctx.textAlign = "right"; ctx.fillText("now", pad.left + innerWidth, height - 7);

    if (hover) {
      const hoverX = x(hover.index);
      ctx.strokeStyle = "rgba(255,255,255,0.72)";
      ctx.setLineDash([3, 3]);
      ctx.beginPath(); ctx.moveTo(hoverX, pad.top); ctx.lineTo(hoverX, pad.top + innerHeight); ctx.stroke();
      ctx.beginPath(); ctx.moveTo(pad.left, y(hover.price)); ctx.lineTo(pad.left + innerWidth, y(hover.price)); ctx.stroke();
      ctx.setLineDash([]);
    }
  }, `${frame.revision}-${depth}-${hover?.index ?? -1}`);

  const hoverSnapshot = hover ? snaps[hover.index] : null;
  const onMove = (event: React.MouseEvent<HTMLCanvasElement>) => {
    const box = bounds.current;
    if (!box || snaps.length < 2) return;
    const x = event.nativeEvent.offsetX;
    const y = event.nativeEvent.offsetY;
    if (x < box.left || x > box.left + box.width || y < box.top || y > box.top + box.height) {
      setHover(null);
      return;
    }
    const index = Math.max(0, Math.min(snaps.length - 1, Math.round(((x - box.left) / box.width) * (snaps.length - 1))));
    const price = box.min + (1 - (y - box.top) / box.height) * (box.max - box.min);
    setHover({ x: x + 10, y: y + 10, index, price });
  };

  return (
    <section className="panel cell-liquidity">
      <div className="panel-head">
        <div className="panel-title"><span className="material-symbols-outlined">grid_on</span>Liquidity Atlas</div>
        <div className="panel-controls">
          <div className="seg">{LOOKBACKS.map((value) => <button key={value} className={lookback === value ? "on" : ""} onClick={() => setLookback(value)}>{value / 10}s</button>)}</div>
          <select className="select" value={depth} onChange={(event) => setDepth(Number(event.target.value) as typeof depth)}>{DEPTHS.map((value) => <option key={value} value={value}>L2 {value}</option>)}</select>
        </div>
      </div>
      <div className="panel-body">
        <canvas ref={ref} className="chart-abs" onMouseMove={onMove} onMouseLeave={() => setHover(null)} />
        <div className="overlay-tl legend overlay-chip"><span><span className="sw" style={{ background: "#55f09b" }} />Bids</span><span><span className="sw" style={{ background: "#ff7078" }} />Asks</span><span><span className="sw" style={{ background: "#f4c542" }} />Mid</span></div>
        {hover && hoverSnapshot && <div className="tooltip" style={{ left: hover.x, top: hover.y }}><div><span className="k">mid</span>${fmtPrice(midOf(hoverSnapshot))}</div><div><span className="k">level</span>${fmtPrice(hover.price)}</div></div>}
        {stats && <div className="liquidity-readout"><span>IMB <strong className={stats.imbalance >= 0 ? "c-green" : "c-red"}>{stats.imbalance >= 0 ? "+" : ""}{(stats.imbalance * 100).toFixed(1)}%</strong></span><span>WALL <strong>{stats.wall ? `${stats.wall.side} $${fmtPrice(stats.wall.price)}` : "-"}</strong></span></div>}
      </div>
    </section>
  );
}
