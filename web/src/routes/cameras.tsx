import { Link, createFileRoute } from "@tanstack/react-router";
import { CameraView, NoCameras } from "../components/CameraView";
import { useStatus } from "../lib/status";

export const Route = createFileRoute("/cameras")({ component: CameraWall });

/** All camera streams at once. Covers the canvas; the GUI keeps running under it. */
function CameraWall() {
  const { status } = useStatus();
  const cameras = status?.cameras ?? [];
  return (
    <div className="absolute inset-0 z-30 flex flex-col bg-glass-backdrop">
      <header className="flex items-center justify-between bg-glass-menubar px-4 py-2 text-sm">
        <h1 className="font-semibold">Cameras</h1>
        <Link to="/" className="underline-offset-2 hover:underline">
          Back to the Sim GUI
        </Link>
      </header>
      <div className="flex-1 overflow-y-auto p-4">
        {cameras.length === 0 ? (
          <div className="mx-auto max-w-md rounded-lg bg-glass-window p-4">
            <NoCameras />
          </div>
        ) : (
          <div className="grid gap-4 [grid-template-columns:repeat(auto-fit,minmax(min(100%,22rem),1fr))]">
            {cameras.map((c) => (
              <CameraView key={`${c.name}:${c.port}`} camera={c} />
            ))}
          </div>
        )}
      </div>
    </div>
  );
}
