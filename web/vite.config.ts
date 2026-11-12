import tailwindcss from "@tailwindcss/vite";
import { tanstackRouter } from "@tanstack/router-plugin/vite";
import react from "@vitejs/plugin-react";
import { defineConfig } from "vitest/config";

// The app is served at a subdomain root and under Coder's path prefix, so the
// build uses only relative URLs (base "./"). During `bun run dev`, requests
// for the daemon's endpoints are proxied to a locally running websimd.
const daemon = process.env.WEBSIMD_URL ?? "http://127.0.0.1:13300";

export default defineConfig({
  base: "./",
  plugins: [
    tanstackRouter({ target: "react", autoCodeSplitting: true }),
    react(),
    tailwindcss(),
  ],
  build: { target: "es2022", sourcemap: false, assetsDir: "assets" },
  server: {
    proxy: Object.fromEntries(
      ["/api", "/wasm", "/cam", "/healthz"]
        .map((p) => [p, { target: daemon }] as const)
        .concat(["/nt", "/hal"].map((p) => [p, { target: daemon, ws: true }] as const)),
    ),
  },
  test: { environment: "node", include: ["src/**/*.test.ts"] },
});
