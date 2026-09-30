#!/usr/bin/env bash
# Proves the Android bridge's new colour-mixing entry points (nativeFullSpectrum/nativeColorMix/
# nativeSliceMultiObjectMix/nativeSlicePaintSessionMix, added to nozzle/bridge/android/slic3r_jni.cpp) return exactly
# what the desktop nozzle-engine CLI does for the same request, without needing an Android device/emulator (adb is
# off-limits for this work): the Android bridge and the desktop CLI both call the exact same C++ functions
# (nozzle_fs::run_full_spectrum, nozzle_cm::run_color_mix, engine::slice_multi_object), so this drives those same
# functions through slic3r_cli_test - a plain host executable built from the shared cli_test.cpp the Android build
# also compiles on-device (see nozzle/bridge/native/CMakeLists.txt's slic3r_cli_test target and
# nozzle/bridge/android/CMakeLists.txt's own copy) - and diffs its output against nozzle-engine's.
#
# Usage: colourmix_test.sh <nozzle-engine> <slic3r_cli_test>
set -uo pipefail
NOZZLE_ENGINE="$1"
CLI_TEST="$2"
NOZZLE_ROOT="${NOZZLE_ROOT:-/mnt/faststorage/Nozzle It All}"
PACK="$NOZZLE_ROOT/app/src/main/assets/slicer_profiles/snapmaker_u1"
CUBE="$NOZZLE_ROOT/site-src/assets/test-cube-20mm.stl"
JOB="$(mktemp -d)"; trap 'rm -rf "$JOB"' EXIT
FAIL=0

fail() { echo "FAIL: $1"; FAIL=1; }
pass() { echo "PASS: $1"; }

# --- 1. Full Spectrum: same request, same response, through both entry points ------------------------------------
cat > "$JOB/fs_request.json" <<'EOF'
{"op":"display","physical":["#ff0000","#00ff00"]}
EOF
"$NOZZLE_ENGINE" --full-spectrum "$JOB/fs_request.json" > "$JOB/fs_cli.json" 2>"$JOB/fs_cli.err"; fs_cli_code=$?
"$CLI_TEST" --full-spectrum "$JOB/fs_request.json" > "$JOB/fs_bridge.json" 2>"$JOB/fs_bridge.err"; fs_bridge_code=$?
if [ "$fs_cli_code" != "$fs_bridge_code" ]; then
    fail "full spectrum exit code: CLI=$fs_cli_code bridge=$fs_bridge_code"
elif ! diff -q "$JOB/fs_cli.json" "$JOB/fs_bridge.json" >/dev/null; then
    fail "full spectrum response differs (see $JOB/fs_cli.json vs $JOB/fs_bridge.json)"
elif [ ! -s "$JOB/fs_cli.json" ]; then
    fail "full spectrum response was empty"
else
    pass "full spectrum: bridge matches CLI (exit $fs_cli_code)"
fi

# --- 2. ColorMix: same request, same response, through both entry points ------------------------------------------
cat > "$JOB/cm_request.json" <<'EOF'
{"op":"mix","colors":["#ff0000","#00ff00"],"ratios":[0.5,0.5]}
EOF
"$NOZZLE_ENGINE" --color-mix "$JOB/cm_request.json" > "$JOB/cm_cli.json" 2>"$JOB/cm_cli.err"; cm_cli_code=$?
"$CLI_TEST" --color-mix "$JOB/cm_request.json" > "$JOB/cm_bridge.json" 2>"$JOB/cm_bridge.err"; cm_bridge_code=$?
if [ "$cm_cli_code" != "$cm_bridge_code" ]; then
    fail "color mix exit code: CLI=$cm_cli_code bridge=$cm_bridge_code"
elif ! diff -q "$JOB/cm_cli.json" "$JOB/cm_bridge.json" >/dev/null; then
    fail "color mix response differs (see $JOB/cm_cli.json vs $JOB/cm_bridge.json)"
elif [ ! -s "$JOB/cm_cli.json" ]; then
    fail "color mix response was empty"
else
    pass "color mix: bridge matches CLI (exit $cm_cli_code)"
fi

