/* ngpref.c - host-only reference Neo Geo Pocket Color (TLCS-900/H + K2GE + timers) used to study the original
 * game and to check the port against it. It never runs on the Amiga.
 *
 *   ngpref rom.ngc FRAMES [options]
 *     -b bios.ngp       the BIOS image: only its character set is used (the system call "set the system font")
 *     -i input.txt      lines "first last BUTTONS" (UP DOWN LEFT RIGHT A B OPTION), frames inclusive
 *     -d f1,f2,...      dump these frames as frame_NNNNN.ppm       -o dir   where dumps go (default .)
 *     -c flow.txt       write the flow file that seeds tools/recomp900.py (see below)
 *     -s                print per-frame load: instructions and CPU states used, interrupts taken
 *     -w a1,a2,...      addresses (hex) of wait loops: instructions executed there do not count as load
 *     -r file           write the 12 KB of work RAM after every frame
 *     -z file           write the Z80's 4 KB (the sound program) each time the game starts the Z80
 *     -W a1,a2,...      like -w for the Z80: addresses (hex) of the sound program's idle loop
 *     -p f1,f2,...      for these frames print where the instructions went (the 12 busiest addresses): shows wait loops
 *     -m                at the end, list every place where the main CPU touches the sound processor: reads and
 *                       writes of the shared RAM (0x7000-0x7FFF) and of its ports (0xB8-0xBC), by instruction address
 *     -t                trace every instruction (huge)
 *
 * What it models: the CPU with its timing, work RAM, the K2GE with scanline timing and a picture per frame,
 * the four 8-bit timers, the interrupt controller with its priorities, the BIOS's system calls and its
 * per-frame work written in C (the real BIOS code is not run), the controller, and the Z80 sound processor
 * with its shared RAM, command port and interrupts (games wait for answers of their sound program).
 * What it does not model: the sound chip's output (writes are counted, nothing is played), micro DMA, the
 * serial port, flash writes (they succeed and are forgotten), the mono mode of games without colour. What a
 * game touched of these is in the summary this program prints at the end ("uses:" line).
 *
 * Flow file lines (all addresses hex):  E pc  = an instruction was executed at pc;  I to from  = the jump at
 * "from" through a register or memory went to "to";  C to from  = the same for a call;  V n to  = interrupt
 * vector n was taken to "to";  B n  = BIOS system call n was used;  H pc  = a HALT was executed at pc.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../../src/ngpc/k2ge.h"

#define LINE_STATES 515                 /* CPU states per scanline */
#define LINES 199                       /* scanlines per frame: 152 shown */

static uint8_t ram[0x4000];             /* 0x4000-0x7FFF: 12 KB work RAM + 4 KB shared with the Z80 */
static uint8_t vid[0x4000];             /* 0x8000-0xBFFF: the K2GE */
static uint8_t io[0x100];               /* 0x00-0xFF: the TMP95C061's own registers and the NGPC ports */
static uint8_t *rom; static long rom_len;
static uint8_t biosimg[0x10000]; static int have_bios;
static uint16_t fb[K2GE_H_LINES][K2GE_W];

static int raster, line_states, frame;
static uint8_t pad;
static int trace, stats;
static FILE *flowf;
static uint8_t *seen_e, *seen_i;
static long frame_instr, frame_states, frame_idle, frame_irq, total_instr;
static unsigned uses;                   /* bit set of features a game touched that are not modelled */
#define USE_Z80 1
#define USE_DMA 2
#define USE_SERIAL 4
#define USE_FLASH 8
#define USE_MONO 16
#define USE_PSG 32
#define USE_DAC 64
static uint32_t waitpc[32]; static int nwait;
static uint32_t cur_pc; static int commlog;       /* -m */
static struct { uint32_t pc, a; char rw; long n; uint8_t first, last; } comm[4096]; static int ncomm;
static void comm_note(uint32_t a, char rw, uint8_t v)
{
    int i;
    if (!commlog || cur_pc >= 0xff0000) return;
    for (i = 0; i < ncomm; i++) if (comm[i].pc == cur_pc && comm[i].a == a && comm[i].rw == rw) { comm[i].n++; comm[i].last = v; return; }
    if (ncomm < 4096) { comm[ncomm].pc = cur_pc; comm[ncomm].a = a; comm[ncomm].rw = rw; comm[ncomm].n = 1; comm[ncomm].first = comm[ncomm].last = v; ncomm++; }
}
static uint32_t *prof; static int profiling;      /* instructions per address in this frame */
static unsigned bios_used;

