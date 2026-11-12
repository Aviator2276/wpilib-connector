// Small shared UI state: whether the camera drawer is open, and its width.

import { useSyncExternalStore } from "react";

interface UiState {
  drawerOpen: boolean;
  drawerWidth: number;
}

const KEY = "wpilib-websim.ui";
const MIN_WIDTH = 220;
const MAX_WIDTH = 900;

function load(): UiState {
  try {
    const raw = JSON.parse(localStorage.getItem(KEY) ?? "{}") as Partial<UiState>;
    return {
      drawerOpen: raw.drawerOpen === true,
      drawerWidth: clampWidth(typeof raw.drawerWidth === "number" ? raw.drawerWidth : 360),
    };
  } catch {
    return { drawerOpen: false, drawerWidth: 360 };
  }
}

export function clampWidth(w: number): number {
  return Math.min(MAX_WIDTH, Math.max(MIN_WIDTH, Math.round(w)));
}

let state = load();
const listeners = new Set<() => void>();

export function setUi(next: Partial<UiState>): void {
  state = { ...state, ...next };
  try {
    localStorage.setItem(KEY, JSON.stringify(state));
  } catch {
    /* private mode: keep it for this page only */
  }
  for (const l of listeners) l();
}

export function useUi(): UiState {
  return useSyncExternalStore(
    (cb) => {
      listeners.add(cb);
      return () => listeners.delete(cb);
    },
    () => state,
  );
}
