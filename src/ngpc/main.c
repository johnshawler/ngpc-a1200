/* main.c - the Amiga side of a Neo Geo Pocket Color port: the kit's skeleton (kit/main.c) around the translated
 * game. The loop in main() is the kit's; keep it as it is.
 *
 *   game             play; Escape quits
 *   game bench N     play N logic ticks from scripted input, quit, write bench.txt (the proof of speed)
 *
 * The display shows the Neo Geo Pocket's 160 x 152 picture at its own size in the middle of a black 320 x 256
 * screen. It draws the picture from the K2GE's memory (k2ge_frame_cached: the 256 palette entries are the 256 pens of
 * an 8-plane screen) and converts it with the kit's chunky-to-planar routine, row by row. It draws a new picture only
 * when the game changed the K2GE's memory; a change of colours alone only reloads the palette.
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

#undef TICKS_PER_SECOND                 /* <proto/dos.h> has its own */
#define TICKS_PER_SECOND 60             /* the Neo Geo Pocket shows 60 frames a second: 6 ticks every 5 frames */

void c2p1x1_8_c5_gen_init(int cx __asm("d0"), int cy __asm("d1"), int offx __asm("d2"), int offy __asm("d3"),
                          int rowlen __asm("d4"), int bplsize __asm("d5"));
void c2p1x1_8_c5_gen(void *chunky __asm("a0"), void *planes __asm("a1"));

#define PIC_X 80                        /* where the picture sits on the 320 x 256 screen: in the middle */
#define PIC_Y 52

/* ---- the game ------------------------------------------------------------------------------ */
#ifdef NGP_PROF                         /* tools/prof_ngpc.sh: a 1 kHz sampling profiler (src/prof68k.s) */
extern void prof_install(void), prof_remove(void); extern ULONG prof_buf[], prof_cnt;
extern void co_resume(void);
#endif

static const DispCfg display = { .planes = 8, .scroll = 0, .fetch = 3, .view_h = 256, .buf_h = 256,
                                 .level_w = 0, .buffers = 2, .double_y = 0, .sprite_w = 16, .sprite_bank = 1 };
static UBYTE *chunky;                   /* 160 x 152 palette entries */
static ULONG palette[256];
static ULONG chunky_gen = ~0UL;          /* ngp_vid_gen when chunky was drawn */
static struct { UBYTE *buf; ULONG gen; ULONG rows[K2GE_H_LINES]; } shown[2];   /* what each screen buffer holds:
                                           the picture, and k2ge_line_gen of each row when it was converted */
static ULONG *rgb_of;                   /* 4,096 entries: a K2GE colour (0x0BGR) as 0x00RRGGBB */
ULONG pictures_drawn, pictures_converted, draw_cck, convert_cck;     /* how many, and the colour clocks they took */
static ULONG logic_total, logic_worst;  /* colour clocks spent in game_tick: the logic's own cost */

static int game_load(void)
{
    ULONG romlen = 0, fontlen = 0;
    UBYTE *rom = sys_load("rom.bin", MEMF_ANY, &romlen);        /* the cartridge: fast RAM if there is any */
    UBYTE *font = sys_load("font.bin", MEMF_ANY, &fontlen);     /* the BIOS's character set, if the user has it */
    if (!rom) { printf("cannot load rom.bin: file missing, or not enough memory for %s\n", "the cartridge"); return 0; }
    chunky = sys_alloc(K2GE_W * K2GE_H_LINES, MEMF_ANY | MEMF_CLEAR);
    rgb_of = sys_alloc(4096 * 4, MEMF_ANY);
    if (!chunky || !rgb_of || !sound_load()) return 0;
    {   ULONG c;
        for (c = 0; c < 4096; c++) rgb_of[c] = ((c & 15) * 17 << 16) | (((c >> 4) & 15) * 17 << 8) | ((c >> 8) & 15) * 17;
    }
    game_init(rom, romlen, (font && fontlen == 0x800) ? font : 0);
    return 1;
}

static void game_start(void)
{
    c2p1x1_8_c5_gen_init(K2GE_W, 1, 0, 0, disp_row, disp_plane);        /* one row of the picture at a time */
#ifdef NGP_PROF
    prof_install();
#endif
    ngp_pal_dirty = 1;
    sound_start();
}

static void vbl(void) { sys_key_poll(); sound_vbl(); }     /* every vertical blank: keys, then a frame of sound */

