// End-to-end test of the web sim against a running robot program.
//
// Expects, on this machine: the `vendor` test robot simulating (started with
// the init script installed), websimd serving the built shell and bundle.
//
//   WEBSIM_URL      daemon URL                      (http://127.0.0.1:13300/)
//   ROBOT_LOG       file with the robot's console output; enables the checks
//                   that the *robot* changed mode, not just the GUI
//   EXPECT_BRIDGE   "1" requires the native bridge, "0" the stock WebSocket
//                   server; unset accepts either
//   GRACE_MS        the daemon's auto-disable grace period (3000)
//
// The GUI must start from its default layout (no simgui*.json in the
// project): the test clicks at the default window positions.
import fs from "node:fs";
import { BASE, guiState, launch, openPage, prefixProxy, sleep, waitFor } from "./lib.mjs";

const EXPECTED_DEVICES = [
  "CANMotor:Talon FX (v6)[1]",
  "CANEncoder:Talon FX (v6)[1]/Rotor Sensor",
  "CANDIO:Talon FX (v6)[1]/Fwd Limit",
  "CANDIO:Talon FX (v6)[1]/Rev Limit",
  "CANEncoder:CANcoder (v6)[2]",
  "CANGyro:Pigeon 2 (v6)[3]",
  "SPARK MAX [4]",
  "navX-Sensor[4]",
];

const results = [];
let failed = false;
async function check(name, fn) {
  let line;
  try {
    const detail = await fn();
    line = `ok    ${name}${detail ? `  (${detail})` : ""}`;
  } catch (e) {
    failed = true;
    line = `FAIL  ${name}\n      ${String(e.message ?? e).split("\n").join("\n      ")}`;
  }
  results.push(line);
  console.log(line);
}
function assert(cond, message) {
  if (!cond) throw new Error(message);
}
const robotLog = () => (process.env.ROBOT_LOG ? fs.readFileSync(process.env.ROBOT_LOG, "utf8") : null);
const lastMode = () => robotLog()?.match(/\[websim-test\] mode=(\w+)/g)?.at(-1)?.split("=")[1];
const status = async () => (await fetch(`${BASE}api/status`)).json();

const browser = await launch();
const { page, logs } = await openPage(browser);
let bridge = false;

await check("daemon sees the robot program", async () => {
  const s = await waitFor("sim.running", async () => {
    const st = await status();
    return st.sim.running && st.nt.up && st.hal.up ? st : null;
  });
  assert(s.sim.project, "project directory not detected");
  assert(s.sim.bundle, "no GUI bundle chosen");
  bridge = s.hal.mode === "bridge";
  if (process.env.EXPECT_BRIDGE === "1") assert(bridge, `HAL mode is ${s.hal.mode}, expected bridge`);
  if (process.env.EXPECT_BRIDGE === "0") assert(!bridge, "expected the stock WebSocket server");
  return `${s.sim.project}, WPILib ${s.sim.wpilibVersion}, HAL ${s.hal.mode}`;
});

await check("GUI starts and connects NetworkTables and the HAL", async () => {
  const st = await waitFor("GUI connected", async () => {
    const g = await guiState(page);
    return g?.nt.connected && g.hal.connected && g.simDevices.length ? g : null;
  }, 45_000);
  assert(st.hal.bridge === bridge, "GUI and daemon disagree about the HAL mode");
  const card = await page.locator("text=No simulation running").count();
  assert(card === 0, "the status card is still showing");
});

await check("vendor devices are in the GUI's device list", async () => {
  const names = (await guiState(page)).simDevices.map((d) => d.name);
  const missing = EXPECTED_DEVICES.filter((n) => !names.includes(n));
  assert(missing.length === 0, `missing: ${missing.join(", ")}\n have: ${names.join(", ")}`);
  return `${names.length} devices`;
});

await check("clicking Teleoperated enables the real robot", async () => {
  await page.mouse.click(50, 104);
  await waitFor("ds.enabled", async () => (await guiState(page)).ds.enabled);
  // robot outputs flow back: the test robot drives PWM 0 with a sine wave
  const seen = new Set();
  await waitFor("PWM 0 moving", async () => {
    const pwm = (await guiState(page)).pwm.find((p) => p.channel === 0);
    if (pwm) seen.add(pwm.speed.toFixed(3));
    return seen.size >= 3;
  }, 10_000, 150);
  if (robotLog() !== null) {
    await waitFor("robot log says teleop", () => lastMode() === "teleop", 5000);
  }
});

