/* main.c - the Amiga side of a Neo Geo Pocket Color port: the kit's skeleton (kit/main.c) around the translated
 * game. The loop in main() is the kit's; keep it as it is.
 *
 *   game             play; Escape quits
 *   game bench N     play N logic ticks from scripted input, quit, write bench.txt (the proof of speed)
 *
 * The display shows the Neo Geo Pocket's 160 x 152 picture at its own size in the middle of a black screen, on the
 * A1200's hardware (FEASIBILITY.md, src/ngpc/hwdisp.c): the course is a hardware-scrolled playfield of tiles, the
 * front plane and the sprites in front of it are five hardware sprites, small objects are blitter objects. Only what
 * the game changed is drawn; nothing is converted.
 * Sound: the streams tools/sndrec_ngpc.c recorded from the game's own sound program (dist/sound.bin), played on
 * Paula at every vertical blank (src/ngpc/sound_ngp.c). Without sound.bin the game is silent.
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <exec/memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sys.h"
#include "disp.h"
#include "perf.h"
#include "ngp.h"
#include "logic.h"
#include "k2ge.h"
#include "sound_ngp.h"
#include "unlzma.h"

#define TICKS_PER_SECOND_DOS 50         /* AmigaDOS's clock (DateStamp ticks) */
#undef TICKS_PER_SECOND                 /* <proto/dos.h> has its own */
#define TICKS_PER_SECOND 60             /* the Neo Geo Pocket shows 60 frames a second: 6 ticks every 5 frames */

#include "hwdisp.h"

/* ---- the game ------------------------------------------------------------------------------ */
#ifdef NGP_PROF                         /* tools/prof_ngpc.sh: a 1 kHz sampling profiler (src/prof68k.s) */
extern void prof_install(void), prof_remove(void); extern ULONG prof_buf[], prof_cnt;
extern void co_resume(void);
#endif

static const DispCfg display = { .planes = HW_PLANES, .scroll = 1, .fetch = 1, .view_h = HW_VIEW_H, .buf_h = HW_BUF_H,
                                 .level_w = HW_LEVEL_W, .buffers = 3, .double_y = 0, .sprite_w = 64, .sprite_bank = HW_SPR_BANK };
static ULONG palette[256];
static ULONG draw_total, draw_worst, hw_tiles, hw_cells, hw_behind;   /* the display's cost and work, for bench.txt */

/* what src/ngpc/hwdisp.c asks of the display: the kit's calls */
void hw_tile(int32_t x, int32_t y, const uint16_t *t) { disp_tile(x, y, (const UWORD *)t); }
void hw_clear(int32_t x, int32_t y, int32_t w, int32_t h)
{
    disp_clear(disp_front, x, y, (UWORD)w, (UWORD)h); disp_clear(disp_back, x, y, (UWORD)w, (UWORD)h);
    disp_clear(disp_clean, x, y, (UWORD)w, (UWORD)h);
}
void hw_bob(int32_t x, int32_t y, const uint16_t *img, const uint16_t *msk, int ww, int h) { disp_bob(x, y, (const UWORD *)img, (const UWORD *)msk, (UWORD)ww, (UWORD)h); }
void hw_sprite(int n, uint16_t *blk, int x, int y, int h, int att) { disp_sprite((UWORD)n, (UWORD *)blk, (WORD)x, (WORD)y, (UWORD)h, (UWORD)att); }
void hw_sprite_off(int n) { disp_sprite_off((UWORD)n); }
void hw_scroll(int32_t x, int32_t y) { disp_scroll(x, y); }
void hw_palette(const uint32_t *rgb, int first, int count) { disp_palette((const ULONG *)rgb, (UWORD)first, (UWORD)count); }
void *hw_chip(uint32_t bytes) { return sys_alloc(bytes, MEMF_CHIP | MEMF_CLEAR); }   /* before sys_take(): hwdisp_init() */
static ULONG logic_total, logic_worst;  /* colour clocks spent in game_tick: the logic's own cost */

/* The cartridge: rom.bin as it is, or rom.lzma, the packed copy a floppy holds (tools/make_adf_ngpc.sh). Fast RAM if
   there is any. */