/* joystick in port 2 = pad, fire = A, second fire = B. Keys: cursor keys, X = A, Z = B, Space or Return = Option */
static UWORD read_input(void)
{
    UWORD in = sys_joy(1);
    if (sys_keys[KEY_UP]) in |= IN_UP;
    if (sys_keys[KEY_DOWN]) in |= IN_DOWN;
    if (sys_keys[KEY_LEFT]) in |= IN_LEFT;
    if (sys_keys[KEY_RIGHT]) in |= IN_RIGHT;
    if (sys_keys[0x32]) in |= IN_A;
    if (sys_keys[0x31]) in |= IN_B;
    if (sys_keys[KEY_SPACE] || sys_keys[KEY_RET]) in |= IN_OPTION;
    return in;
}

static void game_draw(void)
{
    int i, y;
    if (ngp_pal_dirty) {                        /* the K2GE's 256 palette entries are the 256 pens */
        UBYTE neg = ngp_vid[0x12] & 0x80;
        ngp_pal_dirty = 0;
        for (i = 0; i < 256; i++) {
            UWORD c = (UWORD)(ngp_vid[0x200 + i * 2] | ngp_vid[0x201 + i * 2] << 8);
            if (neg) c = ~c;
            palette[i] = rgb_of[c & 0xfff];
        }
        palette[0] = 0;                         /* pen 0 is never in the picture (sprite colour 0 is see-through): the border */
        disp_palette(palette, 0, 256);
    }
    if (chunky_gen != ngp_vid_gen) {           /* the game changed the picture */
        ULONG t0 = sys_now();
        k2ge_frame_cached(ngp_vid, &ngp_dirty, chunky);
        chunky_gen = ngp_vid_gen; pictures_drawn++; draw_cck += sys_now() - t0;
    }
    for (i = 0; i < 2 && shown[i].buf && shown[i].buf != disp_back; i++) ;
    if (i == 2) i = 0;
    if (shown[i].buf != disp_back || shown[i].gen != chunky_gen) {    /* this buffer shows an older picture */
        UBYTE *dst = disp_back + PIC_Y * disp_row + PIC_X / 8; ULONG t0 = sys_now();
        int fresh = shown[i].buf != disp_back;
        for (y = 0; y < K2GE_H_LINES; y++, dst += disp_row)     /* only the rows written since this buffer got them */
            if (fresh || shown[i].rows[y] != k2ge_line_gen[y]) { c2p1x1_8_c5_gen(chunky + y * K2GE_W, dst); shown[i].rows[y] = k2ge_line_gen[y]; }
        shown[i].buf = disp_back; shown[i].gen = chunky_gen; pictures_converted++; convert_cck += sys_now() - t0;
    }
}

/* ---- the loop: keep as it is ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    ULONG bench = (argc > 2 && !strcmp(argv[1], "bench")) ? strtoul(argv[2], 0, 10) : 0;
    ULONG last, now, vblanks, t0, t1, frames = 0, ticks = 0, acc = 0, hz;
    char text[240];

    if (!game_load() || !disp_open(&display)) { disp_close(); sys_free_all(); return 20; }
    if (disp_plane != 320 * 256 / 8) { printf("the display is not the one the C2P routine was assembled for\n"); disp_close(); sys_free_all(); return 20; }
    sys_force_pal = 1;                                          /* NTSC machine: run the picture at 50 Hz */
    if (!sys_take()) { disp_close(); sys_free_all(); return 20; }
    sys_vbl_hook = vbl;                                         /* keys and sound at every vertical blank */
    hz = sys_is_pal ? 50 : 60;
    disp_palette(palette, 0, 256);
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
        sprintf(text, "ticks=%lu checksum=%08lx game_speed=%lu%%\nlogic_cck=%lu max=%lu pics=%lu/%lu draw_cck=%lu conv_cck=%lu",
                (unsigned long)ticks, (unsigned long)game_checksum(), (unsigned long)(ticks * hz * 100 / TICKS_PER_SECOND / (perf_frames + perf_late)),
                (unsigned long)(ticks ? logic_total / ticks : 0), (unsigned long)logic_worst, (unsigned long)pictures_drawn, (unsigned long)pictures_converted,
                (unsigned long)(pictures_drawn ? draw_cck / pictures_drawn : 0), (unsigned long)(pictures_converted ? convert_cck / pictures_converted : 0));
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
