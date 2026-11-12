// Loads and owns the Sim GUI WebAssembly module.
//
// The module is the real WPILib Sim GUI; this file only does what a desktop
// OS would do for it: put its saved layout where it expects to find it, tell
// it where the robot is, answer its file dialogs, and save its layout back.

import { useSyncExternalStore } from "react";
import { api, ws } from "./base";
import { MemfsSync, STORAGE_NAMES } from "./memfsSync";
import type { SimStatus } from "./status";

export interface DialogRequest {
  id: number;
  kind: "open" | "save" | "folder";
  title: string;
  defaultPath: string;
  filters: string[];
}

export type GuiPhase = "idle" | "loading" | "running" | "error";

export interface GuiState {
  phase: GuiPhase;
  error: string | null;
  /** project directory the running module was started for */
  project: string | null;
  /** robot start time the running module was started for */
  startedAt: string | null;
  dialog: DialogRequest | null;
}

interface EmFs {
  mkdirTree(path: string): void;
  chdir(path: string): void;
  writeFile(path: string, data: Uint8Array): void;
  readFile(path: string): Uint8Array;
  readdir(path: string): string[];
  stat(path: string): { mode: number; mtime: Date; size: number };
  isDir(mode: number): boolean;
}

interface SimGuiModule {
  FS: EmFs;
  ccall(name: string, ret: string | null, argTypes: string[], args: unknown[]): unknown;
  _websim_wake(ms: number): void;
  _websim_save_settings?(): void;
}

type ModuleFactory = (options: Record<string, unknown>) => Promise<SimGuiModule>;

let state: GuiState = {
  phase: "idle",
  error: null,
  project: null,
  startedAt: null,
  dialog: null,
};
const listeners = new Set<() => void>();
let gui: SimGuiModule | null = null;
let sync: MemfsSync | null = null;
let cwd = "/project";

function set(next: Partial<GuiState>) {
  state = { ...state, ...next };
  for (const l of listeners) l();
}

export function useGui(): GuiState {
  return useSyncExternalStore(
    (cb) => {
      listeners.add(cb);
      return () => listeners.delete(cb);
    },
    () => state,
  );
}

// ------------------------------------------------------------ layout files

function storageUrl(name: string, relDir: string): string {
  const q = relDir ? `?dir=${encodeURIComponent(relDir)}` : "";
  return api(`api/storage/${name}${q}`);
}

async function fetchBytes(url: string): Promise<Uint8Array | null> {
  try {
    const r = await fetch(url, { cache: "no-store" });
    return r.ok ? new Uint8Array(await r.arrayBuffer()) : null;
  } catch {
    return null;
  }
}

/** The three layout files of a project folder that exist on the server. */
async function fetchLayout(relDir: string): Promise<Map<string, Uint8Array>> {
  const out = new Map<string, Uint8Array>();
  await Promise.all(
    STORAGE_NAMES.map(async (name) => {
      const data = await fetchBytes(storageUrl(name, relDir));
      if (data) out.set(name, data);
    }),
  );
  return out;
}

/**
 * Saved layouts can point at files in the project (a custom Field2d image).
 * On the desktop Glass opens them from disk at startup; here they have to be
 * in the in-memory filesystem first. Returns project-relative paths.
 */
export function referencedProjectFiles(layout: Iterable<Uint8Array>, root: string): string[] {
  const found = new Set<string>();
  const prefix = `${root}/`;
  const walk = (v: unknown) => {
    if (typeof v === "string") {
      if (v.startsWith(prefix) && !v.includes("\0")) found.add(v.slice(prefix.length));
    } else if (Array.isArray(v)) {
      v.forEach(walk);
    } else if (v && typeof v === "object") {
      Object.values(v).forEach(walk);
    }
  };
  for (const data of layout) {
    try {
      walk(JSON.parse(new TextDecoder().decode(data)));
    } catch {
      /* not JSON; Glass will complain about it itself */
    }
  }
  return [...found];
}

function writeFile(path: string, data: Uint8Array) {
  if (!gui) return;
  const slash = path.lastIndexOf("/");
  if (slash > 0) gui.FS.mkdirTree(path.slice(0, slash));
  gui.FS.writeFile(path, data);
  sync?.markSynced(path);
}

async function uploadLayout(name: string, relDir: string, data: Uint8Array): Promise<boolean> {
  try {
    const r = await fetch(storageUrl(name, relDir), {
      method: "PUT",
      body: data as BodyInit,
      // lets the request outlive a tab that is closing
      keepalive: data.byteLength < 60_000,
    });
    return r.ok;
  } catch {
    return false;
  }
}

function saveNow() {
  try {
    gui?._websim_save_settings?.();
  } catch {
    /* module not ready */
  }
  void sync?.scan(true);
}

// ------------------------------------------------------------ file dialogs

function finishDialog(id: number, path: string) {
  gui?.ccall("websim_dialog_result", null, ["number", "string"], [id, path]);
  if (state.dialog?.id === id) set({ dialog: null });
}

export function cancelDialog(): void {
  if (state.dialog) finishDialog(state.dialog.id, "");
}

/**
 * Complete the open dialog with a project-relative path. The chosen file (or
 * a chosen folder's layout files) is copied into the in-memory filesystem
 * under its real path first, because that is where Glass will open it.
 */
export async function chooseDialogPath(rel: string): Promise<void> {
  const dialog = state.dialog;
  if (!dialog || !gui) return;
  const abs = rel ? `${cwd}/${rel}` : cwd;
  if (dialog.kind === "folder") {
    gui.FS.mkdirTree(abs);
    for (const [name, data] of await fetchLayout(rel)) writeFile(`${abs}/${name}`, data);
  } else {
    const data = await fetchBytes(api(`api/fs/file?path=${encodeURIComponent(rel)}`));
    if (!data) {
      finishDialog(dialog.id, "");
      return;
    }
    writeFile(abs, data);
  }
  finishDialog(dialog.id, abs);
}

