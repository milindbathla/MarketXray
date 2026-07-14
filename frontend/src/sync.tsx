import { createContext, useContext, useState, type ReactNode } from "react";

export const WINDOW_PRESETS_MS = [3000, 6000, 12000] as const;

interface SyncState {
  windowMs: number;
  setWindowMs: (ms: number) => void;
  focusTime: number | null;
  setFocusTime: (ms: number | null) => void;
}

const SyncContext = createContext<SyncState | null>(null);

export function SyncProvider({ children }: { children: ReactNode }) {
  const [windowMs, setWindowMs] = useState<number>(WINDOW_PRESETS_MS[1]);
  const [focusTime, setFocusTime] = useState<number | null>(null);

  return (
    <SyncContext.Provider value={{ windowMs, setWindowMs, focusTime, setFocusTime }}>
      {children}
    </SyncContext.Provider>
  );
}

export function useSyncState(): SyncState {
  const ctx = useContext(SyncContext);
  if (!ctx) throw new Error("useSyncState must be used inside SyncProvider");
  return ctx;
}
