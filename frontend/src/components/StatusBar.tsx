import type { FeedFrame } from "../feed";

export default function StatusBar({ frame }: { frame: FeedFrame }) {
  const { status, msgRate, latencyMs, historyMs, snaps } = frame;
  const live = status === "live";
  const label = live ? "LIVE" : status === "sim" ? "SIM" : "CONNECTING";
  const color = live ? "var(--bull)" : status === "sim" ? "var(--amber)" : "var(--bear)";

  return (
    <footer className="statusbar">
      <div className="status-group">
        <span className="status-item">
          <span className="lbl">Connection</span>
          <span className={`dot ${live ? "green" : status === "sim" ? "amber" : "red"}`} />
          <b style={{ color }}>{label}</b>
        </span>
        <span className="vdiv" />
        <span className="status-item">
          <span className="lbl">Data Rate</span>
          <span className="c-text">{msgRate.toFixed(1)} snap/s</span>
        </span>
        <span className="vdiv" />
        <span className="status-item">
          <span className="lbl">Latency</span>
          <span className="c-text">{latencyMs.toFixed(2)}ms</span>
        </span>
        <span className="vdiv" />
        <span className="status-item">
          <span className="lbl">History</span>
          <span className="c-text">{(historyMs / 1000).toFixed(1)}s · {snaps.length} samples</span>
        </span>
      </div>

      <div className="status-group">
        <span className="status-item c-muted">MarketXray v2.1.0</span>
        <span className="vdiv" />
        <span className="status-item c-muted">CS661 · IIT Kanpur</span>
        <span className="vdiv" />
        <span className="status-item" style={{ color: "var(--bull)" }}>
          <span className="dot green" /> All systems operational
        </span>
      </div>
    </footer>
  );
}
