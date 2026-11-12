# Notes: where the brief was unclear, wrong or missing something

(The NES port's notes are in history/llb/NOTES.md. One line for every new place.)

## Found while making the Neo Geo Pocket Color tools (2026-11)

- The brief's toolchain section says the Docker image holds m68k-amigaos-gcc 6.5.0b; the image pulled today reports 16.2.1b. The kit skeleton still benches the same line and checksum with it.
- The brief's memory check assumes assets are converted and small. A Neo Geo Pocket cartridge is 1-4 MB and the translated code reads its data in place: Neo Turf Masters holds 1,544 KB of data, more than a stock A1200 has free. The brief has no row for "the game's data does not fit the machine".
- The Step 1 table has no row for a console with two processors. The Neo Geo Pocket's Z80 runs the sound program, and games wait for its answers: it belongs to the logic pile, not to "sound".
- The brief's per-pixel row gives c2p for 160 x 128 doubled. The kit's 1x1 routine cannot place a narrower picture inside a 320-pixel row (its init ignores the x offset and row length), so a 160 x 152 picture is shown with the 2x1 routine, two Amiga pixels per pixel, in a 320 x 152 view.
- `tools/bench.sh` allows ticks / 25 + 60 seconds. A port whose logic is still slower than the clock (below the 6 pictures a second where the skeleton lets game time fall behind) cannot finish its bench in that time: `tools/long_bench.sh` takes a time limit.
- `tools/port.sh` reads `./port.conf` only. With two kinds of port in one repository it now takes `PORT_CONF=<file>`; the default is unchanged.
- `-Isrc -Isrc/ngpc` finds `src/logic.h` (the NES port's) before `src/ngpc/logic.h` for a file outside `src/ngpc/`. The Neo Geo Pocket sources include their own headers by being in that folder; scripts outside it must put `-Isrc/ngpc` first.
- The referee's per-pixel gate (tools/check_gates.py) did not look at game time, and its PASS line said "logic by the clock" whatever happened. The brief makes game time the first requirement. The bench now reports game_speed (ticks against vertical blanks) and the PASS line says it when game time falls behind.
- The kit's perf_note() keeps 128 characters: a second perf_note() replaces the first. The port puts its two extra lines in one note.
- gcc 16 for the 68020 turns a 4-byte memcpy() into four byte moves and keeps no local symbols in the executable; a profiler needs the linker map to name static functions (tools/prof_ngpc.sh).
- The Step 1 table has no row for "the original's second processor only has to answer, not to compute": Neo Turf Masters' Z80 runs 2,900 instructions a tick, and the game reads two bytes of what it does. A model of the answers replaced it (PLAYBOOK-NGPC.md section 4).
