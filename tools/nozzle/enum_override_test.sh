#!/usr/bin/env bash
# Setting an enum-vector option by name (an override or a G-code config block's "extruder_type = Direct Drive") must not crash.
# These three crashed the engine until Config.cpp gave a default-created enum vector its name table (2026-10-17).
# Usage: enum_override_test.sh ENGINE PROFILE_DIR CUBE_STL
set -euo pipefail
ENGINE="$1"; P="$2"; CUBE="$3"; JOB="$(mktemp -d)"; trap 'rm -rf "$JOB"' EXIT
printf 'out\t%s\nprofile\t%s\nprofile\t%s\nprofile\t%s\nset\textruder_type\tDirect Drive\nset\tnozzle_volume_type\tStandard\nset\tretract_lift_enforce\tAll Surfaces\nobject\t%s\t0\t0\t0\t1\t0\n' \
  "$JOB/o.gcode" "$P/machine.json" "$P/process.json" "$P/filament.json" "$CUBE" > "$JOB/r.txt"
"$ENGINE" "$JOB/r.txt" > /dev/null 2>&1 || { echo "FAIL: enum overrides crashed or failed (exit $?)"; exit 1; }
grep -q "total layer number" "$JOB/o.gcode" && echo "PASS: enum-vector overrides by name slice"