// ------------------------------------------------------------------ wake-up

// The GUI draws only when something changed. Input is one such thing; the
// module cannot see DOM events it has no handler for, so the page tells it.
const WAKE_EVENTS = [
  "pointermove",
  "pointerdown",
  "pointerup",
  "wheel",
  "keydown",
  "keyup",
  "touchstart",
  "touchmove",
  "touchend",
  "paste",
  "focus",
] as const;

function installWake(host: HTMLElement) {
  const wake = () => gui?._websim_wake(250);
  for (const type of WAKE_EVENTS) {
    host.addEventListener(type, wake, { capture: true, passive: true });
  }
  window.addEventListener("resize", wake);
  window.addEventListener("gamepadconnected", wake);
  window.addEventListener("gamepaddisconnected", wake);
  document.addEventListener("visibilitychange", () => {
    if (document.visibilityState === "hidden") saveNow();
    else wake();
  });
  window.addEventListener("pagehide", saveNow);

  // Gamepads have no change events; poll them, and only while one is present.
  let lastStamp = 0;
  setInterval(() => {
    let stamp = 0;
    for (const pad of navigator.getGamepads?.() ?? []) {
      if (pad?.connected) stamp += pad.timestamp;
    }
    if (stamp !== lastStamp) {
      lastStamp = stamp;
      wake();
    }
  }, 50);
}

// -------------------------------------------------------------------- start

interface FieldEntry {
  name: string;
  json: string;
  image: string;
}

async function fetchFields(bundle: string): Promise<FieldEntry[]> {
  try {
    const r = await fetch(api(`api/fields?bundle=${encodeURIComponent(bundle)}`));
    const body = (await r.json()) as { fields?: FieldEntry[] };
    const q = `?bundle=${encodeURIComponent(bundle)}`;
    return (body.fields ?? []).map((f) => ({
      name: f.name,
      json: api(`api/fields/${f.json}${q}`),
      image: api(`api/fields/${f.image}${q}`),
    }));
  } catch {
    return [];
  }
}

/** Start the Sim GUI on `canvas` for the simulation described by `status`. */
export async function startSimGui(canvas: HTMLCanvasElement, status: SimStatus): Promise<void> {
  if (state.phase !== "idle") return;
  const bundle = status.sim.bundle;
  if (!bundle) {
    set({ phase: "error", error: "No Sim GUI bundle is installed in this workspace image." });
    return;
  }
  set({ phase: "loading", project: status.sim.project, startedAt: status.sim.startedAt });
  cwd = status.sim.project ?? "/project";

  try {
    const layout = await fetchLayout("");
    const referenced = new Map<string, Uint8Array>();
    await Promise.all(
      referencedProjectFiles(layout.values(), cwd).map(async (rel) => {
        const data = await fetchBytes(api(`api/fs/file?path=${encodeURIComponent(rel)}`));
        if (data) referenced.set(rel, data);
      }),
    );
    const [fields, factory] = await Promise.all([
      fetchFields(bundle),
      import(/* @vite-ignore */ api(`wasm/${bundle}/simgui.js`)).then(
        (m: { default: ModuleFactory }) => m.default,
      ),
    ]);

    const module = await factory({
      canvas,
      ntUrl: ws("nt"), // the module appends /<client id>
      halUrl: ws("hal"),
      resizeSelector: "#simgui-host",
      websimFields: fields,
      locateFile: (file: string) => api(`wasm/${bundle}/${file}`),
      print: (text: string) => console.log(`[simgui] ${text}`),
      printErr: (text: string) => console.warn(`[simgui] ${text}`),
      preRun: [
        (m: SimGuiModule) => {
          // Same working directory as the robot program, so paths saved in
          // the layout mean the same thing here and on the desktop.
          m.FS.mkdirTree(cwd);
          m.FS.chdir(cwd);
          for (const [name, data] of layout) m.FS.writeFile(`${cwd}/${name}`, data);
          for (const [rel, data] of referenced) {
            const abs = `${cwd}/${rel}`;
            m.FS.mkdirTree(abs.slice(0, abs.lastIndexOf("/")));
            m.FS.writeFile(abs, data);
          }
        },
      ],
      websimFileDialog: (req: DialogRequest) => {
        if (state.dialog) finishDialog(state.dialog.id, "");
        set({ dialog: req });
      },
      websimFileDialogCancel: (id: number) => {
        if (state.dialog?.id === id) set({ dialog: null });
      },
    });

    gui = module;
    // For the browser console and the end-to-end tests:
    //   JSON.parse(__simgui.ccall("websim_debug_state", "string", [], []))
    (window as unknown as { __simgui?: SimGuiModule }).__simgui = module;
    sync = new MemfsSync(module.FS, cwd, uploadLayout);
    for (const name of layout.keys()) sync.markSynced(`${cwd}/${name}`);
    setInterval(() => void sync?.scan(), 1000);
    // Glass writes its layout when ImGui's own settings change and when the
    // desktop window closes. A tab has no dependable closing moment, and
    // some state (a Field2d image, plot settings) changes without ImGui
    // noticing, so ask for a save regularly; unchanged files are not sent.
    setInterval(() => {
      try {
        gui?._websim_save_settings?.();
      } catch {
        /* the module is busy or gone */
      }
    }, 5000);
    installWake(canvas.parentElement ?? canvas);
    set({ phase: "running" });
    canvas.focus();
  } catch (e) {
    console.error(e);
    set({ phase: "error", error: e instanceof Error ? e.message : String(e) });
  }
}
