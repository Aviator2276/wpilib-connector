// Shared helpers for the end-to-end scripts.
import http from "node:http";
import net from "node:net";
import { chromium } from "playwright";

export const BASE = (process.env.WEBSIM_URL ?? "http://127.0.0.1:13300/").replace(/\/*$/, "/");

/** Headless Chromium with software WebGL2 (no GPU on CI runners). */
export async function launch() {
  return chromium.launch({
    args: [
      "--use-gl=angle",
      "--use-angle=swiftshader",
      "--enable-unsafe-swiftshader",
      "--ignore-gpu-blocklist",
    ],
  });
}

export async function openPage(browser, url = BASE, { dpr = 1, width = 1280, height = 720 } = {}) {
  const page = await browser.newPage({ viewport: { width, height }, deviceScaleFactor: dpr });
  const logs = [];
  page.on("console", (m) => logs.push(`${m.type()}: ${m.text()}`));
  page.on("pageerror", (e) => logs.push(`pageerror: ${e}`));
  await page.goto(url);
  return { page, logs };
}

/** The in-browser HAL as the GUI sees it (wasm/src/debug.cpp). */
export function guiState(page) {
  return page.evaluate(() =>
    window.__simgui
      ? JSON.parse(window.__simgui.ccall("websim_debug_state", "string", [], []))
      : null,
  );
}

export async function waitFor(what, fn, timeoutMs = 30_000, everyMs = 250) {
  const deadline = Date.now() + timeoutMs;
  let last;
  for (;;) {
    try {
      last = await fn();
      if (last) return last;
    } catch (e) {
      last = e;
    }
    if (Date.now() > deadline) {
      throw new Error(`timed out waiting for ${what} (last: ${JSON.stringify(last)?.slice(0, 300)})`);
    }
    await new Promise((r) => setTimeout(r, everyMs));
  }
}

export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/**
 * A reverse proxy that serves the app under a path prefix and strips it, the
 * way Coder's path-based app URLs do. Returns the prefixed URL.
 */
export async function prefixProxy(prefix, target = BASE) {
  const t = new URL(target);
  const strip = (url) => (url.startsWith(prefix) ? url.slice(prefix.length - 1) : null);
  const server = http.createServer((req, res) => {
    const path = strip(req.url);
    if (path === null) {
      res.writeHead(404).end("outside the app prefix");
      return;
    }
    const upstream = http.request(
      { host: t.hostname, port: t.port, path, method: req.method, headers: req.headers },
      (up) => {
        res.writeHead(up.statusCode, up.headers);
        up.pipe(res);
      },
    );
    upstream.on("error", () => res.writeHead(502).end());
    req.pipe(upstream);
  });
  server.on("upgrade", (req, socket, head) => {
    const path = strip(req.url);
    if (path === null) return socket.destroy();
    const up = net.connect(Number(t.port), t.hostname, () => {
      const headers = Object.entries(req.headers).map(([k, v]) => `${k}: ${v}`).join("\r\n");
      up.write(`GET ${path} HTTP/1.1\r\n${headers}\r\n\r\n`);
      if (head.length) up.write(head);
      socket.pipe(up).pipe(socket);
    });
    up.on("error", () => socket.destroy());
    socket.on("error", () => up.destroy());
  });
  await new Promise((r) => server.listen(0, "127.0.0.1", r));
  return {
    url: `http://127.0.0.1:${server.address().port}${prefix}`,
    close: () => server.close(),
  };
}
