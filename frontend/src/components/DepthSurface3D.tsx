// ============================================================================
// 3D Limit Order Book Depth Surface — time × price × volume (WebGL/Three.js).
// Green bid mesh / red ask mesh meet at a magenta mid seam. Fully interactive:
// orbit rotate, wheel zoom, right-drag pan, hover tooltip via raycasting,
// Bids/Asks/Both toggle, depth 10/25/50, Reset View.
// ============================================================================

import { useEffect, useRef, useState } from "react";
import * as THREE from "three";
import { OrbitControls } from "three/examples/jsm/controls/OrbitControls.js";
import type { FeedFrame } from "../feed";
import type { BookLevel } from "../types";
import { fmtPrice } from "../types";
import { useSyncState } from "../sync";

type SideMode = "bids" | "asks" | "both";

const T_MAX = 110;         // time slices kept in the surface
const X_SPAN = 2.4, Z_SPAN = 2.0, Y_SPAN = 0.95;
const HOME_POS = new THREE.Vector3(2.3, 1.7, 2.5);
const HOME_TGT = new THREE.Vector3(0, 0.25, 0);

interface HistSlice { bids: BookLevel[]; asks: BookLevel[]; t: number }

interface Hover { x: number; y: number; price: number; vol: number; side: string; ago: string }

const cBidLo = new THREE.Color("#07331d");
const cBidHi = new THREE.Color("#4ade80");
const cAskLo = new THREE.Color("#3b0d0d");
const cAskHi = new THREE.Color("#f87171");
const cMid = new THREE.Color("#a855f7");

