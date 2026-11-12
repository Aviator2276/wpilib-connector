#!/usr/bin/env bash
# Build halsim_webbridge for one WPILib season and package it as a local
# Maven repository that GradleRIO can resolve as a simulation extension.
#
#   bridge/build.sh [season]        (season defaults to 2026)
#
# Output (default bridge/dist):
#   maven/websim/halsim/halsim_webbridge/<version>/
#       halsim_webbridge-<version>-<platform>.zip        (release)
#       halsim_webbridge-<version>-<platform>debug.zip   (same binary)
#       halsim_webbridge-<version>.pom
#
# <platform> is GradleRIO's desktop platform name for the machine the build
# runs on (linuxx86-64 or linuxarm64); the extension is native code, so each
# architecture is built on that architecture. Without a C++ toolchain the
# script re-runs itself in an Ubuntu 22.04 container, old enough that the
# result loads on every supported workspace base image.
#
# Environment: WEBSIM_CACHE (downloads + objects), WEBSIM_BRIDGE_DIST (output)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SEASON="${1:-2026}"
ENV_FILE="$HERE/../wasm/seasons/$SEASON.env"
[ -f "$ENV_FILE" ] || { echo "unknown season '$SEASON'" >&2; exit 1; }
# shellcheck disable=SC1090
source "$ENV_FILE"

CACHE="${WEBSIM_CACHE:-$HERE/.cache}"
DIST="${WEBSIM_BRIDGE_DIST:-$HERE/dist}"
mkdir -p "$CACHE" "$DIST"

if [ "$(uname -s)" != "Linux" ] || ! command -v cmake >/dev/null 2>&1 || ! command -v g++ >/dev/null 2>&1; then
  echo "building in an ubuntu:22.04 container"
  exec docker run --rm \
    -v "$HERE/..:/src:ro" -v "$CACHE:/cache" -v "$DIST:/dist" \
    -e WEBSIM_CACHE=/cache -e WEBSIM_BRIDGE_DIST=/dist \
    ubuntu:22.04 bash -c '
      set -e
      export DEBIAN_FRONTEND=noninteractive
      apt-get update -qq >/dev/null
      apt-get install -y -qq --no-install-recommends g++ cmake make curl unzip zip ca-certificates >/dev/null
      bash /src/bridge/build.sh "$0"' "$SEASON"
fi

case "$(uname -m)" in
  x86_64) PLATFORM=linuxx86-64; ZIP_ARCH=x86-64 ;;
  aarch64 | arm64) PLATFORM=linuxarm64; ZIP_ARCH=arm64 ;;
  *) echo "unsupported architecture $(uname -m)" >&2; exit 1 ;;
esac

MAVEN="${WPILIB_MAVEN:-https://frcmaven.wpi.edu/artifactory/release}"
SDK="$CACHE/wpilib-sdk-$WPILIB_VERSION-$PLATFORM"
BUILD="$CACHE/bridge-build-$WPILIB_VERSION-$PLATFORM"

fetch() { # group-path artifact classifier
  local out="$CACHE/dl/$2-$WPILIB_VERSION-$3.zip"
  mkdir -p "$CACHE/dl"
  if [ ! -s "$out" ]; then
    curl -fsSL --retry 5 --retry-delay 5 --retry-all-errors \
      "$MAVEN/$1/$2/$WPILIB_VERSION/$2-$WPILIB_VERSION-$3.zip" -o "$out.part"
    mv "$out.part" "$out"
  fi
  echo "$out"
}

# --- 1. WPILib headers and shared libraries for this platform ---------------
if [ ! -f "$SDK/.complete" ]; then
  echo "==> fetching WPILib $WPILIB_VERSION ($PLATFORM)"
  rm -rf "$SDK"; mkdir -p "$SDK/include" "$SDK/lib" "$SDK/tmp"
  for spec in edu/wpi/first/hal:hal-cpp edu/wpi/first/wpiutil:wpiutil-cpp edu/wpi/first/wpinet:wpinet-cpp; do
    group="${spec%%:*}"; artifact="${spec##*:}"
    unzip -qo "$(fetch "$group" "$artifact" headers)" -d "$SDK/include"
    unzip -qo "$(fetch "$group" "$artifact" "$PLATFORM")" -d "$SDK/tmp/$artifact"
    find "$SDK/tmp/$artifact" -path "*/shared/*.so" ! -name "*jni*" -exec cp {} "$SDK/lib/" \;
  done
  rm -rf "$SDK/tmp"
  for lib in wpiHal wpinet wpiutil; do
    [ -f "$SDK/lib/lib$lib.so" ] || { echo "lib$lib.so not found in WPILib artifacts" >&2; exit 1; }
  done
  touch "$SDK/.complete"
fi

# --- 2. compile ---------------------------------------------------------------
echo "==> compiling halsim_webbridge"
cmake -S "$HERE" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
  -DWPILIB_SDK="$SDK" -DWPILIB_VERSION="$WPILIB_VERSION" >/dev/null
cmake --build "$BUILD" -j "$(nproc)"

# --- 3. package as GradleRIO expects a sim extension -------------------------
GROUP_PATH=websim/halsim
ART=halsim_webbridge
OUT="$DIST/maven/$GROUP_PATH/$ART/$WPILIB_VERSION"
STAGE="$BUILD/stage/linux/$ZIP_ARCH/shared"
rm -rf "$BUILD/stage"; mkdir -p "$STAGE" "$OUT"
cp "$BUILD/libhalsim_webbridge.so" "$STAGE/"
strip --strip-unneeded "$STAGE/libhalsim_webbridge.so"
cp "$HERE/../LICENSE" "$BUILD/stage/LICENSE.txt" 2>/dev/null || true
for classifier in "$PLATFORM" "${PLATFORM}debug"; do
  rm -f "$OUT/$ART-$WPILIB_VERSION-$classifier.zip"
  (cd "$BUILD/stage" && zip -qr "$OUT/$ART-$WPILIB_VERSION-$classifier.zip" .)
done
cat > "$OUT/$ART-$WPILIB_VERSION.pom" <<POM
<?xml version="1.0" encoding="UTF-8"?>
<project xmlns="http://maven.apache.org/POM/4.0.0">
  <modelVersion>4.0.0</modelVersion>
  <groupId>websim.halsim</groupId>
  <artifactId>$ART</artifactId>
  <version>$WPILIB_VERSION</version>
  <packaging>pom</packaging>
  <name>WPILib Web Sim HAL bridge</name>
  <licenses><license><name>MIT</name></license></licenses>
</project>
POM

echo "==> $OUT"
ls -l "$OUT" | sed 's/^/    /'
