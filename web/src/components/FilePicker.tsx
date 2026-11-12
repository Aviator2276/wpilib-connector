import { useCallback, useEffect, useRef, useState } from "react";
import { api } from "../lib/base";
import { fileMatcher } from "../lib/glob";
import { type DialogRequest, cancelDialog, chooseDialogPath } from "../lib/simgui";

interface Entry {
  name: string;
  type: "dir" | "file";
  size: number;
}

interface Listing {
  root: string;
  dir: string;
  entries: Entry[];
}

function join(dir: string, name: string) {
  return dir ? `${dir}/${name}` : name;
}

function formatSize(n: number) {
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
  return `${(n / 1024 / 1024).toFixed(1)} MB`;
}

/**
 * Stands in for the native file dialogs Glass opens on the desktop (Field2d
 * image and JSON, Mechanism2d background, workspace open / save). It browses
 * the robot project in the workspace, which is where those files live, and
 * can upload a file from this computer into it.
 */
export function FilePicker({ request }: { request: DialogRequest }) {
  const dialogRef = useRef<HTMLDialogElement>(null);
  const [dir, setDir] = useState("");
  const [listing, setListing] = useState<Listing | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const pickFolder = request.kind === "folder";
  const matches = fileMatcher(request.filters);

  const load = useCallback(async (target: string) => {
    setError(null);
    try {
      const r = await fetch(api(`api/fs?dir=${encodeURIComponent(target)}`), { cache: "no-store" });
      if (!r.ok) throw new Error(await r.text());
      const body = (await r.json()) as Listing;
      setListing(body);
      setDir(body.dir);
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  }, []);

  useEffect(() => {
    dialogRef.current?.showModal();
    void load("");
  }, [load]);

  const choose = async (rel: string) => {
    setBusy(true);
    await chooseDialogPath(rel);
  };

  const upload = async (file: File) => {
    setBusy(true);
    setError(null);
    try {
      const form = new FormData();
      form.append("file", file, file.name);
      const r = await fetch(api(`api/fs/upload?dir=${encodeURIComponent(dir)}`), {
        method: "POST",
        body: form,
      });
      if (!r.ok) throw new Error(await r.text());
      const { saved } = (await r.json()) as { saved: string[] };
      if (saved[0]) await chooseDialogPath(saved[0]);
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
      setBusy(false);
    }
  };

  const crumbs = dir ? dir.split("/") : [];
  const entries = (listing?.entries ?? []).filter(
    (e) => e.type === "dir" || (!pickFolder && matches(e.name)),
  );

  return (
    <dialog
      ref={dialogRef}
      onCancel={(e) => {
        e.preventDefault();
        cancelDialog();
      }}
      aria-labelledby="file-picker-title"
      className="m-auto w-[min(92vw,34rem)] rounded-lg border border-white/10 bg-glass-window p-0 text-glass-text shadow-2xl backdrop:bg-black/50"
    >
      <h2 id="file-picker-title" className="rounded-t-lg bg-glass-title px-4 py-2.5 text-sm font-semibold">
        {request.title}
      </h2>

      <nav aria-label="Folder" className="flex flex-wrap items-center gap-1 px-4 pt-3 text-xs">
        <button type="button" className="font-mono hover:underline" onClick={() => void load("")}>
          {listing?.root ?? "project"}
        </button>
        {crumbs.map((part, i) => (
          <span key={crumbs.slice(0, i + 1).join("/")} className="flex items-center gap-1">
            <span className="text-glass-muted">/</span>
            <button
              type="button"
              className="font-mono hover:underline"
              onClick={() => void load(crumbs.slice(0, i + 1).join("/"))}
            >
              {part}
            </button>
          </span>
        ))}
      </nav>

      <ul className="mx-4 my-3 h-64 overflow-y-auto rounded border border-white/10 bg-black/30 text-sm">
        {dir && (
          <li>
            <button
              type="button"
              className="w-full px-3 py-1.5 text-left text-glass-muted hover:bg-glass-header"
              onClick={() => void load(crumbs.slice(0, -1).join("/"))}
            >
              ..
            </button>
          </li>
        )}
        {entries.map((e) => (
          <li key={e.name}>
            <button
              type="button"
              disabled={busy}
              className="flex w-full justify-between gap-3 px-3 py-1.5 text-left hover:bg-glass-header focus-visible:bg-glass-header disabled:opacity-50"
              onClick={() =>
                e.type === "dir" ? void load(join(dir, e.name)) : void choose(join(dir, e.name))
              }
            >
              <span className="truncate">
                {e.type === "dir" ? `${e.name}/` : e.name}
              </span>
              {e.type === "file" && (
                <span className="shrink-0 text-xs text-glass-muted">{formatSize(e.size)}</span>
              )}
            </button>
          </li>
        ))}
        {listing && entries.length === 0 && (
          <li className="px-3 py-2 text-glass-muted">
            {pickFolder ? "No sub-folders here." : "No matching files here."}
          </li>
        )}
      </ul>

      {error && (
        <p role="alert" className="mx-4 mb-2 text-xs text-bad">
          {error}
        </p>
      )}

      <div className="flex items-center justify-between gap-3 border-t border-white/10 px-4 py-3 text-sm">
        {pickFolder ? (
          <span className="text-xs text-glass-muted">Layout files are read from and saved to this folder.</span>
        ) : (
          <label className="cursor-pointer rounded border border-white/15 px-3 py-1.5 hover:bg-glass-header focus-within:outline-2 focus-within:outline-white">
            Upload from this computer…
            <input
              type="file"
              className="sr-only"
              disabled={busy}
              accept={request.filters
                .flatMap((f) => f.split(/\s+/))
                .filter((g) => g.startsWith("*."))
                .map((g) => g.slice(1))
                .join(",")}
              onChange={(e) => {
                const f = e.target.files?.[0];
                if (f) void upload(f);
              }}
            />
          </label>
        )}
        <span className="flex gap-2">
          <button
            type="button"
            className="rounded px-3 py-1.5 hover:bg-white/10"
            onClick={() => cancelDialog()}
          >
            Cancel
          </button>
          {pickFolder && (
            <button
              type="button"
              disabled={busy}
              className="rounded bg-glass-header px-3 py-1.5 font-medium hover:bg-glass-accent disabled:opacity-50"
              onClick={() => void choose(dir)}
            >
              Use this folder
            </button>
          )}
        </span>
      </div>
    </dialog>
  );
}
