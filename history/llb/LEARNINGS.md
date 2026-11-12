# Learnings from porting Little League Baseball (NES) to the A1200

What worked, what did not, and the numbers behind it. Written for whoever continues this port. The general, reusable
part for other NES ports is in `PLAYBOOK-NES.md`. The brief's own rules are in `BRIEF.md`; where it was unclear or wrong is in
`NOTES.md`; the measured results are in `REPORT.txt`.

## 1. The route: translate, never emulate

- A NES ROM is a 6502 binary, so the brief's "6502 → translate once on the host" row applies, but a ROM gives no
  entry points. **Trace first**: `tools/nesref` is a small reference NES (6502, MMC1, scanline PPU) whose `-c`
  option logs every executed address, indirect jump targets, returns that did not go back to their caller, and
  calls into the switchable bank. These seed `tools/recomp.py`; everything reachable from them is decoded
  statically. Three traces (attract mode, "watch game", random 1P input) covered all code the bench runs.
- Things a ROM does that a translator must handle: jump tables after a `JSR` (the dispatcher pops its own return
  address: mark it as non-returning), `PLA PLA RTS` and pushed-address `RTS` tricks, data after branches whose
  outcome is fixed by the previous instruction, the same address meaning different code in different banks.
- **Keep the 6502 stack in NES RAM.** A JSR pushes its return address there *and* makes a native call; RTS
  compares what it pops with what the caller expects. When they differ, unwind to the frame that matches (or to
  the top level). This handles every stack trick without special cases, and the stack page stays byte-exact.
- **Frame model:** one game tick = the NMI handler, then the main thread until it next waits. Waits are loops
  that only read RAM or `$2002` (detect them statically: no writes, fixed addresses, every register redefined
  each pass). The main thread waits in the middle of subroutines, so it runs as a **coroutine** on its own stack
  (`src/coro.s`, `src/coro_host.c`), switched at those loops.
- The NES's own vblank race (`BIT $2002` polled while the NMI also reads it) has to be modelled
  deterministically; real hardware relies on luck. Sprite-0 hit waits are recognised statically and given a
  toggling flag that ends them in two reads and records the split.

## 2. Proving the translation

- Three independent implementations of the same program agreed byte for byte:
  1. `tools/nesref/verify.c`, an instruction-by-instruction interpreter with the same frame model,
  2. the C back end (`src/gen_*.c`, run by the host test),
  3. the 68k back end (`src/gen68k.s`, run on the Amiga).
  NES RAM after every tick, 20,000 ticks, interpreter vs C; checksum after every tick, 6,000 ticks, C vs 68k.
- **Find the first tick that differs, then dump RAM at that tick on both sides.** One differing byte plus a
  gdb watchpoint on the host shows the routine; reading its 68k translation shows the bug. Every 68k bug in this
  port was found that way within minutes (`tools/debug_run.sh` writes per-tick checksums and a RAM dump).
- Hand-written replacements of 6502 routines (`HLE` in `tools/back68k.py`) must reproduce *everything*:
  registers, all four flags, RAM, and the return addresses their subroutine calls leave on the stack page (the
  checksum covers all 2 KB of RAM). Give each a guarded fall-back to the translation (`.orig`).

## 3. Why C was too slow and assembly was not enough

- First C back end: correct, but every 6502 subroutine call copied all registers and flags through globals.
  A 3-instruction routine became 40 68k instructions. 444 KB of code, logic alone 155% of a frame.
- 68k back end: A/X/Y in d0/d1/d2, the 6502 stack as a pre-decrement address register, RAM and ROM banks
  addressed from base registers, **6502 N/Z = 68k N/Z and 6502 C = 68k X** (moves, tests and logic leave X
  alone), V in a data register, and a flag-liveness analysis (per function, plus what callers use after
  each call) to decide where a flag must be protected. 137 KB of code, logic ~100% of a frame on the stock
  machine. Tricks that mattered: `CMP` fused with the next `BCC/BCS`, `CLC; ADC` as a plain `add`, `INX` as a
  table lookup only when the carry is live, `cmpm.w` to check a return in one instruction.
- **The real limit is chip RAM.** On a stock A1200 straight-line code is fetched from chip RAM (the 256-byte
  cache only helps loops). Measured: 10–13 colour clocks per translated 6502 instruction. This game needs
  2,000 (batting) to 11,000 (fielding) 6502 instructions per NES frame, so fielding cannot fit a stock frame
  however good the translation is. With fast RAM the same code is roughly twice as fast.
- **4 MB and 8 MB fast RAM give identical results**: the gain is having code and data outside chip RAM.

## 4. Measuring: what to trust

- **Don't trust static cost estimates.** Weighting the interpreter's instruction profile by generated code
  size pointed at the wrong routines several times.
- **A vertical-blank profiler is biased**: 80% of its samples land in `sys_wait_frame`, because the interrupt
  fires exactly when frames end. Use a timer unrelated to the frame: `src/prof68k.s` samples the program counter
  at 1 kHz from the CIA-B timer (debug builds only, removed before `sys_give()`).
