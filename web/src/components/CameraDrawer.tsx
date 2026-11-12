import { Link } from "@tanstack/react-router";
import { useCallback, useRef } from "react";
import type { Camera } from "../lib/status";
import { clampWidth, setUi } from "../lib/ui";
import { CameraView, NoCameras } from "./CameraView";

/** Resizable side panel next to the canvas. The canvas shrinks; nothing overlaps it. */
export function CameraDrawer({ cameras, width }: { cameras: Camera[]; width: number }) {
  const drag = useRef<{ x: number; w: number } | null>(null);

  const onPointerDown = useCallback(
    (e: React.PointerEvent<HTMLDivElement>) => {
      drag.current = { x: e.clientX, w: width };
      e.currentTarget.setPointerCapture(e.pointerId);
    },
    [width],
  );
  const onPointerMove = useCallback((e: React.PointerEvent<HTMLDivElement>) => {
    if (drag.current) setUi({ drawerWidth: clampWidth(drag.current.w + drag.current.x - e.clientX) });
  }, []);
  const onPointerUp = useCallback(() => {
    drag.current = null;
  }, []);

  return (
    <aside
      aria-label="Cameras"
      style={{ width }}
      className="relative flex h-full shrink-0 flex-col border-l border-black/40 bg-glass-window"
    >
      <div
        role="separator"
        aria-orientation="vertical"
        aria-label="Resize camera panel"
        tabIndex={0}
        onPointerDown={onPointerDown}
        onPointerMove={onPointerMove}
        onPointerUp={onPointerUp}
        onKeyDown={(e) => {
          if (e.key === "ArrowLeft") setUi({ drawerWidth: clampWidth(width + 24) });
          if (e.key === "ArrowRight") setUi({ drawerWidth: clampWidth(width - 24) });
        }}
        className="absolute inset-y-0 -left-1 z-10 w-2 cursor-col-resize hover:bg-glass-accent/60 focus-visible:bg-glass-accent"
      />
      <header className="flex items-center justify-between bg-glass-title px-3 py-1.5 text-sm">
        <span className="font-semibold">Cameras</span>
        <span className="flex gap-3">
          <Link to="/cameras" className="underline-offset-2 hover:underline">
            Wall
          </Link>
          <button
            type="button"
            onClick={() => setUi({ drawerOpen: false })}
            aria-label="Close camera panel"
            className="hover:text-white"
          >
            ✕
          </button>
        </span>
      </header>
      <div className="flex-1 space-y-3 overflow-y-auto p-3">
        {cameras.length === 0 ? (
          <NoCameras />
        ) : (
          cameras.map((c) => <CameraView key={`${c.name}:${c.port}`} camera={c} />)
        )}
      </div>
    </aside>
  );
}
