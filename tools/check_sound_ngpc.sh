#!/bin/bash
# Usage: tools/check_sound_ngpc.sh [ticks]
# The proof of the sound processor's model (src/ngpc/ngp.c). Builds the host test twice, the main CPU interpreted in
# both: once with the game's real sound program on a Z80 interpreter (NGP_Z80_INTERP), once with the model the Amiga
# runs. Compares, after every tick, everything the main CPU can see: work RAM, the K2GE, the ports, the registers,
# and of the sound processor's 4 KB the bytes the main CPU reads (tools/ngpref -m lists them: READS below).
# The rest of the 4 KB is the sound program's own business and differs (the model does not run it).
set -e
cd "$(dirname "$0")/.."
N=${1:-3000}
READS=${READS:-70de}                    # shared-RAM addresses the game reads, hex, comma-separated
OUT=${TMPDIR:-/tmp}/check_sound.$$
mkdir -p "$OUT"
COMMON="src/ngpc/host_test.c src/ngpc/logic.c src/ngpc/ngp.c src/ngpc/k2ge.c src/ngpc/run_interp.c src/coro_host.c"
cc -O2 -Isrc/ngpc -Isrc -DNGP_Z80_INTERP $COMMON -o "$OUT/z80"
cc -O2 -Isrc/ngpc -Isrc $COMMON -o "$OUT/model"
"$OUT/z80" "$N" -r "$OUT/z.bin" > /dev/null
"$OUT/model" "$N" -r "$OUT/m.bin" > /dev/null
python3 -c '
import sys
z, m = open(sys.argv[1], "rb").read(), open(sys.argv[2], "rb").read()
reads = [int(x, 16) - 0x4000 for x in sys.argv[3].split(",")]
n = 33112
skip = (set(range(0x3000, 0x4000)) - set(reads)) | {32768 + 0xa0, 32768 + 0xa1}      # the sound program, the sound chip
keep = [i for i in range(n) if i not in skip]
ticks = len(z) // n
for t in range(ticks):
    a, b = z[t * n:(t + 1) * n], m[t * n:(t + 1) * n]
    if a == b: continue
    bad = [i for i in keep if a[i] != b[i]]
    if bad:
        o = bad[0]
        what = ("RAM at 0x%04x" % (0x4000 + o) if o < 16384 else "K2GE at 0x%04x" % (0x8000 + o - 16384) if o < 32768 else
                "port 0x%02x" % (o - 32768) if o < 33024 else "registers, byte %d" % (o - 33024))
        print("check_sound: DIFFERENT in tick %d: %s (real Z80 %02x, model %02x), %d bytes in all" % (t + 1, what, a[o], b[o], len(bad)))
        sys.exit(1)
print("check_sound: the model = the real sound program, as the main CPU sees it, after every one of %d ticks" % ticks)
' "$OUT/z.bin" "$OUT/m.bin" "$READS"
rm -rf "$OUT"
