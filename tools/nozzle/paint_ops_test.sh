#!/usr/bin/env bash
# Paint tools (`--paint`): a brush stroke paints and splits triangles, a fill takes a whole flat side, erasing clears, the painted
# pieces come back for drawing, and misses / bad requests behave as documented. Usage: paint_ops_test.sh <nozzle-engine>
set -uo pipefail
ENGINE="$1"
JOB="$(mktemp -d)"; trap 'rm -rf "$JOB"' EXIT; FAIL=0
fail() { echo "FAIL: $1"; FAIL=1; }; pass() { echo "PASS: $1"; }
CUBE="$JOB/cube.stl"
python3 - "$CUBE" <<'PY'
import struct,sys
v=[(x,y,z) for z in (0,20) for y in (0,20) for x in (0,20)]
q=[(0,2,3,1),(4,5,7,6),(0,1,5,4),(2,6,7,3),(0,4,6,2),(1,3,7,5)]
t=[]
for a,b,c,d in q: t+= [(a,b,c),(a,c,d)]
with open(sys.argv[1],'wb') as f:
    f.write(b'\0'*80+struct.pack('<I',len(t)))
    for tri in t: f.write(struct.pack('<12fH',0,0,0,*[c for i in tri for c in v[i]],0))
PY
op() { printf '%s' "$1" > "$JOB/r.json"; "$ENGINE" --paint "$JOB/r.json"; }
j() { python3 -c "import json,sys; d=json.load(sys.stdin); print($1)"; }
TOP='{"origin":[5,5,100],"dir":[0,0,-1],"radius":3,"state":3}'

r="$(op "{\"op\":\"stroke\",\"model\":\"$CUBE\",\"strokes\":[$TOP]}")"
[ "$(echo "$r" | j 'd["hits"]')" = "1" ] && [ "$(echo "$r" | j 'len(d["paint"])>0')" = "True" ] && pass "a brush stroke paints the top" || fail "brush: $r"
echo "$r" | python3 -c 'import json,sys; print(json.dumps(json.load(sys.stdin)["paint"]))' > "$JOB/paint.json"
f="$(op "{\"op\":\"facets\",\"model\":\"$CUBE\",\"paint\":$(cat "$JOB/paint.json")}")"
[ "$(echo "$f" | j '[s["state"] for s in d["states"]]')" = "[3]" ] && pass "the painted pieces come back for drawing" || fail "facets: $f"
echo "$f" | j 'all(abs(c-20)<1e-3 for c in d["states"][0]["triangles"][2::3])' | grep -q True && pass "the pieces lie on the painted (top) face" || fail "facet position"

area() { python3 -c "
import json,sys
d=json.load(sys.stdin); t=sum((s['triangles'] for s in d['states'] if s['state']==3),[]); a=0.0
for i in range(0,len(t),9):
    x0,y0,x1,y1,x2,y2=t[i],t[i+1],t[i+3],t[i+4],t[i+6],t[i+7]; a+=abs((x1-x0)*(y2-y0)-(x2-x0)*(y1-y0))/2
print(round(a,1))"; }
r2="$(op "{\"op\":\"stroke\",\"model\":\"$CUBE\",\"paint\":$(cat "$JOB/paint.json"),\"strokes\":[{\"origin\":[5,5,100],\"dir\":[0,0,-1],\"radius\":1,\"state\":0}]}")"
echo "$r2" | python3 -c 'import json,sys; print(json.dumps(json.load(sys.stdin)["paint"]))' > "$JOB/erased.json"
a1="$(echo "$f" | area)"; a2="$(op "{\"op\":\"facets\",\"model\":\"$CUBE\",\"paint\":$(cat "$JOB/erased.json")}" | area)"
python3 -c "import sys; sys.exit(0 if 0 < float('$a2') < float('$a1') else 1)" && pass "erasing takes paint away only where the brush went (painted area $a1 -> $a2 mm2)" || fail "erase ($a1 -> $a2)"

r3="$(op "{\"op\":\"stroke\",\"model\":\"$CUBE\",\"mode\":\"fill\",\"angle\":30,\"strokes\":[{\"origin\":[5,5,100],\"dir\":[0,0,-1],\"state\":4}]}")"
[ "$(echo "$r3" | j 'len(d["paint"])')" = "2" ] && pass "a fill takes the whole flat side (both top triangles)" || fail "fill: $r3"

[ "$(op "{\"op\":\"stroke\",\"model\":\"$CUBE\",\"strokes\":[{\"origin\":[500,500,100],\"dir\":[0,0,-1],\"radius\":3,\"state\":3}]}" | j 'd["hits"]')" = "0" ] && pass "a miss paints nothing" || fail "miss"
op "{\"op\":\"stroke\",\"model\":\"$CUBE\",\"strokes\":[{\"origin\":[5,5,100],\"dir\":[0,0,0],\"state\":3}]}" >/dev/null; [ $? = 2 ] && pass "a stroke without a direction is a bad request" || fail "no dir"
op "{\"op\":\"stroke\",\"model\":\"$CUBE\",\"paint\":[[99,\"4\"]],\"strokes\":[]}" >/dev/null; [ $? = 2 ] && pass "paint outside the model is a bad request" || fail "range"
op '{"op":"bogus"}' >/dev/null; [ $? = 2 ] && pass "unknown op is a bad request" || fail "bad op"
exit $FAIL
