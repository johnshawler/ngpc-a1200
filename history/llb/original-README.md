# The game to port

**Little League Baseball: Championship Series** (SNK, 1990) for the NES/Famicom, USA release.

- What is here: nothing in the repository. The ROM is copyrighted and this repository is public, so it is not
  committed (`.gitignore` keeps it out, and everything derived from it: `assets/`, `src/gen_*`, `src/gen68k.s`).
- To build, put the iNES file here as `original/llb.nes`
  (`Little League Baseball - Championship Series (USA).nes`, 262,160 bytes, SHA-1
  `45564d865b65fabea7687b3e6fe6512c1129a0ab`), then run `tools/make_assets.sh`.
- Hardware: 128 KB PRG ROM + 128 KB CHR ROM on an MMC1 board (mapper 1), 6502 at 1.79 MHz, NTSC, **60 Hz**.
- Controls on the NES: pad, A, B, Select, Start. Modes: Championship series, Exhibition (1P, 2P, watch game),
  power analysis. "Watch game" plays computer against computer: the bench uses it.
- Running it without a window: `tools/nesref/nesref.c` is a reference NES built for this port. It runs the ROM
  headless with scripted input and dumps frames:

      cc -O2 tools/nesref/nesref.c -o nesref
      ./nesref original/llb.nes 3000 -i tools/flow/watch_input.txt -d 1000,2000 -o /tmp

  Its `-c` option writes the traces in `tools/flow/` that seed the translation.

## Neo Geo Pocket Color: the game the tools were brought up on

**Neo Turf Masters** (SNK, 1999), World release (English and Japanese).

- What is here: nothing in the repository, for the same reason. To build, put the cartridge image here as
  `original/ntm.ngc` (`Neo Turf Masters (World) (En,Ja).ngc`, 2,097,152 bytes, SHA-1
  `308361297bd28ebb8fb60713ae5e00cee514415c`) and, if you have it, the console's BIOS image as `original/bios.ngp`
  (65,536 bytes, SHA-1 `edc13192054a59be49c6d55f83b70e2510968e86`; only its character set is used). Then
  `PORT_CONF=port.ngpc.conf tools/make_assets_ngpc.sh`.
- Hardware: 2 MB colour cartridge, 1,544 KB of it data; TLCS-900/H at 6.144 MHz, Z80 sound processor, 60 Hz.
- Controls on the console: pad, A, B, Option. A tap on A every 140 frames from frame 1,560 goes from the title
  through the menus (1 player, stroke play, first golfer, first course) into play and keeps swinging: the bench
  uses it.
- Running it without a window:

      cc -O2 -o ngpref tools/ngpref/ngpref.c src/ngpc/k2ge.c
      ./ngpref original/ntm.ngc 3600 -b original/bios.ngp -d 600,1500,3300 -o /tmp -s -w 200133,200139

  Its `-c` option writes the trace in `tools/flow_ngpc/` that seeds the translation.
