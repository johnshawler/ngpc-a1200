/* logic.c - the game's logic pile: the translated TLCS-900/H program (src/ngpc/gen_*.c, made by
 * tools/recomp900.py) run one Neo Geo Pocket frame per tick. Plain C with no platform header.
 *
 * One tick: the main thread runs until it waits for the next frame (a loop that polls memory an interrupt
 * handler changes, or a HALT). There the frame ends. The next tick starts with what the hardware does at the
 * vertical blank: the controller goes into RAM, the vertical blank interrupt handler runs, the sound processor
 * gets its frame's worth of time, and the interrupts it raised are taken. Then the main thread goes on.
 * The main thread runs as a coroutine (src/coro.h), because it waits in the middle of subroutines.
 */
#ifdef LOGIC_DEBUG
#include <stdio.h>
#endif
#include "ngp.h"
#define CPU900_EXTERN
#include "cpu900.h"
#include "coro.h"
#include "logic.h"
#include "run.h"

static uint32_t co_stack[65536 / 4];            /* the main thread's C stack */
#ifdef LOGIC_TRACE                              /* debug builds: the checksum after every tick */
uint32_t logic_trace[LOGIC_TRACE]; unsigned logic_ntrace;
#endif

void (*logic_irq_hook)(int idx, uint32_t to);   /* host tools: every interrupt taken, and its handler */

static void deliver(void)                       /* run the handlers of the interrupts the CPU would take now */
{
    int idx;
    while (!ngp_fatal_why && (idx = ngp_irq_next()) >= 0) {
        uint32_t ret = cpu.pc, to = ngp_irq_enter(idx);
        if (logic_irq_hook) logic_irq_hook(idx, to);
        if (run_call(to) != ret) { ngp_fatal(to, 4); return; }
        cpu.pc = ret;
    }
}

static unsigned polls;                          /* polls in a row: a few thousand take as long as a frame */

void ngp_wait(uint32_t pc)
{
    cpu.pc = pc;
    polls = 0;
#ifdef LOGIC_DEBUG
    { static int n; if (n++ < LOGIC_DEBUG) fprintf(stderr, "wait %d at %06x (XSP %08x)\n", n, (unsigned)pc, (unsigned)cpu.gpr[3]); }
#endif
    co_yield();                                 /* the frame is over: the Amiga side draws it */
    ngp_frame_start();
    deliver();                                  /* vertical blank */
    ngp_z80_frame();
    deliver();                                  /* what the sound processor asked for */
}

void ngp_poll(uint32_t pc)
{
    if (++polls >= 4096) { ngp_wait(pc); return; }  /* the answer did not come within this frame */
    cpu.pc = pc;
#ifdef LOGIC_DEBUG
    { static int n; if (n++ < LOGIC_DEBUG) fprintf(stderr, "poll %d at %06x\n", n, (unsigned)pc); }
#endif
    ngp_z80_slice();
    deliver();
}

uint32_t ngp_bios_entry(uint32_t pc)
{
    int n = ngp_bios_at(pc);
    if (n >= 0) { ngp_bios_call((unsigned)n); return cpu_pop(4) & 0xffffff; }                  /* a system call returns */
    if (pc == 0xff23df) { cpu_set_sr((uint16_t)cpu_pop(2)); return cpu_pop(4) & 0xffffff; }    /* the default interrupt handler: RETI */
    ngp_fatal(pc, 3);
    return 0;
}

static void ngp_main(void)
{
    uint32_t pc = cpu.pc;
    for (;;) {
        if (ngp_fatal_why) co_yield();          /* stopped: keep the frame loop alive */
        else pc = run_call(pc);                 /* a return nobody called from: go on there */
    }
}

void game_init(const uint8_t *rom, uint32_t len, const uint8_t *font)
{
    ngp_init(rom, len, font);
    co_init(ngp_main, co_stack, sizeof co_stack);
}

void game_tick(unsigned short in)
{
    uint8_t p = 0;
    if (in & IN_UP) p |= 0x01;
    if (in & IN_DOWN) p |= 0x02;
    if (in & IN_LEFT) p |= 0x04;
    if (in & IN_RIGHT) p |= 0x08;
    if (in & IN_A) p |= 0x10;
    if (in & IN_B) p |= 0x20;
    if (in & IN_OPTION) p |= 0x40;
    ngp_pad = p;
    co_resume();
#ifdef LOGIC_TRACE
    if (logic_ntrace < LOGIC_TRACE) logic_trace[logic_ntrace++] = game_checksum();
#endif
}

unsigned short game_bench_input(unsigned int t)
{
    /* BENCH INPUT BEGIN (per game: tools/new_port_ngpc.sh resets it) */
    /* Neo Turf Masters: a tap on A every 140 ticks goes from the title through the menus into a round of
       stroke play and keeps swinging: menus, hole fly-over, tee shots, ball flight. */
    if (t >= 1560 && (t - 1560) % 140 < 5) return IN_A;
    /* BENCH INPUT END */
    return 0;
}

static uint32_t fnv(uint32_t h, const uint8_t *p, unsigned n)
{
    while (n--) { h ^= *p++; h *= 16777619u; }
    return h;
}

uint32_t game_checksum(void)
{
    uint32_t h = 2166136261u;
    uint8_t r[4];
    int i, j;
    h = fnv(h, ngp_ram, sizeof ngp_ram);
    h = fnv(h, ngp_vid, sizeof ngp_vid);
    h = fnv(h, ngp_io, sizeof ngp_io);
    for (i = 0; i < 20; i++) {                  /* the registers, byte by byte: the same on a big-endian machine */
        uint32_t v = i < 16 ? cpu.bank[i >> 2][i & 3] : cpu.gpr[i - 16];
        for (j = 0; j < 4; j++) r[j] = (uint8_t)(v >> (8 * j));
        h = fnv(h, r, 4);
    }
    r[0] = (uint8_t)cpu.sr; r[1] = (uint8_t)(cpu.sr >> 8); r[2] = cpu.f2; r[3] = (uint8_t)ngp_fatal_why;
    return fnv(h, r, 4);
}
