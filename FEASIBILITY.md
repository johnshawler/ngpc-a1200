# Feasibility: Neo Turf Masters on the A1200, hardware-scrolled display (milestone 1)

This replaces the per-pixel display. The picture is no longer rebuilt and converted each frame: the course is drawn
once into bitplanes and moved with the scroll registers, the Pocket's front layer lives in hardware sprites, and only
what the game changes is drawn. Every figure marked *measured* comes from `tools/layers_ngpc.c` and
`tools/front_ngpc.c` (the translated game on the host, 3,600 ticks of the bench input) or from the bench records in
`PLAYBOOK-NGPC.md`. Figures from the kit's cost table are the brief's (Step 2). Figures marked *estimate* are not
measured and are checked at the milestone named.

## 1. Class and route (Step 1)

- Row: **"Assembly or disassembly for a Z80, 6502 or 6809 machine: translate the code once, on the host ... replace
  video, sound and input hardware with kit calls. 50 Hz."** The CPU is the TLCS-900/H (a Z80 descendant), translated
  by `tools/recomp900.py` as CLAUDE.md's Neo Geo Pocket section directs. **Decision 1** below: the brief's own table has
  no TLCS-900/H row, and what we start from is a cartridge image ("Binary only: ask the user before starting").
- Class: **planar**. Gates: `late=0`, `worst_pct` at most 80, equal checksum.
- Game time: the game runs 60 ticks a second and is a translated binary, so its constants cannot be rescaled:
  `TICKS_PER_SECOND 60` (the brief's "exact and quick" option; a small judder every fifth frame when scrolling).
- Gone: `k2ge_frame` (the per-frame picture), the C2P conversion, and the run-time interpreter (`asm_step`,
  `asm_interp`: the stepped instruction forms become native, untranslated code is found by tracing), so that
  `tools/check_rules.py` passes.

## 2. How the game uses its display (measured)

| What | Measured |
| --- | --- |
| Back plane (plane 2): the course | scrolls in 19% of changed pictures; in play mostly vertically, in the attract mode horizontally |
| Front plane (plane 1): HUD, text, mini-map, panels | always in front; scrolls in 3% of pictures; covers 37% of the pixels. In play: text top left, club boxes at the bottom, the mini-map column on the right (40 px, full height) |
| Sprites (64 of 8x8) | median 37 active, 95th 56, max 60; 96% in front of both planes. Grouped by touching: up to 7 objects a picture; by larger side 8 px 11%, 16 px 41%, 32 px 17%, 64 px 9%, larger 22%; largest 128 x 106 (the golfer at the swing) |
| Colours in one picture | median 24, max 54: course up to 28, front plane up to 30, sprites up to 18 |
| Map cells changed a tick | median 0, 95th 114, max 2,048 (a new screen) |
| Characters changed a tick | median 0, 95th 50, max 403 |
| Window narrowed (K2GE window register) | 12% of pictures |
| Logic | 22,782 colour clocks a tick averaged over the bench with the 68k back end, with 4 MB fast RAM: 38% of a frame at 60 ticks a second. Chip RAM only: not measured |

## 3. The display

```c
static const DispCfg display = { .planes = 6, .scroll = 1, .fetch = 1, .view_h = 152, .buf_h = 408,
                                 .level_w = 4096, .buffers = 3, .double_y = 0, .sprite_w = 64, .sprite_bank = 4 };
```

Step 2 row: **"Scrolling, up to 64 colours, with sprites"**: 6 planes, scroll 1, fetch 1, 3 buffers, sprites 0 to 4.

- **Playfield = the course (plane 2) plus the small sprite objects as `disp_bob`.** The 160 x 152 picture sits in the
  middle of the 320-pixel view; `view_h` 152 gives the black border above and below.
- **Vertical wrap.** The Pocket's plane is 256 x 256 and wraps. Buffer rows 0-255 hold the plane, rows 256-407 a copy
  of its rows 0-151, so every vertical scroll position is one `disp_scroll` and costs no drawing. Cells in rows 0-151
  are drawn twice.
- **Horizontal scroll** follows the kit's scrolling map: the scroll register's steps are added up into a running `x`
  (the picture slides through memory, `level_w` 4096). Course columns are drawn as they come into the 160-pixel window,
  and black columns where the window leaves, so the side borders stay black. At the end of `level_w`, `x` is set back
  and the visible 10 x 10 tiles redrawn over several frames (every 3,776 pixels of scrolling one way).
- **Course cells.** A Pocket cell is 8 x 8 with its own palette and flips; `disp_tile` draws 16 x 16 at x multiples of
  16. A changed cell makes its 16 x 16 tile be composed by the CPU from its four cells into a chip RAM scratch tile
  (a table per palette slot, 4 pixels a lookup), then drawn with `disp_tile`. At most **4 tiles a frame**; a scene
  change (up to 2,048 cells) is drawn over several frames, as the kit's scrolling map does with columns.
- **Front layer = the Pocket's plane 1 plus the sprite objects that must be in front of it, in hardware sprites 0-4.**
  Five 64-pixel sprites, 152 lines tall, cover the whole picture: attached pair 0+1 at x 96-159 (15 colours: the
  mini-map), attached pair 2+3 at x 32-95 (15 colours), sprite 4 at x 0-31 (3 colours: the text column). This is the
  brief's status-bar rule ("one 64-pixel sprite per 64 pixels of bar ... change digits by writing into the sprite
  data, only when the value changes"). A changed plane 1 cell is written into the sprite data by the CPU; where the
  front plane is see-through the sprites are colour 0 and the course shows. Each sprite has two data blocks, swapped
  at the flip, so a half-written cell is never shown.
- **Sprite objects.** Objects of the game's sprites that lie over the course only and are at most 32 x 32 (the ball,
  its shadow, the cursor, markers) are `disp_bob` objects; objects over opaque front-plane pixels (mini-map markers)
  and objects larger than 32 x 32 (the golfer at the swing, title pictures) are written into the front layer.
