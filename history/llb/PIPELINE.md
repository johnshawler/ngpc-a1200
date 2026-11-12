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
- Milestone 7 (68k back end) is not built for the TLCS-900/H: the Amiga runs the C translation, and the sound
  program runs through an interpreter there. Both are the first things to replace (playbook section 8).
- Milestones 10 to 14: a correct, slow picture exists (`src/ngpc/main.c`); a fast display and sound do not.

## Part B: backlog for this port, by expected gain per effort

Status after the fast-RAM work: 4 MB fast RAM 44.6 fps, stock 37.2 fps, checksum equal. Field play on 4 MB:
logic ~29k colour clocks per tick (×1.2 per frame), display ~25–35k per frame, 71k available, 57k at 80%.

| Priority | Item | Expected gain | Effort | How to know it worked |
| --- | --- | --- | --- | --- |
| 1 | Sprite clustering + channel assignment in assembly (~8k per frame in C) | −5k per frame | medium | `video.txt` grouping figure; no dropped clusters on `tools/vhost.c` |
| 2 | Tile streaming bookkeeping: tile-set base pointers per frame, no per-tile `chr_tile` call, a clear routine for blank border columns | −3–5k per frame | small | `video.txt` stream figure; harness screenshots unchanged |
| 3 | Next hand-written routines from the field-play profile: `F5_9800`, `F7_e209`, `F7_dfae`, `F7_e087`, then re-profile | −1–2k per tick each | medium each | checksum equal; `ticktime.bin` field figure |
| 4 | `$E298` metasprite loop as one routine around `$E2DE` (saves ~37 call/return pairs a tick) | −1k per tick | small | as 3 |
| 5 | Remaining run-time helpers: inline the ROM fast path of `(zp),y` reads | −1k per tick | small | as 3 |
| 6 | Measure a 68030 + fast RAM machine (FS-UAE `--accelerator` options) and report it | the honest "cheapest upgrade" number | small | a third bench line in `REPORT.txt` |
| 7 | Sprite-behind-background priority (masked by the background under it) | correctness, costs time | medium | harness screenshots of the scenes that use it |
| 8 | Listen to the sound on real speakers / the emulator with audio; tune volumes | quality | small | by ear |
| 9 | Play-test with a person: controls, menus, a full game | quality | small | a written note |
| 10 | Stock A1200 at 50 Hz in field play | not reachable by translation (LEARNINGS.md §3) | — | — |

Items 1–5 together are the plausible path to 50 Hz on 4 MB fast RAM in field play; item 6 tells whether an
accelerator gets there without them.