export default function DepthSurface3D({ frame }: { frame: FeedFrame }) {
  const mountRef = useRef<HTMLDivElement | null>(null);
  const [mode, setMode] = useState<SideMode>("both");
  const [depth, setDepth] = useState(25);
  const [hover, setHover] = useState<Hover | null>(null);
  const { windowMs, setFocusTime } = useSyncState();

  // three.js singletons kept in a ref across renders
  const three = useRef<{
    renderer: THREE.WebGLRenderer;
    scene: THREE.Scene;
    camera: THREE.PerspectiveCamera;
    controls: OrbitControls;
    mesh: THREE.Mesh | null;
    wire: THREE.Mesh | null;
    raycaster: THREE.Raycaster;
    hist: HistSlice[];
  } | null>(null);

  const cfg = useRef({ mode, depth });
  cfg.current = { mode, depth };
  const setHoverRef = useRef(setHover);
  setHoverRef.current = setHover;
  const setFocusTimeRef = useRef(setFocusTime);
  setFocusTimeRef.current = setFocusTime;

  // ---- one-time scene setup ------------------------------------------------
  useEffect(() => {
    const mount = mountRef.current;
    if (!mount) return;

    const renderer = new THREE.WebGLRenderer({ antialias: true, alpha: true });
    renderer.setPixelRatio(Math.min(2, window.devicePixelRatio || 1));
    mount.appendChild(renderer.domElement);

    const scene = new THREE.Scene();
    const camera = new THREE.PerspectiveCamera(46, 1, 0.1, 100);
    camera.position.copy(HOME_POS);

    const controls = new OrbitControls(camera, renderer.domElement);
    controls.target.copy(HOME_TGT);
    controls.enableDamping = true;
    controls.dampingFactor = 0.08;
    controls.minDistance = 1.2;
    controls.maxDistance = 8;

    const grid = new THREE.GridHelper(X_SPAN, 12, 0x24354a, 0x16202f);
    grid.position.y = 0;
    scene.add(grid);

    const st = {
      renderer, scene, camera, controls,
      mesh: null as THREE.Mesh | null,
      wire: null as THREE.Mesh | null,
      raycaster: new THREE.Raycaster(),
      hist: [] as HistSlice[],
    };
    three.current = st;

    const resize = () => {
      const w = mount.clientWidth || 1;
      const h = mount.clientHeight || 1;
      renderer.setSize(w, h, false);
      camera.aspect = w / h;
      camera.updateProjectionMatrix();
    };
    resize();
    const ro = new ResizeObserver(resize);
    ro.observe(mount);

    let raf = 0;
    const loop = () => {
      // self-heal: if the panel was laid out while the tab was hidden
      // (rAF + ResizeObserver suspended), fix the drawing-buffer size on
      // the first visible frame.
      const w = mount.clientWidth, h = mount.clientHeight;
      const size = renderer.getSize(new THREE.Vector2());
      if (w > 0 && h > 0 && (size.x !== w || size.y !== h)) resize();
      controls.update();
      renderer.render(scene, camera);
      raf = requestAnimationFrame(loop);
    };
    raf = requestAnimationFrame(loop);

    // hover raycast → tooltip
    const onMove = (ev: PointerEvent) => {
      const s = three.current;
      if (!s || !s.mesh) return;
      const rect = renderer.domElement.getBoundingClientRect();
      const ndc = new THREE.Vector2(
        ((ev.clientX - rect.left) / rect.width) * 2 - 1,
        -((ev.clientY - rect.top) / rect.height) * 2 + 1
      );
      s.raycaster.setFromCamera(ndc, s.camera);
      const hit = s.raycaster.intersectObject(s.mesh, false)[0];
      if (!hit) { setHoverRef.current(null); return; }

      const { hist } = s;
      const { mode: m, depth: d } = cfg.current;
      const T = hist.length;
      const C = m === "both" ? d * 2 : d;
      if (T < 2) return;
      const ti = Math.max(0, Math.min(T - 1, Math.round(((hit.point.x / X_SPAN) + 0.5) * (T - 1))));
      const ci = Math.max(0, Math.min(C - 1, Math.round(((hit.point.z / Z_SPAN) + 0.5) * (C - 1))));
      const slice = hist[ti];
      const lvl = lookupLevel(slice, m, d, ci);
      if (!lvl) { setHoverRef.current(null); return; }
      const latestT = hist.length > 0 ? hist[hist.length - 1].t : slice.t;
      const ago = (Math.max(0, latestT - slice.t) / 1000).toFixed(1);
      setFocusTimeRef.current(slice.t);
      setHoverRef.current({
        x: ev.clientX - rect.left + 12,
        y: ev.clientY - rect.top + 12,
        price: lvl.price, vol: lvl.vol, side: lvl.side, ago: `${ago}s ago`,
      });
    };
    const onLeave = () => {
      setHoverRef.current(null);
      setFocusTimeRef.current(null);
    };
    renderer.domElement.addEventListener("pointermove", onMove);
    renderer.domElement.addEventListener("pointerleave", onLeave);

    return () => {
      cancelAnimationFrame(raf);
      ro.disconnect();
      renderer.domElement.removeEventListener("pointermove", onMove);
      renderer.domElement.removeEventListener("pointerleave", onLeave);
      controls.dispose();
      disposeMeshes(st);
      renderer.dispose();
      mount.removeChild(renderer.domElement);
      three.current = null;
    };
  }, []);

  // ---- rebuild surface on new data / mode / depth ---------------------------
  const nSnaps = frame.snaps.length;
  useEffect(() => {
    const s = three.current;
    if (!s) return;
    const endTime = frame.times.length > 0 ? frame.times[frame.times.length - 1] : 0;
    const minTime = endTime - windowMs;
    let start = Math.max(0, nSnaps - T_MAX);
    while (start < frame.times.length && frame.times[start] < minTime) start++;
    s.hist = [];
    for (let i = start; i < nSnaps; i++) {
      const sn = frame.snaps[i];
      s.hist.push({ bids: sn.book_depth.bids, asks: sn.book_depth.asks, t: frame.times[i] });
    }
    rebuild(s, mode, depth);
  }, [frame.revision, nSnaps, mode, depth, frame, windowMs]);

  const resetView = () => {
    const s = three.current;
    if (!s) return;
    s.camera.position.copy(HOME_POS);
    s.controls.target.copy(HOME_TGT);
    s.controls.update();
  };

  return (
    <section className="panel cell-surface">
      <div className="panel-head">
        <div className="panel-title">
          <span className="material-symbols-outlined">view_in_ar</span>
          3D Limit Order Book Depth Surface
        </div>
        <div className="panel-controls">
          <div className="seg">
            {(["bids", "asks", "both"] as const).map((m) => (
              <button key={m} className={mode === m ? "on" : ""} onClick={() => setMode(m)}
                style={m === "bids" ? { color: mode === m ? "#4ade80" : undefined }
                  : m === "asks" ? { color: mode === m ? "#f87171" : undefined } : undefined}>
                {m.toUpperCase()}
              </button>
            ))}
          </div>
          <select className="select" value={depth} onChange={(e) => setDepth(Number(e.target.value))}>
            <option value={10}>Depth: 10</option>
            <option value={25}>Depth: 25</option>
            <option value={50}>Depth: 50</option>
          </select>
        </div>
      </div>
      <div className="panel-body">
        <div ref={mountRef} style={{ position: "absolute", inset: 0, cursor: "grab" }} />
        {hover && (
          <div className="tooltip" style={{ left: hover.x, top: hover.y }}>
            <div><span className="k">price</span>${fmtPrice(hover.price)}</div>
            <div><span className="k">volume</span>{hover.vol.toLocaleString()} lots</div>
            <div><span className="k">side</span><span style={{ color: hover.side === "BID" ? "var(--bull)" : "var(--bear)" }}>{hover.side}</span></div>
            <div><span className="k">time</span>{hover.ago}</div>
          </div>
        )}
        <div className="overlay-bl">
          <button className="ghost-btn" onClick={resetView}>Reset View</button>
        </div>
        <div className="overlay-br hint">
          <span>Drag rotate</span><span>Scroll zoom</span><span>Right-drag pan</span>
        </div>
        <div className="overlay-tr" style={{ display: "flex", alignItems: "center", gap: 6 }}>
          <span style={{ fontSize: 8, color: "var(--text-muted)", letterSpacing: "0.08em" }}>VOL</span>
          <div style={{
            width: 8, height: 74, borderRadius: 4,
            background: "linear-gradient(180deg,#ef4444,#eab308,#22c55e,#0d1c2d)",
            border: "1px solid var(--border)",
          }} />
        </div>
        <div className="overlay-tl legend">
          <span><span className="sw" style={{ background: "#4ade80" }} />Bid liquidity</span>
          <span><span className="sw" style={{ background: "#f87171" }} />Ask liquidity</span>
          <span><span className="sw" style={{ background: "#a855f7" }} />Mid seam</span>
        </div>
      </div>
    </section>
  );
}

