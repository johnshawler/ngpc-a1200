#!/bin/bash
# Usage: tools/explore_ngpc.sh [rounds] [scripts a round] [ticks] [first seed]
# Grows the translation until random play stops finding code it does not have. Each round:
#   1. plays random input scripts (seeded, so a run can be repeated) through the translated game on the host
#      (host_test -i): a script that reaches untranslated code stops with reason 2;
#   2. plays each stopping script again with the interpreter build (run_interp.c, the port's own frame model) and
#      writes what ran as a flow file (host_test -i script -c flow);
#   3. adds those flows to tools/flow_ngpc/<game>_explore.txt and translates again (tools/recomp900.py).
# It ends after a round in which no script stopped, or after the rounds given. The seeds of the stopping scripts
# are kept in tools/flow_ngpc/<game>_explore.seeds; tools/flow_ngpc/<game>_*.in are other scripts that must pass.
set -e
cd "$(dirname "$0")/.."
. ./port.conf
ROUNDS=${1:-5}; PER=${2:-8}; N=${3:-36000}; SEED=${4:-1000}
ROM=""; for e in ngc ngp npc; do [ -f "original/$GAME.$e" ] && ROM="original/$GAME.$e"; done
FLOW=tools/flow_ngpc/${GAME}_explore.txt; SEEDS=tools/flow_ngpc/${GAME}_explore.seeds
W=$(mktemp -d /tmp/explore.XXXXXX); trap 'rm -rf "$W"' EXIT
COMMON="src/ngpc/host_test.c src/ngpc/logic.c src/ngpc/ngp.c src/ngpc/k2ge.c src/coro_host.c"
script() {      # a random input script: seed, file, ticks
  python3 -I - "$1" "$2" "$3" <<'PY'
import random, sys
seed, out, end = int(sys.argv[1]), sys.argv[2], int(sys.argv[3])
r = random.Random(seed)
names = ['A', 'A', 'A', 'A', 'B', 'B', 'UP', 'DOWN', 'LEFT', 'RIGHT', 'OPTION', 'UP A', 'DOWN A', 'LEFT A', 'RIGHT A', 'UP B', 'DOWN B']
with open(out, 'w') as f:
    t = 300 + r.randint(0, 600)
    while t < end:
        b = r.choice(names); d = r.randint(1, 20)
        f.write('%d %d %s\n' % (t, t + d, b)); t += d + r.randint(5, 90)
PY
}
for round in $(seq 1 "$ROUNDS"); do
  . ./port.conf
  cc -O2 $HOST_CFLAGS $HOST_TEST -o "$W/translated"
  cc -O2 -fno-strict-aliasing -Isrc/ngpc -Isrc $COMMON src/ngpc/run_interp.c -o "$W/interpreted"
  stops=0; : > "$W/new.txt"
  for f in tools/flow_ngpc/"$GAME"_*.in; do              # the kept scripts first
    [ -f "$f" ] || continue
    if "$W/translated" "$N" -i "$f" 2>&1 | grep -q STOPPED; then
      stops=$((stops + 1)); echo "explore: $f stops"; "$W/interpreted" "$N" -i "$f" -c "$W/f.txt" > /dev/null; cat "$W/f.txt" >> "$W/new.txt"
    fi
  done
  for k in $(seq 1 "$PER"); do
    s=$((SEED + (round - 1) * PER + k)); script "$s" "$W/s.in" "$N"
    r=$("$W/translated" "$N" -i "$W/s.in" 2>&1 | grep STOPPED || true)
    if [ -n "$r" ]; then
      stops=$((stops + 1)); echo "explore: seed $s: $r"; echo "$s" >> "$SEEDS"
      "$W/interpreted" "$N" -i "$W/s.in" -c "$W/f.txt" > /dev/null; cat "$W/f.txt" >> "$W/new.txt"
    fi
  done
  echo "explore: round $round: $stops of $PER random scripts (and the kept ones) stopped"
  [ "$stops" = 0 ] && break
  python3 -I - "$FLOW" "$W/new.txt" <<'PY'
import sys, os
old = open(sys.argv[1]).read().split('\n') if os.path.exists(sys.argv[1]) else []
seen, out = set(), []
for l in old + open(sys.argv[2]).read().split('\n'):
    if l and l not in seen: seen.add(l); out.append(l)
open(sys.argv[1], 'w').write('\n'.join(out) + '\n')
print('explore: %s has %d lines' % (sys.argv[1], len(out)))
PY
  python3 tools/recomp900.py --game "$GAME" "$ROM" src/ngpc tools/flow_ngpc/"$GAME"_*.txt | grep -E "^entries"
  python3 -I - port.conf <<'PY'
import sys, re, glob
conf = open(sys.argv[1]).read()
gen = ' '.join(['src/ngpc/gen_dispatch.c'] + sorted(glob.glob('src/ngpc/gen_[0-9]*.c')))
new = re.sub(r'^GEN="[^"]*"', 'GEN="%s"' % gen, conf, flags=re.M)
if new != conf: open(sys.argv[1], 'w').write(new)
PY
done
