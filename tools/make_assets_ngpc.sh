#!/bin/bash
# Usage: tools/make_assets_ngpc.sh
# Makes everything the build needs from the original cartridge image (original/$GAME.ngc, GAME in port.conf),
# with no manual step:
#   assets/rom.bin, dist/rom.bin     the cartridge as the game reads it (the Amiga program loads it at start)
#   assets/font.bin, dist/font.bin   the BIOS's character set, when original/bios.ngp is there (system call 5)
#   src/ngpc/gen_*                   the game's TLCS-900/H code translated to C by tools/recomp900.py
#   assets/sound.bin, dist/sound.bin the music and sound effects the bench input and the input scripts
#                                    (tools/flow_ngpc/$GAME_*.in) reach, recorded from the game's own sound program
#                                    (tools/sndrec_ngpc.c; SOUND_TICKS, default TICKS, for the bench)
# The flow files tools/flow_ngpc/$GAME_*.txt (made by tools/ngpref -c) seed the translation.
set -e
cd "$(dirname "$0")/.."
CONF="${PORT_CONF:-./port.conf}"
. "$CONF"
ROM=""
for e in ngc ngp npc; do [ -f "original/$GAME.$e" ] && ROM="original/$GAME.$e"; done
[ -n "$ROM" ] || { echo "make_assets: put the game's cartridge image at original/$GAME.ngc (it is not in the repository)"; exit 1; }
mkdir -p assets dist
python3 -I - "$ROM" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
assert d[:28] in (b'COPYRIGHT BY SNK CORPORATION', b' LICENSED BY SNK CORPORATION'), 'not a Neo Geo Pocket cartridge image (no SNK header)'
assert len(d) <= 0x400000, 'larger than 4 MB'
start = int.from_bytes(d[0x1c:0x20], 'little') & 0xffffff
used = sum(1 for i in range(0, len(d), 4096) if any(b != 0xff for b in d[i:i + 4096]) and any(d[i:i + 4096]))
print('make_assets: "%s", %s, %d KB image, %d KB of it hold data, start %06x' % (d[0x24:0x30].decode('ascii', 'replace').strip(),
      'colour' if d[0x23] else 'MONO (the mono mode is not done yet: PLAYBOOK-NGPC.md section 8)', len(d) // 1024, used * 4, start))
if used * 4 > 1100: print('make_assets: NOTE: that data alone does not fit a stock A1200 (2 MB chip RAM, about 1.5 MB usable): this port needs fast RAM')
PY
cp "$ROM" assets/rom.bin; cp "$ROM" dist/rom.bin
if [ -f original/bios.ngp ]; then
  python3 -I -c "import sys; b = open(sys.argv[1], 'rb').read(); assert len(b) == 0x10000, 'the BIOS image must be 64 KB'; open(sys.argv[2], 'wb').write(b[0x8dcf:0x8dcf + 0x800])" original/bios.ngp assets/font.bin
  cp assets/font.bin dist/font.bin
else
  rm -f assets/font.bin dist/font.bin
  echo "make_assets: no original/bios.ngp: text the game draws with the BIOS's character set will show as boxes"
fi
FLOWS=$(ls tools/flow_ngpc/"$GAME"_*.txt 2>/dev/null || true)      # none is fine to start: only the start code is translated then
python3 tools/recomp900.py --game "$GAME" "$ROM" src/ngpc $FLOWS
# the list of generated files in the port description follows what the translator wrote
python3 -I - "$CONF" <<'PY'
import sys, re, glob
conf = open(sys.argv[1]).read()
gen = ' '.join(['src/ngpc/gen_dispatch.c', 'src/ngpc/gen_ops.c'] + sorted(glob.glob('src/ngpc/gen_[0-9]*.c')))
new = re.sub(r'^GEN="[^"]*"', 'GEN="%s"' % gen, conf, flags=re.M)
if new != conf: open(sys.argv[1], 'w').write(new); print('make_assets: updated the GEN list in ' + sys.argv[1])
PY
. "$CONF"                                       # the GEN list may have changed
SR=$(mktemp /tmp/sndrec.XXXXXX)
cc -O2 -fno-strict-aliasing -Isrc/ngpc -Isrc tools/sndrec_ngpc.c src/ngpc/psg.c src/ngpc/sndplay.c src/ngpc/logic.c src/ngpc/ngp.c \
   src/ngpc/k2ge.c src/ngpc/run_c.c src/coro_host.c $GEN -o "$SR"
"$SR" assets/rom.bin "${SOUND_TICKS:-$TICKS}" assets/sound.bin - tools/flow_ngpc/"$GAME"_*.in
rm -f "$SR"
cp assets/sound.bin dist/sound.bin