/* the system call table of the BIOS: the addresses games read at 0xFFFE00 and call */
static const uint32_t sys_addr[27] = {
    0xff27a2, 0xff1034, 0xff1440, 0xff12b4, 0xff1222, 0xff8d8a, 0xff6fd8, 0xff7042, 0xff7082, 0xff149b, 0xff1033, 0xff1487,
    0xff731f, 0xff70ca, 0xff17c4, 0xff1032, 0xff2bbd, 0xff2c0c, 0xff2c44, 0xff2c86, 0xff2cb4, 0xff2d27, 0xff2d33, 0xff2d3a,
    0xff2d4e, 0xff2d6c, 0xff2d85 };

/* ---- timers and interrupts ---- */
static uint8_t tcount[4]; static uint32_t tpre; static int tff3;
static int z80_on, z80_budget; static long z80_instr, psg_writes, frame_z80, frame_z80_idle, frame_z80_irq;
static uint16_t zwait[32]; static int nzwait; static uint32_t *zprof; static const char *zdump; static int z80_starts;
static uint8_t ipend[16];               /* by the number of the vector in RAM: 5 VBL, 6 Z80, 7-10 timers 0-3 */

static void set_tff3(int v);
static void z80_start(void);
static void z80_nmi(void);

static uint8_t mem_r8_(uint32_t a);
static uint8_t mem_r8(uint32_t a)
{
    uint8_t v = mem_r8_(a);
    if ((a >= 0x7000 && a < 0x8000) || (a >= 0xb8 && a <= 0xbc)) comm_note(a, 'r', v);
    return v;
}
static uint8_t mem_r8_(uint32_t a)
{
    if (a < 0x100) {
        switch (a) {
        case 0x71: return (uint8_t)((ipend[5] ? 0x08 : 0) | (ipend[6] ? 0x80 : 0));
        case 0x73: return (uint8_t)((ipend[7] ? 0x08 : 0) | (ipend[8] ? 0x80 : 0));
        case 0x74: return (uint8_t)((ipend[9] ? 0x08 : 0) | (ipend[10] ? 0x80 : 0));
        case 0xb0: return pad;
        case 0xb1: return 0x02 | 0x01;                  /* sub battery fine, power button not pressed */
        }
        return io[a];
    }
    if (a >= 0x4000 && a < 0x8000) return ram[a - 0x4000];
    if (a >= 0x8000 && a < 0xc000) {
        switch (a) {
        case 0x8008: return (uint8_t)((LINE_STATES - line_states) >> 2);
        case 0x8009: return (uint8_t)raster;
        case 0x8010: return raster >= K2GE_H_LINES ? 0x40 : 0;
        }
        return vid[a - 0x8000];
    }
    if (a >= 0x200000 && a < 0x400000) return (long)(a - 0x200000) < rom_len ? rom[a - 0x200000] : 0xff;
    if (a >= 0x800000 && a < 0xa00000) return (long)(a - 0x600000) < rom_len ? rom[a - 0x600000] : 0xff;
    if (a >= 0xfffe00 && a < 0xfffe00 + 27 * 4) return (uint8_t)(sys_addr[(a - 0xfffe00) >> 2] >> ((a & 3) * 8));
    if (a >= 0xff0000) return have_bios ? biosimg[a - 0xff0000] : 0xff;
    return 0;
}
static void mem_w8(uint32_t a, uint8_t v)
{
    if ((a >= 0x7000 && a < 0x8000) || (a >= 0xb8 && a <= 0xbc)) comm_note(a, 'w', v);
    if (a < 0x100) {
        switch (a) {
        case 0x20: if (!(v & 1)) tcount[0] = 0; if (!(v & 2)) tcount[1] = 0; if (!(v & 4)) tcount[2] = 0; if (!(v & 8)) tcount[3] = 0; break;
        case 0x25: if ((v & 0xc0) == 0x00) set_tff3(!tff3); else if ((v & 0xc0) == 0x40) set_tff3(1); else if ((v & 0xc0) == 0x80) set_tff3(0); break;
        case 0x71: if (!(v & 0x08)) ipend[5] = 0; if (!(v & 0x80)) ipend[6] = 0; break;
        case 0x73: if (!(v & 0x08)) ipend[7] = 0; if (!(v & 0x80)) ipend[8] = 0; break;
        case 0x74: if (!(v & 0x08)) ipend[9] = 0; if (!(v & 0x80)) ipend[10] = 0; break;
        case 0x7c: case 0x7d: case 0x7e: case 0x7f: if (v) uses |= USE_DMA; break;
        case 0x50: uses |= USE_SERIAL; break;
        case 0xa0: case 0xa1: uses |= USE_PSG; psg_writes++; break;
        case 0xa2: case 0xa3: if (v != 0x80 && v) uses |= USE_DAC; break;
        case 0xb9: if (v == 0x55) { uses |= USE_Z80; z80_on = 1; z80_start(); } else if (v == 0xaa) z80_on = 0; break;
        case 0xba: z80_nmi(); break;
        }
        io[a] = v; return;
    }
    if (a >= 0x4000 && a < 0x8000) { ram[a - 0x4000] = v; return; }
    if (a >= 0x8000 && a < 0xc000) {
        unsigned o = a - 0x8000;
        if (o >= 0x200 && o < 0x400 && (o & 1)) v &= 0x0f;              /* palette: 12 bits */
        if (o == 0x7e2) { if (vid[0x7f0] != 0xaa) return; if (v & 0x80) uses |= USE_MONO; }
        if (o == 0x10 || o == 0x08 || o == 0x09) return;                /* read-only */
        vid[o] = v; return;
    }
    if ((a >= 0x200000 && a < 0x400000) || (a >= 0x800000 && a < 0xa00000)) { uses |= USE_FLASH; return; }
}