// ---------------------------------------------------------------- helpers

function lookupLevel(
  slice: HistSlice, mode: SideMode, depth: number, ci: number
): { price: number; vol: number; side: string } | null {
  // column order: deepest bid → best bid → best ask → deepest ask (price ascending)
  if (mode === "both") {
    if (ci < depth) {
      const li = depth - 1 - ci;
      const lvl = slice.bids[li];
      return lvl ? { price: lvl[0], vol: lvl[1], side: "BID" } : null;
    }
    const lvl = slice.asks[ci - depth];
    return lvl ? { price: lvl[0], vol: lvl[1], side: "ASK" } : null;
  }
  if (mode === "bids") {
    const lvl = slice.bids[depth - 1 - ci];
    return lvl ? { price: lvl[0], vol: lvl[1], side: "BID" } : null;
  }
  const lvl = slice.asks[ci];
  return lvl ? { price: lvl[0], vol: lvl[1], side: "ASK" } : null;
}

function disposeMeshes(s: { scene: THREE.Scene; mesh: THREE.Mesh | null; wire: THREE.Mesh | null }) {
  for (const m of [s.mesh, s.wire]) {
    if (!m) continue;
    s.scene.remove(m);
    m.geometry.dispose();
    (m.material as THREE.Material).dispose();
  }
  s.mesh = null;
  s.wire = null;
}

