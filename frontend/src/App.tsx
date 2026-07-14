import { useEffect } from "react";
import { feed } from "./feed";
import { useFeed } from "./hooks";
import TopBar from "./components/TopBar";
import KpiStrip from "./components/KpiStrip";
import DepthSurface3D from "./components/DepthSurface3D";
import DepthCommandCenter from "./components/DepthCommandCenter";
import LiquidityHeatmap from "./components/LiquidityHeatmap";
import OFIPriceChart from "./components/OFIPriceChart";
import MarketImpact from "./components/MarketImpact";
import SpoofingScatter from "./components/SpoofingScatter";
import StatusBar from "./components/StatusBar";
import { SyncProvider } from "./sync";

export default function App() {
  useEffect(() => { feed.start(); }, []);
  const frame = useFeed();

  return (
    <SyncProvider>
      <div className="app">
        <TopBar status={frame.status} frame={frame} />
        <main className="main">
          <KpiStrip frame={frame} />
          <div className="grid">
            <DepthSurface3D frame={frame} />
            <DepthCommandCenter frame={frame} />
            <LiquidityHeatmap frame={frame} />
            <div className="cell-mid">
              <OFIPriceChart frame={frame} />
            </div>
            <MarketImpact frame={frame} />
            <SpoofingScatter frame={frame} />
          </div>
        </main>
        <StatusBar frame={frame} />
      </div>
    </SyncProvider>
  );
}
