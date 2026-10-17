#!/usr/bin/env bash
# AI auto-paint ops (`--semantic-paint`): a cube splits into six faces, the views and prompt are produced, a provider answer
# is read back into per-face paint, and bad answers/requests fail the documented way. The engine makes no provider calls.
# Usage: semantic_paint_test.sh <nozzle-engine>
set -uo pipefail
ENGINE="$1"
JOB="$(mktemp -d)"; trap 'rm -rf "$JOB"' EXIT; FAIL=0
CUBE="$JOB/cube.stl"   # a 20 mm cube as a binary STL (the ops read the binary STL the apps write)
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
fail() { echo "FAIL: $1"; FAIL=1; }; pass() { echo "PASS: $1"; }
op() { printf '%s' "$1" > "$JOB/r.json"; "$ENGINE" --semantic-paint "$JOB/r.json"; }
j() { python3 -c "import json,sys; d=json.load(sys.stdin); print($1)"; }
F='"filaments":[{"index":1,"type":"PLA","color":"#FF0000","name":"Red"},{"index":2,"type":"PLA","color":"#0000FF"}]'

seg="$(op "{\"op\":\"segment\",\"model\":\"$CUBE\",\"out_dir\":\"$JOB\",$F,\"object_name\":\"cube\",\"hint\":\"make it festive\"}")"
[ "$(echo "$seg" | j 'len(d["segments"])')" = "6" ] && pass "a cube is six segments" || fail "segments: $seg"
[ "$(echo "$seg" | j 'len(d["views"])')" = "6" ] && [ "$(head -c 4 "$JOB/view-front.png" | tail -c 3)" = "PNG" ] && pass "six PNG views are written" || fail "views"
echo "$seg" | j '"make it festive" in d["prompt"] and "Red" in d["prompt"] and "red" in d["prompt"]' | grep -q True && pass "the prompt carries the filaments and the user's hint" || fail "prompt"
echo "$seg" | j '"view-front.png" in d["command_prompt"]' | grep -q True && pass "the command prompt lists the view files" || fail "command prompt"

reply='Sure. {"object":"cube","assignments":[{"segment":0,"filament":1},{"segment":1,"filament":2},{"segment":2,"filament":9}],"notes":"x"}'
asg="$(op "$(python3 -c "import json;print(json.dumps({'op':'assign','model':'$CUBE','reply':'''$reply''','filament_count':2}))")")"
[ "$(echo "$asg" | j 'd["faces"]')" = "12" ] && [ "$(echo "$asg" | j 'd["segment_to_filament"][:3]')" = "[1, 2, 0]" ] && pass "an answer becomes per-face paint (out-of-range slots stay unpainted)" || fail "assign: $asg"
[ "$(echo "$asg" | j 'sum(n for n,f in d["runs"])')" = "12" ] && pass "runs cover every face" || fail "runs"
op "{\"op\":\"assign\",\"model\":\"$CUBE\",\"reply\":\"no json here\",\"filament_count\":2}" >/dev/null; [ $? = 1 ] && pass "an unreadable answer fails" || fail "unreadable answer"

[ "$(op '{"op":"reply_text","kind":"command","text":"{\"result\":\"hello\"}"}' | j 'd["text"]')" = "hello" ] && pass "claude -p JSON output is unwrapped" || fail "command reply"
[ "$(op '{"op":"reply_text","kind":"openai","text":"{\"choices\":[{\"message\":{\"content\":\"hi\"}}]}"}' | j 'd["text"]')" = "hi" ] && pass "chat completions reply text" || fail "openai reply"
op "{\"op\":\"openai_request\",\"model_name\":\"m\",\"prompt\":\"p\",\"png_paths\":[\"$JOB/view-front.png\"]}" | j '"data:image/png;base64," in d["body"]' | grep -q True && pass "OpenAI request embeds the views" || fail "openai request"
op '{"op":"bogus"}' >/dev/null; [ $? = 2 ] && pass "unknown op is a bad request" || fail "bad op"
exit $FAIL
