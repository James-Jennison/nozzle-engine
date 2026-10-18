#!/usr/bin/env bash
# Mixed filaments on Orca 2.5.0's model: the --mixed-filament ops answer as documented, and a slice with a 3:1 mixed slot
# prints about three quarters of its extrusion with the first component.
# Usage: mixed_filament_test.sh <nozzle-engine>     (NOZZLE_ROOT = the Nozzle It All checkout, for the U1 profile pack and cube)
set -uo pipefail
ENGINE="$1"
NOZZLE_ROOT="${NOZZLE_ROOT:-/mnt/faststorage/Nozzle It All}"
PACK="$NOZZLE_ROOT/app/src/main/assets/slicer_profiles/snapmaker_u1"; CUBE="$NOZZLE_ROOT/site-src/assets/test-cube-20mm.stl"
JOB="$(mktemp -d)"; trap 'rm -rf "$JOB"' EXIT; FAIL=0
fail() { echo "FAIL: $1"; FAIL=1; }; pass() { echo "PASS: $1"; }
P='"physical":[{"color":"#FF0000","type":"PLA"},{"color":"#0000FF","type":"PLA"},{"color":"#FFFF00","type":"PLA"},{"color":"#FFFFFF","type":"PLA"}]'
op() { printf '%s' "$1" > "$JOB/r.json"; "$ENGINE" --mixed-filament "$JOB/r.json"; }
j() { python3 -c "import json,sys; d=json.load(sys.stdin); print($1)"; }

[ "$(op "{\"op\":\"add\",$P,\"mixed\":[],\"components\":[1,2],\"weights\":[7,3]}" | j 'd["mixed"][0]["ratios_text"]')" = "0.7000,0.3000" ] && pass "add stores normalised ratios" || fail "add"
[ "$(op "{\"op\":\"add\",$P,\"mixed\":[{\"components\":[1,2],\"weights\":[7,3]}],\"components\":[1,2],\"weights\":[7,3]}" | j 'd["added"]')" = "False" ] && pass "adding an existing blend is reported, not duplicated" || fail "dedupe"
op "{\"op\":\"add\",$P,\"mixed\":[],\"components\":[1,1],\"weights\":[1,1]}" >/dev/null; [ $? = 1 ] && pass "one distinct filament is refused" || fail "single-component add"
op "{\"op\":\"bogus\",$P}" >/dev/null; [ $? = 2 ] && pass "unknown op is a bad request" || fail "bad op"
[ "$(op "{\"op\":\"delete_physical\",$P,\"mixed\":[{\"components\":[2,3],\"weights\":[1,1]}],\"filament\":1}" | j 'd["mixed"][0]["components"]')" = "[1, 2]" ] && pass "deleting a physical filament renumbers components" || fail "delete_physical"
m="$(op "{\"op\":\"match\",$P,\"target\":\"#800080\"}" | j 'd["delta_e"]<12')"; [ "$m" = "True" ] && pass "match finds a purple recipe" || fail "match"

op "{\"op\":\"slice_overrides\",$P,\"mixed\":[{\"components\":[1,3],\"weights\":[3,1]}]}" | j '"\n".join("set\t%s\t%s"%(k,v) for k,v in d["overrides"].items())' > "$JOB/sets.txt"
{ printf 'out\t%s\nprofile\t%s\nprofile\t%s\nprofile\t%s\n' "$JOB/o.gcode" "$PACK/machine.json" "$PACK/process.json" "$PACK/filament.json"; cat "$JOB/sets.txt"; printf '\nobject\t%s\t0\t0\t0\t1\t5\n' "$CUBE"; } > "$JOB/req.txt"
"$ENGINE" "$JOB/req.txt" >/dev/null 2>"$JOB/err" || { fail "mixed slice: $(tail -3 "$JOB/err")"; exit 1; }
share="$(python3 - "$JOB/o.gcode" <<'PY'
import sys,re
tool=0; e={0:0.,2:0.}; x=y=0.; tower=False
for raw in open(sys.argv[1]):
    if raw.startswith(';TYPE:'): tower=('tower' in raw.lower())
    l=raw.split(';')[0].strip()
    m=re.match(r'T(\d+)$',l)
    if m: tool=int(m.group(1)); continue
    if l.startswith('G1') and ' E' in l:
        g=dict((t[0],float(t[1:])) for t in l.split()[1:] if re.match(r'[XYE]-?[\d.]+$',t))
        nx,ny=g.get('X',x),g.get('Y',y)
        if 'E' in g and (nx!=x or ny!=y) and g['E']>0 and not tower and tool in e: e[tool]+=g['E']
        x,y=nx,ny
    elif l.startswith('G1'):
        for t in l.split()[1:]:
            if t[0]=='X': x=float(t[1:])
            if t[0]=='Y': y=float(t[1:])
print('%.3f'%(e[0]/max(e[0]+e[2],1e-9)))
PY
)"
python3 -c "import sys; s=float('$share'); sys.exit(0 if 0.68<=s<=0.82 else 1)" && pass "a 3:1 mixed slot prints $share of its extrusion with component 1" || fail "mixed share $share, expected about 0.75"
colour="$(op "{\"op\":\"slice_overrides\",$P,\"mixed\":[{\"components\":[1,3],\"weights\":[3,1]}]}" | j 'd["overrides"]["filament_colour"].split(";")[-1]')"
checker="$(dirname "${BASH_SOURCE[0]}")/thumbnail_color.py"
if python3 "$checker" "$JOB/o.gcode" "$colour" >"$JOB/thumb.log" 2>&1; then pass "the G-code thumbnail draws the part in the mix's colour $colour"; else fail "thumbnail is not $colour: $(cat "$JOB/thumb.log")"; fi
exit $FAIL