function rebuild(
  s: {
    scene: THREE.Scene; mesh: THREE.Mesh | null; wire: THREE.Mesh | null; hist: HistSlice[];
  },
  mode: SideMode,
  depth: number
): void {
  const T = s.hist.length;
  if (T < 2) {
    disposeMeshes(s);
    return;
  }
  const C = mode === "both" ? depth * 2 : depth;

  // normalization: robust max (98th pct-ish) so a single wall doesn't flatten all
  let maxV = 1;
  const all: number[] = [];
  for (const sl of s.hist) {
    const rows = mode === "asks" ? [sl.asks] : mode === "bids" ? [sl.bids] : [sl.bids, sl.asks];
    for (const r of rows) for (let i = 0; i < depth && i < r.length; i++) all.push(r[i][1]);
  }
  if (all.length > 0) {
    all.sort((a, b) => a - b);
    maxV = all[Math.floor(all.length * 0.98)] || all[all.length - 1] || 1;
  }

  const positions = new Float32Array(T * C * 3);
  const colors = new Float32Array(T * C * 3);
  const tmp = new THREE.Color();

  for (let ti = 0; ti < T; ti++) {
    const slice = s.hist[ti];
    for (let ci = 0; ci < C; ci++) {
      const lvl = lookupLevel(slice, mode, depth, ci);
      const vol = lvl ? lvl.vol : 0;
      const h = Math.min(1.25, vol / maxV);
      const idx = (ti * C + ci) * 3;
      positions[idx] = (ti / (T - 1) - 0.5) * X_SPAN;
      positions[idx + 1] = h * Y_SPAN;
      positions[idx + 2] = (ci / (C - 1) - 0.5) * Z_SPAN;

      const isBid = mode === "bids" || (mode === "both" && ci < depth);
      if (isBid) tmp.lerpColors(cBidLo, cBidHi, Math.min(1, h));
      else tmp.lerpColors(cAskLo, cAskHi, Math.min(1, h));
      // magenta seam at the touch
      if (mode === "both") {
        const distMid = Math.abs(ci - (depth - 0.5));
        if (distMid < 1.6) tmp.lerp(cMid, 0.55 * (1 - distMid / 1.6));
      }
      colors[idx] = tmp.r; colors[idx + 1] = tmp.g; colors[idx + 2] = tmp.b;
    }
  }

  const indices: number[] = [];
  for (let ti = 0; ti < T - 1; ti++) {
    for (let ci = 0; ci < C - 1; ci++) {
      const a = ti * C + ci, b = a + 1, c = a + C, d = c + 1;
      indices.push(a, c, b, b, c, d);
    }
  }

  const geo = new THREE.BufferGeometry();
  geo.setAttribute("position", new THREE.BufferAttribute(positions, 3));
  geo.setAttribute("color", new THREE.BufferAttribute(colors, 3));
  geo.setIndex(indices);
  geo.computeVertexNormals();

  disposeMeshes(s);

  const solid = new THREE.Mesh(
    geo,
    new THREE.MeshBasicMaterial({ vertexColors: true, transparent: true, opacity: 0.5, side: THREE.DoubleSide })
  );
  const wire = new THREE.Mesh(
    geo.clone(),
    new THREE.MeshBasicMaterial({ vertexColors: true, wireframe: true, transparent: true, opacity: 0.6 })
  );
  s.scene.add(solid);
  s.scene.add(wire);
  s.mesh = solid;
  s.wire = wire;
}
