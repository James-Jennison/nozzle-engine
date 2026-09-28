#!/usr/bin/env bash
# Builds the headless desktop engine (nozzle-engine, Linux x86_64) from THIS fork's working tree.
# Same configuration as Nozzle It All's engine/native/scripts/build_engine_snapmaker.sh, but the source is this repo
# instead of a pinned export + patch. Dependencies: Snapmaker Orca's deps superbuild prefix (read only).
#   heavy-build -- tools/nozzle/build_desktop_engine.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NOZZLE_ROOT="${NOZZLE_ROOT:-/mnt/faststorage/Nozzle It All}"
PREFIX="${SNAPMAKER_DEPS:-/mnt/faststorage/Snapmaker-Orca/OrcaSlicer/deps/build/destdir/usr/local}"
WORK="${ENGINE_WORK:-/mnt/faststorage/build-work/test-slicer-engine}"
BDIR="$WORK/build-desktop"; DIST="$WORK/dist"
BRIDGE="${NOZZLE_BRIDGE:-$ROOT/nozzle/bridge/native}"
JOBS="${HEAVY_BUILD_JOBS:-${JOBS:-6}}"
export PKG_CONFIG_PATH="$PREFIX/lib64/pkgconfig:$PREFIX/lib/pkgconfig"
cmake -S "$ROOT" -B "$BDIR" -GNinja -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PREFIX" \
  -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DSLIC3R_GUI=OFF -DSLIC3R_CAD=OFF -DSLIC3R_STATIC=ON -DSLIC3R_PCH=OFF -DSLIC3R_BUILD_SANDBOXES=OFF -DBUILD_TESTS=OFF \
  -DORCA_TOOLS=OFF -DSLIC3R_SENTRY=OFF -DCMAKE_C_FLAGS=-fPIC -DCMAKE_CXX_FLAGS=-fPIC \
  "-DCMAKE_EXE_LINKER_FLAGS=-static-libstdc++ -static-libgcc" \
  -DBoost_USE_STATIC_LIBS=ON -DBoost_ROOT="$PREFIX" -DTBB_DIR="$PREFIX/lib/cmake/TBB" -DOpenCV_DIR="$PREFIX/lib/cmake/opencv4" \
  -DCGAL_DIR="$PREFIX/lib/cmake/CGAL" -DOPENSSL_ROOT_DIR="$PREFIX" -DOPENSSL_USE_STATIC_LIBS=ON \
  "-DANDROID_JNI_BRIDGE_DIR=$BRIDGE"
cmake --build "$BDIR" -j"$JOBS" --target nozzle-engine
mkdir -p "$DIST"; cp "$BDIR/android_jni/nozzle-engine" "$DIST/"; strip "$DIST/nozzle-engine"
{ echo "nozzle-engine built $(date -u +%FT%TZ) from $(git -C "$ROOT" rev-parse HEAD)$(git -C "$ROOT" diff --quiet || echo ' (dirty)')"
  echo "bridge: $BRIDGE"; echo "deps: $PREFIX"
  (cd "$DIST" && sha256sum nozzle-engine); } | tee "$DIST/PROVENANCE.txt"
