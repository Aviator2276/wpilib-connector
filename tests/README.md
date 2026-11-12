# Tests

| Suite | Where | Needs | What it checks |
|---|---|---|---|
| Daemon unit tests | `daemon/tests` | `uv` | HAL cache and coalescing, snapshot on connect, single upstream connection, echo to other tabs, auto-disable, storage and path confinement, NT and camera proxies, static hosting |
| Shell unit tests | `web/src/**/*.test.ts` | `bun` | URL derivation under a path prefix, layout-file sync, file-dialog filters |
| Module | `registry/.../wpilib-sim.tftest.hcl`, `main.test.ts` | Terraform; the Coder registry harness for the bun test | Variables, app wiring, script templating |
| WASM build | `wasm/build.sh` | Docker | The patch set applies to the pinned tag and everything links |
| End to end | `e2e/run.mjs` | a running robot, daemon and Chromium | See below |
| Vendor matrix | `vendor-matrix/run.sh` | same | One robot per vendor library; its devices and cameras appear in the web GUI |
| Visual parity | `parity/` | same, plus Xvfb and Mesa for the native capture | Web GUI screenshot against the native GUI |
| Performance | `perf/measure.mjs` | same | Tab main-thread time and heap, optionally CPU-throttled |
| Image | `ci/run-in-image.sh` | Docker and a built image | Runs the module script, a robot, the end-to-end suite and the vendor matrix inside the image |

`ci/run-in-image.sh` is the one command that exercises everything together,
and what CI runs on the linux/amd64 image.

## End to end

`e2e/run.mjs` drives headless Chromium against a live simulation of the
`vendor` robot and asserts on the state of the HAL inside the GUI
(`websim_debug_state()`), not on pixels:

- the daemon finds the robot program, project and bundle;
- the GUI connects NetworkTables and the HAL;
- the vendor devices are in the GUI's device list;
- clicking Teleoperated enables the real robot (and the robot's log agrees);
- with the bridge, the Timing window shows the robot's clock and Pause / Run
  stop and start it;
- a layout change is saved into the robot project;
- the camera stream is listed and shows video;
- the app works under a path prefix, including a deep link;
- the robot is disabled after the last tab closes.

It clicks at the default window positions, so the project must have no
`simgui*.json`. `EXPECT_BRIDGE=0` with `WPILIB_WEBSIM_BRIDGE=0` in the robot's
environment covers the stock WebSocket fallback.

## Test robots

`robots/make-project.sh <vendor|photon> <dir>` assembles a project from the
stock template in `images/seed` and one of the robots. The projects contain
nothing about the web sim.

- `vendor`: every Sim GUI device type, Phoenix 6, REVLib, Studica navX and a
  `CameraServer` stream. Runs on x86-64 and arm64.
- `photon`: adds a PhotonVision simulated camera. x86-64 only (PhotonLib
  publishes no arm64 desktop natives).

## Vendor matrix

Each directory in `vendor-matrix/libs/` has the library's vendordep, a Java
snippet constructing its devices, and the names expected in the GUI. Covered:
Phoenix 6, REVLib, Studica, PhotonLib (skipped on arm64 hosts). The other 18 libraries in
WPILib's 2026 list are not covered; adding one is a new directory.

## Visual parity

```bash
tests/parity/native.sh ~/projects/robot native.png
```

```bash
(cd tests/e2e && node shot.mjs web.png)
```

```bash
PARITY_MASK="10,200,60,115;520,350,170,18" node tests/parity/compare.mjs native.png web.png diff.png
```

`parity/reference/` holds one such pair for the `vendor` robot on WPILib
2026.2.1 (0.14% of pixels differ beyond tolerance, all text antialiasing).
The comparison is run by hand; it is not part of CI, because the native
capture needs an X stack the image deliberately lacks.
