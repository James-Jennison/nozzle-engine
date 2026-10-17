#!/usr/bin/env bash
# Builds the browser engine (nozzle-engine.js/.wasm) from this fork with Nozzle It All's emsdk environment and browser
# dependency prefix (engine/wasm/scripts/env.sh, read only) and the same CMake arguments as its build_engine_snapmaker.sh.
#   heavy-build -- tools/nozzle/build_wasm_engine.sh
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NOZZLE_ROOT="${NOZZLE_ROOT:-/mnt/faststorage/Nozzle It All}"
source "$NOZZLE_ROOT/engine/wasm/scripts/env.sh"
WORK="${ENGINE_WORK:-/mnt/faststorage/build-work/test-slicer-engine}"; SRC="$WORK/wasm-src"; BDIR="$WORK/build-wasm"
"$HERE/export_source.sh" "$SRC"
emcmake cmake -S "$SRC" -B "$BDIR" -GNinja "${CMAKE_COMMON_ARGS[@]}" \
  -DSLIC3R_GUI=OFF -DSLIC3R_CAD=OFF -DSLIC3R_STATIC=ON -DSLIC3R_PCH=OFF -DSLIC3R_BUILD_SANDBOXES=OFF -DBUILD_TESTS=OFF -DORCA_TOOLS=OFF \
  -DSLIC3R_SENTRY=OFF -DUSE_BLOSC=OFF \
  -DCMAKE_CXX_FLAGS="$NOZZLE_WASM_FLAGS -Wno-error=missing-template-arg-list-after-template-kw" \
  -DCMAKE_EXE_LINKER_FLAGS="$LDFLAGS -L$PREFIX/lib" \
  -DBoost_USE_STATIC_LIBS=ON -DBoost_USE_STATIC_RUNTIME=ON -DBoost_ROOT="$PREFIX" -DTBB_DIR="$PREFIX/lib/cmake/TBB" -DOpenCV_DIR="$PREFIX/lib/cmake/opencv4" \
  -DCGAL_DIR="$PREFIX/lib/cmake/CGAL" -DZLIB_ROOT="$PREFIX" -DEXPAT_ROOT="$PREFIX" -DPNG_ROOT="$PREFIX" -DJPEG_ROOT="$PREFIX" \
  -DANDROID_JNI_BRIDGE_DIR="$SRC/nozzle/bridge/wasm" >/dev/null
cmake --build "$BDIR" -j"$JOBS" --target nozzle-engine
mkdir -p "$WORK/dist-wasm"; cp "$BDIR"/android_jni/nozzle-engine.js "$BDIR"/android_jni/nozzle-engine.wasm "$WORK/dist-wasm/"
(cd "$WORK/dist-wasm" && sha256sum nozzle-engine.js nozzle-engine.wasm)
