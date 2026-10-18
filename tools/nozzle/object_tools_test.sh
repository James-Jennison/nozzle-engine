#!/usr/bin/env bash
# Object tools beyond the plate ops: parts (negative volumes, support blockers / enforcers), height range settings, variable
# layer height (a profile, and the adaptive profile op), per-axis scale and mirroring, all as slice request lines.
# Usage: object_tools_test.sh <nozzle-engine>     (NOZZLE_ROOT = the Nozzle It All checkout, for the U1 profile pack and cube)
set -uo pipefail
ENGINE="$1"
NOZZLE_ROOT="${NOZZLE_ROOT:-/mnt/faststorage/Nozzle It All}"
PACK="$NOZZLE_ROOT/app/src/main/assets/slicer_profiles/generic_klipper"; CUBE="$NOZZLE_ROOT/site-src/assets/test-cube-20mm.stl"
JOB="$(mktemp -d)"; trap 'rm -rf "$JOB"' EXIT; FAIL=0
fail() { echo "FAIL: $1"; FAIL=1; }; pass() { echo "PASS: $1"; }

# An L-shaped prism (asymmetric in X and Y) and a cone (a surface whose slope changes the adaptive layer height).
python3 - "$JOB" <<'PY'
import struct, sys, math
def write(path, tris):
    with open(path, 'wb') as f:
        f.write(b'\0'*80 + struct.pack('<I', len(tris)))
        for t in tris: f.write(struct.pack('<12fH', 0,0,0, *[c for v in t for c in v], 0))
def prism(poly, h):
    t = []
    n = len(poly)
    for i in range(1, n-1):
        t.append(((poly[0][0],poly[0][1],0),(poly[i+1][0],poly[i+1][1],0),(poly[i][0],poly[i][1],0)))
        t.append(((poly[0][0],poly[0][1],h),(poly[i][0],poly[i][1],h),(poly[i+1][0],poly[i+1][1],h)))
    for i in range(n):
        a, b = poly[i], poly[(i+1)%n]
        t.append(((a[0],a[1],0),(b[0],b[1],0),(b[0],b[1],h))); t.append(((a[0],a[1],0),(b[0],b[1],h),(a[0],a[1],h)))
    return t
# An L is not convex; fan-triangulate its two convex halves.
tris = prism([(0,0),(30,0),(30,10),(0,10)], 10) + prism([(0,10),(10,10),(10,30),(0,30)], 10)
write(sys.argv[1] + '/l.stl', tris)
r, h, s = 15.0, 30.0, 32
cone = []
for i in range(s):
    a0, a1 = 2*math.pi*i/s, 2*math.pi*(i+1)/s
    p0, p1 = (r*math.cos(a0), r*math.sin(a0), 0), (r*math.cos(a1), r*math.sin(a1), 0)
    cone.append((p0, p1, (0,0,h))); cone.append(((0,0,0), p1, p0))
write(sys.argv[1] + '/cone.stl', cone)
PY
req() { # req <out> <object lines...>
  local out="$1"; shift
  { printf 'out\t%s\nprofile\t%s\nprofile\t%s\nprofile\t%s\n' "$out" "$PACK/machine.json" "$PACK/filament.json" "$PACK/process.json"; printf '%s\n' "$@"; } > "$JOB/req.txt"
  "$ENGINE" "$JOB/req.txt" >/dev/null 2>"$JOB/err"
}
layers() { grep -c '^;LAYER_CHANGE' "$1"; }
filament() { python3 - "$1" <<'PY'
import re,sys
for l in open(sys.argv[1]):
    m=re.match(r'; (?:total )?filament used \[mm\] = ([\d.]+)',l)
    if m: print(m.group(1)); break
PY
}
bbox() { python3 - "$1" <<'PY'   # x/y extent and extruded-length centroid of the print
import re,sys
x=y=0.; xs=[]; ys=[]; cx=cy=w=0.
for l in open(sys.argv[1]):
    l=l.split(';')[0].strip()
    if not l.startswith(('G0','G1')): continue
    g={t[0]:float(t[1:]) for t in l.split()[1:] if re.match(r'[XYE]-?[\d.]+$',t)}
    nx,ny=g.get('X',x),g.get('Y',y)
    if 'E' in g and g['E']>0 and (nx!=x or ny!=y):
        xs+=[x,nx]; ys+=[y,ny]; d=((nx-x)**2+(ny-y)**2)**.5; cx+=d*(x+nx)/2; cy+=d*(y+ny)/2; w+=d
    x,y=nx,ny
print(min(xs),max(xs),min(ys),max(ys),cx/w,cy/w)
PY
}

