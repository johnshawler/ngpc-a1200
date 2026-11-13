# Neo Geo Pocket Color → A1200 porting playbook

The know-how for porting Neo Geo Pocket Color games to the Amiga 1200 with this template: the tools, the
machine, the method, what was measured and what is not done. **Read all of it before starting a port, and add
to it before finishing one** (section 9). The brief (`BRIEF.md`) and the referee (`tools/port.sh`) apply
unchanged; the order of work is `PIPELINE.md` part A. The NES playbook (`PLAYBOOK-NES.md`) holds the same
method for another machine and its lessons are worth reading too: the two share the frame model, the proof by
checksum and the profiler.

A new session remembers nothing but this file and the tools next to it. To start a port see `KICKOFF-NGPC.md`.

State of the tools (2026-11, on one game, Neo Turf Masters): a game can be run, looked at, measured, traced,
translated to C and to 68k assembly and proven equal to the interpreter; its sound program is replaced by a model of
what the game sees of it, proven against the real one; it runs on the emulated A1200 with fast RAM at its own size in
a black border, with its music and effects. It FAILS the brief: its display rebuilds each picture in software (the
per-pixel class, which the brief keeps for per-pixel renderers; 4.5 new pictures a second of the 13.7 the game makes,
late=1927 in the bench), and it interprets untranslated code at run time. The brief's route for a Neo Geo Pocket game
is a hardware-scrolled tile map with sprites and kit objects; that display is not built yet. Start a port with an
approved FEASIBILITY.md (CLAUDE.md, tools/check_rules.py).

## 1. What is reusable, what is per game