static UBYTE *load_cartridge(ULONG *len)
{
    ULONG plen = 0, n;
    UBYTE *rom = sys_load("rom.bin", MEMF_ANY, len), *packed;
    UWORD *work;
    if (rom) return rom;
    if (!(packed = sys_load("rom.lzma", MEMF_ANY, &plen))) return 0;
    if ((n = unlzma_size(packed, plen)) == 0xffffffffUL) return 0;
    printf("Unpacking the cartridge (%lu KB)...\n", (unsigned long)(n / 1024));
    rom = sys_alloc(n, MEMF_ANY);
    work = sys_alloc(unlzma_work(packed), MEMF_ANY);
    if (!rom || !work) {
        printf("not enough memory to unpack the cartridge: %lu KB needed in one piece, %lu KB is (%lu KB free in all)\n",
               (unsigned long)(n / 1024), (unsigned long)(AvailMem(MEMF_ANY | MEMF_LARGEST) / 1024), (unsigned long)(AvailMem(MEMF_ANY) / 1024));
        return 0;
    }
    {   struct DateStamp t0, t1;
        LONG ticks;
        DateStamp(&t0);
        if ((plen = unlzma(packed, plen, rom, n, work)) != n) { printf("rom.lzma is damaged (%ld of %lu bytes)\n", (long)plen, (unsigned long)n); return 0; }
        DateStamp(&t1);
        ticks = (t1.ds_Minute - t0.ds_Minute) * 60 * TICKS_PER_SECOND_DOS + (t1.ds_Tick - t0.ds_Tick);
        printf("Unpacked in %ld.%ld s\n", (long)(ticks / TICKS_PER_SECOND_DOS), (long)(ticks % TICKS_PER_SECOND_DOS * 10 / TICKS_PER_SECOND_DOS));
    }
    *len = n;
    return rom;
}

static int game_load(void)
{
    ULONG romlen = 0, fontlen = 0;
    UBYTE *rom = load_cartridge(&romlen);
    UBYTE *font = sys_load("font.bin", MEMF_ANY, &fontlen);     /* the BIOS's character set, if the user has it */
    if (!rom) { printf("cannot load rom.bin or rom.lzma: file missing, or not enough memory for %s\n", "the cartridge"); return 0; }
    if (!hwdisp_init()) { printf("not enough chip RAM for the display\n"); return 0; }
    if (!sound_load()) return 0;
    game_init(rom, romlen, (font && fontlen == 0x800) ? font : 0);
    return 1;
}

static void game_start(void)
{
#ifdef NGP_PROF
    prof_install();
#endif
    sound_start();
}

static void vbl(void) { sys_key_poll(); sound_vbl(); }     /* every vertical blank: keys, then a frame of sound */

/* The controls of every Neo Geo Pocket port (PLAYBOOK-NGPC.md section 7): joystick in port 2 = pad; button 1 (A) =
   joystick button 1 (fire) and Z; button 2 (B) = joystick button 2 (second fire) and X; Option = Space or Return */
static UWORD read_input(void)
{
    UWORD in = sys_joy(1);
    if (sys_keys[KEY_UP]) in |= IN_UP;
    if (sys_keys[KEY_DOWN]) in |= IN_DOWN;
    if (sys_keys[KEY_LEFT]) in |= IN_LEFT;
    if (sys_keys[KEY_RIGHT]) in |= IN_RIGHT;
    if (sys_keys[0x31]) in |= IN_A;             /* Z */
    if (sys_keys[0x32]) in |= IN_B;             /* X */
    if (sys_keys[KEY_SPACE] || sys_keys[KEY_RET]) in |= IN_OPTION;
    return in;
}

static void game_draw(void)
{
    ULONG t0 = sys_now(), t;
    disp_begin();                               /* erase the objects of two frames ago */
    hwdisp_frame(ngp_vid, &ngp_dirty);          /* tiles, front layer, objects, sprites, scroll, colours */
    t = sys_now() - t0;
    draw_total += t; if (t > draw_worst) draw_worst = t;
    hw_tiles += hwdisp_last.tiles; hw_cells += hwdisp_last.cells;
    if (hwdisp_last.tiles_left || hwdisp_last.cells_left) hw_behind++;
}