OBJ="object\t$CUBE\t0\t0\t0\t1\t0"
req "$JOB/base.gcode" "$(printf "$OBJ")" || { fail "base slice: $(tail -3 "$JOB/err")"; exit 1; }
base_layers="$(layers "$JOB/base.gcode")"; base_fil="$(filament "$JOB/base.gcode")"

# Negative volume: a box cut out of the cube takes filament away.
req "$JOB/neg.gcode" "$(printf "$OBJ")" "$(printf 'object_part\t1\tnegative:box:0,0,0:12,12,40')" && \
  python3 -c "import sys; sys.exit(0 if float('$(filament "$JOB/neg.gcode")') < float('$base_fil') else 1)" && pass "a negative volume removes material" || fail "negative volume ($(tail -2 "$JOB/err"))"
# Support blocker / enforcer / modifier parts are accepted.
for kind in blocker enforcer; do
  req "$JOB/$kind.gcode" "$(printf "$OBJ")" "$(printf "object_part\t1\t$kind:box:0,0,10:6,6,6")" && pass "a support $kind part is accepted" || fail "$kind part ($(tail -2 "$JOB/err"))"
done
req "$JOB/mod.gcode" "$(printf "$OBJ")" "$(printf 'object_part\t1\tmodifier:cylinder:0,0,10:10,10,10:sparse_infill_density=100%%')" && \
  python3 -c "import sys; sys.exit(0 if float('$(filament "$JOB/mod.gcode")') > float('$base_fil') else 1)" && pass "a modifier part's settings apply inside it" || fail "modifier part ($(tail -2 "$JOB/err"))"
req "$JOB/part.gcode" "$(printf "$OBJ")" "$(printf 'object_part\t1\tpart:box:0,0,25:10,10,10')" && \
  python3 -c "import sys; sys.exit(0 if float('$(filament "$JOB/part.gcode")') > float('$base_fil') else 1)" && pass "an added part prints" || fail "added part ($(tail -2 "$JOB/err"))"

# Height ranges: a finer layer height between 5 and 10 mm adds layers.
req "$JOB/range.gcode" "$(printf "$OBJ")" "$(printf 'object_range\t1\t5\t10\tlayer_height\t0.08')" && \
  [ "$(layers "$JOB/range.gcode")" -gt "$base_layers" ] && pass "a height range with a finer layer height adds layers ($base_layers -> $(layers "$JOB/range.gcode"))" || fail "height range ($(tail -2 "$JOB/err"))"
req "$JOB/badrange.gcode" "$(printf "$OBJ")" "$(printf 'object_range\t1\t10\t5\tlayer_height\t0.08')"; [ $? = 1 ] && pass "an inverted height range is refused" || fail "inverted range"
req "$JOB/badkey.gcode" "$(printf "$OBJ")" "$(printf 'object_range\t1\t5\t10\tnozzle_temperature\t200')"; [ $? = 1 ] && pass "a range setting that isn't an object setting is refused" || fail "range key"

# Variable layer height: a profile of z / height pairs.
req "$JOB/var.gcode" "$(printf "$OBJ")" "$(printf 'object_layers\t1\t0,0.2,10,0.2,10,0.1,20,0.1')" && \
  [ "$(layers "$JOB/var.gcode")" -gt "$base_layers" ] && pass "a variable layer height profile changes the layers ($base_layers -> $(layers "$JOB/var.gcode"))" || fail "layer profile ($(tail -2 "$JOB/err"))"
