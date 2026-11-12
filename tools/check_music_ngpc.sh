#!/bin/bash
# Usage: tools/check_music_ngpc.sh [ticks]
# How close the Amiga's sound is to the game's own sound program, on the host, over the bench (default TICKS):
#   - tools/sndrec_ngpc.c records the streams (as tools/make_assets_ngpc.sh does) and plays the bench's commands
#     through src/ngpc/sndplay.c, the Amiga's player, 50 times a second;
#   - tools/psgtrace_ngpc.c runs the game with its real sound program (Z80 interpreter) and takes the sound chip's
#     state at the same times.
# Per voice, of the 50 Hz frames where either side sounds: the share where both hold the same note and volume, and
# the share within 3 frames and one volume step (the driver runs at about 245 Hz, so a fast envelope sampled at
# 50 Hz from another phase differs by a step). Nothing here is a gate: it says how faithful the recording is.
set -e
cd "$(dirname "$0")/.."
. ./port.conf
N=${1:-$TICKS}
OUT=$(mktemp -d /tmp/check_music.XXXXXX)
cc -O2 -fno-strict-aliasing -Isrc/ngpc -Isrc tools/sndrec_ngpc.c src/ngpc/psg.c src/ngpc/sndplay.c src/ngpc/logic.c src/ngpc/ngp.c \
   src/ngpc/k2ge.c src/ngpc/run_c.c src/coro_host.c $GEN -o "$OUT/sndrec"
cc -O2 -fno-strict-aliasing -DNGP_Z80_INTERP -Isrc/ngpc -Isrc tools/psgtrace_ngpc.c src/ngpc/psg.c src/ngpc/logic.c src/ngpc/ngp.c \
   src/ngpc/k2ge.c src/ngpc/run_c.c src/coro_host.c $GEN -o "$OUT/psgtrace"
"$OUT/sndrec" assets/rom.bin "$N" "$OUT/sound.bin" "$OUT/player.txt"
"$OUT/psgtrace" assets/rom.bin "$N" "$OUT/real.txt"
python3 -I - "$OUT/real.txt" "$OUT/player.txt" <<'PY'
import sys
def load(fn): return [bytes(int(x, 16) for x in l.split()[1:]) for l in open(fn)]
A, B = load(sys.argv[1]), load(sys.argv[2])
n = min(len(A), len(B))
def voice(s, v):                                    # (left, right attenuation, what sets the pitch) or None if silent
    att = s[9 + v]
    if att == 0xff: return None
    if v < 3: return (att >> 4, att & 15, s[2 * v] | s[2 * v + 1] << 8)
    return (att >> 4, att & 15, (s[8], s[6] | s[7] << 8) if (s[8] & 3) == 3 else (s[8],))
def close(a, b): return a is not None and b is not None and a[2] == b[2] and abs(a[0] - b[0]) <= 1 and abs(a[1] - b[1]) <= 1
for v in range(4):
    aud = eq = near = 0
    for i in range(n):
        a, b = voice(A[i], v), voice(B[i], v)
        if a is None and b is None: continue
        aud += 1
        if a == b: eq += 1
        elif any(close(a, voice(B[j], v)) for j in range(max(0, i - 3), min(n, i + 4))): near += 1
    print('check_music: voice %s sounds in %4d of %d frames: same %5.1f%%, within 3 frames and a step %5.1f%%'
          % ('012N'[v], aud, n, 100.0 * eq / max(1, aud), 100.0 * (eq + near) / max(1, aud)))
PY
rm -rf "$OUT"
