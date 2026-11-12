#!/usr/bin/env bash
# Vendor matrix: one robot project per vendor library, each constructing that
# library's main device classes. For every library: simulate headless, load
# the web GUI in headless Chrome, and assert the library's devices (and
# cameras) appear in it.
#
#   tests/vendor-matrix/run.sh [library ...]      default: every directory in libs/
#
# Run inside a workspace (or the workspace image) where websimd is running
# and the Gradle init script is installed; needs Node and Playwright
# (tests/e2e). A library that fails to resolve, crashes in simulation or does
# not show up is reported as a failure; nothing is skipped silently except
# libraries that declare they have no build for this CPU architecture.
#
#   WEBSIM_URL   daemon URL (default http://127.0.0.1:13300/)
#   MATRIX_DIR   where projects are generated (default ~/projects/vendor-matrix)
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="${MATRIX_DIR:-$HOME/projects/vendor-matrix}"
NT_PORT=5810
if [ "$#" -gt 0 ]; then
  LIBS=("$@")
else
  mapfile -t LIBS < <(find "$HERE/libs" -mindepth 1 -maxdepth 1 -type d -exec basename {} \; | sort)
fi

stop_robot() {
  pkill -f "[D]java.library.path=$WORK" 2> /dev/null || true
  for _ in $(seq 1 50); do
    (exec 3<> "/dev/tcp/127.0.0.1/$NT_PORT") 2> /dev/null || return 0
    sleep 0.2
  done
}

declare -A RESULT
status=0
for lib in "${LIBS[@]}"; do
  dir="$HERE/libs/$lib"
  [ -d "$dir" ] || { echo "no such library: $lib" >&2; RESULT[$lib]="FAIL (unknown)"; status=1; continue; }
  want_arch="$(sed -n 's/^arch:[[:space:]]*//p' "$dir/expected.txt")"
  if [ -n "$want_arch" ] && [ "$want_arch" != "$(uname -m)" ]; then
    RESULT[$lib]="not run: the library has no $(uname -m) desktop build"
    continue
  fi
  echo "=== $lib"
  proj="$WORK/$lib"
  rm -rf "$proj"
  mkdir -p "$proj"
  cp -R "$HERE/../../images/seed/." "$proj/"
  sed -i '/image.s read-only Gradle cache/,/^wpi\.sim\.addWebsocketsServer()/d' "$proj/build.gradle"
  mkdir -p "$proj/vendordeps"
  cp "$dir"/*.json "$proj/vendordeps/"
  awk -v f="$dir/Devices.java" '/@DEVICES@/ { while ((getline line < f) > 0) print line; next } { print }' \
    "$HERE/Robot.java.in" > "$proj/src/main/java/frc/robot/Robot.java"
  chmod +x "$proj/gradlew"

  stop_robot
  (cd "$proj" && ./gradlew simulateJava --console=plain > "$proj/sim.log" 2>&1) &
  up=""
  for _ in $(seq 1 180); do
    if (exec 3<> "/dev/tcp/127.0.0.1/$NT_PORT") 2> /dev/null; then up=1; break; fi
    grep -q "BUILD FAILED\|quit unexpectedly" "$proj/sim.log" 2> /dev/null && break
    sleep 1
  done
  if [ -z "$up" ] || grep -q "quit unexpectedly" "$proj/sim.log"; then
    RESULT[$lib]="FAIL (robot did not start; see $proj/sim.log)"
    grep -m 5 -i "error\|exception\|could not" "$proj/sim.log" | sed 's/^/    /'
    status=1
  elif (cd "$HERE" && node check.mjs "$dir/expected.txt"); then
    RESULT[$lib]="ok"
  else
    RESULT[$lib]="FAIL (not in the web GUI)"
    status=1
  fi
  stop_robot
done

echo
echo "vendor matrix ($(uname -m)):"
for lib in "${LIBS[@]}"; do printf '  %-12s %s\n' "$lib" "${RESULT[$lib]}"; done
exit $status