if (bridge) {
  await check("Timing window shows and controls the robot's clock", async () => {
    const a = (await guiState(page)).timing;
    assert(a.notifiers.length > 0, "no notifiers listed");
    await sleep(500);
    const b = (await guiState(page)).timing;
    assert(b.fpgaTime > a.fpgaTime, "FPGA time is not advancing");
    await page.mouse.click(67, 186); // Pause
    await waitFor("paused", async () => (await guiState(page)).timing.paused);
    await sleep(400);
    const p1 = (await guiState(page)).timing.fpgaTime;
    await sleep(600);
    const p2 = (await guiState(page)).timing.fpgaTime;
    assert(p1 === p2, `clock moved while paused (${p1} -> ${p2})`);
    await page.mouse.click(26, 186); // Run
    await waitFor("resumed", async () => !(await guiState(page)).timing.paused);
    await sleep(500);
    assert((await guiState(page)).timing.fpgaTime > p2, "clock did not resume");
    return `notifiers: ${a.notifiers.join(", ")}`;
  });
}

await check("layout changes are saved into the robot project", async () => {
  await page.mouse.move(60, 29); // drag the Robot State window by its title bar
  await page.mouse.down();
  await page.mouse.move(300, 420, { steps: 6 });
  await page.mouse.up();
  const body = await waitFor("simgui-window.json", async () => {
    const r = await fetch(`${BASE}api/storage/simgui-window.json`);
    return r.ok ? await r.json() : null;
  }, 20_000, 500);
  assert(body.MainWindow?.GLOBAL, "saved file is not a Glass window layout");
});

await check("camera stream is listed and viewable", async () => {
  const s = await waitFor("a camera", async () => {
    const st = await status();
    return st.cameras.length ? st : null;
  });
  await page.hover("text=NT");
  await page.click("button:has-text('Cameras')");
  const size = await waitFor("video frames", () =>
    page.evaluate(() => {
      const img = document.querySelector("aside img");
      return img && img.naturalWidth > 0 ? [img.naturalWidth, img.naturalHeight] : null;
    }), 15_000);
  await page.click("button[aria-label='Close camera panel']");
  return `${s.cameras.map((c) => `${c.name}:${c.port}`).join(", ")} at ${size.join("x")}`;
});

await check("works under a path prefix (Coder path-based apps)", async () => {
  const proxy = await prefixProxy("/@user/ws.main/apps/wpilib-sim/");
  try {
    const second = await openPage(browser, proxy.url);
    await waitFor("GUI connected through the prefix", async () => {
      const g = await guiState(second.page);
      return g?.nt.connected && g.hal.connected && g.simDevices.length > 0;
    }, 45_000);
    // a deep link must load the shell and still find its assets
    await second.page.goto(`${proxy.url}cameras`);
    await second.page.waitForSelector("text=Back to the Sim GUI", { timeout: 10_000 });
    await second.page.close();
  } finally {
    proxy.close();
  }
});

await check("the robot is disabled after the last tab closes", async () => {
  assert((await guiState(page)).ds.enabled, "robot should still be enabled here");
  await page.close();
  await sleep(Number(process.env.GRACE_MS ?? 3000) + 2500);
  assert((await status()).clients === 0, "daemon still counts a tab");
  if (robotLog() !== null) {
    assert(lastMode() === "disabled", `robot is in mode ${lastMode()}`);
    return "robot log says disabled";
  }
  // without the robot's log, ask a fresh GUI what the robot's state is
  const again = await openPage(browser);
  const g = await waitFor("GUI reconnected", async () => {
    const st = await guiState(again.page);
    return st?.hal.connected && st.simDevices.length ? st : null;
  }, 45_000);
  assert(!g.ds.enabled, "robot is still enabled");
});

await browser.close();
console.log(`\n${results.filter((r) => r.startsWith("ok")).length} passed, ${results.filter((r) => r.startsWith("FAIL")).length} failed`);
if (failed) {
  console.log("\n--- browser console ---\n" + logs.slice(-40).join("\n"));
  process.exit(1);
}
process.exit(0);