# A profile whose first height isn't the process's first layer height (the Snapmaker U1 prints a 0.25 mm first layer) still applies.
SAVED_PACK="$PACK"; PACK="$NOZZLE_ROOT/app/src/main/assets/slicer_profiles/snapmaker_u1"
req "$JOB/u1base.gcode" "$(printf "$OBJ")"; u1_layers="$(layers "$JOB/u1base.gcode")"
req "$JOB/u1var.gcode" "$(printf "$OBJ")" "$(printf 'object_layers\t1\t0,0.2,10,0.2,10,0.1,20,0.1')" && \
  [ "$(layers "$JOB/u1var.gcode")" -gt "$u1_layers" ] && pass "a layer profile that doesn't start at the first layer height is brought to it ($u1_layers -> $(layers "$JOB/u1var.gcode"))" || fail "u1 profile ($(tail -2 "$JOB/err"))"
PACK="$SAVED_PACK"
req "$JOB/oddprofile.gcode" "$(printf "$OBJ")" "$(printf 'object_layers\t1\t0,0.2,10')"; [ $? = 1 ] && pass "a profile that isn't pairs is refused" || fail "odd profile"

# Adaptive layers: finer where the cone's wall is shallow than where it is steep.
printf '%s' "{\"op\":\"adaptive_layers\",\"model\":\"$JOB/cone.stl\",\"profiles\":[\"$PACK/machine.json\",\"$PACK/filament.json\",\"$PACK/process.json\"],\"quality\":0.9}" > "$JOB/a.json"
"$ENGINE" --plate "$JOB/a.json" > "$JOB/a.out"
python3 - "$JOB/a.out" <<'PY' && pass "the adaptive profile is z / height pairs up the object" || fail "adaptive: $(cat "$JOB/a.out")"
import json, sys
p = json.load(open(sys.argv[1]))["profile"]
assert len(p) >= 4 and len(p) % 2 == 0, p
zs, hs = p[0::2], p[1::2]
assert zs == sorted(zs) and max(zs) > 20 and min(hs) > 0
PY
printf '%s' "{\"op\":\"adaptive_layers\",\"model\":\"$JOB/nope.stl\"}" > "$JOB/a.json"; "$ENGINE" --plate "$JOB/a.json" >/dev/null; [ $? = 2 ] && pass "adaptive layers on a missing model is a bad request" || fail "adaptive missing model"

# Per-axis scale and mirroring.
req "$JOB/l.gcode" "$(printf "object\t$JOB/l.stl\t0\t0\t0\t1\t0")" || fail "L slice"
read x0 x1 y0 y1 cx cy < <(bbox "$JOB/l.gcode")
req "$JOB/scaled.gcode" "$(printf "object\t$JOB/l.stl\t0\t0\t0\t1\t0")" "$(printf 'object_scale\t1\t2\t1\t1')" || fail "scaled slice ($(tail -2 "$JOB/err"))"
read sx0 sx1 sy0 sy1 scx scy < <(bbox "$JOB/scaled.gcode")
python3 -c "
w0=$x1-$x0; w1=$sx1-$sx0; d0=$y1-$y0; d1=$sy1-$sy0
import sys; sys.exit(0 if 1.9 < w1/w0 < 2.1 and 0.95 < d1/d0 < 1.05 else 1)" && pass "scaling X by 2 doubles only the X extent" || fail "per-axis scale ($x0 $x1 / $sx0 $sx1)"
req "$JOB/mirror.gcode" "$(printf "object\t$JOB/l.stl\t0\t0\t0\t1\t0")" "$(printf 'object_scale\t1\t1\t1\t1\tx')" || fail "mirrored slice ($(tail -2 "$JOB/err"))"
read mx0 mx1 my0 my1 mcx mcy < <(bbox "$JOB/mirror.gcode")
python3 -c "
# the extrusion centroid sits on the other side of the bounding box centre once mirrored in X, and not in Y
bx=($x0+$x1)/2; mbx=($mx0+$mx1)/2
import sys; sys.exit(0 if ($cx-bx)*($mcx-mbx) < 0 and ($cy-($y0+$y1)/2)*($mcy-($my0+$my1)/2) > 0 else 1)" && pass "mirroring X flips the shape in X only" || fail "mirror ($cx $mcx)"
req "$JOB/neg.gcode" "$(printf "$OBJ")" "$(printf 'object_scale\t1\t-1\t1\t1')"; [ $? = 2 ] && pass "a non-positive scale is a bad request (mirror is its own field)" || fail "negative scale"
exit $FAIL
