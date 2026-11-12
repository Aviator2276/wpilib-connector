# WPILib Simulator for Coder (web)

The WPILib Robot Simulation GUI in a browser tab, for FRC teams working in
[Coder](https://coder.com) workspaces. One button on the dashboard; no VNC, no
display, nothing to install on the student's computer.

The GUI is not a rewrite. It is WPILib's own Sim GUI (halsim_gui, Glass,
wpigui, Dear ImGui) compiled to WebAssembly and drawn on a WebGL2 canvas, so
the windows, menus, fonts, docking and saved layouts are the desktop ones. The
robot program runs headless in the workspace; only data crosses the network.

```
Chromebook / Chrome tab                         Coder workspace
┌───────────────────────────────┐               ┌────────────────────────────────────────┐
│ React shell (status, cameras) │   HTTPS/WSS   │ websimd (daemon, 127.0.0.1:<port>)     │
│ ┌───────────────────────────┐ │◄─────────────►│  static files, /api, WS + MJPEG proxy  │
│ │ simgui.wasm on <canvas>   │ │  via Coder    │     │            │            │        │
│ │  halsim_gui + glass       │ │  coder_app    │   NT4 :5810   HAL :3301   cams :1181+  │
│ │  ntcore client (NT4)      │ │               │     ▼            ▼            ▼        │
│ │  mirror HAL               │ │               │ robot JVM (./gradlew simulateJava or   │
│ └───────────────────────────┘ │               │ "WPILib: Simulate Robot Code")         │
└───────────────────────────────┘               │  no X server, no GUI extension         │
                                                └────────────────────────────────────────┘
```

This is version 2 of the module. Version 1 streamed the native GUI over
KasmVNC; v2 keeps the module's slug and display name and drops the X/VNC
stack.

## Using it

Build on the workspace image (`ghcr.io/aviator2276/wpilib-workspace:2026`,
linux/amd64, built by CI from `images/Dockerfile`) and add the module to the template:

```tf
module "wpilib_sim" {
  count    = data.coder_workspace.me.start_count
  source   = "git::https://github.com/<you>/wpilib-connector.git//registry/aviator2277/modules/wpilib-sim"
  agent_id = coder_agent.main.id
}
```

Students then run `./gradlew simulateJava` or **WPILib: Simulate Robot Code**
as usual and click **WPILib Simulator** on the dashboard. Their projects are
not modified: a Gradle init script switches the native GUI off and the web
sim's extensions on.

See [`docs/DEPLOYING.md`](docs/DEPLOYING.md) for the checklist, measured
numbers and known limits, and the
[module README](registry/aviator2277/modules/wpilib-sim/README.md) for its
variables.

## Layout

| Path | What it is |
|---|---|
| `wasm/` | Emscripten build of the Sim GUI from a pinned allwpilib tag, the per-season patch set, and the browser-side glue (mirror HAL, field images, redraw-on-demand) |
| `web/` | The shell: React, Tailwind, TanStack Router. Connection states, camera views, file picker |
| `daemon/` | `websimd` (Python, aiohttp): static hosting, NT / HAL / camera proxies, layout files, sim detection, auto-disable |
| `bridge/` | `halsim_webbridge`: native HAL extension loaded into the robot JVM (timing, power distribution, typed SimValues, binary protocol) |
| `protocol/` | What the browser and the robot agree on: the HAL field table and the bridge wire format, shared by `wasm/` and `bridge/` |
| `registry/aviator2277/modules/wpilib-sim/` | The Coder module |
| `images/` | Workspace image, seed project for the Gradle cache, the Gradle init script |
| `tests/` | End-to-end, visual parity, vendor matrix and performance scripts, and the test robots |
| `docs/` | Architecture and deployment notes |

## Building and testing

Everything builds in containers; Docker is the only host requirement besides
`uv` and `bun` for the daemon and shell tests.

```bash
wasm/build.sh 2026          # dist/2026.2.1/{simgui.js,simgui.wasm,fields/}
```

```bash
bridge/build.sh 2026        # dist/maven/... for this machine's architecture
```

```bash
(cd daemon && uv sync && uv run pytest)
```

```bash
(cd web && bun install && bun run test && bun run build)
```

```bash
docker build -f images/Dockerfile -t wpilib-workspace:dev .
```

```bash
tests/ci/run-in-image.sh    # module script + robot + e2e + vendor matrix, in the image
```

[`tests/README.md`](tests/README.md) describes each suite;
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) explains how the pieces fit and
how to add a WPILib season.

## Licence

MIT; see [`LICENSE`](LICENSE). The GUI bundle is compiled from WPILib (BSD)
and contains third-party software; see
[`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md). This project is not
affiliated with or endorsed by FIRST or WPILib.
