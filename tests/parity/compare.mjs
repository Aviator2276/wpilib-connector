// Compare a web Sim GUI screenshot with a native one.
//
//   node compare.mjs <native.png> <web.png> [diff.png]
//
//   PARITY_FUZZ   per-channel tolerance, 0-255            (default 5, about 2%)
//   PARITY_MAX    largest share of differing pixels, in %  (default 1)
//   PARITY_MASK   regions to ignore, "x,y,w,h;x,y,w,h"
//
// The shell's status pill (bottom right) is masked by default. Mask live
// numbers yourself; for the default layout the Timing window's values are
// "10,200,60,115". The virtual display's pointer rests at the screen centre
// and highlights whatever is under it in the native capture
// ("520,350,170,18" for the default layout).
// Exits 1 when the images differ by more than PARITY_MAX.
//
// The comparison runs in a headless page, so no image library is needed.
import fs from "node:fs";
import path from "node:path";
import { launch } from "../e2e/lib.mjs";

const [nativePath, webPath, diffPath] = process.argv.slice(2);
if (!nativePath || !webPath) {
  console.error("usage: node compare.mjs <native.png> <web.png> [diff.png]");
  process.exit(2);
}
const fuzz = Number(process.env.PARITY_FUZZ ?? 5);
const maxPercent = Number(process.env.PARITY_MAX ?? 1);
const DEFAULT_MASK = "1160,680,120,40"; // status pill
const masks = (process.env.PARITY_MASK ? `${DEFAULT_MASK};${process.env.PARITY_MASK}` : DEFAULT_MASK)
  .split(";")
  .map((r) => r.split(",").map(Number));

const dataUrl = (p) => `data:image/png;base64,${fs.readFileSync(p).toString("base64")}`;
const browser = await launch();
const page = await browser.newPage();
const result = await page.evaluate(
  async ({ a, b, fuzz, masks }) => {
    const load = (src) =>
      new Promise((resolve, reject) => {
        const img = new Image();
        img.onload = () => resolve(img);
        img.onerror = reject;
        img.src = src;
      });
    const [ia, ib] = await Promise.all([load(a), load(b)]);
    if (ia.width !== ib.width || ia.height !== ib.height) {
      return { error: `sizes differ: ${ia.width}x${ia.height} vs ${ib.width}x${ib.height}` };
    }
    const w = ia.width;
    const h = ia.height;
    const pixels = (img) => {
      const c = new OffscreenCanvas(w, h).getContext("2d");
      c.drawImage(img, 0, 0);
      return c.getImageData(0, 0, w, h);
    };
    const pa = pixels(ia).data;
    const pb = pixels(ib).data;
    const out = new ImageData(w, h);
    const masked = (x, y) => masks.some(([mx, my, mw, mh]) => x >= mx && x < mx + mw && y >= my && y < my + mh);
    let exact = 0;
    let fuzzy = 0;
    let counted = 0;
    for (let y = 0; y < h; y++) {
      for (let x = 0; x < w; x++) {
        const i = (y * w + x) * 4;
        const skip = masked(x, y);
        const d = Math.max(
          Math.abs(pa[i] - pb[i]),
          Math.abs(pa[i + 1] - pb[i + 1]),
          Math.abs(pa[i + 2] - pb[i + 2]),
        );
        if (!skip) {
          counted++;
          if (d > 0) exact++;
          if (d > fuzz) fuzzy++;
        }
        // diff image: the web frame dimmed, differences in red, masks in blue
        const g = (pb[i] + pb[i + 1] + pb[i + 2]) / 6;
        out.data[i] = !skip && d > fuzz ? 255 : g;
        out.data[i + 1] = g;
        out.data[i + 2] = skip ? 160 : g;
        out.data[i + 3] = 255;
      }
    }
    const c = document.createElement("canvas");
    c.width = w;
    c.height = h;
    c.getContext("2d").putImageData(out, 0, 0);
    return { w, h, counted, exact, fuzzy, diff: c.toDataURL("image/png") };
  },
  { a: dataUrl(nativePath), b: dataUrl(webPath), fuzz, masks },
);
await browser.close();

if (result.error) {
  console.error(result.error);
  process.exit(1);
}
if (diffPath) {
  fs.mkdirSync(path.dirname(path.resolve(diffPath)), { recursive: true });
  fs.writeFileSync(diffPath, Buffer.from(result.diff.split(",")[1], "base64"));
}
const pct = (n) => ((100 * n) / result.counted).toFixed(2);
console.log(`${result.w}x${result.h}, ${result.counted} pixels compared`);
console.log(`differ at all:            ${result.exact} (${pct(result.exact)}%)`);
console.log(`differ by more than ${fuzz}/255: ${result.fuzzy} (${pct(result.fuzzy)}%)`);
if ((100 * result.fuzzy) / result.counted > maxPercent) {
  console.log(`FAIL: more than ${maxPercent}% of pixels differ`);
  process.exit(1);
}
console.log(`ok: within ${maxPercent}%`);