- Map samples with `m68k-amigaos-nm` and the runtime addresses of one symbol per hunk (vasm and gcc code land in
  different hunks; `nm` numbers them in one overlapping space). Export translated functions and run-time helpers
  as symbols so samples get names.
- Profile **the busiest scene only** (field play, ticks 2000–2500 of the bench), using the per-frame
  (vblank, tick) log to select samples. Whole-run profiles hide the problem behind idle frames.
- Per-tick logic time (`ticktime.bin`) and per-part display time (`video.txt`) from `tools/debug_run.sh` are the
  quickest before/after numbers. Remember the debug build runs more ticks per drawn frame than the real one.

## 5. What actually bought speed (4 MB fast RAM, field play, colour clocks per tick unless noted)

| Change | Effect |
| --- | --- |
| 68k back end instead of C | logic ~3x faster |
| Controller read `$CEBD` by hand (was ~20% of 6502 instructions: bit-by-bit `$4016` reads) | −11% logic |
| MMC1 bank switch by hand, then entirely in assembly (no C call) | −12% logic, −2k more |
| OAM DMA as one `memcpy` (was 256 calls of the generic memory read) | 36.5k → 32.6k |
| PPU nametable writes (`$2007`) in assembly | 30.5k → 29.1k |
| Multiply `$C378`, metasprite builder `$E2DE`, hashed dispatch, cheaper RTS | ~10% together |
| Display: tile and sprite inner loops in assembly | display halved |
| Display: reuse last frame's sprite shapes when unchanged (copy, not draw) | sprites 31k → 22k per frame |
| Display: one copy of each tile row instead of two; don't re-blank border columns | small |
| Screen changes drawn over a few more black frames | ~half the transition late frames |

The surprises: the two biggest single wins (OAM DMA, controller) were I/O plumbing, not game logic. Moving the
sprite compositing from chip RAM to fast RAM changed nothing: the cost was instruction count (~300
instructions per 8×8 sprite), not memory.

## 6. Display on AGA, for NES games

- 4 bitplanes hold the NES's 16 background colours with the attribute bits as planes 2–3, so a palette change
  is a colour-register write and the attribute planes need no pixel mask (colour 0 of every sub-palette is the
  same colour).
- Hardware scroll over an unwrapped world; NES tile columns are streamed in as the camera reaches them. The
  vertical wrap (240 lines) is done by keeping each tile row at y and y+240, drawing only the copy the scroll
  shows. Columns outside the 256-pixel NES picture are drawn blank (the NES never shows them; their nametable
  content belongs to the other side of the wrap). Honour PPUMASK's hidden left 8 pixels, or streamed columns show.
- NES sprites → hardware sprites: cluster them (≤ 64 px wide), put clusters in 64-pixel **attached pairs**
  (15 colours = all four NES sprite palettes) and reuse each hardware sprite down the screen by writing more
  control words into its list. 3 pairs + sprite 6 were enough: 1 dropped cluster in 6,000 frames.
- A raster split that only switches tile sets can be drawn row by row with the right tile set; one that changes
  scroll could not be shown with the kit.
- Write a host harness for the display (`tools/vhost.c`: fake kit calls, renders bitplanes + sprite lists to
  PPM). It found every display bug faster than the emulator, and it runs AddressSanitizer. Watch byte order:
  the Amiga is big-endian, so use explicit big-endian access where the host must read the same bytes.

## 7. Sound

- The NES APU is register-driven tone generation: model length counters, envelopes, sweep, the triangle's linear
  counter and the 240 Hz frame sequencer, then set Paula period/volume each frame with one-period looped
  waveforms (8-sample pulse per duty, 16-sample triangle, LFSR noise). `tools/shost.c` renders it to a WAV on the
  host. Pitches above Paula's fastest rate (period 124) are clamped.

## 8. Practical pitfalls

- vasm cannot relocate `bsr` to another object file: include the helper file in the generated one.
- Watch `bra.s` ranges when helpers grow, and label clashes between assembly helpers and C variables
  (`_mmc_prg` was both).
- `<proto/dos.h>` defines `TICKS_PER_SECOND` 50.
- Docker's daemon is not running after a session restart: `tools/setup.sh` starts it.
- Parallel emulator runs must not share a build name (`tools/shot_at.sh` builds `dist/llb_hold`).
- The repository is public: the ROM and everything derived from it (assets, generated code, sound renders) stay
  out of git (`.gitignore`); `tools/make_assets.sh` regenerates them from `original/llb.nes`.

## 9. What to try next

In the order I would do it, for field play on fast RAM (now ~29k logic per tick + 25–35k display per frame, 71k
available):

1. Sprite clustering and channel assignment in assembly (~8k per frame in C).
2. Cheaper tile streaming: precompute tile-set base pointers per frame, avoid the per-tile C bookkeeping, write
   blank border columns with a clear routine.
3. More hand-written game routines, chosen with the 1 kHz profiler restricted to field play
   (`F5_9800`, `F7_e209`, `F7_dfae`, `F7_e087` were next in line), each proven by the checksum.
4. A 68030 accelerator is the likely way to 50 Hz without rewriting the game; measure it.
