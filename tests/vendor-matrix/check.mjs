// Assert that what a vendor library creates in the robot process shows up in
// the web GUI: SimDevices in the GUI's own HAL, cameras in the daemon status.
//   node check.mjs <expected.txt>
import fs from "node:fs";
import { BASE, guiState, launch, openPage, waitFor } from "../e2e/lib.mjs";

const lines = fs.readFileSync(process.argv[2], "utf8").split("\n").map((l) => l.trim());
const devices = lines.filter((l) => l.startsWith("device:")).map((l) => l.slice(7).trim());
const cameras = lines.filter((l) => l.startsWith("camera:")).map((l) => l.slice(7).trim());

const browser = await launch();
const { page } = await openPage(browser);
let state;
let status;
try {
  await waitFor("everything the library creates", async () => {
    state = await guiState(page);
    status = await (await fetch(`${BASE}api/status`)).json();
    if (!state?.hal.connected || !state.nt.connected) return false;
    const names = state.simDevices.map((d) => d.name);
    const cams = status.cameras.map((c) => c.name);
    return devices.every((d) => names.includes(d)) && cameras.every((c) => cams.includes(c));
  }, 60_000);
  console.log(`ok: ${devices.length} device(s), ${cameras.length} camera(s) present in the web GUI`);
  await browser.close();
  process.exit(0);
} catch {
  const names = state?.simDevices.map((d) => d.name) ?? [];
  const cams = status?.cameras.map((c) => c.name) ?? [];
  console.log("MISSING devices:", devices.filter((d) => !names.includes(d)));
  console.log("MISSING cameras:", cameras.filter((c) => !cams.includes(c)));
  console.log("GUI has devices:", names, "cameras:", cams);
  await browser.close();
  process.exit(1);
}
