// Sends the Sim GUI's layout files back to the robot project.
//
// Glass writes simgui.json, simgui-ds.json and simgui-window.json into its
// working directory, which in the browser is Emscripten's in-memory
// filesystem. This watches that directory tree and uploads a layout file once
// it has stopped changing, so the project on disk stays in the desktop format
// and moves between Coder and a laptop unchanged.

export const STORAGE_NAMES = ["simgui.json", "simgui-ds.json", "simgui-window.json"] as const;

/** The parts of Emscripten's FS this module uses. */
export interface MemFs {
  readdir(path: string): string[];
  stat(path: string): { mode: number; mtime: Date | number; size: number };
  isDir(mode: number): boolean;
  readFile(path: string): Uint8Array;
}

export type Uploader = (name: string, relDir: string, data: Uint8Array) => Promise<boolean>;

const MAX_DEPTH = 8;

export class MemfsSync {
  /** Signature of each file as last uploaded (or as loaded from the server). */
  private synced = new Map<string, string>();
  /** Signature seen on the previous scan; an upload waits for it to settle. */
  private settling = new Map<string, string>();
  private busy = false;

  constructor(
    private fs: MemFs,
    private root: string,
    private upload: Uploader,
  ) {}

  /**
   * Content signature (length and FNV-1a hash). Not the modification time:
   * Glass rewrites all three files on every save, changed or not.
   */
  private signature(path: string): string | null {
    try {
      const data = this.fs.readFile(path);
      let h = 0x811c9dc5;
      for (let i = 0; i < data.length; i++) {
        h = Math.imul(h ^ data[i]!, 0x01000193);
      }
      return `${data.length}:${h >>> 0}`;
    } catch {
      return null;
    }
  }

  /** Record that `path` matches the server, so it is not uploaded back. */
  markSynced(path: string): void {
    const sig = this.signature(path);
    if (sig !== null) this.synced.set(path, sig);
  }

  private layoutFiles(dir: string, depth: number, out: string[]): void {
    let names: string[];
    try {
      names = this.fs.readdir(dir);
    } catch {
      return;
    }
    for (const name of names) {
      if (name === "." || name === "..") continue;
      const path = `${dir}/${name}`;
      let isDir = false;
      try {
        isDir = this.fs.isDir(this.fs.stat(path).mode);
      } catch {
        continue;
      }
      if (isDir) {
        if (depth < MAX_DEPTH) this.layoutFiles(path, depth + 1, out);
      } else if ((STORAGE_NAMES as readonly string[]).includes(name)) {
        out.push(path);
      }
    }
  }

  /** Files that differ from the server copy. */
  changed(): string[] {
    const files: string[] = [];
    this.layoutFiles(this.root, 0, files);
    return files.filter((p) => this.signature(p) !== this.synced.get(p));
  }

  /**
   * One scan. A changed file is uploaded when it has the same signature on
   * two consecutive scans (the debounce), or at once when `force` is set.
   */
  async scan(force = false): Promise<void> {
    if (this.busy) return;
    this.busy = true;
    try {
      const pending = this.changed();
      for (const key of [...this.settling.keys()]) {
        if (!pending.includes(key)) this.settling.delete(key);
      }
      for (const path of pending) {
        const sig = this.signature(path);
        if (sig === null) continue;
        if (!force && this.settling.get(path) !== sig) {
          this.settling.set(path, sig);
          continue;
        }
        const slash = path.lastIndexOf("/");
        const name = path.slice(slash + 1);
        const relDir = path.slice(this.root.length + 1, Math.max(slash, this.root.length + 1));
        let data: Uint8Array;
        try {
          data = this.fs.readFile(path);
        } catch {
          continue;
        }
        if (await this.upload(name, relDir.replace(/\/$/, ""), data)) {
          this.synced.set(path, sig);
          this.settling.delete(path);
        }
      }
    } finally {
      this.busy = false;
    }
  }
}
