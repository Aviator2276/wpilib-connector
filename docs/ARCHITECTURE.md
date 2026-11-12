# Architecture

## The idea

The desktop Sim GUI is a HAL extension: it lives inside the robot process and
reads and writes hardware state by calling `HALSIM_*` functions, and it shares
the robot's NetworkTables instance. In a browser there is no robot process.
So the WebAssembly module carries its own copy of the simulation HAL, and two
links keep it equal to the robot's:

- **NetworkTables**: ntcore is compiled in and runs as an ordinary NT4 client
  of the robot's server.
- **HAL**: a *mirror* applies the robot's hardware state to the local HAL and
  sends the GUI's edits of inputs back.

The GUI code itself (`halsim_gui`, Glass, wpigui, Dear ImGui) is upstream's,
entered through its normal `HALSIM_InitExtension()`.

## Components

| Component | Where it runs | Source |
|---|---|---|
| `simgui.wasm` | browser | `wasm/` |
| Shell | browser | `web/` |
| `websimd` | workspace | `daemon/` |
| `halsim_webbridge` | robot JVM | `bridge/` |
| Gradle init script | workspace (`~/.gradle/init.d`) | `images/gradle/` |
| Coder module | Coder | `registry/aviator2277/modules/wpilib-sim/` |

## The WebAssembly GUI (`wasm/`)

`wasm/build.sh <season>` clones allwpilib at the tag in
`wasm/seasons/<season>.env`, applies `wasm/patches/<season>/`, and compiles
with Emscripten (single-threaded, WebGL2 and WebSockets only, no
SharedArrayBuffer).

The patch set, in three forms so drift is caught at build time:

| Kind | Files | Why |
|---|---|---|
| `*.patch` (must apply) | `0001-wpigui-browser.patch` | The browser owns the frame loop (`requestAnimationFrame`), so `gui::Main()` cannot block; window coordinates are CSS pixels, so the content scale must not enlarge fonts twice; glyphs are oversampled on HiDPI screens |
| `overlay/` (replacements and additions) | `ntcore/…/NetworkClient.{h,cpp}`, `NetworkServer.h` | NT4 client on the browser WebSocket API instead of libuv; a browser cannot host an NT server |
| | `wpigui/…/webgl/wpigui_webgl.cpp` | WebGL2 platform layer (from `wpigui_opengl3.cpp`) |
| | `wpigui/…/portable-file-dialogs.cpp`, `wpigui_openurl.cpp` | File dialogs go to the shell; URLs open in a tab |
| `remove.txt` (must exist upstream) | libuv-only ntcore files | No browser equivalent |

Everything else upstream compiles unmodified. One wpimath file is left out
(`Ellipse2d.cpp`, which needs an optimizer nothing in the GUI calls).

Season-independent code is in `wasm/src/`:

| File | Purpose |
|---|---|
| `main.cpp` | Starts the HAL, the NT client, the GUI extension and the mirror |
| `mirror.cpp` | Mirror core: transport, local HAL callbacks, SimDevices, and the link-time wrappers (`-Wl,--wrap`) that point the Timing window's HAL calls at the robot |
| `mirror_json.cpp` / `mirror_bridge.cpp` | The two HAL codecs ([`protocol/PROTOCOL.md`](../protocol/PROTOCOL.md)) |
| `frame.cpp` | Redraw-on-demand |
| `fields.cpp` | Built-in field images, downloaded on first use instead of embedded |
| `debug.cpp` | `websim_debug_state()`: the in-browser HAL as JSON, for tests |

### Things that differ from the desktop, on purpose

- **When frames are drawn.** A frame is drawn during input, about 30 times a
  second while the robot is enabled or a plot is visible, at most 10 times a
  second for incoming data otherwise, and twice a second when idle. What a
  frame contains is unchanged.
- **NetworkTables Info** shows a client's view; on the desktop the GUI shares
  the robot's server instance.
- **File dialogs** browse the robot project in the workspace
  (`/api/fs`), with an upload button.
- **Closing the tab does not stop the robot.** The daemon disables it instead
  (see below).
- **Without the bridge** (stock WebSocket fallback) the Timing window shows
  the browser's clock and its buttons do nothing, and Power Distribution, REV
  PH, analog gyro and duty cycle windows stay empty.

### Paths

The module's working directory in Emscripten's in-memory filesystem is the
robot project's real path. Layout files and anything a layout refers to (a
custom field image) are copied in under the same absolute paths before
`main()` runs, so a `simgui.json` means the same thing in the browser and on
a desktop in that workspace.

## The shell (`web/`)

A static single-page app; there is no Node process at runtime.

- States: no simulation, robot found and connecting, connected (canvas fills
  the tab), simulation ended (last frame dimmed). A new robot run reloads the
  page, because the module holds one run's devices and layout.
- While connected the only shell element over the canvas is a status pill in
  the bottom corner that fades; hovering it opens the camera controls.
