#!/bin/bash
# Usage: tools/make_adf_ngpc.sh [out.adf] [command]
# A bootable 880 KB floppy image (DD, Fast File System) of the port, made from dist/ (build the program first):
#   <name>                a small loader (src/ngpc/loader.c) that unpacks <name>.lzma in memory and runs it
#   <name>.lzma           the program, symbols stripped, packed with LZMA (Python's lzma module)
#   rom.lzma              the cartridge packed the same way; the program unpacks it at start (src/ngpc/unlzma.c)
#                         when there is no rom.bin
#   font.bin, sound.bin   if dist/ has them
#   S/Startup-Sequence    runs "command" (default: the program, to play; "ntm bench 600" for a proof run)
# The machine needs fast RAM for the cartridge, as from a hard disk. The image holds the user's cartridge (and the
# BIOS's characters): it goes to the user only, never into the repository. Needs amitools (xdftool), installed here
# from PyPI if missing.
set -e
cd "$(dirname "$0")/.."
. ./port.conf
OUT="${1:-dist/$NAME.adf}"; CMD="${2:-$NAME}"
[ -f "dist/$NAME" ] && [ -f dist/rom.bin ] || { echo "make_adf: build first (dist/$NAME, dist/rom.bin)"; exit 1; }
command -v xdftool >/dev/null || python3 -m pip install --quiet amitools
T=dist/.adf.$$                                  # inside the project: the compiler's container sees it
rm -rf "$T"; mkdir -p "$T"
trap 'rm -rf "$T"' EXIT
CFLAGS="-Isrc/ngpc" tools/build.sh "$T/loader" src/ngpc/loader.c src/ngpc/unlzma.c > /dev/null
docker run --rm -v "$PWD":/work -w /work amigadev/crosstools:m68k-amigaos bash -c \
  "m68k-amigaos-strip -o '$T/game' 'dist/$NAME' && m68k-amigaos-strip '$T/loader'"
pack() {        # LZMA, "alone" format with the size in the header (the encoder writes "unknown")
  python3 -I - "$1" "$2" "$3" <<'EOP'
import sys, lzma, struct
d = open(sys.argv[1], 'rb').read()
lc, lp, pb = (0, 0, 0) if sys.argv[3] == 'data' else (3, 1, 1)     # the cartridge: bytes; the program: 68k words
f = [{'id': lzma.FILTER_LZMA1, 'preset': 9 | lzma.PRESET_EXTREME, 'lc': lc, 'lp': lp, 'pb': pb, 'nice_len': 273, 'dict_size': 1 << 22}]
c = bytearray(lzma.compress(d, format=lzma.FORMAT_ALONE, filters=f))
c[5:13] = struct.pack('<Q', len(d))
open(sys.argv[2], 'wb').write(c)
print('make_adf: %s, %d KB packed to %d KB' % (sys.argv[1].split('/')[-1], len(d) // 1024, len(c) // 1024))
EOP
}
pack dist/rom.bin "$T/rom.lzma" data
pack "$T/game" "$T/$NAME.lzma" code
printf 'Echo "Loading %s: about a minute from the floppy, then about a minute to unpack."\n%s\n' "$NAME" "$CMD" > "$T/startup-sequence"
rm -f "$OUT"
ARGS=("$OUT" create + format "$(echo "$NAME" | tr a-z A-Z)" ffs + boot install + write "$T/loader" "$NAME" \
      + write "$T/$NAME.lzma" "$NAME.lzma" + write "$T/rom.lzma" rom.lzma)
for f in font.bin sound.bin; do [ -f "dist/$f" ] && ARGS+=(+ write "dist/$f" "$f"); done
ARGS+=(+ makedir S + write "$T/startup-sequence" S/Startup-Sequence)
xdftool "${ARGS[@]}"
xdftool "$OUT" list
xdftool "$OUT" info | tail -3
