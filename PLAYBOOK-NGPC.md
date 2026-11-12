# Neo Geo Pocket Color → A1200 porting playbook

The know-how for porting Neo Geo Pocket Color games to the Amiga 1200 with this template: the tools, the
machine, the method, what was measured and what is not done. **Read all of it before starting a port, and add
to it before finishing one** (section 9). The brief (`BRIEF.md`) and the referee (`tools/port.sh`) apply
unchanged; the order of work is `PIPELINE.md` part A. The NES playbook (`PLAYBOOK-NES.md`) holds the same
method for another machine and its lessons are worth reading too: the two share the frame model, the proof by
checksum and the profiler.

A new session remembers nothing but this file and the tools next to it. To start a port see `KICKOFF-NGPC.md`.

State of the tools (2026-11, after bringing them up on one game, Neo Turf Masters): a game can be run, looked
at, measured, traced, translated to C and proven equal to the interpreter on the host, and built and run on the
emulated A1200. What a finished port still needs is listed plainly in section 8: a fast display, sound, and
faster code than the C translation gives.

## 1. What is reusable, what is per game

| Kind | Files | For a new game |
| --- | --- | --- |
| CPU | `tools/tlcs900.py` (decoder, formatter), `tools/dis900.py`, `tools/asm900.py`, `src/ngpc/cpu900.h` (interpreter and the primitives the translation is built from), `src/ngpc/cycles900.h` | use as is |
| Reference machine | `tools/ngpref/ngpref.c` (timed machine: CPU, Z80, timers, interrupts, BIOS calls in C, pictures), `src/ngpc/z80.h`, `src/ngpc/k2ge.c` | use; extend for what the game uses (section 8) |
| Translator | `tools/recomp900.py` | use; add native instruction forms the report asks for |
| Port machine and frame model | `src/ngpc/ngp.{c,h}`, `src/ngpc/logic.{c,h}`, `src/ngpc/run_c.c`, `src/coro.{h,s}`, `src/coro_host.c` | use as is |
| Proof | `src/ngpc/run_interp.c`, `src/ngpc/host_test.c`, `tools/verify_ngpc.sh` | use as is |
| Amiga side | `src/ngpc/main.c` (kit skeleton, first display) | replace the display (section 7) |
| Starting a port | `tools/new_port_ngpc.sh`, `tools/make_assets_ngpc.sh`, `port.ngpc.conf`, `skills/amiga-ngpc-port/SKILL.md` | run once / install once |
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
5. Trace with `-c tools/flow_ngpc/<game>_<scene>.txt` over every scene you can reach, then
   `tools/make_assets_ngpc.sh` and `tools/verify_ngpc.sh <ticks>`: the translation must equal the interpreter
   after every tick. Only then the Amiga.
6. `tools/port.sh fast` (and `tools/port.sh` if the game fits a stock machine). `tools/long_bench.sh` runs a
   port that is still too slow for the referee's time limit.

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
  processor* with a timeout. Neo Turf Masters does this for every answer of its sound program, and without a
  running Z80 it never gets past its logo. There the sound processor gets a slice of time; after 4,096 polls in
  a row the frame ends.
- **The sound program is part of the logic.** It answers the game through shared RAM, so it must run in the
  host test and on the Amiga. It idles in polling loops that use its whole processor. `ngp.c` skips that time
  by itself: when the Z80 comes back to the head of a loop in exactly the state it had there before, with
  nothing written by either processor, it would go round until something changes. Skipping moves the sound
  processor's own variables slightly (its interrupts arrive at another point of the loop); over the 3,600-tick
  bench of Neo Turf Masters the game's RAM and the K2GE's memory were identical with and without it
  (`-DNGP_NO_IDLE_SKIP` builds without).
- **The frame model has no clock inside a frame.** The game starts the Z80 and soon sends it an NMI; on the
  console the Z80 has run its start-up code by then. In the model it has not run at all, took the NMI with no
  stack and crashed, silently: the game went on. So before an NMI the model lets the Z80 run until it idles.
  Check that the sound program is alive: `host_test N -v` prints its instructions per tick, and a profile of
  it must show its idle loop, not address 0x0038 and 0xFFFF.
- **Lag.** With the reference machine, check that the original finishes its work within a frame in the busy
  scenes (`-s`: busy states stay under 102,485). If it does not, game time on the console is slower there and
  the frame model would need cycle budgets. Neo Turf Masters: at most 65% of a frame in the bench.
- **Routines stored as pointers.** Games keep task lists and state machines as code addresses in RAM and call
  through them. The translator only knows the targets the traces saw: trace every scene you can reach; a target
  it does not know stops the port with reason 2 and the address (put it in `EXTRA_ENTRIES`, or trace that
  scene). A jump table entry may point at the instruction right after the jump: the tracer logs those too.
- **Not modelled in the frame model yet**: interrupts of the main CPU's timers (so no raster effects), the
  blanking flag and raster position at `0x8010`/`0x8009` (they read 0), micro DMA.
- The checksum covers the 16 KB of RAM, the K2GE's memory, the ports and the registers.

## 5. The translator

- **Model**: the CPU's stack stays in the game's RAM. `CALL` pushes its return address and calls the target's
  C function, which returns the address where execution goes on. A caller that gets back another address than
  its own continuation returns it upward. A jump to another function is a tail call; a jump through a register
  is a `switch` over the targets the traces saw at that instruction.
- **One place for every flag.** The generated C is built from the primitives of `src/ngpc/cpu900.h`
  (`cpu_alu`, `cpu_rd`, `cpu_setr`, ...): the same functions the interpreter uses. An instruction form the
  translator has no native code for is run by the interpreter's `cpu_step()` on the spot (`STEP`): always
  right, slower. The translator's report lists those forms; add the frequent ones to `native()`, each mirrors
  its case in `cpu900.h`.
