#!/bin/bash
# Usage: tools/port.sh [fast]
# The referee. Builds the port described in port.conf, runs its bench on the emulated stock A1200
# (or, with "fast", on one with 4 MB fast RAM), runs the host test, and checks the brief's gates.
# Leaves bench.txt and shot.png in the project folder. Exit code 0 = PASS. CI runs exactly this.
cd "$(dirname "$0")/.."
. "${PORT_CONF:-./port.conf}"                # PORT_CONF=<file>: bench another description (e.g. port.ngpc.conf)
OPT=""; OUT=bench.txt; SHOT=shot.png
if [ "$1" = fast ]; then OPT="--fast_memory=4096"; OUT=bench_fast.txt; SHOT=shot_fast.png; fi
# KICKSTART=<file>: boot a Kickstart ROM the user supplied (keep it in original/, never commit it) instead of the free
# AROS ROM the brief measures with
[ -n "$KICKSTART" ] && OPT="$OPT --kickstart_file=$(realpath "$KICKSTART")"
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
ARGS="--class ${CLASS:-planar} --ticks $TICKS"
[ -n "$HOSTSUM" ] && ARGS="$ARGS --host $HOSTSUM"
[ -n "$EXPECT_CHECKSUM" ] && ARGS="$ARGS --expect $EXPECT_CHECKSUM"
python3 tools/check_gates.py "$OUT" $ARGS
