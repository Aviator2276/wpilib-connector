/** Dims the canvas after the robot program exits; the last frame stays visible. */
export function EndedOverlay() {
  return (
    <div className="absolute inset-0 z-20 grid place-items-center bg-black/55" role="status">
      <div className="rounded-lg border border-white/10 bg-glass-window px-6 py-4 text-center shadow-2xl">
        <p className="text-base font-semibold">Simulation ended</p>
        <p className="mt-1 text-sm text-glass-muted">
          The robot program stopped. This page reconnects when you start it again.
        </p>
      </div>
    </div>
  );
}