- **Proof**: `tools/verify_ngpc.sh N` builds the host test twice from the same machine and frame model, once
  with the generated code and once with the interpreter, and compares RAM and K2GE memory after every tick.
  It prints the first tick and offset that differ. Run it after every change of the translator.
  Setting `NATIVE = False` in the game file sends every instruction through the step: if the difference goes
  away, the bug is in a native form; if not, it is in the control flow.
- An address register of `(-r)` and `(r+)` moves *before* the operation (as in `cpu900.h`): the generated
  code computes the address first for the same reason.
- `LDIR` and its kind re-execute themselves: the step is repeated until the address moves on.
- What the C translation costs on the Amiga is in section 6. Registers go through memory in it: the NES port
  needed a 68k back end for the same reason (`tools/back68k.py` is the worked example).

## 6. The verdict, before writing code

Two numbers decide: the memory, and the instructions per frame of *both* processors outside their wait loops.

| Neo Turf Masters, measured on the reference machine | |
| --- | --- |
| Cartridge data | 1,544 KB of a 2 MB image: does not fit a stock A1200 at all |
| Main CPU, per frame, outside its wait loop | median 1,700-3,900 instructions (9-21% of the console's CPU), 90th percentile about 5,200, 13,100 at scene changes |
| Sound program, per frame, outside its idle loop | median 1,500-4,200 instructions while music plays, up to 6,200; 4 interrupts a frame |
| Interrupts of the main CPU | vertical blank only |

What an instruction costs on the emulated A1200 with 4 MB fast RAM, as the tools are now (from two benches of
different mix, 300 and 900 ticks, solved for the two unknowns):

| | Colour clocks each | A frame has 71,051 |
| --- | --- | --- |
| Main CPU instruction, C translation (registers in memory) | about 53 | 2,000 instructions a tick are 1.5 frames |
| Sound program instruction, through the Z80 interpreter | about 249 | 2,600 instructions a tick are 9 frames |

So today the sound program is 86% of the logic time, and the logic alone is ten times too slow. Neither number
is the end of the road: the interpreter is a stopgap, and the NES port's 68k back end ran its translated code
three times faster than its C back end. What a 68k back end and a translated sound program reach on this
machine has not been measured. The console's CPU does up to 16,000 instructions a frame, and they are 16- and
32-bit instructions: it is not a slower processor than a 14 MHz 68EC020, so expect a game that keeps it busy
to need more than a stock A1200, and say so in the report from the start.

## 7. Display and sound: what exists, what to build

**Exists: the first display** (`src/ngpc/main.c`). It computes every pixel from the K2GE's memory
(`k2ge_line_idx`: the 256 palette entries are the 256 pens of an 8-plane screen) and converts with the kit's
C2P routine, each pixel two Amiga pixels wide. It is right for every game the renderer is right for, and far
too slow to play: it exists so that the Amiga build can be checked against the reference pictures early
(`CLASS=perpixel` in the port description: no 50 Hz gate).

**To build for a real port (not done, design only):**

- The picture is 160 x 152 in a 320 x 256 screen: shown 1:1 it covers a quarter of the screen; doubled it
  would need 304 lines, which a PAL screen does not have. Decide with the user: 1:1 with a border, or double
  width only (distorted), or double both ways and lose 24 lines.
- A planar display in the brief's sense, per game, from what the reference machine shows the game doing:
  which plane scrolls, how many palettes each layer uses at once, how many sprites, whether priorities matter.
  One scroll plane as the hardware-scrolled playfield with tiles streamed in (the 32 x 32 map wraps: keep an
  unwrapped copy, as the NES port did); the other plane as stamps when it stands still (a status display) or
  as the second playfield of a dual-playfield screen (the kit has none: that is kit work); sprites as hardware
  sprites, clustered into attached pairs and reused down the screen (`src/video.c` of the NES port does
  exactly this and is the code to start from). 16 palettes of 3 colours per layer do not fit 15 sprite colours
  unless the game uses few of them at once: measure it.
- Raster effects (timer 0 on horizontal blanks) need the frame model to deliver timer interrupts and the
  display to change registers down the screen: neither exists.

**Sound (not done).** The Amiga build runs the sound program but nothing listens to its writes to the sound
chip (`ngp_io[0xA0]`, `ngp_io[0xA1]` hold the last bytes). The route: decode the T6W28 writes into four voices
(three squares, noise) and play them on Paula with looped one-period waveforms, as `src/sound.c` does for the
NES's sound chip; each voice's left and right volume must be folded into Paula's fixed left and right
channels. DAC samples need a sample player on a Paula channel.

## 8. Not done yet (do it when a game needs it; the tools say when)

- **Speed.** The translation is C with the registers in memory, and the sound program runs through the Z80
  *interpreter* (`src/ngpc/z80.h`) on the Amiga too. The second is against the brief's rule and is only there
  so that the logic is complete and provable: the next step for any port is to translate the sound program
  (it is 4 KB and never changes) and to give the main CPU a 68k back end. Section 9 has the measured cost.
- **A fast display and sound** (section 7).
- **Memory.** The whole cartridge image is loaded. A game that should run on a stock machine needs its data
  cut down to what it reads (the reference machine could log that; it does not yet).
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
- Next time: do the memory check and the two load numbers before anything else (they took minutes once the
  tools existed); look at what the sound program executes in the port's frame model, not only on the reference
  machine; translate the sound program before trying the Amiga; build the 68k back end before judging speed.
