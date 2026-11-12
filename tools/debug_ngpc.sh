#!/bin/bash
# Usage: tools/debug_ngpc.sh <outdir> <ticks> [seconds] [fs-uae options]
# Finds the first tick where the Amiga and the host disagree: builds the port with a checksum after every tick
# (-DLOGIC_TRACE), runs "<name>_dbg bench <ticks>" with <outdir> as its disk, runs the host test for the same ticks,
# and compares the two lists. Then look at that tick: the translated code that ran in it, and dumps of both sides.
set -e
cd "$(dirname "$0")/.."
. ./port.conf
OUT="$1"; N="$2"; SECS="${3:-900}"; shift 3 2>/dev/null || shift $#
EXE=${NAME}_dbg
CFLAGS="$CFLAGS -DLOGIC_TRACE=$N" tools/build.sh "dist/$EXE" $SOURCES > /dev/null
rm -rf "$OUT"; mkdir -p "$OUT/S"; cp "dist/$EXE" dist/rom.bin "$OUT/"; [ -f dist/font.bin ] && cp dist/font.bin "$OUT/"
printf '%s bench %s\n' "$EXE" "$N" > "$OUT/S/startup-sequence"
UAE_DONE="$OUT/bench.txt" tools/run_uae.sh "$OUT" "$SECS" "$OUT/shot.png" "$@"
cat "$OUT/bench.txt" 2>/dev/null || echo "no bench.txt"
HT=$(mktemp /tmp/host_test.XXXXXX)
cc -O2 $HOST_CFLAGS $HOST_TEST -o "$HT"
"$HT" "$N" -V | grep -v '=' > "$OUT/host.txt"
rm -f "$HT"
python3 -c '
import sys, struct
a = open(sys.argv[1], "rb").read(); amiga = struct.unpack(">%dI" % (len(a) // 4), a)
host = [int(x, 16) for x in open(sys.argv[2]).read().split()]
n = min(len(amiga), len(host))
for t in range(n):
    if amiga[t] != host[t]: print("debug: first difference after tick %d (Amiga %08x, host %08x)" % (t + 1, amiga[t], host[t])); sys.exit(1)
print("debug: Amiga = host after every one of %d ticks (Amiga wrote %d, host %d)" % (n, len(amiga), len(host)))
' "$OUT/trace.bin" "$OUT/host.txt"
