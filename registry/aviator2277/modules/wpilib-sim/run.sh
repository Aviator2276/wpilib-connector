#!/usr/bin/env bash
# WPILib Simulator (web): start the daemon that serves the browser Sim GUI.
#
# The workspace image provides everything under /opt/wpilib-websim (the built
# web app, the WebAssembly GUI bundles, the daemon and the Gradle init
# script). This script wires them into the user's home at workspace start:
#
#   1. Gradle init script  -> ~/.gradle/init.d/   (headless sim, no project edits)
#   2. VS Code setting     -> simulate without the extension picker
#   3. websimd             -> 127.0.0.1:<port>, replacing any previous instance
#   4. WPILib VS Code extension, pinned, from the VSIX baked into the image
#
# Terraform templatefile: single-dollar braces are injected by the module,
# double-dollar braces are shell.
set -euo pipefail

PORT="${PORT}"
AUTO_DISABLE="${AUTO_DISABLE}"
INJECT_GRADLE="${INJECT_GRADLE}"
PROJECTS_DIR="${PROJECTS_DIR}"

INSTALL=/opt/wpilib-websim
STATE="$HOME/.cache/wpilib-websim"
INIT_SCRIPT="$HOME/.gradle/init.d/wpilib-websim.gradle"

log() { printf '[wpilib-sim] %s\n' "$*"; }

if [ ! -x "$INSTALL/venv/bin/websimd" ] || [ ! -f "$INSTALL/web/index.html" ]; then
  log "ERROR: $INSTALL is missing or incomplete."
  log "This module needs the WPILib web sim workspace image (see the module README)."
  exit 1
fi
mkdir -p "$STATE"

# --- 1. Gradle init script ----------------------------------------------------
# Turns the native Sim GUI extension off and the web sim extensions on for
# every GradleRIO project, for both `./gradlew simulateJava` and the VS Code
# command. The file is ours: refreshed on every start, removed when disabled.
if [ "$INJECT_GRADLE" = "true" ]; then
  mkdir -p "$(dirname "$INIT_SCRIPT")"
  cp "$INSTALL/gradle/wpilib-websim.gradle" "$INIT_SCRIPT"
  log "installed Gradle init script"
else
  rm -f "$INIT_SCRIPT"
fi

# --- 2. VS Code: simulate without asking which extensions to run --------------
# "WPILib: Simulate Robot Code" shows a picker unless this is set; with it the
# extension uses the defaults, which the init script has already chosen.
"$INSTALL/venv/bin/python" - "$HOME/.local/share/code-server/Machine/settings.json" << 'PY' || log "could not update code-server settings (continuing)"
import json, os, sys

path = sys.argv[1]
try:
    with open(path) as f:
        settings = json.load(f)
    if not isinstance(settings, dict):
        raise ValueError("settings.json is not an object")
except FileNotFoundError:
    settings = {}
if settings.get("wpilib.skipSelectSimulateExtension") is not True:
    settings["wpilib.skipSelectSimulateExtension"] = True
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(settings, f, indent=2)
        f.write("\n")
    os.replace(tmp, path)
PY

# --- 3. the daemon ------------------------------------------------------------
# Loopback only and no authentication of its own: Coder's app proxy and the
# user's session are the gate. Restarting it does not disturb a running robot.
PIDFILE="$STATE/websimd.pid"
if [ -f "$PIDFILE" ]; then
  old="$(cat "$PIDFILE" 2> /dev/null || true)"
  if [ -n "$old" ] && kill -0 "$old" 2> /dev/null \
    && grep -q websimd "/proc/$old/cmdline" 2> /dev/null; then
    kill "$old" 2> /dev/null || true
    for _ in $(seq 1 20); do
      kill -0 "$old" 2> /dev/null || break
      sleep 0.1
    done
    kill -9 "$old" 2> /dev/null || true
  fi
  rm -f "$PIDFILE"
fi

disable_flag="--auto-disable"
[ "$AUTO_DISABLE" = "true" ] || disable_flag="--no-auto-disable"

# shellcheck disable=SC2086
nohup "$INSTALL/venv/bin/websimd" \
  --host 127.0.0.1 --port "$PORT" \
  --web-dir "$INSTALL/web" --wasm-dir "$INSTALL/wasm" \
  --projects-dir "$PROJECTS_DIR" \
  $disable_flag \
  --log-file "$STATE/websimd.log" \
  > "$STATE/websimd.out" 2>&1 &
echo $! > "$PIDFILE"

for _ in $(seq 1 50); do
  if curl -fsS -o /dev/null "http://127.0.0.1:$PORT/healthz" 2> /dev/null; then
    log "websimd listening on 127.0.0.1:$PORT"
    break
  fi
  if ! kill -0 "$(cat "$PIDFILE")" 2> /dev/null; then
    log "ERROR: websimd exited; see $STATE/websimd.out"
    tail -n 20 "$STATE/websimd.out" 2> /dev/null || true
    exit 1
  fi
  sleep 0.2
done

# --- 4. WPILib VS Code extension pin -------------------------------------------
# Open VSX (code-server's marketplace) stopped at vscode-wpilib 2021.3.1, which
# generates Gradle-6 projects that cannot build on JDK 17. The image bakes the
# current VSIX; install it once code-server is up, replacing any stale version.
# The template must NOT also list wpilibsuite.vscode-wpilib in the code-server
# module's extensions, or the two installers race.
(
  vsix="$(ls "$INSTALL"/vscode-wpilib-*.vsix 2> /dev/null | head -1)"
  if [ -n "$vsix" ]; then
    ver="$${vsix##*vscode-wpilib-}"
    ver="$${ver%.vsix}"
    ext_dir="$HOME/.local/share/code-server/extensions"
    for _ in $(seq 1 60); do
      cs=""
      if [ -x /tmp/code-server/bin/code-server ]; then
        cs=/tmp/code-server/bin/code-server
      elif command -v code-server > /dev/null 2>&1; then
        cs=code-server
      fi
      if [ -n "$cs" ]; then
        if [ ! -d "$ext_dir/wpilibsuite.vscode-wpilib-$ver" ]; then
          rm -rf "$ext_dir"/wpilibsuite.vscode-wpilib-* 2> /dev/null || true
          "$cs" --install-extension "$vsix" --force \
            && log "installed vscode-wpilib $ver from baked VSIX"
        fi
        break
      fi
      sleep 5
    done
  fi
) >> "$STATE/vscode-wpilib.log" 2>&1 &

log "ready"
