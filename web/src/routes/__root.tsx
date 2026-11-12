import { Outlet, createRootRoute } from "@tanstack/react-router";
import { useEffect, useRef, useState } from "react";
import { CameraDrawer } from "../components/CameraDrawer";
import { EndedOverlay } from "../components/EndedOverlay";
import { FilePicker } from "../components/FilePicker";
import { StatusCard } from "../components/StatusCard";
import { StatusPill } from "../components/StatusPill";
import { startSimGui, useGui } from "../lib/simgui";
import { useStatus } from "../lib/status";
import { useUi } from "../lib/ui";

// How long to wait for the HAL link before starting the GUI with
// NetworkTables only (a project can opt out of the HAL extension).
const HAL_WAIT_MS = 4000;

export const Route = createRootRoute({ component: Root });

function Root() {
  const state = useStatus();
  const gui = useGui();
  const ui = useUi();
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const { status } = state;
  const running = status?.sim.running ?? false;

  // HAL_WAIT_MS after the robot program appears, stop waiting for the HAL.
  const [halWaitOver, setHalWaitOver] = useState(false);
  useEffect(() => {
    if (!running) {
      setHalWaitOver(false);
      return;
    }
    const t = setTimeout(() => setHalWaitOver(true), HAL_WAIT_MS);
    return () => clearTimeout(t);
  }, [running]);

  // Start the GUI once the robot is there and its links are up.
  const ready = running && status!.nt.up && (status!.hal.up || halWaitOver);
  useEffect(() => {
    if (ready && gui.phase === "idle" && canvasRef.current && status) {
      void startSimGui(canvasRef.current, status);
    }
  }, [ready, gui.phase, status]);

  // The module holds the state of one robot run (devices, layout, project).
  // When a new run starts, begin again from a clean page.
  const started = gui.phase === "running";
  useEffect(() => {
    if (started && running && status!.sim.startedAt !== gui.startedAt) {
      location.reload();
    }
  }, [started, running, status, gui.startedAt]);

  const showCard = gui.phase !== "running";
  const ended = started && !running;

  return (
    <div className="flex h-full w-full">
      <main className="relative h-full min-w-0 flex-1">
        {/* The Sim GUI. Nothing wraps or restyles it; the module sizes the
            canvas to this host element. */}
        <div id="simgui-host" className="absolute inset-0">
          <canvas
            id="simgui-canvas"
            ref={canvasRef}
            tabIndex={0}
            aria-label="WPILib Robot Simulation GUI"
            onContextMenu={(e) => e.preventDefault()}
          />
        </div>
        {showCard && (
          <StatusCard state={state} loading={gui.phase === "loading"} error={gui.error} />
        )}
        {ended && <EndedOverlay />}
        {started && status && <StatusPill status={status} />}
        <Outlet />
      </main>
      {ui.drawerOpen && started && status && (
        <CameraDrawer cameras={status.cameras} width={ui.drawerWidth} />
      )}
      {gui.dialog && <FilePicker key={gui.dialog.id} request={gui.dialog} />}
    </div>
  );
}
