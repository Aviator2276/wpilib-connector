# Deploying

## Checklist

1. **Image.** Base the workspace on `ghcr.io/aviator2276/wpilib-workspace:2026`
   (or build `images/Dockerfile` from the repository root). The module needs
   `/opt/wpilib-websim` from that image and stops with an error without it.
2. **Module.** Add `registry/aviator2277/modules/wpilib-sim` to the template.
   If the template also uses the code-server module, do **not** list
   `wpilibsuite.vscode-wpilib` in its extensions; this module installs the
   pinned VSIX.
3. **Port.** With one network namespace per workspace the default port is
   fine. If workspaces share a namespace, pass a per-workspace random `port`.
4. **Access.** Keep `share = "owner"`. Anyone who can open the app can enable
   the simulated robot.
5. **Migrating from v1.** The slug and display name are unchanged. Remove the
   v1-only variables from the module block: `resolution`, `sim_fps`,
   `max_frame_rate`, `jpeg_quality_*`, `enable_clipboard`, `kasm_version`,
   `vnc_auth`, `vnc_username`, `vnc_password`, `wpilib_year`. The module no
   longer exports `DISPLAY` or the software-rendering variables.

Both subdomain and path-based apps work; path mode needs no bounce page.

## Networking

- Per-workspace network namespace: the robot's fixed ports (5810, 3300, 3301,
  1181+) are private to the workspace. The daemon binds loopback.
- Shared namespace: two simultaneous simulations still collide on 5810,
  exactly as in v1. This project does not change that.

## Measured

On an Apple-silicon host, Linux arm64 containers, WPILib 2026.2.1, the
`vendor` test robot (Phoenix 6 TalonFX + CANcoder + Pigeon 2, SPARK MAX, navX,
every roboRIO device type, one 320x240 camera stream). Browser numbers are
headless Chromium with **software** WebGL (SwiftShader), so they are an upper
bound for the rendering share; nothing here was measured on a Chromebook.

| What | Result |
|---|---|
| `simgui.wasm` | 4.42 MB; 2.08 MB Brotli; 2.57 MB gzip |
| `simgui.js` | 152 KB uncompressed |
| Shell | 318 KB JS (88 KB Brotli), 18 KB CSS |
| Page load to connected GUI | 1.6–2.7 s over loopback |
| Tab main thread, robot disabled, no input | 3.6% (6.5% under a 6x CPU throttle) |
| Tab main thread, robot enabled | 3.4% (16.8% under a 6x CPU throttle) |
| JS heap | 4 MB (the WebAssembly heap is separate; 64 MB initial) |
| `websimd`, one tab, bridge mode | 2% of a core, 59 MB resident |
| `websimd`, one tab, stock WebSocket mode | 4% of a core, 59 MB resident |
| Robot JVM, headless | about 13% of a core |
| Visual difference from the native GUI, same robot and layout | 0.14% of pixels beyond 5/255 (text antialiasing) |
| Image size | 4.15 GB (arm64) |

Against the plan's targets: the tab's idle cost and the bridge-mode daemon are
inside them. The stock-WebSocket daemon is over its 3% target and at the edge
of its 60 MB one; that path is the fallback. The bundle is about at its 2 MB
Brotli target; the JS glue is not under 80 KB.

## Verified

- `./gradlew simulateJava` on the unmodified project template, in the image,
  as the `coder` user, with only the module's start script run.
- Enabling the robot from the browser; Timing pause and resume on the robot's
  clock; layout files written to the project; camera stream; auto-disable
  after the last tab closes (seen in the robot's own log); the app under a
  path prefix through a prefix-stripping proxy.
- Phoenix 6, REVLib and Studica devices in the GUI's device list.
- The robot JVM starts with no X or Mesa libraries installed; `libatomic1` is
  the only extra native dependency.
- HiDPI rendering at device pixel ratio 2.
- Field2d by hand in headless Chromium: the built-in field list and images
  (downloaded on first use), choosing a custom image from the project through
  the file picker, the path saved in `simgui.json` as the project's real
  path, and the image restored after a reload.

## Not verified

- A live Coder deployment (subdomain or path mode), including server-sent
  events and long-lived MJPEG through Coder's proxy. The shell falls back to
  polling if events do not arrive.
- **WPILib: Simulate Robot Code** from VS Code. The Gradle task it runs
  (`simulateExternalJavaRelease`) is exercised by the image build, which checks
  that the bridge is in its extension list.
- The amd64 image and bridge. CI builds both; only arm64 was run here.
- A real Chromebook, touch input, a two-hour soak.
- Uploading a file from the student's computer through the file picker,
  workspace open / save to another folder, gamepads and clipboard in a
  browser. They are implemented; the file endpoints have daemon tests.
- 18 of the 22 libraries in WPILib's 2026 vendor list.

## Known limits

- **PhotonVision needs x86-64.** PhotonLib publishes desktop native code for
  x86-64 only; on an arm64 workspace constructing a `PhotonCamera` ends the
  robot program. This is the vendor's packaging, and it applies to the native
  GUI as well. WPILib `CameraServer` streams work on both architectures.
- **Java only.** C++ and Python robot projects are not handled by the init
  script.
- **Hidden tabs.** Browsers stop animation frames for a hidden tab, and the
  GUI sends driver-station data from its frame loop, as on the desktop. An
  enabled robot keeps its last inputs while the tab is hidden; it is disabled
  only when the tab closes.
- **Two tabs** show the same state; the last writer wins on inputs.
- **Vendor GUI hooks.** Code that draws its own ImGui windows inside the robot
  process (`HALSIMGUI_EXT_*`) cannot appear in the browser.
- **Stock WebSocket fallback** (no bridge build for the project's WPILib
  version): no Timing control, Power Distribution or REV PH; SimValue enums
  show as numbers.
- **No real Driver Station** (`halsim_ds_socket` needs UDP and a native DS).

## Troubleshooting

| Symptom | Look at |
|---|---|
| Dashboard button unhealthy | `~/.cache/wpilib-websim/websimd.out` and `websimd.log` |
| "No simulation running" while one is | Is port 5810 open? Is the robot's working directory a GradleRIO project? |
| Native GUI tries to open (and fails) | `~/.gradle/init.d/wpilib-websim.gradle` missing, or `WPILIB_WEBSIM=0` set |
| "address already in use" on the next run | A robot JVM outlived its Gradle client: `pkill -f java.library.path` |
| Hardware windows empty, NetworkTables fine | Browser console: a table-hash mismatch means the image's bundle and bridge were built from different sources |
