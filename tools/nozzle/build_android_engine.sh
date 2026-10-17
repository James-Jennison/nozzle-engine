#!/usr/bin/env bash
# Builds Nozzle It All's Android engine library (libslic3rengine.so, arm64-v8a) from this fork, outside Gradle, the way
# build-work/nozzle-android-sm/standalone.sh does: Nozzle's app/src/main/cpp CMake project over an engine root whose
# orcaslicer/ is this fork's export. Dependencies: Nozzle's prepared Android prefix (GMP with C++ classes), linked, read only.
# Nozzle's repository is only read.   heavy-build -- tools/nozzle/build_android_engine.sh
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NOZZLE_ROOT="${NOZZLE_ROOT:-/mnt/faststorage/Nozzle It All}"
WORK="${ENGINE_WORK:-/mnt/faststorage/build-work/test-slicer-engine}"; ER="$WORK/android-root"; B="$WORK/build-android"
PREPARED_DEPS="${ANDROID_PREPARED_DEPS:-/mnt/faststorage/build-work/nozzle-android-sm/deps}"
SDK="${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}"; NDK="${ANDROID_NDK_ROOT:-$SDK/ndk/29.0.14206865}"; SDKCM="${ANDROID_CMAKE_BIN:-$SDK/cmake/3.22.1/bin}"
"$HERE/export_source.sh" "$ER/orcaslicer"
[ -e "$ER/deps" ] || ln -s "$PREPARED_DEPS" "$ER/deps"
"$SDKCM/cmake" -H"$NOZZLE_ROOT/app/src/main/cpp" -B"$B" -GNinja -DCMAKE_MAKE_PROGRAM="$SDKCM/ninja" \
  -DCMAKE_SYSTEM_NAME=Android -DCMAKE_SYSTEM_VERSION=28 -DANDROID_PLATFORM=android-28 -DANDROID_ABI=arm64-v8a -DCMAKE_ANDROID_ARCH_ABI=arm64-v8a \
  -DANDROID_NDK="$NDK" -DCMAKE_ANDROID_NDK="$NDK" -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release -DORCASLICER_ENGINE_ROOT="$ER" >/dev/null
"$SDKCM/ninja" -C "$B" -j"${HEAVY_BUILD_JOBS:-6}" slic3rengine
mkdir -p "$WORK/dist-android"; cp "$B/android_jni/libslic3rengine.so" "$WORK/dist-android/"
"$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" -o "$WORK/dist-android/libslic3rengine.stripped.so" "$WORK/dist-android/libslic3rengine.so"
(cd "$WORK/dist-android" && sha256sum libslic3rengine.so libslic3rengine.stripped.so)
