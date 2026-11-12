# Development pipeline, reprioritised

What this port taught (see `LEARNINGS.md`) changes the order of work. Part A replaces the milestone order of
BRIEF.md Step 4 for ports of translated binaries (NES/6502 and similar); the gates and the referee
(`tools/port.sh`) are unchanged. Part B is the prioritised backlog for this port.

## Why the order changes

- The deciding number came last in the brief's order: how busy the original CPU is. It is measurable on day one
  with a reference emulator, and it alone decided that 50 Hz on a stock machine was out of reach.
- Correctness tooling paid for itself many times over: every translator bug was found in minutes by "first tick
  that differs, then a RAM dump". Built late, every bug would have been a guess.
- The cheapest large wins were I/O plumbing (OAM DMA, controller, mapper, PPU writes), found only with an
  unbiased profiler. Static cost estimates pointed the wrong way several times.
- Display bugs were found on the host harness, not in the emulator.

## Part A: milestone order for a translated-binary port

| # | Milestone | Gate | Was |
| --- | --- | --- | --- |
| 1 | Reference emulator of the original machine, headless, scripted input, frame dumps | Frames match the real game's screens | new |
| 2 | **Load measurement**: original CPU instructions per frame by scene (exclude wait loops) | Number per scene, written into the feasibility note | new |
| 3 | Feasibility note with the load: instructions/frame × 10–13 colour clocks (stock, chip RAM) or ×5–6 (fast RAM) + the display budget of Step 2, against 57k (80%) | Honest verdict per machine *before* code is written | 1 |
| 4 | Kit skeleton builds and benches | `late=0` | 2 |
| 5 | Traces → static translation to C; frame model (waits, NMI, coroutine) | Translated C equals an instruction interpreter tick by tick for ≥ 20,000 ticks | 3 |
| 6 | Asset script (ROM split, translation) with no manual step | `tools/make_assets.sh` from the ROM alone | 4 |
| 7 | 68k back end (registers in registers, 6502 flags in 68k flags, liveness) | Amiga checksum = host checksum for every tick of the bench | 8 (moved up) |
| 8 | Debug tooling on the Amiga: per-tick checksum and RAM dump, per-tick time, **1 kHz CIA profiler**, per-frame (vblank, tick) log | Profile of the busiest scene names its top routines | new |
| 9 | I/O fast paths and hand-written hot routines, chosen by the profiler, each guarded and proven by the checksum | Logic per tick in the busiest scene within its share of the budget | new |
| 10 | Host display harness (fake kit calls → PPM) | Screens match the reference emulator's | new |
| 11 | Background, scrolling, splits | Harness screenshots, then emulator screenshots, match | 5, 6 |
| 12 | Sprites (hardware first, multiplexed if needed) | No dropped clusters on the harness over the bench | 7 |
| 13 | Display cost: profile the display in the busiest scene, cut instruction count (not memory traffic) | Display + logic under 80% in the busiest scene | new |
| 14 | Sound | Still the same `late` | 9 |
| 15 | Final bench on stock and fast RAM | The brief's gates; report | 10 |

Rules that come with it:
- Measure before optimising, and only in the busiest scene. Re-measure after every change; keep the checksum
  equal after every change.
- If milestone 3 says the stock machine cannot hold 50 Hz, say so in the report early and target the cheapest
  upgrade explicitly. Don't spend days on the stock machine.

### Part A for a Neo Geo Pocket Color port

The same order, with these differences (details in `PLAYBOOK-NGPC.md`):

- Milestones 1 and 2 exist as tools (`tools/ngpref`): running them on a new game takes minutes. Milestone 2
  measures **two** processors (main CPU and the Z80's sound program) and is preceded by a memory check: how
  much of the cartridge holds data.
- Milestone 5's gate is `tools/verify_ngpc.sh`; milestone 6 is `tools/make_assets_ngpc.sh`.
- Milestone 7 (68k back end) is `tools/back900.py`: the Amiga runs 68k assembly made from the same functions and
  liveness as the C (`BACKEND=c` in `port.conf` builds the C instead). The sound program does not run on the Amiga:
  a model of what the game sees of it does (playbook section 4).
- Milestones 10 to 13: a correct picture exists (`src/ngpc/main.c`), not yet at 50 Hz. Milestone 14 (sound) is the
  recorder and player of playbook section 7.

## Part B: backlog for this port (Neo Turf Masters), by expected gain per effort

Status: referee PASS as a per-pixel game with 4 MB fast RAM, game time at 95% of the clock, 4.5 new pictures a second
(the game makes 13.7),
checksum equal; music and effects recorded from the game's own sound program (93-98% of sounding frames as the real
driver within 3 frames); the stock machine cannot hold the cartridge. Logic (68k back end) 22,751 colour clocks a
tick over the bench (38% of the machine at 60 ticks a second), a picture 181,138 to draw and 71,372 to convert. A
model of the kit's loop fed with the bench's instructions per tick and average costs predicts 95% (measured 84%:
the lost time is at scene changes, where heavy ticks meet full redraws, which averages hide). The previous port's
backlog is in history/llb/PIPELINE.md.

| Priority | Item | Why |
| --- | --- | --- |
| 1 | The display: `draw_cell`, overlay and sprite loops in assembly | 250k colour clocks a picture |
| 2 | The 68k back end: flags kept in the CCR until read, a word routine for video memory (map cells are words) | logic is 38% of the machine |
| 3 | Sound: more input scripts so the recorder sees every song and effect; stereo | commands the bench never sends are silent |
| 4 | Ticks that take several frames alone (scene changes: 16,000 instructions and 7,600 video bytes in one tick, 9.5 frames): video writes and copies faster | the rest of the lost game time |
| 5 | Trace the rest of the game (other courses, modes) | untraced code stops the port |
| 6 | Why the profiling build does not finish 3,600 ticks | profiles of play need it |