/* ---- the loop: keep as it is ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    ULONG bench = (argc > 2 && !strcmp(argv[1], "bench")) ? strtoul(argv[2], 0, 10) : 0;
    ULONG last, now, vblanks, t0, t1, frames = 0, ticks = 0, acc = 0, hz;
    char text[240];

    if (!game_load() || !disp_open(&display)) { disp_close(); sys_free_all(); return 20; }
    sys_force_pal = 1;                                          /* NTSC machine: run the picture at 50 Hz */
    if (!sys_take()) { disp_close(); sys_free_all(); return 20; }
    sys_vbl_hook = vbl;                                         /* keys and sound at every vertical blank */
    hz = sys_is_pal ? 50 : 60;
    disp_palette(palette, 0, 256);                              /* black; hwdisp_frame() sets the colours */
    disp_on();
    game_start();
    perf_reset();
    sys_wait_frame();
    last = sys_frame;
    for (;;) {
        t0 = sys_now();
        now = sys_frame; vblanks = now - last; last = now;      /* vertical blanks since the previous pass */
        if (frames == 0) vblanks = 1;
        perf_frame_start(vblanks);                              /* the report counts every vertical blank */
        if (vblanks > 8) vblanks = 8;                           /* game time: after a long stall, do not spiral */
        acc += vblanks * TICKS_PER_SECOND;                      /* game time follows the clock, not the renderer */
        while (acc >= hz && !(bench && ticks >= bench)) {       /* bench mode stops at exactly N ticks */
            acc -= hz;
            t1 = sys_now();
            game_tick(bench ? game_bench_input(ticks) : read_input());
            t1 = sys_now() - t1; logic_total += t1; if (t1 > logic_worst) logic_worst = t1;
            ticks++;
        }
        game_draw();
        frames++;
        perf_frame_end(sys_now() - t0);
        if (sys_keys[KEY_ESC] || (bench && ticks >= bench) || ngp_fatal_why) break;
        disp_flip();                                            /* waits for the vertical blank */
    }
#ifdef NGP_PROF
    prof_remove();
#endif
    sys_vbl_hook = 0;
    sound_stop();
    sys_give();
    disp_close();
    sys_free_all();
    if (ngp_fatal_why) printf("stopped: reason %d at %06lx (see src/ngpc/ngp.h)\n", ngp_fatal_why, (unsigned long)ngp_fatal_pc);
    if (bench) {
        /* game_speed: game time against the clock (100 = kept); the skeleton drops what a pass takes beyond 8 frames */
        sprintf(text, "ticks=%lu checksum=%08lx game_speed=%lu%%\nlogic_cck=%lu max=%lu draw_cck=%lu max=%lu tiles=%lu cells=%lu behind=%lu",
                (unsigned long)ticks, (unsigned long)game_checksum(), (unsigned long)(ticks * hz * 100 / TICKS_PER_SECOND / (perf_frames + perf_late)),
                (unsigned long)(ticks ? logic_total / ticks : 0), (unsigned long)logic_worst, (unsigned long)(frames ? draw_total / frames : 0),
                (unsigned long)draw_worst, (unsigned long)hw_tiles, (unsigned long)hw_cells, (unsigned long)hw_behind);
        perf_note(text);
        perf_save("bench.txt");
#ifdef SOUND_TRACE
        { extern void sound_trace_save(void); sound_trace_save(); }
#endif
#ifdef LOGIC_TRACE                      /* tools/debug_ngpc.sh: the checksum after every tick */
        {   extern uint32_t logic_trace[]; extern unsigned logic_ntrace; BPTR fh = Open((CONST_STRPTR)"trace.bin", MODE_NEWFILE);
            if (fh) { Write(fh, logic_trace, logic_ntrace * 4); Close(fh); } }
#endif
#ifdef NGP_PROF
        {   BPTR fh = Open((CONST_STRPTR)"prof.bin", MODE_NEWFILE);     /* where main and co_resume are, then the samples */
            ULONG where[2]; where[0] = (ULONG)main; where[1] = (ULONG)co_resume;
            if (fh) { Write(fh, where, 8); Write(fh, prof_buf, prof_cnt * 4); Close(fh); } }
#endif
    }
    return 0;
}