# --- 3. A real 50/50 two-slot blend, sliced through the same virtual-extruders path the Android bridge's -----------
#        nativeSliceMultiObjectMix takes (engine::slice_multi_object(..., virtual_extruders_json)), proving the
#        resulting G-code's tool changes alternate layer by layer. Uses the bundled Snapmaker U1 profile pack and the
#        real 4-slot filament_diameter/filament_colour/filament_type/nozzle_temperature* override recipe verified in
#        Nozzle It All's ToolAssignmentSlicingDeviceTest.kt (a single real filament_diameter entry in the bundled
#        filament.json otherwise clamps every tool index back to 1 - see that file's own header comment).
#        Virtual id 3 blends physical extruders 1 and 2 (50/50): PrintObject.cpp's layer-cycle math (build_layer_cycle,
#        ~line 3723) sets each component's cycle height from its ratio, so an equal 50/50 split at a 0.2mm layer
#        height alternates the two physical tools every single layer.
if [ ! -f "$PACK/machine.json" ] || [ ! -f "$CUBE" ]; then
    fail "cannot find Snapmaker U1 profile pack / test cube under NOZZLE_ROOT=$NOZZLE_ROOT (skipping slice test)"
else
    cat > "$JOB/virtual_extruders.json" <<'EOF'
{"version":1,"virtual_extruders":[{"id":3,"kind":"fullspectrum","components":[{"extruder":1,"ratio":0.5},{"extruder":2,"ratio":0.5}]}]}
EOF
    {
        printf 'out\t%s\n' "$JOB/blend.gcode"
        printf 'profile\t%s\n' "$PACK/machine.json" "$PACK/process.json" "$PACK/filament.json"
        printf 'set\tlayer_height\t0.2\n'
        printf 'set\tfilament_diameter\t1.75,1.75,1.75,1.75\n'
        printf 'set\tfilament_colour\t#FF0000;#00FF00;#0000FF;#FFFF00\n'
        printf 'set\tfilament_type\tPLA;PLA;PLA;PLA\n'
        printf 'set\tnozzle_temperature\t210,210,210,210\n'
        printf 'set\tnozzle_temperature_initial_layer\t210,210,210,210\n'
        printf 'object\t%s\t0\t0\t0\t1\t3\n' "$CUBE"
        printf 'virtual_extruders\t%s\n' "$JOB/virtual_extruders.json"
    } > "$JOB/blend_request.txt"

    "$NOZZLE_ENGINE" "$JOB/blend_request.txt" >"$JOB/blend_cli.log" 2>&1; blend_cli_code=$?
    cli_gcode="$JOB/blend.gcode"
    if [ "$blend_cli_code" != 0 ] || [ ! -s "$cli_gcode" ]; then
        fail "desktop CLI failed to slice the 50/50 blend (exit $blend_cli_code); see $JOB/blend_cli.log"
    else
        tool_sequence_cli="$(grep -oE '^T[0-9]+' "$cli_gcode" | uniq)"
        distinct_tools_cli="$(echo "$tool_sequence_cli" | sort -u | wc -l)"
        rm -f "$cli_gcode"

        "$CLI_TEST" --slice "$JOB/blend_request.txt" >"$JOB/blend_bridge.log" 2>&1; blend_bridge_code=$?
        if [ "$blend_bridge_code" != 0 ] || [ ! -s "$cli_gcode" ]; then
            fail "bridge (slic3r_cli_test) failed to slice the 50/50 blend (exit $blend_bridge_code); see $JOB/blend_bridge.log"
        else
            tool_sequence_bridge="$(grep -oE '^T[0-9]+' "$cli_gcode" | uniq)"
            if [ "$tool_sequence_cli" != "$tool_sequence_bridge" ]; then
                fail "blend G-code tool-change sequence differs between CLI and bridge"
            elif [ "$distinct_tools_cli" -lt 2 ]; then
                fail "expected at least 2 distinct tools in the blend's tool-change sequence, got: $tool_sequence_cli"
            else
                # "Alternate layer by layer": a 50/50 blend's layer cycle is A,B,A,B..., so the tool changes (runs of
                # the same tool collapsed by `uniq` above) must come about once per layer, not just once or twice.
                layers="$(grep -c '^;LAYER_CHANGE' "$cli_gcode")"
                changes="$(( $(echo "$tool_sequence_cli" | wc -l) - 1 ))"
                if [ "$layers" -ge 10 ] && [ "$changes" -ge $(( layers * 8 / 10 )) ]; then
                    pass "50/50 blend: CLI and bridge agree, $changes tool changes over $layers layers"
                else
                    fail "tool changes did not alternate layer by layer: $changes changes over $layers layers"
                fi
            fi
        fi
    fi
fi

if [ "$FAIL" != 0 ]; then
    echo "colourmix_test: one or more checks failed"
    exit 1
fi
echo "colourmix_test: all checks passed"