| Kind | Files | For a new game |
| --- | --- | --- |
| CPU | `tools/tlcs900.py` (decoder, formatter), `tools/dis900.py`, `tools/asm900.py`, `src/ngpc/cpu900.h` (interpreter and the primitives the translation is built from), `src/ngpc/cycles900.h` | use as is |
| Reference machine | `tools/ngpref/ngpref.c` (timed machine: CPU, Z80, timers, interrupts, BIOS calls in C, pictures), `src/ngpc/z80.h`, `src/ngpc/k2ge.c` | use; extend for what the game uses (section 8) |
| Translator | `tools/recomp900.py` (C), `tools/back900.py` (its 68k back end) with `src/ngpc/run68k.c` | use; add native instruction forms the report asks for |
| Port machine and frame model | `src/ngpc/ngp.{c,h}` (with the sound processor's model), `src/ngpc/gen900.h` (what the generated code is written in), `src/ngpc/logic.{c,h}`, `src/ngpc/run_c.c`, `src/coro.{h,s}`, `src/coro_host.c` | use; a game with another sound driver needs its own model (section 4) |
| Proof | `src/ngpc/run_interp.c`, `src/ngpc/host_test.c` (`-k` checks the renderers), `tools/verify_ngpc.sh`, `tools/check_sound_ngpc.sh` | use as is |
| Amiga side | `src/ngpc/main.c` (kit skeleton, the picture at 1:1 in a black border), `src/ngpc/k2ge.c` (`k2ge_frame_cached`) | use; a faster display is section 7 |
| Sound | `src/ngpc/psg.{c,h}` (the T6W28's registers), `tools/sndrec_ngpc.c` (records the game's own sound program per command, host), `src/ngpc/sndplay.{c,h}` (plays and mixes the recordings, host and Amiga), `src/ngpc/sound_ngp.c` (Paula), `tools/psgtrace_ngpc.c` and `tools/check_music_ngpc.sh` (how close it is) | use; a game with another driver may need other rules in `sndplay.c` (section 7) |
| Measuring on the Amiga | `tools/prof_ngpc.sh` (1 kHz profiler, names static functions too), `tools/long_bench.sh` | use as is |
| Starting a port | `tools/new_port_ngpc.sh`, `tools/make_assets_ngpc.sh`, `port.ngpc.conf`, `skills/amiga-ngpc-port/SKILL.md` | run once / install once |
| Coverage | `tools/explore_ngpc.sh` (random play on the host until nothing untranslated is reached), `src/ngpc/script_input.h`, `host_test -i script [-c flow]` | run until a round is clean |
| A floppy | `tools/make_adf_ngpc.sh` (bootable 880 KB image), `src/ngpc/loader.c`, `src/ngpc/unlzma.c`, `tools/test_adf_ngpc.sh` | run for the user; never commit the image |
| Per game | `tools/games/<game>.py`, `game_bench_input` in `src/ngpc/logic.c`, the flow files `tools/flow_ngpc/<game>_*.txt`, `original/README.md` | write new |
| Checks of the tools themselves | `tools/ngpref/check_tables.py`, `tools/ngpref/test_asm.py`, `tools/ngpref/README.md` | run after changing the tables |

## 2. Start of a port: the first day

1. `tools/new_port_ngpc.sh <game> <rom.ngc> [bios.ngp]`, `tools/setup.sh`. Neither image is ever committed.
2. Build the reference machine and look at the game:

       cc -O2 -o ngpref tools/ngpref/ngpref.c src/ngpc/k2ge.c
       ./ngpref original/<game>.ngc 1500 -b original/bios.ngp -d 300,600,900,1200,1500 -o /tmp/frames

   Find a way from power-on into play with an input script (`-i`: lines `first last BUTTONS`). Read the
   `uses:` line it prints at the end: it names what the game touched that the tools do not model.
3. **Memory first.** `tools/make_assets_ngpc.sh` prints how much of the cartridge holds data. The Amiga
   program loads the whole cartridge image, so that number plus the program must fit: a stock A1200 has about
   1.5 MB. If it does not fit, the port needs fast RAM whatever its speed: say so at once.
4. **Then the load, before any code**: find the wait loops with `-p <frames>` (the addresses where a quiet
   frame's instructions go, for both processors), then measure with them left out:

       ./ngpref ... -s -w <main wait loop addresses> -W <sound program idle loop addresses>

   Do it per scene. Write the numbers and the verdict (section 6) in the feasibility note.
5. **What does the game read from its sound processor?** `./ngpref ... -m` lists every instruction of the main
   CPU that touches the shared RAM or the ports 0xB8-0xBC. Neo Turf Masters reads two things: the answer byte and
   a tick counter (section 4). If a new game reads more, extend the model in `ngp.c` or give it its own.
6. Trace with `-c tools/flow_ngpc/<game>_<scene>.txt` over every scene you can reach (keep each input script next
   to its trace as `<game>_<scene>.in`; an empty one is the attract mode), then `tools/make_assets_ngpc.sh`,
   `tools/explore_ngpc.sh` (random play until it finds no more untranslated code), `tools/verify_ngpc.sh <ticks>`
   and `tools/verify_ngpc.sh <ticks> tools/flow_ngpc/<game>_<scene>.in` for each script (the translation must
   equal the interpreter after every tick) and `tools/check_sound_ngpc.sh <ticks>` (the sound model must equal the
   real sound program as the game sees it). Only then the Amiga.
7. `tools/port.sh fast` (and `tools/port.sh` if the game fits a stock machine), then `tools/prof_ngpc.sh` to see
   where the time goes. `tools/long_bench.sh` runs a port that is still too slow for the referee's time limit.

## 3. The machine, as far as a port needs it

Sources: MAME's `ngp.cpp`, `tmp95c061.cpp` and TLCS-900 core (BSD-3-Clause) and Mednafen's NGP module, both read
as documentation; the opcode tables were compared with MAME's entry by entry and the pictures with Mednafen's
(section 5 of `tools/ngpref/README.md`). Nothing was checked on a real console.

| Part | Facts |
| --- | --- |
| CPU | Toshiba TMP95C061: TLCS-900/H core. Eight 32-bit registers (XWA XBC XDE XHL in four banks, XIX XIY XIZ XSP), flags S Z H V N C, little-endian, 24-bit addresses. 515 CPU states per scanline, 199 lines: 102,485 states a frame, 59.95 frames a second |
| Memory | `0x000000` CPU registers and console ports (256 bytes); `0x004000` 12 KB work RAM; `0x007000` 4 KB shared with the Z80; `0x008000` K2GE (16 KB); `0x200000` cartridge, 2 MB; `0x800000` second 2 MB of a 4 MB cartridge; `0xFF0000` BIOS, 64 KB |
| Cartridge header | `0x00` 28 bytes " LICENSED BY SNK CORPORATION" or "COPYRIGHT BY SNK CORPORATION"; `0x1C` start address; `0x23` 0x10 = colour, 0 = mono; `0x24` 12 bytes of title |
| Interrupts | The BIOS owns the CPU's vectors and jumps through a table in RAM at `0x6FB8`, 4 bytes each: entry 5 (`0x6FCC`) vertical blank, 6 the Z80, 7-10 timers 0-3. Levels are in the registers `0x71` (vertical blank low nibble, Z80 high), `0x73` (timers 0, 1), `0x74` (timers 2, 3). An interrupt is taken when its level is 1-6 and not below the CPU's mask |
| Vertical blank | At line 152, if bit 7 of `0x8000` is set. Before the game's handler runs, the BIOS copies the controller into `0x6F82`: bit 0 up, 1 down, 2 left, 3 right, 4 A, 5 B, 6 Option |
| Timers | Four 8-bit timers (`0x20` run bits, `0x22 0x23 0x26 0x27` compare values, `0x24 0x28` clock select, `0x25` flip-flop control). Timer 0 can count horizontal blanks: that is how games make raster effects. Timer 3's flip-flop output is the Z80's interrupt |
| BIOS calls | The game loads the call number into `RW3`, parameters into other registers of bank 3, reads the routine's address from the table at `0xFFFE00` and calls it (or uses `SWI 1`). Numbers: 0 power off, 1 clock gear, 2 read date and time, 4 set an interrupt level, 5 copy the system font into character RAM, 6-8 flash write and erase (saved games), 9 and 11 alarm, 13 flash protect, 14 video mode, 16-26 serial link |
| Start state | What the BIOS leaves when it starts a game is in `ngp_init()` (`src/ngpc/ngp.c`): stack at `0x6C00`, header copies in RAM, all RAM vectors at the BIOS's own "return from interrupt", K2GE defaults, interrupt levels |
| K2GE | 160 x 152. Two scroll planes of 32 x 32 tiles (`0x9000`, `0x9800`: 9 bits tile, palette, two flip bits), 64 sprites of 8 x 8 (`0x8800`, palette numbers at `0x8C00`; chain bits make large objects, 2 bits of priority against the planes), 512 tiles of 2 bits a pixel at `0xA000`. 16 palettes of 3 colours + transparent for the sprites and for each plane (`0x8200`, `0x8280`, `0x8300`, 12 bits a colour), a background colour, a window (`0x8002`-`0x8005`). Scroll offsets `0x8032`-`0x8035`, plane order bit 7 of `0x8030`, sprite offset `0x8020/21`. The exact drawing rules are `src/ngpc/k2ge.c` |
| Sound | A Z80 at half the CPU clock with its program in the shared 4 KB (the game copies it there, then starts the Z80 by writing 0x55 to `0xB9`). It drives a T6W28 (three square waves and noise, each with a left and a right volume) at its addresses `0x4000/0x4001`; `0x8000` is a byte shared with the main CPU (`0xBC` there), a write to `0xC000` interrupts the main CPU, a write of the main CPU to `0xBA` gives the Z80 an NMI. Two 8-bit DACs at `0xA2/0xA3` play samples |

## 4. What the frame model must get right

One tick is one Neo Geo Pocket frame. `src/ngpc/logic.c`: the main thread runs until it waits for the next
frame; there the frame ends. The next tick starts with the controller copy, the vertical blank handler, the
sound processor's frame of time, and the interrupts it raised.

- **Frame waits.** A loop that only re-reads memory can only be ended by an interrupt handler: the translator
  finds these loops by itself (`wait_kind` in `tools/recomp900.py`), also `HALT`. Check its list
  (`src/ngpc/gen_waits.h`) against the profile of a quiet frame (`ngpref -p`).
- **Polls.** A loop that re-reads the same memory and counts a register down is waiting for the *other
  processor* with a timeout. Neo Turf Masters does this for every answer of its sound program. There the sound
  processor gets a slice of time; after 4,096 polls in a row the frame ends.
- **The sound processor is part of the logic, as events.** It answers the game through shared RAM, so the game
  cannot run without it. The frame model gives it time in fixed amounts (a frame, a poll); that time decides when
  its timer interrupts fall, and what it does in answer to an event (started, a timer interrupt, an NMI from the
  main CPU) is done at once. On the console that takes some hundred microseconds and the game waits anyway.
- **A model of the sound driver, not a Z80.** All the main CPU of Neo Turf Masters sees of its sound program is:
  a command byte written to 0xBC with an NMI is answered with its complement; command 0xFF restarts the driver
  (answer 0, then one interrupt to the main CPU); every timer interrupt advances a counter at 0x70DE. `ngp.c`
  implements exactly that (read from the Z80 code; SNK's driver, so other SNK games may share it).
  `tools/check_sound_ngpc.sh` builds the host test once with the model and once with the real sound program on the
  Z80 interpreter (`-DNGP_Z80_INTERP`) and compares everything the main CPU can see after every tick: equal for
  3,600 ticks. A game with another driver: `ngpref -m` says what it reads; write its model the same way and prove
  it the same way. The interpreter build is the fallback (on the host only: on the Amiga it cost 249 colour
  clocks a Z80 instruction, 86% of the logic).
- **Check that the real sound program is alive** in the interpreter build: `host_test N -v` prints its
  instructions per tick. A first version of the model let the game's first NMI hit the Z80 before it had run its
  start-up code; it crashed silently and only a strange profile (addresses 0x0038 and 0xFFFF) showed it.
- **Lag.** With the reference machine, check that the original finishes its work within a frame in the busy
  scenes (`-s`: busy states stay under 102,485). If it does not, game time on the console is slower there and
  the frame model would need cycle budgets. Neo Turf Masters: at most 65% of a frame in the bench.
- **Routines stored as pointers.** Games keep task lists and state machines as code addresses in RAM and call
  through them. The translator only knows the targets the traces saw. On the host a target it does not know
  stops the run with reason 2 and the address, so gaps show; the Amiga program stops the same way (the brief
  forbids interpreting the original CPU at run time: `tools/check_rules.py` fails a port that does). A jump
  table entry may point at the instruction right after the jump: the tracers log those too.
- **Coverage is what makes a port playable.** The bench's trace was enough for the bench and nothing else: left
  at the title, Neo Turf Masters reached untranslated code within a minute (its attract mode), and 8 of 8 random
  ten-minute input scripts stopped. `tools/explore_ngpc.sh` plays seeded random scripts through the translation
  on the host; each one that stops is replayed with the interpreter build (`host_test N -i script -c flow`: the
  port's own frame model, so it traces exactly what the port reaches) and added to
  `tools/flow_ngpc/<game>_explore.txt`, then it translates again. For Neo Turf Masters: 583 functions from the
  bench, 740 with an idle, a tapping and a random ten-minute script, 976 after 26 rounds of exploration, when 1 in
  30 random ten-minute scripts still found something new, late in the run. What is still missing stops the game,
  so keep exploring until the scripts stop finding anything.
- **The display on the hardware** (`src/ngpc/hwdisp.c`, the brief's route; check it on the host with
  `tools/hwdisp_test.c`, which models the kit's display and compares every picture with `k2ge.c`'s): the back plane
  is a hardware-scrolled playfield of 16 x 16 tiles (world x = W0 + the summed scroll steps; buffer rows 256-407 repeat
  rows 0-151, so vertical wrap is free); columns outside the window are cleared to black with `disp_clear` (black
  tiles cost too much when the game jumps its scroll a whole screen); the front plane and the sprites over it are five
  64-pixel sprites in three strips, one of them 3 colours (the right strip measured best for Neo Turf Masters); small
  objects are `disp_bob`. Things the game does that this had to handle: wipes through the window register (mark only
  the cells whose inside/outside changed; a blank window costs only a palette change), a BIOS call that writes the
  font into character RAM (it must mark the characters dirty, `ngp.c`), the front plane scrolling 1 pixel every 12
  ticks on the title (a whole front-layer redraw each time).
- **Instructions with no inline form** (DIV, CPIR, EX with memory, PUSH/POP SR, LDF, RETI, calls and jumps
  through registers in the C back end) are one C function each, `op_<address>()` in `gen_ops.c`, written by
  `recomp900.op_body` from the decoded instruction with `cpu900.h`'s primitives and called by both back ends.
  They decode nothing at run time; `verify_ngpc.sh` proves them like the rest.
- **Not modelled in the frame model yet**: interrupts of the main CPU's timers (so no raster effects), the
  blanking flag and raster position at `0x8010`/`0x8009` (they read 0), micro DMA.
- The checksum covers the 16 KB of RAM, the K2GE's memory, the ports and the registers.

## 5. The translator

- **Model**: the CPU's stack stays in the game's RAM. `CALL` pushes its return address and calls the target's
  C function, which returns the address where execution goes on. A caller that gets back another address than
  its own continuation returns it upward. A jump to another function is a tail call; a jump through a register
  is a `switch` over the targets the traces saw at that instruction.
- **What the C is written in** (`src/ngpc/gen900.h`): registers by their address through a current-bank pointer
  (`cpu.cur`, kept by `CPU_RFP_SET()` in `cpu900.h`), so a byte or word register is one memory access on either
  byte order; RAM, the cartridge, the stack and video memory without side effects (0x8800-0xBFFF) read and
  written directly; everything rarer through `ngp.c`. The registers are reached as bytes and words of a long:
  build with `-fno-strict-aliasing`.
- **Flags live in locals and only the read ones are computed.** Liveness per function, plus two facts per
  function that are a fixed point over the whole program: which flags it reads on entry (`in_use`) and which its
  callers read after it returns (`ret_live`). Flags go through `cpu.sr` only at calls, returns, the step and
  waits (an interrupt pushes SR into RAM, so all six are exact there). A game that unwinds its stack in odd ways
  can set `PRECISE_RET = False` in its file (every return then stores all flags).
- **Every form mirrors `cpu900.h`.** An instruction form without native code runs through the interpreter's
  `cpu_step()` on the spot (`STEP`): always right, slower. The translator prints how many and which.
- **Proof**: `tools/verify_ngpc.sh N` builds the host test twice from the same machine and frame model, once
  with the generated code and once with the interpreter, and compares RAM, the K2GE, the ports and the registers
  after every tick. It prints the first tick and place that differ. Run it after every change of the translator.
  Setting `NATIVE = False` in the game file sends every instruction through the step: if the difference goes
  away, the bug is in a native form; if not, it is in the control flow. The Amiga is big-endian and the host is
  not: the Amiga checksum against the host's is the proof of the byte order.
- An address register of `(-r)` and `(r+)` moves *before* the operation (as in `cpu900.h`): the generated
  code computes the address first for the same reason. `LDIR` and its kind re-execute themselves.
- **What it costs** (section 6): the registers are in memory, so gcc reloads them after every store. The 68k back
  end below keeps them in 68k registers.

**The 68k back end** (`tools/back900.py`, run by `recomp900.py` unless `NGP_NO68K=1`; it writes `src/ngpc/gen68k.s`,
whose C side is `src/ngpc/run68k.c`). The same functions, the same liveness, the same frame model, in 68k assembly.
`port.conf` chooses it with `BACKEND=68k` (the default) or the C translation with `BACKEND=c`.

- **Registers**: d4-d7 = XWA, XBC, XDE, XHL of the current bank, a2-a5 = XIX, XIY, XIZ, XSP, d3 = the flags in the
  68k condition codes' bit order (C, V, Z, S in bits 0-3, H in 5, N in 6: one `move.w ccr,d3` saves the four the
  68k computes), a6 = work RAM - 0x4000; d0, d1, d2, a0, a1 are scratch. A function returns the next address in
  d0, as in C. Byte and word registers are rotated into the low end of their long and back.
- **Everything else goes to C with the registers written out** (`SYNCOUT`, `asm_regs_out`) and read back: waits,
  BIOS calls, `SWI`, unknown addresses, and every form without 68k code, which runs through the interpreter's step
  (`asm_step`). The generator prints how many sites and which forms.
- **Registers named by their bank** (`XWA1`, `XDE2`: TLCS-900 games use the other banks as extra globals) are
  longs in `cpu.bank`, read at fixed addresses (`_cpu+offset`; `run68k.c` checks the layout when it compiles).
  Only when that bank is the current one are they in d4-d7: one pointer compare per straight-line run of such
  instructions chooses, and the rare case (d4-d7 written out, the same code, read back) is out of line. Neo Turf
  Masters runs bank 0 for all but 99 of 1.8 million instructions.
- **Memory**: an access at an address known when translating goes straight to RAM, the cartridge or the video
  memory routine; one through a register has the work-RAM case inline and calls the routine (`rd8`...`wr32` in the
  file's header) for everything else. Writes to 0x8800-0xBFFF go through `vidw`, which marks the changed cell or
  character as `ngp_vidw` does. Values with bits 24-31 set take the routine, which masks them.
- **Proof**: as for the C. The host test is the reference; the Amiga's checksum must equal it (`tools/port.sh`),
  and `tools/debug_ngpc.sh <dir> N` finds the first tick where they differ (a build with the checksum after every
  tick, compared with `host_test N -V`). A profiling build exports every function's label (`-DNGP_PROF` for vasm),
  so `tools/prof_ngpc.sh` names `F_<address>` and the runtime routines.
- **Block copies are one call** (`asm_ldir` in `run68k.c`): the whole block, straight from the cartridge or RAM into
  maps, sprites and characters (whole characters compared and copied as four longs), element by element for anything
  else. libnix's `memcmp` and `memcpy` were slower than that on 16 bytes. A game's scene changes are mostly such
  uploads: Neo Turf Masters' heaviest ticks copy 2,500 words into character memory with one `LDIRW`.
- **Count the step by executions, not by sites.** 50 executions a tick of four stepped `LD XWA2,#imm` cost a third
  of the logic: each step writes out and reads back every register and decodes in C. A host build of the
  interpreter that counts executions per address (as `run_interp.c` with a counter) finds them in seconds.

## 6. The verdict, before writing code

Two numbers decide: the memory, and the instructions per frame of *both* processors outside their wait loops.

| Neo Turf Masters, measured on the reference machine | |
| --- | --- |
| Cartridge data | 1,544 KB of a 2 MB image: does not fit a stock A1200 at all |
| Main CPU, per frame, outside its wait loop | median 1,700-3,900 instructions (9-21% of the console's CPU), 90th percentile about 5,200, 13,100 at scene changes |
| Sound program, per frame, outside its idle loop | median 1,500-4,200 instructions while music plays, up to 6,200; 4 interrupts a frame. The game reads only two of its answers: a model replaces it (section 4) |
| Interrupts of the main CPU | vertical blank only |

What it costs on the emulated A1200 with 4 MB fast RAM, step by step (logic: colour clocks a tick over the first 900
ticks of the bench; a frame has 71,051, and 60 ticks a second need 1.2 a frame):

| State | Logic a tick |
| --- | --- |
| C translation, registers through generic functions, sound program on the Z80 interpreter | 764,273 |
| The sound processor as events, and a model of its driver instead of the Z80 (section 4) | 178,759 |
| Flags in locals with liveness, registers by address, direct memory (`gen900.h`) | 43,478 |
| Video memory written directly by the translated code | 31,325 |
| 68k back end, first version (fixed-bank registers, `EX` and calls through registers stepped) | 114,951 |
| ... `EX`, `LD` with a fixed bank, calls through registers native | 84,885 |
| ... fixed-bank registers as ordinary operands (still spilling d4-d7 around each) | 61,995 |
| ... video memory written by `vidw` in assembly, not through C | 32,576 |
| ... addresses known when translating go straight to memory | 30,251 |
| ... one pointer compare per run instead of spilling d4-d7 | 28,456 |
| ... the work-RAM case inline at every access through a register | 26,879 |
| ... `LDIR`/`LDDR` as one call for the whole block (`asm_ldir`: uploads into video memory directly) | 22,009 |

Over the whole 3,600-tick bench, logic averages 35,184 colour clocks a tick in C and 22,782 with the 68k back end: at
60 ticks a second that is 59% and 38% of the machine before any picture is drawn. The console's CPU does up to 16,000 instructions a frame and they are 16-
and 32-bit instructions: it is not a slower processor than a 14 MHz 68EC020. Plan for fast RAM and say so in the
report from the start.

## 7. Display and sound: what exists, what to build

**Controls: the same in every port** (`read_input` in `src/ngpc/main.c`, the user's rule). The Neo Geo Pocket's
button 1 (A) is the Amiga joystick's button 1 (fire) and Z on the keyboard; its button 2 (B) is the joystick's
button 2 (second fire) and X. The stick and the cursor keys are the pad; Space or Return is Option; Esc quits. The
kit's `JOY_FIRE` (16) and `JOY_FIRE2` (32) are the port's `IN_A` and `IN_B`, so the joystick needs no mapping; the
raw key codes are 0x31 for Z and 0x32 for X. A game that labels its buttons differently keeps this mapping: write
what the buttons do in its report, not a different layout. This applies to Neo Turf Masters.

**Exists: the picture at its own size in a black border** (`src/ngpc/main.c`, the user's choice for Neo Turf
Masters; doubled it would need 304 lines, which a PAL screen does not have). 8 planes: the K2GE's 256 palette
entries are the 256 pens, so a colour change is a palette load and nothing is redrawn. The picture is drawn into a
160 x 152 byte buffer and converted with the kit's 1x1 chunky-to-planar routine, one row at a time, into the middle
of the 320 x 256 screen. Pictures are drawn only when the game changed the K2GE's memory, and converted only into a
buffer that does not show them yet.

The renderer (`k2ge_frame_cached` in `src/ngpc/k2ge.c`) keeps both scroll planes drawn in 256 x 256 pen buffers and
redraws only the cells and characters the game changed (the writes mark them: `ngp_vidw` in `ngp.h`). A picture is
then 160-pixel windows of the two planes laid over each line four pixels at a time, with the sprites between, back
to front. `host_test -k` proves it, and `k2ge_frame_idx` (the same picture without the cache), equal to
`k2ge_line_idx` (the reference) after every tick.

| Measured (fast RAM, 3,600-tick bench) | Colour clocks a picture |
| --- | --- |
| `k2ge_line_idx` line by line (first version) | 670,310 (900 ticks) |
| `k2ge_frame_idx`, lookup tables, long writes | 406,621 (900 ticks) |
| `k2ge_frame_cached` | 276,441 drawn (305 of them), conversion 126,203 (329) |
| ... only lines whose inputs changed, opaque back plane copied, empty front plane skipped; only rows written since that screen buffer got them converted | 181,138 drawn (381), conversion 71,372 (464) |

A line is drawn again only if its inputs changed: the registers every line reads, the plane buffer rows it shows
(marked by `draw_cell`), and its sprites (entry, number, row, palette, x, compared with the last picture's). Each
plane buffer row keeps a bit per cell column for "all opaque" and "all see-through": an opaque back plane is a copy,
an empty front plane is skipped. `k2ge_line_gen[]` counts the writes of each line, so `main.c` converts into a screen
buffer only the rows written since that buffer got them. In Neo Turf Masters characters change in 90% of the
pictures (about 46 each, so 90-170 cells are redrawn), which is now the largest part of a picture (`draw_cell`).
Only cells the 160 x 152 view shows are redrawn; a changed cell outside it is kept in a bit mask per cell row and
drawn when it scrolls in (a plane is 32 x 32 cells, the view about 21 x 20). At a scene change that halves the
redraw: the worst pass of the bench went from 1.71 to 1.31 million colour clocks.

**Game time against pictures.** The kit's loop gives a pass at most 8 frames of game time; a pass whose ticks are
heavy and that also draws a picture goes over and loses the rest. `game_draw` (ours, not the loop) skips the new
picture in a pass whose ticks took over 2 frames, at most 8 passes in a row; the conversion still runs, so the
screen never shows an older picture than before. Neo Turf Masters: game time from 83% to 92%, new pictures drawn
from 373 to 239 over the bench. Read `shown_fps` with care here: the kit counts passes, and a pass that draws nothing
still shows the newest picture; the number of new pictures is `pics=` (drawn) in the bench note, and
`host_test N` prints how many ticks changed the picture on the console (`pictures=`). A model of the loop on the host (the instructions and video
changes per tick from the interpreter, costs from the bench) chose the rule; it predicts game time too high (it
uses average costs) but ranks the choices right.

Tried and dropped: a cache of decoded characters for `draw_cell` (per character and mirroring, dropped when the game
changes it) made no difference in Neo Turf Masters (182.8k against 182.3k colour clocks a picture): its characters
change about as often as the cells that show them are drawn.

Next for the display, by expected gain: `draw_cell`, the overlay and sprite loops in assembly;
a planar display per game in the brief's sense (one scroll plane hardware-scrolled, the other as stamps or as a
dual playfield the kit does not have yet, sprites as hardware sprites clustered as `src/video.c` of the NES port
does). Raster effects (timer 0 on horizontal blanks) need the frame model to deliver timer interrupts and the
display to change registers down the screen: neither exists.

**Sound: the game's own sound program, recorded.** The Z80 cannot run on the Amiga: Neo Turf Masters' driver
averages 3,110 instructions a frame (more than the game's main CPU, 2,076), so even translated it would cost more than
the whole logic. What it plays is recorded on the host instead and played back:

- **What a command plays depends on more than the command byte.** The game copies the driver into the sound
  processor's 4 KB at power-on, and before each new song a block of song data (0x0AB5-0x0FFF in Neo Turf Masters),
  then restarts the driver (0xFF) and sends the command (often 0x03). A recording is keyed by the command and a hash of
  the 4 KB at that moment (`sound_key`). The model in `ngp.c` hands every command to `ngp_snd_hook`.
- **The recorder** (`tools/sndrec_ngpc.c`, run by `tools/make_assets_ngpc.sh`) plays the bench with the model as the
  Amiga does and collects the keys; for each one it starts the real program on a private Z80 from that 4 KB, sends
  the command, gives timer interrupts at the game's rate (about 245 Hz) and takes the sound chip's state 50 times a
  second. It stops where the driver's state repeats (the music's loop: the stream loops there) or where it goes quiet
  for good (an effect ends). The state never repeats byte for byte: tempo and envelope accumulators (0x00D5,
  0x00D6, 0x0680, 0x06A2 ...) run on; bytes that change at more than half the interrupts are left out of the
  comparison, and a repeat must be at least 2 s apart. Neo Turf Masters' songs loop after 17-44 s; the bench's
  13 keys take 19.6 KB.
- **The chip keeps its registers across driver restarts.** A stream holds only the bytes the program wrote; the
  others stay as the chip had them (the noise period of one song is used by the next).
- **Mixing** (`src/ngpc/sndplay.c`, the same code on the host and the Amiga): music (a stream that loops, lasts over
  3 s or uses three voices) and effects; an effect covers the music on its voices, and when it ends that voice
  stays silent until the music's next note there, as SNK's driver does. A command that sounds nothing by itself is
  replayed over the music the game played when it sent it: Neo Turf Masters' 0x01 stops everything, 0xC8 fades
  everything out over 89 frames.
- **Paula** (`src/ngpc/sound_ngp.c`): at every vertical blank (the kit's `sys_vbl_hook`) one frame of state; a
  two-sample square for each tone (Paula period = 18.47 x the T6W28 period), the T6W28's own 15-bit noise sequences
  for the noise (36.94 x its period), the louder of each voice's left and right volume (the T6W28 volume steps are
  Paula's 64, 50, 39, ... 0), tones 0 and noise on the left, tones 1 and 2 on the right.
- **How close**: `tools/check_music_ngpc.sh` runs the bench twice on the host, once through the player and once with
  the real program on the Z80, and compares the chip state 50 times a second. Neo Turf Masters: 80-92% of the frames
  where a tone sounds are identical, 93-98% within 3 frames and one volume step (a 245 Hz envelope sampled at 50 Hz
  from another phase); the noise 55% and 93%. On the Amiga the sound costs about 2% of the game speed.
- **What it cannot do**: a command the recorder never saw (a scene the bench does not reach) plays nothing; the
  Amiga counts those (`sound_missing`). Trace more scenes into the bench input, or a second input script, and run
  `make_assets` again. Effects that depend on what the driver was doing (a song's tempo change, an effect that
  continues a melody) are played from a fresh driver. DAC samples (ports 0xA2/0xA3) are not played.

**A floppy for the user** (`tools/make_adf_ngpc.sh [out.adf] [command]`). A 2 MB cartridge does not fit an 880 KB
disk; LZMA packs Neo Turf Masters' to 486 KB, and with its 500 KB program packed too (to 131 KB, behind a 12 KB
loader, `src/ngpc/loader.c`, that unpacks it in memory and loads it with `InternalLoadSeg`) the disk is 77% full.
The cartridge is unpacked by the program itself (`src/ngpc/unlzma.c`, written for this template from the format's
description and checked against Python's `lzma` on 13 streams). The disk boots straight into the game: Kickstart
3.1, an A1200 with 4 MB fast RAM, about a minute to load from the floppy and 54 s to unpack. With the free AROS ROM
the unpacking ran out of memory in one piece (2 MB): test floppies with a real Kickstart. `tools/test_adf_ngpc.sh
<ticks>` makes an image that runs the bench, boots it with no hard disk and reads `bench.txt` back from the disk:
Neo Turf Masters gives the host's checksum after 600 ticks. The image holds the user's cartridge: it goes to the
user only. Unpacking was 103 s with the decoder's state in a struct; keeping it in locals (registers) halved that.

**Kickstart.** The brief measures with the free AROS ROM. A Kickstart ROM the user supplies goes in `original/`
(ignored by git) and is used with `KICKSTART=original/<file>.rom tools/port.sh fast` (or `tools/test_adf_ngpc.sh`).
Neo Turf Masters on Kickstart 3.1 (40.68): the same checksum and the same bench numbers as on AROS within 0.2%.

## 8. Not done yet (do it when a game needs it; the tools say when)

- **Speed of the logic.** The 68k back end still calls a routine for every access that is not work RAM, keeps
  bank registers in memory, saves flags with `move.w ccr,d3` after every instruction whose flags are read, and
  steps `DIV`, `LDF`, `CPIR` and a few `PUSH`/`POP`/`EX` forms (25 sites in Neo Turf Masters, none hot). The next
  gains: flags left in the CCR until they are needed, the display (below) before any of it.
- **Speed of the display** (section 7).
- **Coverage.** Random play still finds untranslated code about once in five hours of play; it then runs through
  the interpreter on the Amiga (slower there, and against the brief's rule, which is why it is counted). Better
  input for `tools/explore_ngpc.sh` (playing whole rounds, not random presses) would close more of it.
- **Sound beyond the recordings** (section 7): commands the bench never sends are silent; stereo is folded (a
  voice on both sides plays on one); DAC samples are not played.
- **Memory.** The whole cartridge image is loaded. A game that should run on a stock machine needs its data cut
  down to what it reads (the reference machine could log that; it does not yet).
- **The profiler build hangs on long runs** (3,600 ticks with fast RAM did not finish in 15 minutes; 900 ticks
  work). Profile up to 900 ticks, or find the cause in `src/prof68k.s` (its buffer fills after 65 seconds).
- Main CPU timer interrupts and raster effects, micro DMA, the serial link, the real-time clock (a fixed date
  is returned), flash writes (saved games are accepted and forgotten), the mono mode of games without colour,
  code a game copies to RAM, `SWI` other than 1.
- Running the real BIOS code in the reference machine (only its character set is used). The start state and
  the system calls follow what other emulators document.
- A free test cartridge, so that CI could build something without a copyrighted image. `tools/asm900.py` is
  there to write one.
- `tools/ngpref` has no sound output and no check against a real console.

## 9. Lessons log (append one entry per port, newest last)

Each entry: game, sizes, verdict numbers, what was new, what to do differently next time. Then update the
sections above if a lesson is general.

### 1. Neo Turf Masters (SNK 1999), 2 MB colour cartridge, 2026-11: bringing the tools up

- This entry is the first game the tools ran, not a finished port: `REPORT.txt` has the state.
- New: every tool in section 1.
- The game would not get past its logo without a Z80: it polls shared RAM for the sound program's answer
  with a 32-bit countdown. The sound processor is part of the logic, not an extra.
- With the Z80, the reference machine plays from power-on through the intro, the menus and a hole of stroke
  play on taps of A alone (the bench input). Two of its pictures were compared with the packaged Mednafen
  running the same image: 0 of 24,320 pixels differ in both.
- Translation: 583 functions, 13,882 instructions, 98% of the non-flow instructions as native C; equal to the
  interpreter after every one of 3,600 ticks (work RAM, shared RAM and K2GE memory compared each tick).
- Bugs the proof found, each in minutes: `LDIR` executed once instead of to the end; a jump table whose entry
  points at the next instruction was missing from the trace.
- Verdict from day-one numbers: the cartridge data alone (1,544 KB) rules out a stock A1200 (the program
  says "not enough memory for the cartridge" and ends); the two processors together need several thousand
  translated instructions a frame.
- On the emulated A1200 with 4 MB fast RAM the checksum equals the host test's after 300 ticks (fa876f99) and
  after 900 (544160df). Logic costs 205,074 colour clocks a tick over the first 300 ticks and 764,273 over the
  first 900: 0.9 and 0.3 pictures a second with the first display, which itself takes 25 to 30 frames a picture.
- The frame model had the Z80 crash silently at the game's first NMI (it had not run its start-up code yet in
  the model). It showed only as a strange load: 7,300 Z80 instructions every tick, at addresses 0x0038 and
  0xFFFF. Fixing it made the Amiga logic 7 times cheaper. The game's own state looked fine all along: the
  checksum cannot show a fault that both sides share.
- Second session: the sound program out of the logic (a model of its protocol, proven against it), the C
  translation rewritten (flags in locals with liveness, registers by address), video memory written directly,
  the picture at 1:1 in a black border with a cached renderer, a profiler that names static functions. Logic
  from 764,273 to 31,325 colour clocks a tick over 900 ticks (24 times less), a picture from 25-30 frames to about
  6. Referee: PASS as a per-pixel game, 8.7 pictures a second, game time at 74% of the clock, checksum equal.
- What paid most, in order: the sound processor (the game needed two of its answers, not its 2,900 instructions a
  tick); flags computed only where read; the game's own copies into video memory taking the generic path.
  Each time the profiler pointed at it, not a guess.
- Next time: do the memory check and the two load numbers before anything else (they took minutes once the
  tools existed); run `ngpref -m` on day one: what the game reads from its sound processor decides between a
  model and a translation; profile before optimising (`tools/prof_ngpc.sh`); build the 68k back end before judging
  speed.
- Third session: the 68k back end (`tools/back900.py`). Its first version was slower than the C (114,951 against
  31,325 colour clocks a tick) because everything it could not do went through the interpreter's step with all
  registers written out and read back; counting the step's executions per address on the host, then making those
  forms native, brought it to 26,879 (900 ticks), 14% below the C. Referee: 10.6 pictures a second, game time at 80%
  of the clock, checksum equal. What paid, in order: registers of other banks as operands (the game uses banks 1
  and 2 as globals while running in bank 0), video writes in assembly, then a pointer compare instead of spilling
  d4-d7, addresses known when translating, and the work-RAM case inline.
- A model of the kit's loop on the host (the instructions and video changes of each tick from the interpreter, a
  cost per instruction and per picture from the Amiga bench) predicted the bench's game time within 3 points and
  says what to speed up: here logic and display count about equally.
- Next time: write the 68k back end with the bank guard, static addresses and inline RAM from the start; count
  stepped executions on the host before the first Amiga run; build the loop model before choosing what to speed up.
- Sound, same session: the game's own driver, recorded per (4 KB, command) on the host and mixed on Paula
  (section 7). The first try keyed recordings by the command byte alone and failed at once: the same command 0x03
  played four different songs, because the game uploads each song's data before it. Two driver habits had to be
  copied into the mixer before the comparison with the real driver reached 93-98%: a voice an effect used stays
  silent until the music's next note, and chip registers survive a driver restart.
- Next time: compare with the real thing from the first stream (`tools/check_music_ngpc.sh`); every rule in the
  mixer came from a difference it showed.
- Floppy and coverage, same session (the user asked for an ADF): the first image booted and stopped at the title
  within a minute, in code no trace had reached. Tracing an idle run, longer taps and random presses, then
  `tools/explore_ngpc.sh` (random scripts on the host, the stopping ones traced with the port's own interpreter),
  took the translation from 583 to 976 functions. A scripted proof (`tools/verify_ngpc.sh N script`) then found a
  bug the bench never touched: the interpreter read a 24-bit address as 32 bits, putting the next opcode byte in
  the top byte of `LDA`'s result (`cpu_rd` size 3, in `cpu900.h`, shared with `ngpref`). Untranslated code that
  play still reaches runs through the interpreter on the Amiga instead of ending the game.
- Next time: trace the attract mode and a long session of varied input on the first day, and verify the
  translation over those scripts as well as the bench; a bench path proves only the bench path.