- Cameras are plain `<img>` elements on the daemon's MJPEG proxy, mounted only
  while visible.
- Every URL is derived from `document.baseURI` (`web/src/lib/base.ts`); the
  daemon injects a `<base>` so deep links work under Coder's path prefix.
- The shell asks the GUI to save its layout every five seconds and when the
  tab is hidden or closed (a tab has no dependable "window closed" moment,
  which is when the desktop GUI saves). A layout file is uploaded only when
  its content differs from the server's copy and has stopped changing
  (`web/src/lib/memfsSync.ts`).

## The daemon (`daemon/`)

All paths are relative to the app base.

| Method | Path | Purpose |
|---|---|---|
| GET | `/`, `/assets/*` | Shell (hashed assets cached forever; `.br` / `.gz` siblings served when accepted) |
| GET | `/wasm/<version>/*` | GUI bundle, field images, licence notices |
| GET | `/healthz` | `{"status":"ok"}`; the Coder healthcheck |
| GET | `/api/status` | Snapshot: sim, NT, HAL, cameras, tab count, config |
| GET | `/api/events` | Server-sent events: the same object on every change |
| GET, PUT | `/api/storage/<name>[?dir=]` | `simgui.json`, `simgui-ds.json`, `simgui-window.json` only; ETag, `If-Match` |
| GET | `/api/fields`, `/api/fields/<file>` | Built-in field index and files |
| GET | `/api/fs?dir=`, `/api/fs/file?path=` | Listing and download under the project root |
| POST | `/api/fs/upload?dir=` | Upload into the project (16 MB cap) |
| POST | `/api/sim/disable` | Force the robot to disabled |
| WS | `/nt/<client-id>` | Proxy to the robot's NT4 server; subprotocol passed through |
| WS | `/hal` | HAL relay |
| GET | `/cam/<port>/stream.mjpg` | MJPEG proxy; only ports advertised in NT or 1181–1190 |

- **Detection.** Once a second it tries the NT port. On success it finds the
  listening process through `/proc`, takes its working directory as the
  project (if it has `.wpilib/wpilib_preferences.json`), reads the GradleRIO
  version from `build.gradle`, and picks the matching GUI bundle.
- **HAL relay.** If the bridge port is open, each tab is proxied to the
  bridge and the daemon keeps a control connection. Otherwise it holds the one
  connection `halsim_ws_server` allows and fans out with a cache and 33 ms
  coalescing.
- **Auto-disable.** When the last tab disconnects and none returns within
  three seconds, the robot is disabled. On by default; `auto_disable = false`
  in the module turns it off.
- **Security.** Loopback only, no authentication of its own; Coder's app
  proxy and session are the gate. File endpoints are confined to the project
  root after symlink resolution. The proxies connect only to loopback.

## Making projects cooperate (`images/gradle/wpilib-websim.gradle`)

Installed to `~/.gradle/init.d/`. For GradleRIO projects only, after the
project is evaluated, it sets the Sim GUI extension's `defaultEnabled` to
false, adds and enables the simulation WebSocket server, and adds the bridge
from the image's local Maven repository when a build exists for the project's
WPILib version and the CPU architecture. Both launch paths read those flags:
`simulateJava` builds `HALSIM_EXTENSIONS` from them, and the VS Code command
reads them from `build/sim/release_java.json`.

`WPILIB_WEBSIM=0 ./gradlew simulateJava` restores stock behaviour for one
command; `WPILIB_WEBSIM_BRIDGE=0` leaves only the bridge out.

## The bridge (`bridge/`)

A HAL extension like WPILib's own, loaded through `HALSIM_EXTENSIONS`. It
registers a callback on every table value, keeps the latest values, and
serves them over WebSocket; it applies the GUI's writes with the same
`HALSIM_Set*` calls the desktop GUI makes. It links against WPILib's
published `hal`, `wpinet` and `wpiutil` libraries, which the robot process
has already loaded, and is packaged as
`websim.halsim:halsim_webbridge:<wpilib version>:<platform>@zip`, the shape
GradleRIO resolves for `wpi.sim.addDep(...)`.

## Adding a WPILib season

1. Add `wasm/seasons/<season>.env` (version, tag, Emscripten version).
2. Copy `wasm/patches/<previous>/` to `wasm/patches/<season>/` and run
   `wasm/build.sh <season>`. Fix whatever no longer applies; the build names
   the patch or path.
3. Run `bridge/build.sh <season>`. `protocol/hal_fields.def` fails to compile
   if a HAL accessor was renamed or removed.
4. Add the season to the `SEASON` build argument handling in
   `images/Dockerfile` and to the CI matrix.

Bundles live side by side under `/wasm/<version>/`, and the daemon picks one
from the running project's GradleRIO version, so one image can serve several
seasons.

For 2027: wpigui moves from GLFW to SDL3, so the wpigui patch and platform
file are rewritten rather than ported; the plugin id and Maven groups move to
`org.wpilib` (the init script already matches both ids). No 2027 patch set
exists yet.
