#!/usr/bin/env bash
# Assemble a runnable GradleRIO project from the stock template in
# images/seed plus one of the test robots here.
#
#   tests/robots/make-project.sh <robot> <destination>
#
#   <robot>   vendor   every Sim GUI device type, Phoenix 6, REVLib, Studica
#                      navX and a CameraServer stream; runs on x86-64 and arm64
#             photon   adds a PhotonVision simulated camera; x86-64 only
#
# The project is the unmodified template: nothing in it mentions the web sim.
# What makes it feed the web GUI is the init script in ~/.gradle/init.d.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROBOT="${1:?robot name (vendor | photon)}"
DEST="${2:?destination directory}"
[ -f "$HERE/$ROBOT/Robot.java" ] || { echo "no such robot: $ROBOT" >&2; exit 1; }

mkdir -p "$DEST"
cp -R "$HERE/../../images/seed/." "$DEST/"
rm -rf "$DEST/build" "$DEST/.gradle"
# the template as a student gets it: without the image-cache-only extra line
sed -i.bak '/image.s read-only Gradle cache/,/^wpi\.sim\.addWebsocketsServer()/d' "$DEST/build.gradle"
rm -f "$DEST/build.gradle.bak"
cp "$HERE/$ROBOT/Robot.java" "$DEST/src/main/java/frc/robot/Robot.java"
mkdir -p "$DEST/vendordeps"
cp "$HERE/$ROBOT/vendordeps/"*.json "$DEST/vendordeps/"
chmod +x "$DEST/gradlew"
echo "$ROBOT robot project written to $DEST"