#include "../../src/ngpc/cpu900.h"

/* ---- the Z80 sound processor: 4 KB shared with the main CPU at 0x7000, the sound chip, a command byte ---- */
static uint8_t z80_mr(uint16_t a)
{
    if (a < 0x1000) return ram[0x3000 + a];
    if (a == 0x8000) return io[0xbc];
    return 0xff;
}
static void z80_mw(uint16_t a, uint8_t v)
{
    if (a < 0x1000) ram[0x3000 + a] = v;
    else if (a == 0x4000 || a == 0x4001) { uses |= USE_PSG; psg_writes++; }
    else if (a == 0x8000) io[0xbc] = v;
    else if (a == 0xc000) ipend[6] = 1;                 /* interrupt to the main CPU (INT5) */
}
static uint8_t z80_in(uint16_t port);
static void z80_out(uint16_t port, uint8_t v);
#include "../../src/ngpc/z80.h"
static uint8_t z80_in(uint16_t port) { (void)port; return 0xff; }
static void z80_out(uint16_t port, uint8_t v) { (void)port; (void)v; z80.irq_line = 0; }   /* any port write ends its interrupt */
static void z80_start(void)
{
    z80_reset(); z80_budget = 0; z80_starts++;
    if (zdump) { FILE *f = fopen(zdump, z80_starts == 1 ? "wb" : "ab"); if (f) { fwrite(ram + 0x3000, 1, 0x1000, f); fclose(f); } }
}
static void z80_nmi(void) { z80.nmi_pending = 1; }
static void set_tff3(int v) { if (v && !tff3) z80.irq_line = 1; tff3 = v; }        /* timer 3's output is the Z80's interrupt */

static void flow(char kind, uint32_t a, uint32_t b)
{
    if (!flowf) return;
    if (kind == 'E') { if (seen_e[a >> 3] & (1 << (a & 7))) return; seen_e[a >> 3] |= (uint8_t)(1 << (a & 7)); fprintf(flowf, "E %06x\n", a); }
    else if (kind == 'I' || kind == 'C') {                  /* once per (target, source) pair */
        static uint32_t pairs[65536][2]; static int npairs; int i;
        for (i = 0; i < npairs; i++) if (pairs[i][0] == a && pairs[i][1] == b) return;
        if (npairs < 65536) { pairs[npairs][0] = a; pairs[npairs][1] = b; npairs++; }
        fprintf(flowf, "%c %06x %06x\n", kind, a, b);
    }
    else if (kind == 'V') { static uint32_t last[16]; if (a < 16 && last[a] != b + 1) { last[a] = b + 1; fprintf(flowf, "V %u %06x\n", a, b); } }
    else if (kind == 'B') fprintf(flowf, "B %u\n", a);
    else fprintf(flowf, "%c %06x\n", kind, a);
}

