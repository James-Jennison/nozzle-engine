#!/usr/bin/env bash
# The prime tower prints on the bed. libslic3r leaves the tower at its default corner (wipe_tower_x / wipe_tower_y 15,
# 220) unless the caller places it; upstream's GUI does that, and the bridge does it for headless slices (slic3r_engine.cpp:
# place_prime_tower before slicing, prime_tower_overrun once the tower's real footprint is known). On a real Elegoo
# Centauri Carbon (256 mm bed) a four-filament slice at 0.1 mm layers put the tower's brim at Y 258.8 (2026-10-01).
#
# Each case slices 20 mm cubes, one per filament, and checks every extruding move against the G-code's printable_area
# (tools/nozzle/within_bed.py):
#   1. Centauri Carbon, four filaments, 0.1 mm layers: the case above, on both of its CANVAS packs. The Elegoo firmware
#      pack sets no position, so its tower starts at upstream OrcaSlicer's corner (back, right of the middle), as in
#      ElegooSlicer, and is moved in once its real depth is known. The COSMOS pack may set its own position (Nozzle
#      It All moved it away from COSMOS's purge tray), so only the bed check applies to it.
#   2. Snapmaker U1, four filaments, 0.1 mm layers: Snapmaker Orca's corner (back left) for Snapmaker's printers; the
#      generated tower is deeper than the estimate, so it is moved.
#   3. Creality Ender-3 (220 mm bed slinger), two filaments: the tower starts at the bed slinger corner and its skirt,
#      which goes around the tower, stays on the bed too.
#   4. A position the caller sets is kept.
#   5. Five and six filaments with no flushing volumes of their own: the default flush_volumes_matrix is 4 x 4, which the
#      tower's tool-change tables were read past (a crash, or a slice that never finished), so the bridge grows it.
#
# Usage: prime_tower_test.sh <nozzle-engine>
set -uo pipefail
ENGINE="$1"
NOZZLE_ROOT="${NOZZLE_ROOT:-/mnt/faststorage/Nozzle It All}"
PACKS="$NOZZLE_ROOT/app/src/main/assets/slicer_profiles"; CUBE="$NOZZLE_ROOT/site-src/assets/test-cube-20mm.stl"
CHECK="$(dirname "${BASH_SOURCE[0]}")/within_bed.py"
JOB="$(mktemp -d)"; trap 'rm -rf "$JOB"' EXIT
FAIL=0

fail() { echo "FAIL: $1"; FAIL=1; }
pass() { echo "PASS: $1"; }
repeat() { local i out=""; for ((i = 0; i < $2; i++)); do out+="${out:+$3}$1"; done; printf '%s' "$out"; }

# slice <name> <pack> <filaments> [set line...]: writes $JOB/<name>.gcode
slice() {
    local name="$1" pack="$PACKS/$2" n="$3" i; shift 3
    {
        printf 'out\t%s\n' "$JOB/$name.gcode"
        printf 'profile\t%s\n' "$pack/machine.json" "$pack/process.json" "$pack/filament.json"
        printf 'set\tfilament_diameter\t%s\n' "$(repeat 1.75 "$n" ,)"
        printf 'set\tfilament_colour\t%s\n' "$(repeat '#FF0000' "$n" ';')"
        printf 'set\tfilament_type\t%s\n' "$(repeat PLA "$n" ';')"
        printf 'set\tnozzle_temperature\t%s\n' "$(repeat 210 "$n" ,)"
        printf 'set\tnozzle_temperature_initial_layer\t%s\n' "$(repeat 210 "$n" ,)"
        for i in "$@"; do printf 'set\t%s\n' "$i"; done
        for ((i = 1; i <= n; i++)); do printf 'object\t%s\t%s\t0\t0\t1\t%s\n' "$CUBE" $(( (i - 1) * 30 - (n - 1) * 15 )) "$i"; done
    } > "$JOB/$name.txt"
    timeout 600 "$ENGINE" "$JOB/$name.txt" > "$JOB/$name.log" 2>&1
}

# on_bed <name> <what>: the slice succeeded and prints inside the printable area
on_bed() {
    if [ ! -s "$JOB/$1.gcode" ]; then
        fail "$2: the slice failed or did not finish: $(grep -v -E '^\[|^progress' "$JOB/$1.log" | tail -1)"
    elif ! python3 "$CHECK" "$JOB/$1.gcode" > "$JOB/$1.check" 2>&1; then
        fail "$2: $(cat "$JOB/$1.check")"
    else
        pass "$2: $(cat "$JOB/$1.check")"
    fi
}

# corner <name> <x> <what>: the tower starts at that X (the vendor's default corner)
corner() {
    [ -s "$JOB/$1.gcode" ] || return
    local at; at="$(setting "$1" wipe_tower_x)"
    if [ "$at" = "$2" ] || [ "$at" = "$2.000" ]; then pass "$3 (wipe_tower_x $at)"; else fail "$3: wipe_tower_x is $at, expected $2"; fi
}

setting() { grep -m1 -E "^; $2 = " "$JOB/$1.gcode" | sed -E 's/^; [a-z_]+ = //'; }

if [ ! -f "$CUBE" ]; then
    fail "cannot find the test cube under NOZZLE_ROOT=$NOZZLE_ROOT"
else
    slice cc1 elegoo_centauri_carbon_cosmos_afc 4 $'layer_height\t0.1'
    on_bed cc1 "Centauri Carbon (COSMOS pack), four filaments at 0.1 mm"

    slice stock elegoo_centauri_carbon_canvas 4 $'layer_height\t0.1'
    on_bed stock "Centauri Carbon (Elegoo firmware pack), four filaments at 0.1 mm"
    corner stock 165 "Centauri Carbon (Elegoo firmware pack): upstream's corner, back and right of the middle"

    slice u1 snapmaker_u1 4 $'layer_height\t0.1'
    on_bed u1 "Snapmaker U1, four filaments at 0.1 mm"
    corner u1 13 "Snapmaker U1: Snapmaker's corner, back left"

    slice ender3 creality_ender_3 2
    on_bed ender3 "Ender-3, two filaments and a skirt"

    slice placed elegoo_centauri_carbon_cosmos_afc 4 $'layer_height\t0.1' $'wipe_tower_x\t150' $'wipe_tower_y\t30'
    on_bed placed "Centauri Carbon, tower placed by the caller"
    if [ -s "$JOB/placed.gcode" ]; then
        at="$(setting placed wipe_tower_x) $(setting placed wipe_tower_y)"
        if [ "$at" = "150 30" ] || [ "$at" = "150.000 30.000" ]; then pass "the caller's tower position is kept ($at)"; else fail "the caller's tower position (150, 30) became $at"; fi
    fi

    slice five elegoo_centauri_carbon_cosmos_afc 5
    on_bed five "Centauri Carbon, five filaments and the default flushing volumes"
    slice six prusa_mk4s_mmu3 6 $'layer_height\t0.1'
    on_bed six "Prusa MK4S MMU3, six filaments at 0.1 mm and the default flushing volumes"
    if [ -s "$JOB/six.gcode" ]; then
        pairs="$(setting six flush_volumes_matrix | tr ',' '\n' | wc -l)"
        if [ "$pairs" = 36 ]; then pass "six filaments get a 6 x 6 flushing matrix"; else fail "six filaments have $pairs flushing volumes, expected 36"; fi
    fi
fi

if [ "$FAIL" != 0 ]; then
    echo "prime_tower_test: one or more checks failed"
    exit 1
fi
echo "prime_tower_test: all checks passed"