- **Colours.** Playfield: course up to 28 + bob objects up to 18 + background = 47 of 64, pens given out per palette
  slot (3 pens) for each scene; a palette change (the game's fades) is a `disp_palette` call. Front layer: one bank
  of 16 (`sprite_bank` 4, colours 64-79): 15 for the pairs, entries 9-11 also used by sprite 4.
- **The window register** (narrowed in 12% of pictures): the area outside is filled in the front layer with the
  outside colour when it changes.
- **Sound** stays as it is (the recorded sound program on Paula, ptplayer's effect channels).

## 4. Objects and sizes

| Object | Made of | Size | Count, busiest moment |
| --- | --- | --- | --- |
| Course | playfield, hardware scroll | 256 x 256, window 160 x 152 | 1 |
| Ball, shadow, cursor/target | `disp_bob`, 6 planes | 16 x 16 | 3 |
| Front layer: text, digits, club boxes, mini-map, mini-map markers | sprites 0-4, CPU writes into sprite data | 64 x 152 each | 5 sprites; at most 20 changed 8 x 8 cells a frame |
| Golfer at the swing (largest object) | front layer | up to 128 x 106 | 1, in the swing scene |
| Course tiles | `disp_tile` + composing | 16 x 16 | at most 4 a frame |

## 5. Budget (Step 2), percent of a frame

Busiest moment: **a shot in flight**: the course scrolls, the ball, shadow and cursor move, the distance digits and the
mini-map marker change.

| Item | Source | % |
| --- | --- | --- |
| Logic, 1.2 ticks a frame | measured, with fast RAM | 38 |
| 3 `disp_bob` 16 x 16, 6 planes | kit table 3.4 each | 10.2 |
| 4 tiles: `disp_tile` 2.0 + composing 1.4 (*estimate*, milestone 5) | | 13.6 |
| Front layer: 20 cells at 0.3 (*estimate*: 8 rows x 4 byte writes + lookups; milestone 7) | | 6 |
| 5 `disp_sprite` | kit table 0.2 each | 1 |
| Hardware scroll | | 0 |
| **Sum** | | **68.8** |

Under 80. The swing scene: logic 38 + the golfer redrawn in the front layer when its picture changes (about 110 cells
x 0.3 = 33, *estimate*) + 1 = 72, under 80 only if the golfer changes at most once a frame; more cells than that are
spread over the next frames (the golfer then shows a frame late).

The logic figure is with 4 MB fast RAM, the machine this port requires (decision 3).

## 6. Chip RAM (Step 2)

Display buffers: 3 x 6 x (64 x 408 + 4096 / 8 + 64) = 480,384 bytes (469 KB). Front-layer sprites: 5 x 2 x 154 x 16
= 24 KB. Tables and scratch: under 16 KB. Program now: 518 KB (shrinks when the interpreter goes). **The cartridge:
1,544 KB of its 2 MB are used, and the game reads it at run time.** Together about 2.5 MB: over the brief's 1.5 MB and
over the whole 2 MB of a stock A1200. Decided (decision 3): the port requires 4 MB fast RAM. The cartridge, the
program and its tables go in fast RAM (`MEMF_ANY`); chip RAM holds only the display buffers, sprites, the scratch
tile and the sound: about 520 KB.

## 7. What is cut, in the brief's order

1. Fewer planes: 6, not 8 (the per-pixel display used 8).
2. Front-layer colours: the Pocket's front plane and front sprites use up to 48 colours in one picture; the front
   layer has 15 (the pairs) and 3 (the 32-pixel text strip at the left). Colours are mapped to the nearest of those
   per scene. Visible: the HUD and the swing golfer lose shades.
3. Bursts are spread: a new screen (up to 2,048 cells), a front-plane scroll (3% of pictures: the whole front layer
   is rewritten) and the golfer's bigger frames take more than one frame to appear; frames are not late, the
   changed parts show up to a few frames after the game made them.
4. Raster effects stay unmodelled, as now.
5. Not cut: 50 pictures a second (the 25-a-second fallback is not planned).

## 8. Decisions for the user

1. **The route.** The brief has no TLCS-900/H row and says "Binary only: ask the user". This plan uses the Z80/6502/
   6809 row with the template's TLCS-900/H translator. Approving this plan approves that.
2. **The sprites go to the front layer**, so the golfer and the HUD are limited to one 15-colour bank (item 2 of the
   cuts). The other way, a dual playfield or a narrower display window, needs display set-up the kit does not have:
   writing it is under "Never do these" and is not in this plan.
3. **Memory: decided by the user, (a).** The game cannot fit a stock A1200 (section 6), so the port requires 4 MB
   fast RAM for the cartridge and the program. This departs from the brief's stock target with the user's word. The
   gates are judged on `tools/port.sh fast` (FS-UAE A1200, cycle-exact, plus 4 MB fast RAM), and the report says so;
   the stock bench is still run and reported as it comes out (the program cannot load the cartridge there).
4. **Order of work** (`PIPELINE.md`): first the interpreter out (rules), then milestones 5 (course on screen), 6
   (scrolling, `late=0` over the bench), 7 (front layer and objects), 8 (checksum), 9 (sound), 10 (3,000+ ticks).

To approve: `tools/approve.sh plan`, then commit `APPROVALS`.
