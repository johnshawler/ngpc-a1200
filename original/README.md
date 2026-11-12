# The game to port

**Neo Turf Masters** (SNK, 1999), World release (English and Japanese).

- What is here: nothing in the repository. The image is copyrighted and this repository is public, so it is not
  committed (`.gitignore` keeps it out, and everything made from it: `assets/`, `dist/rom.bin`, `src/ngpc/gen_*`). To build, put the cartridge image here as
  `original/ntm.ngc` (`Neo Turf Masters (World) (En,Ja).ngc`, 2,097,152 bytes, SHA-1
  `308361297bd28ebb8fb60713ae5e00cee514415c`) and, if you have it, the console's BIOS image as `original/bios.ngp`
  (65,536 bytes, SHA-1 `edc13192054a59be49c6d55f83b70e2510968e86`; only its character set is used). Then
  `tools/make_assets_ngpc.sh`.
- Hardware: 2 MB colour cartridge, 1,544 KB of it data; TLCS-900/H at 6.144 MHz, Z80 sound processor, 60 Hz.
- Controls on the console: pad, A, B, Option. A tap on A every 140 frames from frame 1,560 goes from the title
  through the menus (1 player, stroke play, first golfer, first course) into play and keeps swinging: the bench
  uses it.
- Running it without a window:

      cc -O2 -o ngpref tools/ngpref/ngpref.c src/ngpc/k2ge.c
      ./ngpref original/ntm.ngc 3600 -b original/bios.ngp -d 600,1500,3300 -o /tmp -s -w 200133,200139

  Its `-c` option writes the trace in `tools/flow_ngpc/` that seeds the translation.

The NES game of the previous port is described in history/llb/original-README.md.
