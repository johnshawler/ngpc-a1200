#!/bin/bash
# Usage: tools/verify_ngpc.sh [ticks] [input script]
# The proof of the translation. Builds the host test twice from the same machine and frame model: once with the
# code tools/recomp900.py generated, once with the interpreter. Both write the machine after every tick (RAM, the
# K2GE, the ports, the registers: host_test.c); the two files must be equal byte for byte. Prints the first tick
# and the place that differ, if any. With an input script (tools/flow_ngpc/<game>_*.in, tools/ngpref's format) both
# play it instead of the bench input.
set -e
cd "$(dirname "$0")/.."
N=${1:-3000}
IN=""; [ -n "$2" ] && IN="-i $(realpath "$2")"
OUT=${TMPDIR:-/tmp}/verify_ngpc.$$
mkdir -p "$OUT"
COMMON="src/ngpc/host_test.c src/ngpc/logic.c src/ngpc/ngp.c src/ngpc/k2ge.c src/coro_host.c"
cc -O2 -fno-strict-aliasing -Isrc/ngpc -Isrc $COMMON src/ngpc/run_c.c src/ngpc/gen_dispatch.c src/ngpc/gen_ops.c src/ngpc/gen_[0-9]*.c -o "$OUT/translated"
cc -O2 -fno-strict-aliasing -Isrc/ngpc -Isrc $COMMON src/ngpc/run_interp.c -o "$OUT/interpreted"
"$OUT/translated" "$N" $IN -r "$OUT/t.bin" | tail -1
"$OUT/interpreted" "$N" $IN -r "$OUT/i.bin" | tail -1
if cmp -s "$OUT/t.bin" "$OUT/i.bin"; then
  echo "verify: translation = interpreter after every one of $N ticks ($(stat -c %s "$OUT/t.bin") bytes compared)"
  rm -rf "$OUT"; exit 0
fi
BYTE=$(cmp "$OUT/t.bin" "$OUT/i.bin" | sed 's/.*\(byte\|char\) \([0-9]*\).*/\2/')
python3 -c '
import sys
b = int(sys.argv[1]) - 1; n = 33112
t, o = b // n + 1, b % n
what = ("RAM at 0x%04x" % (0x4000 + o) if o < 16384 else "K2GE at 0x%04x" % (0x8000 + o - 16384) if o < 32768 else
        "port 0x%02x" % (o - 32768) if o < 33024 else "registers, byte %d (20 longs XWA0..XSP, SR, F-dash, reason, pc)" % (o - 33024))
print("verify: DIFFERENT. First difference in tick %d: %s" % (t, what))
' "$BYTE"
echo "        The two dumps are in $OUT (host_test.c says what each tick's record holds)"
exit 1
