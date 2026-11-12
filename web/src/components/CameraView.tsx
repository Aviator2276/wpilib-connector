import { useEffect, useRef, useState } from "react";
import { api } from "../lib/base";
import type { Camera } from "../lib/status";

/**
 * One camera stream. Chrome decodes MJPEG natively in an <img>, so the GUI
 * module does no work for video. The image exists only while the tile is on
 * screen: removing it closes the HTTP stream, which stops the daemon's proxy
 * and lets the robot stop encoding frames nobody is looking at.
 */
export function CameraView({ camera }: { camera: Camera }) {
  const ref = useRef<HTMLDivElement>(null);
  const [visible, setVisible] = useState(false);
  const [failed, setFailed] = useState(0);

  useEffect(() => {
    const el = ref.current;
    if (!el) return;
    const io = new IntersectionObserver(
      (entries) => setVisible(entries.some((e) => e.isIntersecting)),
      { threshold: 0.01 },
    );
    io.observe(el);
    const onVisibility = () => {
      if (document.visibilityState === "hidden") setVisible(false);
      else io.takeRecords(), io.unobserve(el), io.observe(el);
    };
    document.addEventListener("visibilitychange", onVisibility);
    return () => {
      io.disconnect();
      document.removeEventListener("visibilitychange", onVisibility);
    };
  }, []);

  // retry a failed stream a few seconds later (the camera may still be starting)
  useEffect(() => {
    if (failed === 0) return;
    const t = setTimeout(() => setFailed(0), 3000);
    return () => clearTimeout(t);
  }, [failed]);

  return (
    <figure ref={ref} className="overflow-hidden rounded border border-white/10 bg-black">
      <div className="grid aspect-video place-items-center">
        {visible && failed === 0 ? (
          <img
            src={api(`cam/${camera.port}/stream.mjpg`)}
            alt={`Camera ${camera.name}`}
            className="size-full object-contain"
            onError={() => setFailed((n) => n + 1)}
          />
        ) : (
          <span className="text-xs text-glass-muted">
            {failed ? "No video; retrying…" : "Paused"}
          </span>
        )}
      </div>
      <figcaption className="flex justify-between bg-glass-window px-2 py-1 text-xs">
        <span className="truncate">{camera.name}</span>
        <span className="text-glass-muted">:{camera.port}</span>
      </figcaption>
    </figure>
  );
}

export function NoCameras() {
  return (
    <p className="px-1 text-sm text-glass-muted">
      No camera streams. They appear here when the robot program publishes one (for example a
      PhotonVision simulated camera).
    </p>
  );
}
