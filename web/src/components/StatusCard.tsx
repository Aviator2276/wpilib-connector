import type { ReactNode } from "react";
import type { StatusState } from "../lib/status";
import { Dot } from "./Dot";

function Row({ ok, waiting, children }: { ok: boolean; waiting: boolean; children: ReactNode }) {
  return (
    <li className="flex items-center gap-3 py-1.5">
      <Dot
        tone={ok ? "ok" : waiting ? "wait" : "off"}
        label={ok ? "connected" : waiting ? "connecting" : "not connected"}
      />
      <span className={ok ? "text-glass-text" : "text-glass-muted"}>{children}</span>
    </li>
  );
}

function Shell({ title, children }: { title: string; children: ReactNode }) {
  return (
    <div className="absolute inset-0 z-20 grid place-items-center bg-glass-backdrop p-6">
      <section
        aria-live="polite"
        className="w-full max-w-md rounded-lg border border-white/10 bg-glass-window shadow-2xl"
      >
        <h1 className="rounded-t-lg bg-glass-title px-5 py-3 text-base font-semibold">{title}</h1>
        <div className="px-5 py-4 text-sm leading-relaxed">{children}</div>
      </section>
    </div>
  );
}

/** Shown instead of the canvas until a simulation is running and connected. */
export function StatusCard({
  state,
  loading,
  error,
}: {
  state: StatusState;
  loading: boolean;
  error: string | null;
}) {
  const { status, daemonUp } = state;

  if (error) {
    return (
      <Shell title="The Sim GUI could not start">
        <p className="text-glass-muted">{error}</p>
        <button
          type="button"
          onClick={() => location.reload()}
          className="mt-4 rounded bg-glass-header px-3 py-1.5 font-medium hover:bg-glass-accent focus-visible:outline-2 focus-visible:outline-white"
        >
          Reload
        </button>
      </Shell>
    );
  }

  if (!status) {
    return (
      <Shell title="WPILib Simulator">
        <p className="text-glass-muted">
          {daemonUp ? "Connecting to the workspace…" : "The workspace is not answering. Retrying…"}
        </p>
      </Shell>
    );
  }

  const running = status.sim.running;
  return (
    <Shell title={running ? "Robot program found, connecting" : "No simulation running"}>
      {!running && (
        <>
          <p>
            Start one from VS Code with{" "}
            <strong className="font-semibold">WPILib: Simulate Robot Code</strong>, or run this in
            your robot project:
          </p>
          <pre className="my-3 overflow-x-auto rounded bg-black/40 px-3 py-2 font-mono text-[13px]">
            ./gradlew simulateJava
          </pre>
          <p className="text-glass-muted">
            Robot projects live in <code className="font-mono">{status.config.projectsDir}</code>.
            This page connects by itself when the simulation starts.
          </p>
        </>
      )}
      <ul className="mt-3 border-t border-white/10 pt-3">
        <Row ok={running} waiting={false}>
          Robot program{running && status.sim.project ? ` — ${status.sim.project}` : ""}
        </Row>
        <Row ok={status.nt.up} waiting={running}>
          NetworkTables
        </Row>
        <Row ok={status.hal.up} waiting={running}>
          Hardware (HAL)
        </Row>
        {loading && (
          <Row ok={false} waiting>
            Loading the Sim GUI
            {status.sim.bundle ? ` for WPILib ${status.sim.bundle}` : ""}
          </Row>
        )}
      </ul>
    </Shell>
  );
}
