import { useEffect, useLayoutEffect, useRef, useSyncExternalStore } from "react";
import { feed, type FeedFrame } from "./feed";

/** Subscribe a component to the feed store (re-renders on every new frame). */
export function useFeed(): FeedFrame {
  useSyncExternalStore(feed.subscribe, feed.getVersion);
  return feed.frame();
}

/**
 * Canvas helper: keeps the bitmap sized to the element × DPR and schedules one
 * draw per display frame. Stream updates may arrive faster than the monitor can
 * paint; coalescing them avoids visible clear-and-redraw flicker.
 */
export function useCanvas(
  draw: (ctx: CanvasRenderingContext2D, w: number, h: number) => void,
  dep: unknown
): React.RefObject<HTMLCanvasElement> {
  const ref = useRef<HTMLCanvasElement>(null);
  const drawRef = useRef(draw);
  const renderRef = useRef<() => void>(() => {});
  const animationFrame = useRef<number | null>(null);
  drawRef.current = draw;

  renderRef.current = () => {
    const canvas = ref.current;
    if (!canvas) return;
    const rect = canvas.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;
    const w = Math.max(1, Math.round(rect.width));
    const h = Math.max(1, Math.round(rect.height));
    if (canvas.width !== w * dpr || canvas.height !== h * dpr) {
      canvas.width = w * dpr;
      canvas.height = h * dpr;
    }
    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);
    drawRef.current(ctx, w, h);
  };

  const scheduleRender = () => {
    if (animationFrame.current !== null) return;
    animationFrame.current = window.requestAnimationFrame(() => {
      animationFrame.current = null;
      renderRef.current();
    });
  };

  // Run before paint so the previous chart is never visibly cleared.
  useLayoutEffect(() => {
    scheduleRender();
  }, [dep]);

  // The observer is intentionally retained across stream updates. Recreating it
  // for every tick was unnecessary work and made charts appear to refresh.
  useEffect(() => {
    const canvas = ref.current;
    if (!canvas) return;
    const ro = new ResizeObserver(scheduleRender);
    ro.observe(canvas);
    return () => {
      ro.disconnect();
      if (animationFrame.current !== null) {
        window.cancelAnimationFrame(animationFrame.current);
        animationFrame.current = null;
      }
    };
  }, []);

  return ref;
}