/* ---- the BIOS in C ---- */
static void bios_call(unsigned n)
{
    uint32_t xhl3 = cpu.bank[3][3], xde3 = cpu.bank[3][2];
    uint8_t ra3 = (uint8_t)cpu.bank[3][0], rb3 = (uint8_t)(cpu.bank[3][1] >> 8), rc3 = (uint8_t)cpu.bank[3][1];
    unsigned i, j;
    if (n < 32 && !(bios_used & (1u << n))) { bios_used |= 1u << n; flow('B', n, 0); }
    switch (n) {
    case 0: fprintf(stderr, "frame %d: the game asked for power off (system call 0)\n", frame); cpu.halted = 1; break;
    case 1: io[0x80] = rb3; break;                                      /* clock gear: recorded, not acted on */
    case 2: if (xhl3 < 0xc000) for (i = 0; i < 7; i++) mem_w8(xhl3 + i, io[0x91 + i]); break;   /* date and time */
    case 4: {                                                           /* interrupt level of one source */
        static const uint8_t reg[10] = { 0x70, 0x71, 0x73, 0x73, 0x74, 0x74, 0x79, 0x79, 0x7a, 0x7a };
        static const uint8_t hi[10] = { 0, 1, 0, 1, 0, 1, 0, 1, 0, 1 };
        if (rc3 < 10) { uint8_t r = io[reg[rc3]]; r = hi[rc3] ? (uint8_t)((r & 0x0f) | ((rb3 & 7) << 4)) : (uint8_t)((r & 0xf0) | (rb3 & 7)); mem_w8(reg[rc3], r); }
        break; }
    case 5:                                                             /* system font into character RAM */
        for (i = 0; i < 0x800; i++) {
            uint8_t c = have_bios ? biosimg[0x8dcf + i] : (uint8_t)((i & 7) == 0 || (i & 7) == 7 ? 0xff : 0x81);
            unsigned w = 0;
            for (j = 0; j < 8; j++, c <<= 1) w = (w << 2) | ((c & 0x80) ? (ra3 & 3u) : (unsigned)(ra3 >> 4) & 3u);
            vid[0x2000 + i * 2] = (uint8_t)w; vid[0x2001 + i * 2] = (uint8_t)(w >> 8);
        }
        break;
    case 6: case 7: case 8: uses |= USE_FLASH; cpu.bank[3][0] &= ~0xffu; break;     /* flash write and erase: "done" */
    case 9: case 11: case 13: case 16: cpu.bank[3][0] &= ~0xffu; break;             /* alarm, protect, serial init */
    case 17: case 18: case 19: case 20: case 21: case 22: case 23: case 24: case 25: case 26: uses |= USE_SERIAL; break;
    default: break;                                                     /* 3, 10, 12, 14, 15: nothing to do here */
    }
    (void)xde3;
}

/* the BIOS's interrupt entry: straight to the game's handler in the RAM vector table at 0x6FB8 */
static void take_irq(int idx, int level)
{
    uint32_t to;
    cpu_push(4, cpu.pc); cpu_push(2, cpu.sr);
    cpu.sr = (uint16_t)((cpu.sr & ~0x7000) | ((level < 7 ? level + 1 : 7) << 12));
    to = cpu_rd(4, 0x6fb8 + idx * 4) & 0xffffff;
    flow('V', (uint32_t)idx, to);
    cpu.pc = to; cpu.halted = 0; frame_irq++;
}
static void check_irq(void)
{
    static const struct { uint8_t idx, reg, shift; } src[6] = {
        { 5, 0x71, 0 }, { 6, 0x71, 4 }, { 7, 0x73, 0 }, { 8, 0x73, 4 }, { 9, 0x74, 0 }, { 10, 0x74, 4 } };
    int i;
    for (i = 0; i < 6; i++) {
        int prio = (io[src[i].reg] >> src[i].shift) & 7;
        if (ipend[src[i].idx] && prio && prio != 7 && (int)CPU_IFF <= prio) { ipend[src[i].idx] = 0; take_irq(src[i].idx, prio); return; }
    }
}

