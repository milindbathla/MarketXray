import { useState } from "react";
import { feed } from "../feed";
import type { FeedFrame } from "../feed";
import { fmtClock, type FeedStatus } from "../types";
import MarketScanner from "./MarketScanner";

export default function TopBar({ status, frame }: { status: FeedStatus; frame: FeedFrame }) {
  const [settingsOpen, setSettingsOpen] = useState(false);
  const [preferSim, setPreferSim] = useState(feed.getPreferSim());

  const toggleFullscreen = () => {
    if (document.fullscreenElement) void document.exitFullscreen();
    else void document.documentElement.requestFullscreen();
  };

  const srcLabel = status === "live" ? "NASDAQ ITCH" : status === "sim" ? "SIMULATED FEED" : "CONNECTING…";
  const srcDot = status === "live" ? "green" : status === "sim" ? "amber" : "red";
  const marketTimeMs = frame.clockMs > 0 ? frame.clockMs : frame.times[frame.times.length - 1];
  const clockText = marketTimeMs !== undefined ? fmtClock(marketTimeMs) : "--:--:--.---";
  const clockUnit = status === "live" ? "market time · ITCH" : status === "sim" ? "sim time" : "waiting";

  return (
    <header className="topbar" style={{ position: "relative" }}>
      <div className="topbar-left">
        <div className="topbar-brand">
          <span className="brand">MARKET <b>X-RAY</b></span>
        </div>
        <div className="topbar-scanner">
          <MarketScanner frame={frame} compact />
        </div>
      </div>

      <div className="top-right">
        <span className="src-badge">
          <span className={`dot ${srcDot} ${status !== "live" ? "pulse" : ""}`} />
          {srcLabel}
        </span>
        <div className="clock">
          <span className="big">{clockText}</span>
          <span className="unit">{clockUnit}</span>
        </div>
        <button className="icon-btn" title="Settings" onClick={() => setSettingsOpen((v) => !v)}>
          <span className="material-symbols-outlined">settings</span>
        </button>
        <button className="icon-btn" title="Fullscreen" onClick={toggleFullscreen}>
          <span className="material-symbols-outlined">fullscreen</span>
        </button>
      </div>

      {settingsOpen && (
        <div className="settings-pop">
          <h4>Data Source</h4>
          <div className="settings-row">
            <span>Force simulator</span>
            <input
              type="checkbox"
              checked={preferSim}
              onChange={(e) => {
                setPreferSim(e.target.checked);
                feed.setPreferSim(e.target.checked);
              }}
            />
          </div>
          <div className="settings-row c-muted" style={{ fontSize: 10 }}>
            Live engine: ws://localhost:9001 — auto-reconnects every 5 s when
            unchecked.
          </div>
        </div>
      )}
    </header>
  );
}
