// Take a screenshot of the running web Sim GUI.
//   node shot.mjs out.png            WEBSIM_URL, DPR and WAIT (ms) are optional
import { BASE, launch, openPage, sleep } from "./lib.mjs";

const browser = await launch();
const { page, logs } = await openPage(browser, BASE, { dpr: Number(process.env.DPR ?? 1) });
await sleep(Number(process.env.WAIT ?? 9000));
await page.screenshot({ path: process.argv[2] ?? "websim.png" });
if (process.env.LOGS) console.log(logs.join("\n"));
await browser.close();
