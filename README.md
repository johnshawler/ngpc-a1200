# Amiga 1200 port template: Neo Geo Pocket Color

This repository is the template for porting **Neo Geo Pocket Color** games to the Amiga 1200, made from
[a1200-final](https://github.com/johnshawler/a1200-final) (the NES template) and keeping all of it. Its current port is
Neo Turf Masters: `REPORT.txt`, `PLAYBOOK-NGPC.md`. To start the next one see `KICKOFF-NGPC.md`. The NES port that
came before is in `history/llb/`.

A starting point for porting one game to a stock Commodore Amiga 1200 at full speed, set up so that a coding
model can do the port. It holds the instructions (`BRIEF.md`), a tested kit (`kit/`, `tools/`), and a referee
(`tools/port.sh`, also run by CI) that decides whether the port really holds 50 Hz.

## Use it

1. Create a new repository from this template with the **Use this template** button. One repository per port.
2. Put the game to port in `original/`: its source, its data, and a README saying how it runs.
3. Give a coding agent this prompt:

   > You are a coding agent working in this repository. `BRIEF.md` holds your instructions: read all of it
   > first, then follow it exactly. The game to port is in `original/`. Port it to the Commodore Amiga 1200.
   > Run `tools/setup.sh` once, describe the port in `port.conf`, and use `tools/port.sh` to build, bench and
   > check the gates. Do not change `kit/`. Keep `NOTES.md` with every place the brief was unclear or wrong.
   > When you finish, or are stuck at a gate after real attempts, write the PORT REPORT in the brief's format
   > to `REPORT.txt`.

4. The port is done when `tools/port.sh` prints `PASS` and `REPORT.txt` is written. CI runs the same script on
   every push and keeps `bench.txt` and a screenshot.

## Ports of console games (translated binaries)

Two kinds of game come with a playbook, tools and a way to start: both translate the original's machine code
once, on the host, and prove the translation against an interpreter tick by tick.

| Console | Start here | State |
| --- | --- | --- |
| NES / Famicom (6502) | `KICKOFF.md`, `PLAYBOOK-NES.md`, `tools/new_port.sh` | one port made: Little League Baseball (`history/llb/REPORT.txt`) |
| Neo Geo Pocket Color (TLCS-900/H) | `KICKOFF-NGPC.md`, `PLAYBOOK-NGPC.md`, `tools/new_port_ngpc.sh` | Neo Turf Masters in progress (`REPORT.txt`): correct on the emulated Amiga with fast RAM, music and effects from its own sound program, game time at 92% of the clock |

## What is here

| Path | What it is |
| --- | --- |
| `BRIEF.md` | The work brief: the frame budget, the rules, the four steps, reference code, how speed is proved |
| `CLAUDE.md`, `AGENTS.md` | Short pointers that make a coding agent read the brief first |
| `kit/` | Machine takeover and restore, AGA display (scrolling, tiles, blitter objects, sprites), frame-time proof, the game skeleton `main.c`, chunky-to-planar routines, the ptplayer music library |
| `tools/build.sh`, `bench.sh`, `run_uae.sh`, `gfx.py` | Cross-compile in Docker, run a bench on an emulated stock A1200, convert graphics and sound |
| `tools/setup.sh` | Installs the emulator and the compiler image (Ubuntu 24.04) |
| `tools/port.sh`, `check_gates.py`, `port.conf` | The referee: build, bench, host test, gates |
| `tools/check_kit.sh`, `tools/kit.sha256`, `KIT_VERSION` | Proof that the kit is unchanged |
| `examples/` | Four small programs built on the kit: scrolling, still screen, per-pixel, sound |
| `tools/ngpref/`, `tools/tlcs900.py`, `tools/recomp900.py`, `tools/back900.py`, `src/ngpc/` | Neo Geo Pocket Color: reference machine, CPU tables, translator with its 68k back end, the port's machine layer and frame model |
| `tools/sndrec_ngpc.c`, `src/ngpc/psg.c`, `src/ngpc/sndplay.c`, `src/ngpc/sound_ngp.c` | Neo Geo Pocket Color sound: the game's own sound program recorded per command on the host, mixed and played on Paula |
| `tools/nesref/`, `tools/recomp.py`, `tools/back68k.py`, `src/*.c` | NES: reference machine, translator with a 68k back end, the Little League Baseball port |
| `samples/` | Three small PC games for trying a model out |
| `original/`, `src/`, `assets/`, `dist/` | The game to port, the port's code, its converted data, the built program |

## The referee

`tools/port.sh` reads `port.conf`, builds the program, runs `<program> bench <TICKS>` on the emulated stock
A1200 (FS-UAE, cycle-exact 68EC020, 2 MB chip RAM, no fast RAM), compiles and runs the host test, and checks:

- `late=0` and `worst_pct` at most 80, unless `CLASS=perpixel`;
- the tick count;
- that the game-state checksum equals the host test's.

`tools/port.sh fast` repeats the run with 4 MB of fast RAM. As it comes, `port.conf` describes the unchanged kit
skeleton, so a fresh copy of this template passes:

    tools/setup.sh
    tools/port.sh
    frames=500 late=0 worst_pct=3 avg_pct=3 shown_fps=50.0 ticks=500 checksum=00235400
    PASS (50 Hz held)

`tools/setup.sh` puts a time limit on its downloads and tries them twice. If it still ends with
`setup: FAILED`, the machine or its network is at fault, not the port: run it again, or re-run the CI job.

## What has and has not been tested

- Everything in `kit/` and `tools/` ran on an emulated stock A1200 under the free AROS ROM. Nothing ran on a
  real A1200 or under a Commodore Kickstart ROM: the last word is `<program> bench 3000` on real hardware.
- Five trial ports of the `samples/` games were made by smaller models from the brief and kit alone; the
  results are in the brief's last section and in `samples/README.md`.
- The CI workflow has run on GitHub's hosted `ubuntu-24.04` runners: the bench there printed the same line and
  checksum as the local run. GitHub's own package mirrors were unreachable from the runner that day, so the run
  also went through the second download try in `tools/setup.sh`.
- The emulator charges too little time for multiply, divide and shift, which is why the gate is 80% and not 100%.

## Licence

MIT: see `LICENSE`. It covers everything in this repository except the two third-party parts below.

A repository made from this template gets a copy of that file. The copy still covers only what came from the
template. The game you put in `original/`, and your port of it, are governed by the game's own licence.

## Third-party parts

Both are public domain and keep their own terms.

- `kit/c2p/` is from Mikael Kalms' C2P collection: see `kit/c2p/kalms-readme.txt`.
- `kit/ptplayer/` is Frank Wille's ptplayer: see `kit/ptplayer/license`.
