#!/usr/bin/env bash
# Capture the *native* WPILib Sim GUI for a robot project, as the reference
# the web GUI is compared against.
#
#   tests/parity/native.sh <robot project dir> <out.png>
#
# Runs the project's simulation with the web sim switched off
# (WPILIB_WEBSIM=0, so the stock Sim GUI extension loads) on a virtual
# 1280x720 display with software OpenGL, waits for the window, and saves a
# screenshot. The robot is stopped afterwards.
#
# Needs: xvfb, Mesa (libgl1 libglx-mesa0 libgl1-mesa-dri), the X client
# libraries GLFW loads, and ImageMagick. On Debian/Ubuntu:
#   apt-get install xvfb libgl1 libglx-mesa0 libgl1-mesa-dri libx11-6 libxext6 \
#     libxrender1 libxrandr2 libxcursor1 libxinerama1 libxi6 libxxf86vm1 \
#     libxkbcommon0 imagemagick
set -euo pipefail

PROJECT="${1:?robot project directory}"
OUT="${2:?output png}"
WAIT="${PARITY_WAIT:-25}"
DISPLAY_NUM="${PARITY_DISPLAY:-97}"

export DISPLAY=":$DISPLAY_NUM"
export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe WPILIB_WEBSIM=0

Xvfb "$DISPLAY" -screen 0 1280x720x24 -nolisten tcp > /dev/null 2>&1 &
XVFB=$!
cleanup() {
  # Killing the Gradle client does not stop the robot JVM; stop that too.
  pkill -f "[D]java.library.path=$PROJECT" 2> /dev/null || true
  kill "$XVFB" 2> /dev/null || true
}
trap cleanup EXIT
sleep 1

# a clean default layout, like the web capture
rm -f "$PROJECT"/simgui*.json
(cd "$PROJECT" && ./gradlew simulateJava --console=plain > "${OUT%.png}.log" 2>&1) &

for _ in $(seq 1 120); do
  grep -q "Simulator GUI Initialized" "${OUT%.png}.log" 2> /dev/null && break
  sleep 1
done
grep -q "Simulator GUI Initialized" "${OUT%.png}.log" || { echo "native GUI did not start; see ${OUT%.png}.log" >&2; exit 1; }
sleep "$WAIT"
# -strip: ImageMagick adds gamma and chromaticity chunks, which make a
# browser colour-manage the reference and shift every pixel by a few levels.
import -window root -strip "PNG24:$OUT"
echo "native screenshot: $OUT"
