import { Link } from "@tanstack/react-router";
import { useEffect, useState } from "react";
import type { SimStatus } from "../lib/status";
import { setUi, useUi } from "../lib/ui";
import { Dot } from "./Dot";

/**
 * The only shell element over the canvas while connected: a small pill in the
 * bottom corner that fades out, and opens on hover or focus into a slim strip
 * with the camera controls. It sits at the bottom because the Sim GUI's own
 * menu bar owns the top edge.
 */
export function StatusPill({ status }: { status: SimStatus }) {
  const ui = useUi();
  const [fresh, setFresh] = useState(true);
  const healthy = status.nt.up && status.hal.up;

  // Stay opaque for a few seconds after anything changes, then fade.
  useEffect(() => {
    setFresh(true);
    const t = setTimeout(() => setFresh(false), 4000);
    return () => clearTimeout(t);
  }, [status.nt.up, status.hal.up, status.cameras.length]);

  return (
    <div
      className={`group absolute right-2 bottom-2 z-10 flex items-center gap-3 rounded-full border border-white/10 bg-glass-window/90 px-3 py-1 text-xs shadow-lg transition-opacity duration-500 focus-within:opacity-100 hover:opacity-100 ${
        fresh || !healthy ? "opacity-100" : "opacity-25"
      }`}
    >
      <span
        className="flex items-center gap-1.5"
        title={status.nt.up ? "NetworkTables connected" : "NetworkTables not connected"}
      >
        <Dot tone={status.nt.up ? "ok" : "wait"} label={status.nt.up ? "connected" : "connecting"} />
        NT
      </span>
      <span
        className="flex items-center gap-1.5"
        title={
          status.hal.up
            ? status.hal.mode === "bridge"
              ? "Hardware connected (native bridge)"
              : "Hardware connected. The Timing, Power Distribution and REV PH windows need the native bridge."
            : "Hardware not connected"
        }
      >
        <Dot tone={status.hal.up ? "ok" : "wait"} label={status.hal.up ? "connected" : "connecting"} />
        HAL
      </span>
      <span className="hidden items-center gap-3 border-l border-white/15 pl-3 group-focus-within:flex group-hover:flex">
        <button
          type="button"
          aria-pressed={ui.drawerOpen}
          onClick={() => setUi({ drawerOpen: !ui.drawerOpen })}
          className="underline-offset-2 hover:underline focus-visible:underline"
        >
          Cameras{status.cameras.length ? ` (${status.cameras.length})` : ""}
        </button>
        <Link to="/cameras" className="underline-offset-2 hover:underline focus-visible:underline">
          Wall
        </Link>
        <span
          className="max-w-56 text-glass-muted"
          title="The robot program keeps running when this tab is closed. Stop it from VS Code or the terminal."
        >
          {status.config.autoDisable
            ? "Closing this tab disables the robot"
            : "The robot stays enabled if this tab closes"}
        </span>
      </span>
    </div>
  );
}