static void timer_tick(int n)                           /* one clock for timer n; a match gives its interrupt */
{
    static const uint8_t reg[4] = { 0x22, 0x23, 0x26, 0x27 };
    if (++tcount[n] != io[reg[n]]) return;
    tcount[n] = 0;
    ipend[7 + n] = 1;
    if (n == 0 && (io[0x20] & 2) && !(io[0x24] & 0x0c)) timer_tick(1);          /* timer 1 counts timer 0's matches */
    if (n == 2 && (io[0x20] & 8) && !(io[0x28] & 0x0c)) timer_tick(3);
    if (n == 3 && (io[0x25] & 0x20)) set_tff3(!tff3);
}
static void timers(int states)
{
    static const uint8_t sh0[4] = { 0, 7, 9, 11 }, sh1[4] = { 0, 7, 11, 15 }, sh2[4] = { 7, 7, 9, 11 };
    uint32_t old = tpre; int n, m;
    if (!(io[0x20] & 0x80)) return;                     /* the prescaler is stopped */
    tpre += (uint32_t)states;
    if ((io[0x20] & 1) && (m = io[0x24] & 3) != 0) for (n = (int)((tpre >> sh0[m]) - (old >> sh0[m])); n > 0; n--) timer_tick(0);
    if ((io[0x20] & 2) && (m = (io[0x24] >> 2) & 3) != 0) for (n = (int)((tpre >> sh1[m]) - (old >> sh1[m])); n > 0; n--) timer_tick(1);
    if (io[0x20] & 4) { m = io[0x28] & 3; for (n = (int)((tpre >> sh2[m]) - (old >> sh2[m])); n > 0; n--) timer_tick(2); }
    if ((io[0x20] & 8) && (m = (io[0x28] >> 2) & 3) != 0) for (n = (int)((tpre >> sh1[m]) - (old >> sh1[m])); n > 0; n--) timer_tick(3);
}

static void end_of_line(void)
{
    if ((raster < K2GE_H_LINES - 1 || raster == LINES - 1) && (vid[0] & 0x40) && (io[0x20] & 1) && !(io[0x24] & 3)) timer_tick(0);   /* horizontal blank clocks timer 0 */
    if (raster < K2GE_H_LINES) k2ge_line(vid, raster, fb[raster]);
    raster++;
    if (raster == K2GE_H_LINES) {                       /* vertical blank: the BIOS reads the controller, then the game's handler */
        ram[0x6f82 - 0x4000] = pad;
        if (vid[0] & 0x80) ipend[5] = 1;
    }
    if (raster == LINES) raster = 0;
}

static void reset(void)
{
    int i;
    memset(&cpu, 0, sizeof cpu);
    cpu.pc = (rom[0x1c] | rom[0x1d] << 8 | rom[0x1e] << 16) & 0xffffff;
    cpu.sr = 0xf800;
    cpu.gpr[0] = 0xff23c3; cpu.gpr[1] = 0xff23df; cpu.gpr[2] = 0x006480; cpu.gpr[3] = 0x6c00;
    /* what the BIOS leaves in RAM and in the K2GE when it starts a game */
    for (i = 0; i < 4; i++) ram[0x6c00 - 0x4000 + i] = rom[0x1c + i];
    ram[0x6c04 - 0x4000] = rom[0x20]; ram[0x6c05 - 0x4000] = rom[0x21]; ram[0x6e82 - 0x4000] = rom[0x20]; ram[0x6e83 - 0x4000] = rom[0x21];
    ram[0x6c06 - 0x4000] = rom[0x22]; ram[0x6e84 - 0x4000] = rom[0x22];
    for (i = 0; i < 12; i++) ram[0x6c08 - 0x4000 + i] = rom[0x24 + i];
    ram[0x6c58 - 0x4000] = 1; ram[0x6c59 - 0x4000] = rom_len > 0x200000; ram[0x6c55 - 0x4000] = 1;
    ram[0x6f80 - 0x4000] = 0xff; ram[0x6f81 - 0x4000] = 0x03;           /* battery full */
    ram[0x6f84 - 0x4000] = 0x40;                                        /* started by power on */
    ram[0x6f87 - 0x4000] = 1;                                           /* language: English */
    ram[0x6f91 - 0x4000] = rom[0x23]; ram[0x6f95 - 0x4000] = rom[0x23]; /* colour or mono game */
    for (i = 0; i < 18; i++) { uint8_t *p = ram + 0x6fb8 - 0x4000 + i * 4; p[0] = 0xdf; p[1] = 0x23; p[2] = 0xff; p[3] = 0; }
    vid[0x000] = 0xc0; vid[0x004] = 0xff; vid[0x005] = 0xff; vid[0x006] = 0xc6; vid[0x118] = 0x80;
    vid[0x3e0] = 0xff; vid[0x3e1] = 0x0f; vid[0x3f0] = 0xff; vid[0x3f1] = 0x0f; vid[0x400] = 0xff; vid[0x402] = 0x80;
    vid[0x7e2] = rom[0x23] ? 0x00 : 0x80;
    if (!rom[0x23]) uses |= USE_MONO;
    /* the CPU's own registers as the BIOS leaves them: timers stopped, vertical blank at level 2, the Z80 at 3 */
    io[0x20] = 0x80; io[0x22] = 0x01; io[0x23] = 0x90; io[0x24] = 0x03; io[0x25] = 0xb0; io[0x26] = 0x90; io[0x27] = 0x62; io[0x28] = 0x05;
    io[0x6f] = 0x4e; io[0x70] = 0x02; io[0x71] = 0x32; io[0xb8] = 0xaa; io[0xb9] = 0xaa;
    io[0x91] = 0x99; io[0x92] = 0x01; io[0x93] = 0x01; io[0x94] = 0x12; io[0x95] = 0x00; io[0x96] = 0x00; io[0x97] = 0x05;  /* a fixed date */
}

