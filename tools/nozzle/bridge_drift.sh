#!/usr/bin/env bash
# Compares this fork's bridge copies (nozzle/bridge) with Nozzle It All's working tree; exits 1 on any drift.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"; N="${NOZZLE_ROOT:-/mnt/faststorage/Nozzle It All}"; rc=0
cmpdir(){ diff -r -q -x CMakeLists.txt -x README.md -x LICENSE.txt "$1" "$2" || rc=1; }
cmpdir "$ROOT/nozzle/bridge/android" "$N/app/src/main/cpp/bridge"
cmpdir "$ROOT/nozzle/bridge/native" "$N/engine/native/bridge"
cmpdir "$ROOT/nozzle/bridge/wasm" "$N/engine/wasm/bridge"
for d in native wasm; do
  src=$([ $d = native ] && echo "$N/engine/native/bridge" || echo "$N/engine/wasm/bridge")
  diff -q <(sed 's|/../../../app/src/main/cpp/bridge|/../android|' "$src/CMakeLists.txt") "$ROOT/nozzle/bridge/$d/CMakeLists.txt" >/dev/null || { echo "CMakeLists differs: $d"; rc=1; }
done
diff -q "$N/app/src/main/cpp/bridge/CMakeLists.txt" "$ROOT/nozzle/bridge/android/CMakeLists.txt" >/dev/null || { echo "CMakeLists differs: android"; rc=1; }
[ $rc = 0 ] && echo "bridges match Nozzle It All $(git -C "$N" rev-parse --short HEAD)"; exit $rc
