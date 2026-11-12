#!/usr/bin/env bash
# Build the Sim GUI WebAssembly bundle for one WPILib season.
#
#   wasm/build.sh [season]        (season defaults to 2026)
#
# Steps: fetch allwpilib at the season's pinned tag, apply the season's patch
# set (a patch that no longer applies fails the build), compile with
# Emscripten, and write dist/<wpilib-version>/{simgui.js,simgui.wasm,fields/}.
#
# Runs inside the emscripten/emsdk image; when emcc is not on PATH the script
# re-runs itself in Docker.
#
# Environment:
#   WEBSIM_CACHE  where the allwpilib checkout and object files live
#                 (default: wasm/.cache)
#   WEBSIM_DIST   output directory (default: wasm/dist)
#   WEBSIM_JOBS   parallel compile jobs (default: all cores)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SEASON="${1:-2026}"
ENV_FILE="$HERE/seasons/$SEASON.env"
PATCH_DIR="$HERE/patches/$SEASON"
[ -f "$ENV_FILE" ] || { echo "unknown season '$SEASON' (no $ENV_FILE)" >&2; exit 1; }
[ -d "$PATCH_DIR" ] || { echo "no patch set at $PATCH_DIR" >&2; exit 1; }
# shellcheck disable=SC1090
source "$ENV_FILE"

CACHE="${WEBSIM_CACHE:-$HERE/.cache}"
DIST="${WEBSIM_DIST:-$HERE/dist}"
mkdir -p "$CACHE" "$DIST"

if ! command -v emcc >/dev/null 2>&1; then
  image="emscripten/emsdk:$EMSDK_VERSION"
  case "$(uname -m)" in arm64 | aarch64) image="$image-arm64" ;; esac
  echo "emcc not found; building in $image"
  exec docker run --rm \
    -v "$HERE:/src/wasm:ro" -v "$HERE/../protocol:/src/protocol:ro" \
    -v "$CACHE:/cache" -v "$DIST:/dist" \
    -e WEBSIM_CACHE=/cache -e WEBSIM_DIST=/dist -e WEBSIM_JOBS="${WEBSIM_JOBS:-}" \
    "$image" bash /src/wasm/build.sh "$SEASON"
fi

SRC="$CACHE/allwpilib-$ALLWPILIB_TAG"
BUILD="$CACHE/build-$SEASON"
OUT="$DIST/$WPILIB_VERSION"
JOBS="${WEBSIM_JOBS:-$(nproc)}"

# --- 1. pristine checkout at the pinned tag -------------------------------
git config --global --add safe.directory "$SRC" 2>/dev/null || true
if [ ! -d "$SRC/.git" ]; then
  echo "==> cloning allwpilib $ALLWPILIB_TAG"
  git clone --quiet --depth 1 --branch "$ALLWPILIB_TAG" -c advice.detachedHead=false \
    "$ALLWPILIB_REPO" "$SRC"
fi
git -C "$SRC" reset --quiet --hard "$ALLWPILIB_TAG"
git -C "$SRC" clean --quiet -fdx

# --- 2. patch set ----------------------------------------------------------
echo "==> applying patches/$SEASON"
shopt -s nullglob
for p in "$PATCH_DIR"/*.patch; do
  git -C "$SRC" apply --check "$p" || { echo "patch does not apply: $(basename "$p")" >&2; exit 1; }
  git -C "$SRC" apply "$p"
  echo "    $(basename "$p")"
done
while IFS= read -r line; do
  line="${line%%#*}"; line="$(echo "$line" | xargs)"
  [ -n "$line" ] || continue
  [ -e "$SRC/$line" ] || { echo "remove.txt: $line no longer exists upstream" >&2; exit 1; }
  rm -rf "${SRC:?}/$line"
done < "$PATCH_DIR/remove.txt"
cp -R "$PATCH_DIR/overlay/." "$SRC/"

# --- 3. compile --------------------------------------------------------------
echo "==> compiling ($JOBS jobs)"
emcmake cmake -S "$HERE" -B "$BUILD" \
  -DCMAKE_BUILD_TYPE="${WEBSIM_BUILD_TYPE:-MinSizeRel}" \
  -DALLWPILIB_DIR="$SRC" -DWPILIB_VERSION="$WPILIB_VERSION" >/dev/null
cmake --build "$BUILD" -j "$JOBS"

# --- 4. bundle ---------------------------------------------------------------
rm -rf "$OUT"; mkdir -p "$OUT/fields"
cp "$BUILD/simgui.js" "$BUILD/simgui.wasm" "$OUT/"

# Built-in field images are served, not embedded (9.7 MB of resources).
FIELDS="$SRC/fieldImages/src/main/native/resources/edu/wpi/first/fields"
cp "$FIELDS"/* "$OUT/fields/"
python3 "$HERE/tools/fields_index.py" "$SRC/fieldImages/src/main/native/cpp/fields.cpp" \
  "$OUT/fields" > "$OUT/fields/index.json"

# Licences for everything compiled into the module travel with it.
python3 "$HERE/tools/collect_licenses.py" "$SRC" > "$OUT/THIRD-PARTY-NOTICES.txt"

printf '{"wpilibVersion":"%s","allwpilibTag":"%s","emsdk":"%s"}\n' \
  "$WPILIB_VERSION" "$ALLWPILIB_TAG" "$EMSDK_VERSION" > "$OUT/bundle.json"

echo "==> $OUT"
ls -l "$OUT" | sed 's/^/    /'
gzip -9 -c "$OUT/simgui.wasm" | wc -c | awk '{printf "    simgui.wasm gzip: %.2f MB\n", $1/1048576}'