/* one instruction, with what the BIOS would do at the addresses the game can reach in it */
static int step(void)
{
    uint32_t pc0 = cpu.pc;
    int st, i;
    if (cpu.halted) return 4;
    if (pc0 >= 0xff0000) {                              /* inside the BIOS: a system call, or the default interrupt return */
        for (i = 0; i < 27; i++) if (sys_addr[i] == pc0) { bios_call((unsigned)i); cpu.pc = cpu_pop(4) & 0xffffff; return 20; }
        if (pc0 == 0xff23df) { cpu_set_sr((uint16_t)cpu_pop(2)); cpu.pc = cpu_pop(4) & 0xffffff; return 12; }    /* RETI */
        fprintf(stderr, "frame %d: jump into the BIOS at %06x, which is not a system call\n", frame, pc0); exit(1);
    }
    flow('E', pc0, 0);
    cur_pc = pc0;
    if (trace) printf("%06x  XWA %08x XBC %08x XDE %08x XHL %08x XIX %08x XIY %08x XIZ %08x XSP %08x SR %04x\n", pc0,
                      cpu.bank[CPU_RFP][0], cpu.bank[CPU_RFP][1], cpu.bank[CPU_RFP][2], cpu.bank[CPU_RFP][3], cpu.gpr[0], cpu.gpr[1], cpu.gpr[2], cpu.gpr[3], cpu.sr);
    {   uint8_t f = mem_r8(pc0), s2 = mem_r8((pc0 + 1) & 0xffffff);
        st = cpu_step();
        if (cpu.bad) { fprintf(stderr, "frame %d: not an instruction at %06x: %02x %02x %02x %02x\n", frame, pc0, f, s2, mem_r8(pc0 + 2), mem_r8(pc0 + 3)); exit(1); }
        if (f >= 0xf8 && (f & 7) == 1) { bios_call(cpu.bank[3][0] >> 8 & 0xff); cpu_set_sr((uint16_t)cpu_pop(2)); cpu.pc = cpu_pop(4) & 0xffffff; }
        if (flowf) {
            int at = -1;                                /* where the opcode byte of a "destination" instruction is */
            if (f == 0x05) flow('H', pc0, 0);
            if (f >= 0xb0 && f <= 0xb7) at = 1; else if (f >= 0xb8 && f <= 0xbf) at = 2; else if (f == 0xf4 || f == 0xf5) at = 2;
            else if (f == 0xf3) at = (s2 & 3) == 0 ? 2 : 4;
            if (at > 0) {
                uint8_t op = mem_r8((pc0 + (uint32_t)at) & 0xffffff);
                /* taken: always for the condition T, else when the pc is not the next instruction (a table may point there too) */
                if (op >= 0xd0 && op <= 0xef && ((op & 15) == 8 || cpu.pc != ((pc0 + (uint32_t)at + 1) & 0xffffff)) && cpu.pc < 0xff0000) flow(op < 0xe0 ? 'I' : 'C', cpu.pc, pc0);
            }
        }
    }
    frame_instr++; total_instr++;
    if (profiling) prof[pc0]++;
    for (i = 0; i < nwait; i++) if (waitpc[i] == pc0) { frame_idle += st; frame_instr--; break; }
    return st;
}

