import { useMemo, useState } from "react";
import type { FeedFrame } from "../feed";
import type { BookLevel } from "../types";
import { fmtNum, fmtPrice, midOf } from "../types";

function Ladder({ side, levels, maxVolume }: { side: "bid" | "ask"; levels: BookLevel[]; maxVolume: number }) {
  return <div className={`book-ladder ${side}`}><div className="ladder-head"><span>{side === "bid" ? "BIDS" : "ASKS"}</span><span>SIZE</span></div><div className="ladder-list">{levels.map(([price, volume]) => <div className="ladder-row" key={`${side}-${price}`}><i className="ladder-fill" style={{ width: `${Math.max(4, Math.min(100, volume / maxVolume * 100))}%` }} /><b>${fmtPrice(price)}</b><span>{fmtNum(volume)}</span></div>)}</div></div>;
}

export default function DepthCommandCenter({ frame }: { frame: FeedFrame }) {
  const [depth, setDepth] = useState(8);
  const latest = frame.snaps.at(-1);
  const model = useMemo(() => {
    if (!latest) return null;
    const bids = latest.book_depth.bids.slice(0, depth);
    const asks = latest.book_depth.asks.slice(0, depth);
    const all = [...bids, ...asks];
    const bidVolume = bids.reduce((total, [, volume]) => total + volume, 0);
    const askVolume = asks.reduce((total, [, volume]) => total + volume, 0);
    return {
      bids, asks,
      maxVolume: Math.max(1, ...all.map(([, volume]) => volume)),
      imbalance: bidVolume + askVolume ? (bidVolume - askVolume) / (bidVolume + askVolume) : 0,
      touchBid: bids.slice(0, 3).reduce((total, [, volume]) => total + volume, 0),
      touchAsk: asks.slice(0, 3).reduce((total, [, volume]) => total + volume, 0),
      wall: all.reduce<BookLevel | null>((best, level) => !best || level[1] > best[1] ? level : best, null),
    };
  }, [latest, depth]);

  return <section className="panel cell-command">
    <div className="panel-head"><div className="panel-title"><span className="material-symbols-outlined">account_tree</span>Market Depth</div><div className="panel-controls"><span className="symbol-tag">{frame.selectedSymbol} L2</span><select className="select" value={depth} onChange={(event) => setDepth(Number(event.target.value))}><option value={6}>6 levels</option><option value={8}>8 levels</option><option value={12}>12 levels</option></select></div></div>
    <div className="depth-command-body">{!model || !latest ? <div className="depth-empty">Waiting for two-sided market depth.</div> : <>
      <div className="depth-quote-strip"><div><span>BEST BID</span><b className="c-green">${fmtPrice(latest.microstructure.best_bid)}</b></div><div><span>MIDPOINT</span><strong>${fmtPrice(midOf(latest))}</strong><small>{latest.microstructure.spread_ticks.toFixed(1)} ticks</small></div><div><span>BEST ASK</span><b className="c-red">${fmtPrice(latest.microstructure.best_ask)}</b></div></div>
      <div className="touch-pressure"><div><span>NEAR-TOUCH</span><b className={model.touchBid >= model.touchAsk ? "c-green" : "c-red"}>{model.touchBid >= model.touchAsk ? "BID-LED" : "ASK-LED"}</b></div><i><em style={{ width: `${Math.max(4, model.touchBid / Math.max(1, model.touchBid + model.touchAsk) * 100)}%` }} /></i></div>
      <div className="ladder-columns"><Ladder side="bid" levels={model.bids} maxVolume={model.maxVolume} /><Ladder side="ask" levels={model.asks} maxVolume={model.maxVolume} /></div>
      <div className="depth-summary"><span>IMBALANCE <strong className={model.imbalance >= 0 ? "c-green" : "c-red"}>{model.imbalance >= 0 ? "+" : ""}{(model.imbalance * 100).toFixed(1)}%</strong></span><span>WALL <strong>{model.wall ? `${fmtNum(model.wall[1])} @ $${fmtPrice(model.wall[0])}` : "-"}</strong></span></div>
    </>}</div>
  </section>;
}
