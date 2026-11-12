// Simulation status from the daemon: server-sent events, with polling as a
// fallback in case a proxy buffers the event stream.

import { useSyncExternalStore } from "react";
import { api } from "./base";

export interface Camera {
  name: string;
  port: number;
}

export interface SimStatus {
  sim: {
    running: boolean;
    pid: number | null;
    project: string | null;
    wpilibVersion: string | null;
    bundle: string | null;
    startedAt: string | null;
  };
  nt: { up: boolean };
  hal: { up: boolean; mode: "ws" | "bridge" | null };
  cameras: Camera[];
  clients: number;
  config: { projectsDir: string; autoDisable: boolean; bundles: string[] };
}

export interface StatusState {
  /** null until the daemon has answered once */
  status: SimStatus | null;
  /** false while the daemon itself cannot be reached */
  daemonUp: boolean;
}

const POLL_MS = 2000;
const SSE_SILENCE_MS = 20_000; // the daemon sends a keepalive every 15 s

let state: StatusState = { status: null, daemonUp: true };
const listeners = new Set<() => void>();
let started = false;

function set(next: Partial<StatusState>) {
  const merged = { ...state, ...next };
  if (
    merged.daemonUp === state.daemonUp &&
    JSON.stringify(merged.status) === JSON.stringify(state.status)
  ) {
    return;
  }
  state = merged;
  for (const l of listeners) l();
}

async function pollOnce() {
  try {
    const r = await fetch(api("api/status"), { cache: "no-store" });
    if (!r.ok) throw new Error(String(r.status));
    set({ status: (await r.json()) as SimStatus, daemonUp: true });
  } catch {
    set({ daemonUp: false });
  }
}

function start() {
  if (started) return;
  started = true;

  let lastEvent = 0;
  let source: EventSource | null = null;

  const open = () => {
    source?.close();
    source = new EventSource(api("api/events"));
    source.onmessage = (ev) => {
      lastEvent = Date.now();
      try {
        set({ status: JSON.parse(ev.data) as SimStatus, daemonUp: true });
      } catch {
        /* ignore a malformed event; the next one replaces it */
      }
    };
    // EventSource reconnects by itself; polling covers the gap.
  };
  open();
  void pollOnce();

  setInterval(() => {
    const silent = Date.now() - lastEvent > SSE_SILENCE_MS;
    if (silent || source?.readyState !== EventSource.OPEN) {
      void pollOnce();
      if (silent && source?.readyState === EventSource.OPEN) open(); // stalled stream
    }
  }, POLL_MS);
}

export function useStatus(): StatusState {
  return useSyncExternalStore(
    (cb) => {
      start();
      listeners.add(cb);
      return () => listeners.delete(cb);
    },
    () => state,
  );
}

export function getStatus(): StatusState {
  return state;
}

/** Ask the daemon to force the robot to disabled. */
export async function disableRobot(): Promise<boolean> {
  try {
    const r = await fetch(api("api/sim/disable"), { method: "POST" });
    return r.ok;
  } catch {
    return false;
  }
}
