import { describe, expect, it } from "vitest";
import { type MemFs, MemfsSync } from "./memfsSync";

const DIR = 0o040000;

/** A tiny in-memory stand-in for Emscripten's FS. */
class FakeFs implements MemFs {
  files = new Map<string, { data: Uint8Array; mtime: number }>();
  clock = 1000;
  write(path: string, text: string) {
    this.files.set(path, { data: new TextEncoder().encode(text), mtime: this.clock++ });
  }
  readdir(path: string): string[] {
    const names = new Set<string>([".", ".."]);
    for (const p of this.files.keys()) {
      if (p.startsWith(`${path}/`)) names.add(p.slice(path.length + 1).split("/")[0]!);
    }
    if (names.size === 2 && !this.isDirPath(path)) throw new Error("ENOENT");
    return [...names];
  }
  private isDirPath(path: string) {
    return [...this.files.keys()].some((p) => p.startsWith(`${path}/`));
  }
  stat(path: string) {
    const f = this.files.get(path);
    if (f) return { mode: 0o100644, mtime: f.mtime, size: f.data.length };
    if (this.isDirPath(path)) return { mode: DIR, mtime: 0, size: 0 };
    throw new Error("ENOENT");
  }
  isDir(mode: number) {
    return (mode & 0o170000) === DIR;
  }
  readFile(path: string) {
    return this.files.get(path)!.data;
  }
}

function setup() {
  const fs = new FakeFs();
  const uploads: { name: string; dir: string; text: string }[] = [];
  let ok = true;
  const sync = new MemfsSync(fs, "/home/coder/projects/robot", async (name, dir, data) => {
    if (ok) uploads.push({ name, dir, text: new TextDecoder().decode(data) });
    return ok;
  });
  return { fs, uploads, sync, fail: (v: boolean) => (ok = !v) };
}

const ROOT = "/home/coder/projects/robot";

describe("MemfsSync", () => {
  it("does not upload files that came from the server", async () => {
    const { fs, sync, uploads } = setup();
    fs.write(`${ROOT}/simgui.json`, "{}");
    sync.markSynced(`${ROOT}/simgui.json`);
    await sync.scan();
    await sync.scan();
    expect(uploads).toEqual([]);
  });

  it("uploads a changed layout file once it has settled", async () => {
    const { fs, sync, uploads } = setup();
    fs.write(`${ROOT}/simgui.json`, "{}");
    sync.markSynced(`${ROOT}/simgui.json`);

    fs.write(`${ROOT}/simgui.json`, '{"a":1}');
    await sync.scan(); // first sighting: wait
    expect(uploads).toEqual([]);
    fs.write(`${ROOT}/simgui.json`, '{"a":2}'); // still being written
    await sync.scan();
    expect(uploads).toEqual([]);
    await sync.scan(); // unchanged since the last scan
    expect(uploads).toEqual([{ name: "simgui.json", dir: "", text: '{"a":2}' }]);
    await sync.scan();
    expect(uploads).toHaveLength(1);
  });

  it("only ever uploads the three layout files", async () => {
    const { fs, sync, uploads } = setup();
    fs.write(`${ROOT}/build.gradle`, "plugins {}");
    fs.write(`${ROOT}/field.png`, "png");
    fs.write(`${ROOT}/simgui-ds.json`, "{}");
    fs.write(`${ROOT}/simgui-window.json`, "{}");
    await sync.scan(true);
    expect(uploads.map((u) => u.name).sort()).toEqual(["simgui-ds.json", "simgui-window.json"]);
  });

  it("reports the folder for a workspace saved elsewhere in the project", async () => {
    const { fs, sync, uploads } = setup();
    fs.write(`${ROOT}/layouts/match/simgui.json`, "{}");
    await sync.scan(true);
    expect(uploads).toEqual([{ name: "simgui.json", dir: "layouts/match", text: "{}" }]);
  });

  it("ignores a rewrite with identical content", async () => {
    const { fs, sync, uploads } = setup();
    fs.write(`${ROOT}/simgui.json`, '{"a":1}');
    sync.markSynced(`${ROOT}/simgui.json`);
    fs.write(`${ROOT}/simgui.json`, '{"a":1}'); // Glass saved again, nothing changed
    await sync.scan(true);
    expect(uploads).toEqual([]);
  });

  it("retries after a failed upload", async () => {
    const { fs, sync, uploads, fail } = setup();
    fs.write(`${ROOT}/simgui.json`, "{}");
    fail(true);
    await sync.scan(true);
    expect(uploads).toEqual([]);
    fail(false);
    await sync.scan(true);
    expect(uploads).toHaveLength(1);
  });
});