static void run_frame(void)
{
    int done = 0;
    frame_instr = frame_states = frame_idle = frame_irq = 0; frame_z80 = frame_z80_idle = frame_z80_irq = 0;
    while (!done) {
        int st;
        check_irq();
        st = step();
        if (cpu.halted) frame_idle += st;
        frame_states += st;
        line_states += st;
        if (line_states >= LINE_STATES) { line_states -= LINE_STATES; end_of_line(); if (raster == 0) done = 1; }
        timers(st);
        if (z80_on) {
            z80_budget += st;
            while (z80_budget > 0) {
                uint16_t zpc = z80.pc; int k, irq = z80.irq_line && z80.iff1;
                z80_budget -= 2 * z80_step(); z80_instr++; frame_z80++; frame_z80_irq += irq;
                if (z80.halted) frame_z80_idle++; else for (k = 0; k < nzwait; k++) if (zwait[k] == zpc) { frame_z80_idle++; break; }
                if (profiling && zprof) zprof[zpc]++;
            }
        }
    }
}

static void dump_ppm(const char *dir, int n)
{
    char name[512]; FILE *f; int x, y;
    snprintf(name, sizeof name, "%s/frame_%05d.ppm", dir, n);
    f = fopen(name, "wb"); if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", K2GE_W, K2GE_H_LINES);
    for (y = 0; y < K2GE_H_LINES; y++) for (x = 0; x < K2GE_W; x++) {
        unsigned c = fb[y][x];
        fputc((c & 15) * 17, f); fputc(((c >> 4) & 15) * 17, f); fputc(((c >> 8) & 15) * 17, f);
    }
    fclose(f);
}

