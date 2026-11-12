// Measure what the browser tab costs: main-thread time and heap, idle and
// with the robot enabled, optionally under Chrome's CPU throttle.
//
//   node measure.mjs            WEBSIM_URL as for the e2e tests
//     PERF_SECONDS   seconds per measurement         (default 10)
//     PERF_THROTTLE  CPU slowdown factor, e.g. 6      (default 1)
//
// Headless Chromium here renders WebGL in software (SwiftShader), so the
// "busy" share includes rasterization that a Chromebook's GPU would do.
// Treat the numbers as an upper bound and as a regression signal; real
// device measurements need a real device.
import { BASE, guiState, launch, openPage, sleep, waitFor } from "../e2e/lib.mjs";

const seconds = Number(process.env.PERF_SECONDS ?? 10);
const throttle = Number(process.env.PERF_THROTTLE ?? 1);

const browser = await launch();
const { page } = await openPage(browser, BASE);
const cdp = await page.context().newCDPSession(page);
await cdp.send("Performance.enable");
if (throttle > 1) await cdp.send("Emulation.setCPUThrottlingRate", { rate: throttle });

const metrics = async () => {
  const { metrics } = await cdp.send("Performance.getMetrics");
  return Object.fromEntries(metrics.map((m) => [m.name, m.value]));
};
async function sample(label) {
  const a = await metrics();
  const framesBefore = await page.evaluate(() => {
    window.__frames = 0;
    const tick = () => {
      window.__frames++;
      requestAnimationFrame(tick);
    };
    if (!window.__counting) {
      window.__counting = true;
      requestAnimationFrame(tick);
    }
    return 0;
  });
  await sleep(seconds * 1000);
  const b = await metrics();
  const busy = (100 * (b.TaskDuration - a.TaskDuration)) / seconds;
  const script = (100 * (b.ScriptDuration - a.ScriptDuration)) / seconds;
  console.log(
    `${label.padEnd(28)} main thread busy ${busy.toFixed(1).padStart(5)}%   script ${script
      .toFixed(1)
      .padStart(5)}%   JS heap ${(b.JSHeapUsedSize / 1048576).toFixed(0)} MB`,
  );
  void framesBefore;
}

const t0 = Date.now();
await waitFor("GUI connected", async () => {
  const g = await guiState(page);
  return g?.nt.connected && g.hal.connected && g.simDevices.length > 0;
}, 60_000);
console.log(`first connected frame after ${((Date.now() - t0) / 1000).toFixed(1)} s (throttle ${throttle}x)`);

await page.mouse.click(50, 70); // Disabled
await sleep(1500);
await sample("robot disabled, no input");

await page.mouse.click(50, 104); // Teleoperated
await waitFor("enabled", async () => (await guiState(page)).ds.enabled);
await sample("robot enabled (teleop)");

await page.mouse.click(50, 70); // leave the robot disabled
const wasm = await page.evaluate(() =>
  performance.getEntriesByType("resource").filter((r) => /simgui\.(wasm|js)$/.test(r.name))
    .map((r) => `${r.name.split("/").pop()} ${(r.transferSize / 1048576).toFixed(2)} MB transferred`),
);
console.log(wasm.join(", "));
await browser.close();
process.exit(0);
