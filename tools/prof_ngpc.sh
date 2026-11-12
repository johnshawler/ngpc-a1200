#!/bin/bash
# Usage: tools/prof_ngpc.sh <outdir> <ticks> [seconds] [fs-uae options, e.g. --fast_memory=4096]
# Where the time goes on the emulated Amiga: builds the port with the 1 kHz sampling profiler (src/prof68k.s,
# -DNGP_PROF), runs "<name>_prof bench <ticks>" with <outdir> as its disk, and prints the functions that got
# the most samples. Translated game code is F_<address>; the display is k2ge_* and c2p*.
set -e
cd "$(dirname "$0")/.."
. ./port.conf
OUT="$1"; N="$2"; SECS="${3:-600}"; shift 3 2>/dev/null || shift $#
EXE=${NAME}_prof
# the same build as tools/build.sh, linked with a map: static functions have no symbol in the executable, the map
# says at least which object file a sample fell in
docker run --rm -v "$PWD":/work -w /work -e CFLAGS="$CFLAGS -DNGP_PROF" amigadev/crosstools:m68k-amigaos bash -c '
set -e; OBJS=""; rm -rf dist/obj_prof; mkdir -p dist/obj_prof
for f in '"$SOURCES src/prof68k.s"'; do
  o=dist/obj_prof/$(echo "$f" | tr "/." "__").o
  case "$f" in
    *.c) m68k-amigaos-gcc -m68020 -O2 -fomit-frame-pointer -noixemul -Wall $CFLAGS -c "$f" -o "$o" ;;
    *.s|*.asm) vasmm68k_mot -quiet -Fhunk -m68020 -DNGP_PROF=1 -I. -I"$(dirname "$f")" -o "$o" "$f" ;;
  esac
  OBJS="$OBJS $o"
done
m68k-amigaos-gcc -m68020 -noixemul $OBJS -o dist/'"$EXE"' -Wl,-Map=dist/'"$EXE"'.map'
rm -rf "$OUT"; mkdir -p "$OUT/S"; cp "dist/$EXE" dist/rom.bin "$OUT/"; [ -f dist/font.bin ] && cp dist/font.bin "$OUT/"; [ -f dist/sound.bin ] && cp dist/sound.bin "$OUT/"
printf '%s bench %s\n' "$EXE" "$N" > "$OUT/S/startup-sequence"
UAE_DONE="$OUT/bench.txt" tools/run_uae.sh "$OUT" "$SECS" - "$@"
cat "$OUT/bench.txt" 2>/dev/null || { echo "no bench.txt"; exit 1; }
docker run --rm -v "$PWD":/work -w /work amigadev/crosstools:m68k-amigaos m68k-amigaos-objdump -t -h "dist/$EXE" > "$OUT/symbols.txt"
cp "dist/$EXE.map" "$OUT/map.txt"
docker run --rm -v "$PWD":/work -w /work amigadev/crosstools:m68k-amigaos bash -c 'for o in dist/obj_prof/*.o; do echo "== $o"; m68k-amigaos-nm "$o" | grep -i " t "; done' > "$OUT/objsyms.txt"
python3 - "$OUT/prof.bin" "$OUT/symbols.txt" "$OUT/map.txt" "$OUT/objsyms.txt" <<'PY'
import sys, struct, re, collections, bisect
data = open(sys.argv[1], 'rb').read()
main_rt, coro_rt = struct.unpack('>II', data[:8])
samples = struct.unpack('>%dI' % ((len(data) - 8) // 4), data[8:])
gl = dict((m.group(2), int(m.group(1), 16)) for m in re.finditer(r'^([0-9a-f]{8}) \w\s+\w*\s+\.text\s+[0-9a-f]+\s+(?:\S+\s+)?(_?\w+)$', open(sys.argv[2]).read(), re.M))
objs = []                                                   # (hunk, start in the hunk, size, object)
hunk, hstart = -1, 0                                        # code hunks: gcc's .text and vasm's code
for line in open(sys.argv[3]):
    m = re.match(r'^(\.text|code)\s+0x([0-9a-f]+)', line)
    if m: hunk += 1; hstart = int(m.group(2), 16); continue
    if re.match(r'^\S', line): hstart = None
    m = re.match(r'^ (\.text|code)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)', line)
    if m and hstart is not None: objs.append((hunk, int(m.group(2), 16) - hstart, int(m.group(3), 16), m.group(4).split('/')[-1]))
osyms = collections.defaultdict(list); cur = None
for line in open(sys.argv[4]):
    if line.startswith('== '): cur = line[3:].strip().split('/')[-1]; continue
    p = line.split()
    if len(p) == 3: osyms[cur].append((int(p[0], 16), p[2]))
hunk_of = {}
for h, st, sz, o in objs:
    for v, n in osyms.get(o, []):
        if n in ('_main', '_co_resume'): hunk_of[n] = (h, st + v)
base = {hunk_of['_main'][0]: main_rt - hunk_of['_main'][1], hunk_of['_co_resume'][0]: coro_rt - hunk_of['_co_resume'][1]}
count = collections.Counter(); byobj = collections.Counter(); other = 0
for smp in samples:
    for h, st, sz, o in objs:
        if h in base and 0 <= smp - base[h] - st < sz:
            off = smp - base[h] - st
            syms = sorted(osyms.get(o, []))
            i = bisect.bisect_right([v for v, n in syms], off) - 1
            name = syms[i][1] if i >= 0 else '?'
            count[name.lstrip('_') + '  (' + o.replace('.o', '') + ')'] += 1; byobj[o.replace('.o', '')] += 1
            break
    else: other += 1
total = len(samples)
print('%d samples (1 ms each); outside the program (operating system, interrupts): %d' % (total, other))
print('by object file:')
for n, c in byobj.most_common(12): print('  %5.1f%%  %s' % (100.0 * c / max(total, 1), n))
print('busiest functions:')
for n, c in count.most_common(30): print('  %5.1f%%  %6d  %s' % (100.0 * c / max(total, 1), c, n))
PY
