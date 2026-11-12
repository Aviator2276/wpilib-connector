---
display_name: WPILib Simulator
description: The WPILib Robot Simulation GUI in a browser tab, running natively as WebAssembly
icon: ../../../../.icons/wpilib.svg
verified: false
tags: [wpilib, frc, simulation, webassembly]
---

# WPILib Simulator

Adds one dashboard button that opens the [WPILib Robot Simulation GUI](https://docs.wpilib.org/en/stable/docs/software/wpilib-tools/robot-simulation/simulation-gui.html) in the browser. The GUI is WPILib's own, compiled to WebAssembly: it runs in the tab and talks to the robot program in the workspace, so there is no VNC session, display or password. When no simulation is running, the page says how to start one and connects by itself when it starts.

```tf
module "wpilib_sim" {
  count    = data.coder_workspace.me.start_count
  source   = "registry.coder.com/aviator2277/wpilib-sim/coder"
  version  = "2.0.0"
  agent_id = coder_agent.main.id
}
```

`./gradlew simulateJava` and the WPILib VS Code extension's **Simulate Robot Code** then feed the web GUI with no change to the robot project: the module installs a Gradle init script that turns the native GUI extension off and the web simulator's extensions on.

## Requirements

- The workspace image must provide `/opt/wpilib-websim` (the web app, the GUI bundle, the daemon and the HAL bridge). Use `ghcr.io/aviator2276/wpilib-workspace:2026` or build `images/Dockerfile` from the [source repository](https://github.com/Aviator2276/wpilib-connector). The start script fails with a clear message otherwise.
- If the template uses the code-server module, do not list `wpilibsuite.vscode-wpilib` in its extensions. Open VSX carries a version too old to build current projects; this module installs the pinned VSIX from the image.
- Chrome or another browser with WebGL2.

## Examples

A network namespace shared between workspaces needs a port per workspace:

```tf
resource "random_integer" "sim_port" {
  min = 11000
  max = 65535
}

module "wpilib_sim" {
  count    = data.coder_workspace.me.start_count
  source   = "registry.coder.com/aviator2277/wpilib-sim/coder"
  version  = "2.0.0"
  agent_id = coder_agent.main.id
  port     = random_integer.sim_port.result
}
```

Without a wildcard access URL, serve the app under a path:

```tf
module "wpilib_sim" {
  count     = data.coder_workspace.me.start_count
  source    = "registry.coder.com/aviator2277/wpilib-sim/coder"
  version   = "2.0.0"
  agent_id  = coder_agent.main.id
  subdomain = false
}
```

Teams that drive the simulated robot from another dashboard, with the simulator tab closed, can keep the robot enabled when the tab goes away:

```tf
module "wpilib_sim" {
  count        = data.coder_workspace.me.start_count
  source       = "registry.coder.com/aviator2277/wpilib-sim/coder"
  version      = "2.0.0"
  agent_id     = coder_agent.main.id
  auto_disable = false
}
```

## Behaviour worth knowing

- **Closing the tab does not stop the robot program.** It keeps running until it is stopped from VS Code or the terminal. By default the module disables the robot three seconds after the last simulator tab closes.
- **Saved layouts stay in the project.** `simgui.json`, `simgui-ds.json` and `simgui-window.json` are read from and written to the robot project in the desktop format.
- **Cameras.** Streams the robot publishes (a PhotonVision simulated camera, `CameraServer`) are shown in a side panel and on a camera wall; the Sim GUI itself has no camera viewer.
- **One command without the web simulator:** `WPILIB_WEBSIM=0 ./gradlew simulateJava`.
- **Shared network namespaces:** two simulations at once still collide on NetworkTables port 5810.
- **PhotonVision** simulation needs an x86-64 workspace; PhotonLib publishes no arm64 desktop build.

## Upgrading from 1.x

The slug and display name are unchanged. Remove the variables that configured the VNC session: `resolution`, `sim_fps`, `max_frame_rate`, `jpeg_quality_min`, `jpeg_quality_max`, `enable_clipboard`, `kasm_version`, `vnc_auth`, `vnc_username`, `vnc_password` and `wpilib_year`. The default `port` changed from 16800 to 13300, and the outputs `vnc_port` and `display` are replaced by `port`.
