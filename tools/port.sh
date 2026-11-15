#!/bin/bash
# Usage: tools/port.sh [fast]
# The referee. Builds the port described in port.conf, runs its bench on the emulated stock A1200
# (or, with "fast", on one with 4 MB fast RAM), runs the host test, and checks the brief's gates.
# Leaves bench.txt and shot.png in the project folder. Exit code 0 = PASS. CI runs exactly this.
# First tools/check_rules.py: without a plan (FEASIBILITY.md) nothing is built; a class the user
# did not approve is judged as planar; a broken "never do these" rule is a FAIL even when the gates pass.
cd "$(dirname "$0")/.."
. "${PORT_CONF:-./port.conf}"                # PORT_CONF=<file>: bench another description (e.g. port.ngpc.conf)
OPT=""; OUT=bench.txt; SHOT=shot.png
if [ "$1" = fast ]; then OPT="--fast_memory=4096"; OUT=bench_fast.txt; SHOT=shot_fast.png; fi
# m1230: an ESTIMATE for the user's A1200 with an M-Tec M1230 XA (68030 at 33 MHz, 2 MB chip, 4 MB fast). FS-UAE cannot run
# a cycle-exact 68030 at that clock (its cycle-exact mode keeps memory at A1200 speed: x4 to x16 the clock bought 10-25%),
# so the CPU runs untimed but throttled, with the chipset and the blitter cycle-exact. The throttle is calibrated to the
# brief's table: a 50 MHz 68030 with fast RAM is about 6x a stock A1200 for logic, fast RAM alone 1.5x, so 33 MHz is about
# 2.7x this port's logic with fast RAM (18.5k -> about 6.9k colour clocks a tick). Not a cycle-exact measurement: the brief
# accepts only those (or a real machine) as proof, so this mode never claims a PASS.
if [ "$1" = m1230 ]; then
  OPT="--fast_memory=4096 --cpu=68030 --uae_cpu_cycle_exact=false --uae_cpu_compatible=true --uae_cpu_speed=max --uae_blitter_cycle_exact=true --uae_cpu_throttle=-660"
  OUT=bench_m1230.txt; SHOT=shot_m1230.png; ESTIMATE=1
fi
# KICKSTART=<file>: boot a Kickstart ROM the user supplied (keep it in original/, never commit it) instead of the free
# AROS ROM the brief measures with
[ -n "$KICKSTART" ] && OPT="$OPT --kickstart_file=$(realpath "$KICKSTART")"
RC=$(mktemp /tmp/rules_class.XXXXXX); RO=$(mktemp /tmp/rules_out.XXXXXX)
RULES=0; RULES_CLASS_FILE="$RC" python3 tools/check_rules.py "${PORT_CONF:-./port.conf}" > "$RO" || RULES=1
cat "$RO"; EFFCLASS=$(cat "$RC")
if grep -q "^FAIL: plan:" "$RO"; then
  rm -f "$RC" "$RO"; echo "FAIL: not built: no FEASIBILITY.md (the brief's milestone 1 plan)"; exit 1
fi
rm -f "$RC" "$RO"
mkdir -p dist
rm -f "dist/$NAME" "$OUT" "$SHOT"
CFLAGS="$CFLAGS" tools/build.sh "dist/$NAME" $SOURCES || { echo "FAIL: build"; exit 1; }
[ -s "dist/$NAME" ] || { echo "FAIL: build produced no dist/$NAME"; exit 1; }
# the screenshot is taken about half-way through the run (start-up takes about 10 seconds)
SHOT_AT="${SHOT_AT:-$(( 10 + TICKS / 100 ))}" tools/bench.sh dist "$NAME" "$TICKS" "$PWD/$SHOT" $OPT > "$OUT"
HOSTSUM=""
if [ -n "$HOST_TEST" ]; then
  HT=$(mktemp /tmp/host_test.XXXXXX)
  cc -O2 $HOST_CFLAGS $HOST_TEST -o "$HT" -lm || { echo "FAIL: host test does not compile"; exit 1; }
  HOSTSUM=$("$HT" "$TICKS" | grep -o -i -E '\b[0-9a-f]{8}\b' | tail -1)
  rm -f "$HT"
  [ -n "$HOSTSUM" ] || { echo "FAIL: host test printed no 8-digit hex checksum"; exit 1; }
fi
ARGS="--class ${EFFCLASS:-planar} --ticks $TICKS --picture-hz ${PICTURE_HZ:-50}"
[ -n "$HOSTSUM" ] && ARGS="$ARGS --host $HOSTSUM"
[ -n "$EXPECT_CHECKSUM" ] && ARGS="$ARGS --expect $EXPECT_CHECKSUM"
if [ -n "$ESTIMATE" ]; then                   # tools/port.sh m1230: the gates as a guide only, never a PASS
  echo "ESTIMATE ONLY: throttled 68030 calibrated to 33 MHz (not cycle-exact; the brief takes no frame rate from this mode)"
  python3 tools/check_gates.py "$OUT" $ARGS | sed 's/^PASS/ESTIMATE: would pass/'
  exit 1
fi
GATES=0; python3 tools/check_gates.py "$OUT" $ARGS || GATES=1
if [ $RULES = 1 ]; then echo "FAIL: rules (tools/check_rules.py above)"; fi
[ $GATES = 0 ] && [ $RULES = 0 ]