static struct { int first, last; uint8_t buttons; } script[4096]; static int nscript;
static void load_script(const char *name)
{
    static const char *names[7] = { "UP", "DOWN", "LEFT", "RIGHT", "A", "B", "OPTION" };
    FILE *f = fopen(name, "r"); char line[256];
    if (!f) { fprintf(stderr, "cannot read %s\n", name); exit(2); }
    while (fgets(line, sizeof line, f) && nscript < 4096) {
        int a, b, n = 0, i; char *p;
        if (sscanf(line, "%d %d%n", &a, &b, &n) < 2) continue;
        script[nscript].first = a; script[nscript].last = b; script[nscript].buttons = 0;
        for (p = strtok(line + n, " \t\r\n"); p; p = strtok(0, " \t\r\n"))
            for (i = 0; i < 7; i++) if (!strcmp(p, names[i])) script[nscript].buttons |= (uint8_t)(1 << i);
        nscript++;
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *dumps = 0, *outdir = ".", *ramlog = 0, *profs = 0;
    FILE *f, *rf = 0;
    int frames, i;
    if (argc < 3) { fprintf(stderr, "usage: ngpref rom.ngc FRAMES [-b bios] [-i input] [-d frames] [-o dir] [-c flow] [-s] [-w addrs] [-r ramfile] [-t]\n"); return 2; }
    f = fopen(argv[1], "rb"); if (!f) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END); rom_len = ftell(f); fseek(f, 0, SEEK_SET);
    rom = malloc((size_t)rom_len); if (fread(rom, 1, (size_t)rom_len, f) != (size_t)rom_len) return 2;
    fclose(f);
    frames = atoi(argv[2]);
    for (i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) { profs = argv[++i]; prof = calloc(1 << 24, sizeof *prof); zprof = calloc(1 << 16, sizeof *zprof); }
        else if (!strcmp(argv[i], "-z") && i + 1 < argc) zdump = argv[++i];
        else if (!strcmp(argv[i], "-W") && i + 1 < argc) { char *p = argv[++i]; while (*p && nzwait < 32) { zwait[nzwait++] = (uint16_t)strtoul(p, &p, 16); if (*p == ',') p++; } }
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) { f = fopen(argv[++i], "rb"); if (!f || fread(biosimg, 1, 0x10000, f) != 0x10000) { fprintf(stderr, "cannot read the BIOS image\n"); return 2; } fclose(f); have_bios = 1; }
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) load_script(argv[++i]);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) dumps = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outdir = argv[++i];
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) { flowf = fopen(argv[++i], "w"); seen_e = calloc(1 << 21, 1); seen_i = calloc(1 << 21, 1); }
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) ramlog = argv[++i];
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) { char *p = argv[++i]; while (*p && nwait < 32) { waitpc[nwait++] = (uint32_t)strtoul(p, &p, 16); if (*p == ',') p++; } }
        else if (!strcmp(argv[i], "-s")) stats = 1;
        else if (!strcmp(argv[i], "-m")) commlog = 1;
        else if (!strcmp(argv[i], "-t")) trace = 1;
    }
    if (ramlog) rf = fopen(ramlog, "wb");
    reset();
    for (frame = 1; frame <= frames; frame++) {
        char key[32], list[4096];
        pad = 0;
        for (i = 0; i < nscript; i++) if (frame >= script[i].first && frame <= script[i].last) pad |= script[i].buttons;
        snprintf(key, sizeof key, ",%d,", frame);
        if (profs) { snprintf(list, sizeof list, ",%s,", profs); profiling = strstr(list, key) != 0; }
        run_frame();
        if (profiling) {
            int k; uint32_t a;
            printf("frame %d: busiest addresses (instructions executed there):", frame);
            for (k = 0; k < 12; k++) { uint32_t best = 0; for (a = 0x200000; a < 0xa00000; a++) if (prof[a] > prof[best]) best = a; if (!prof[best]) break; printf(" %06x:%u", best, prof[best]); prof[best] = 0; }
            printf("\nframe %d: busiest Z80 addresses:", frame);
            for (k = 0; k < 12; k++) { uint32_t best = 0; for (a = 0; a < 0x10000; a++) if (zprof[a] > zprof[best]) best = a; if (!zprof[best]) break; printf(" %04x:%u", best, zprof[best]); zprof[best] = 0; }
            printf("\n"); memset(prof + 0x200000, 0, 0x800000 * sizeof *prof); memset(zprof, 0, 0x10000 * sizeof *zprof); profiling = 0;
        }
        if (stats) printf("frame %5d: %6ld instructions, %6ld of %d states busy (%3ld%%), %ld interrupts; Z80 %5ld instructions outside its idle loop, %ld interrupts\n", frame, frame_instr,
                          frame_states - frame_idle, LINE_STATES * LINES, (frame_states - frame_idle) * 100 / (LINE_STATES * LINES), frame_irq, frame_z80 - frame_z80_idle, frame_z80_irq);
        if (rf) fwrite(ram, 1, 0x3000, rf);
        if (dumps) { snprintf(key, sizeof key, ",%d,", frame); snprintf(list, sizeof list, ",%s,", dumps); if (strstr(list, key)) dump_ppm(outdir, frame); }
    }
    if (rf) fclose(rf);
    if (commlog) {
        int k;
        printf("main CPU and sound processor: instruction, address, read or write, times, first and last value\n");
        for (k = 0; k < ncomm; k++) printf("  %06x %06x %c %8ld  %02x %02x\n", comm[k].pc, comm[k].a, comm[k].rw, comm[k].n, comm[k].first, comm[k].last);
    }
    if (flowf) fclose(flowf);
    fprintf(stderr, "%d frames, %ld instructions, %ld Z80 instructions, %ld sound chip writes. BIOS calls used:", frames, total_instr, z80_instr, psg_writes);
    for (i = 0; i < 32; i++) if (bios_used & (1u << i)) fprintf(stderr, " %d", i);
    fprintf(stderr, "\nuses:%s%s%s%s%s%s%s%s\n", uses & USE_Z80 ? " Z80(sound program)" : "", uses & USE_PSG ? " sound-chip" : "", uses & USE_DAC ? " DAC" : "",
            uses & USE_DMA ? " micro-DMA" : "", uses & USE_SERIAL ? " serial" : "", uses & USE_FLASH ? " flash-writes" : "", uses & USE_MONO ? " mono-mode" : "", uses ? "" : " nothing that is not modelled");
    return 0;
}
