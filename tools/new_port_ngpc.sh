#!/bin/bash
# Usage: tools/new_port_ngpc.sh <game> <rom.ngc> [bios.ngp]
# Turns a copy of this repository into the start of a new Neo Geo Pocket Color port, keeping everything earlier
# ports learned:
#   - the previous port's records (REPORT*.txt, NOTES.md, LEARNINGS.md, PIPELINE.md) move to history/<old game>/
#   - port.conf is made from port.ngpc.conf and names the new game
#   - tools/games/<game>.py is created empty (tools/games/ntm.py stays: it is the example to copy from)
#   - the bench input is reset; the cartridge image is copied to original/<game>.ngc and the BIOS image, if you
#     give one, to original/bios.ngp (neither is ever committed: .gitignore)
#   - generated files are removed
# PLAYBOOK-NGPC.md, the reference machine, the translator, the machine layer and the tools are kept as they are.
# Run it on a fresh copy (Use this template on GitHub, or a clone), then commit. Nothing is pushed.
set -e
cd "$(dirname "$0")/.."
GAME="$1"; ROM="$2"; BIOS="$3"
[ -n "$GAME" ] && [ -f "$ROM" ] || { echo "usage: tools/new_port_ngpc.sh <game> <rom.ngc> [bios.ngp]"; exit 2; }
case "$GAME" in *[!a-z0-9_]*) echo "new_port: the game name must be lower-case letters, digits and _"; exit 2;; esac
OLD=$(sed -n 's/^GAME=\([a-z0-9_]*\).*/\1/p' port.conf)
[ -n "$OLD" ] || OLD=previous
[ ! -e "history/$OLD" ] || { echo "new_port: history/$OLD exists already"; exit 2; }

echo "new_port: archiving $OLD to history/$OLD"
mkdir -p "history/$OLD"
for f in REPORT.txt REPORT-NGPC.txt NOTES.md LEARNINGS.md PIPELINE.md port.conf; do [ -e "$f" ] && cp "$f" "history/$OLD/$(basename "$f")"; done
[ -e original/README.md ] && cp original/README.md "history/$OLD/original-README.md"
git rm -q --cached REPORT.txt REPORT-NGPC.txt LEARNINGS.md 2>/dev/null || true
rm -f REPORT.txt REPORT-NGPC.txt LEARNINGS.md
rm -f src/ngpc/gen_* assets/rom.bin assets/font.bin dist/rom.bin dist/font.bin

python3 - "$GAME" "$OLD" <<'PY'
import sys, re
game, old = sys.argv[1], sys.argv[2]
s = open('PIPELINE.md').read()
if '## Part B' in s:
    open('PIPELINE.md', 'w').write(s[:s.index('## Part B')] + '## Part B: backlog for this port\n\n(Fill in after the load measurement and the first profile. '
                                   'The last port\'s backlog is in history/%s/PIPELINE.md; PLAYBOOK-NGPC.md section 8 lists what the tools cannot do yet.)\n' % old)
open('NOTES.md', 'w').write('# Notes: where the brief was unclear, wrong or missing something\n\n(Earlier ports: history/*/NOTES.md. Add one line for every new place.)\n')
t = open('src/ngpc/logic.c').read()
t = re.sub(r'(/\* BENCH INPUT BEGIN[^\n]*\n).*?(    /\* BENCH INPUT END \*/)',
           r'\1    (void)t;      /* TODO: script a way from power-on through the busiest scenes (PLAYBOOK-NGPC.md section 2) */\n\2', t, flags=re.S)
open('src/ngpc/logic.c', 'w').write(t)
open('tools/games/%s.py' % game, 'w').write('''"""%s.py - what the tools need to know about this game that they cannot find out by themselves.
Loaded by tools/recomp900.py (--game %s). tools/games/ntm.py is a filled-in example; PLAYBOOK-NGPC.md says how to find
each entry.
"""
EXTRA_ENTRIES = set()   # functions no trace reached (task and state routines stored as pointers)
NOT_WAITS = set()       # branches that look like a wait loop to the translator and are not
''' % (game, game))
c = open('port.ngpc.conf').read()
c = re.sub(r'^GAME=\S+', 'GAME=%s' % game, c, flags=re.M)
c = re.sub(r'^NAME=\S+', 'NAME=%s' % game, c, flags=re.M)
c = re.sub(r'^GEN="[^"]*"', 'GEN="src/ngpc/gen_dispatch.c src/ngpc/gen_ops.c src/ngpc/gen_00.c"', c, flags=re.M)
c = c.replace("# port.ngpc.conf - describes a Neo Geo Pocket Color port to tools/port.sh. tools/new_port_ngpc.sh copies it to\n# port.conf with the game's name.",
              '# port.conf - describes this Neo Geo Pocket Color port to tools/port.sh and to CI.')
open('port.conf', 'w').write(c)
open('original/README.md', 'w').write('''# The game to port

**(name, publisher, year)** for the Neo Geo Pocket Color. The cartridge image is `original/%s.ngc`; it is not in the
repository (copyrighted; `.gitignore` keeps it and everything made from it out). `original/bios.ngp` (optional,
also never committed) gives the BIOS's character set.

- Image size, how much of it holds data, colour or mono (tools/make_assets_ngpc.sh prints them):
- A way from power-on into play for the bench (input script):
- Controls:
- Load per scene (tools/ngpref -s -w ... -W ...): main CPU instructions per frame, sound program instructions per frame:
- Verdict for a stock A1200 and for 4 MB fast RAM (memory first, then speed):

The previous port's description is in history/%s/original-README.md.
''' % (game, old))
PY
mkdir -p original
cp "$ROM" "original/$GAME.ngc"
[ -z "$BIOS" ] || cp "$BIOS" original/bios.ngp
cat <<EOF2
new_port: ready for $GAME. Next, as in PLAYBOOK-NGPC.md section 2:
  1. tools/setup.sh; read PLAYBOOK-NGPC.md (all of it, the lessons log too)
  2. cc -O2 -o ngpref tools/ngpref/ngpref.c src/ngpc/k2ge.c; run the game, dump frames, find the way into play
  3. measure the load per scene (-s -p -w -W), check the memory, write the verdict BEFORE writing code
  4. trace (-c tools/flow_ngpc/${GAME}_<scene>.txt), tools/make_assets_ngpc.sh, tools/verify_ngpc.sh
  5. tools/port.sh fast, profile, then the display and the sound (PLAYBOOK-NGPC.md sections 7 and 8)
  6. at the end: REPORT.txt, and a lessons-log entry in PLAYBOOK-NGPC.md
EOF2
