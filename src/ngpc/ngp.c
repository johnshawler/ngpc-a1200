/* ngp.c - see ngp.h. The machine without its timing: what a read or a write does, the BIOS's system calls,
 * the sound processor's share of a frame. */
#include <string.h>
#include "ngp.h"
#define CPU900_EXTERN
#define CPU900_DEFINE                   /* the one CPU state lives in this file */
#include "cpu900.h"

uint8_t ngp_ram[0x4000], ngp_vid[0x4000], ngp_io[0x100];
const uint8_t *ngp_rom; uint32_t ngp_rom_len; const uint8_t *ngp_font;
uint8_t ngp_pad;
uint32_t ngp_fatal_pc; int ngp_fatal_why;
uint32_t ngp_frames;
uint32_t ngp_vid_gen; uint8_t ngp_pal_dirty;
const uint8_t ngp_even[256] = {               /* 1 = an even number of bits set: the V flag of logic operations */
    1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1,0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0,
    0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0,1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1,
    0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0,1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1,
    1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1,0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0,
    0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0,1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1,
    1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1,0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0,
    1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1,0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0,
    0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0,1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1,
};
uint32_t ngp_z80_steps, ngp_cpu_steps;      /* instructions run: sound program (interpreter build); main CPU (interpreter) */

static uint8_t ipend[16];               /* by number in the RAM vector table: 5 vertical blank, 6 Z80, 7-10 timers */
static uint8_t snd_on, tff3;
static uint32_t z80_serial;             /* counts what the main CPU did to the sound processor */
static int32_t snd_irq_in;              /* T-states of the sound processor's time until its next interrupt */

/* the system call table of the BIOS: the addresses games read at 0xFFFE00 and call */
static const uint32_t sys_addr[27] = {
    0xff27a2, 0xff1034, 0xff1440, 0xff12b4, 0xff1222, 0xff8d8a, 0xff6fd8, 0xff7042, 0xff7082, 0xff149b, 0xff1033, 0xff1487,
    0xff731f, 0xff70ca, 0xff17c4, 0xff1032, 0xff2bbd, 0xff2c0c, 0xff2c44, 0xff2c86, 0xff2cb4, 0xff2d27, 0xff2d33, 0xff2d3a,
    0xff2d4e, 0xff2d6c, 0xff2d85 };

/* ---- the sound processor ----
 * The frame model gives the sound processor time in fixed amounts: a frame's worth at each frame start, a little at
 * each poll of the main CPU. That time decides when its timer interrupts fall. What it does in answer to an event
 * (being started, a timer interrupt, an NMI from the main CPU) is done at once, before the main CPU goes on: on the
 * console it takes some hundred microseconds, and games wait for the answer anyway.
 *
 * Two implementations of the same behaviour, chosen when compiling:
 *   default          a model of what the main CPU can see of the sound program: no Z80 at all. It is what the
 *                    Amiga runs. It holds for SNK's sound driver as Neo Turf Masters has it (see below);
 *                    tools/check_sound_ngpc.sh proves it for a game, tick by tick, against
 *   NGP_Z80_INTERP   the game's real sound program on a Z80 interpreter (host only).
 */
static void snd_start(void);            /* the main CPU switched the Z80 on: it runs its start-up code */
static void snd_nmi(void);              /* the main CPU signalled it (NMI): a command byte waits in 0xBC */
static void snd_irq(void);              /* timer 3's output rose: the sound processor's timer interrupt */

/* T-states between two interrupts of the sound processor: timer 3's output toggles at every match, the rising
   edge is the interrupt. 0 = none. */
static int32_t snd_irq_period(void)
{
    static const uint8_t sh1[4] = { 0, 7, 11, 15 }, sh2[4] = { 7, 7, 9, 11 };
    uint32_t t3 = ngp_io[0x27] ? ngp_io[0x27] : 256, t2 = ngp_io[0x26] ? ngp_io[0x26] : 256, m = (ngp_io[0x28] >> 2) & 3, states;
    if (!(ngp_io[0x20] & 0x80) || !(ngp_io[0x20] & 8) || !(ngp_io[0x25] & 0x20)) return 0;
    if (m) states = t3 << sh1[m];
    else { if (!(ngp_io[0x20] & 4)) return 0; states = (t2 * t3) << sh2[ngp_io[0x28] & 3]; }
    return (int32_t)states;                             /* two matches per interrupt, two CPU states per T-state */
}
static void snd_time(int32_t t)                         /* t T-states pass: the interrupts that fall due are taken */
{
    int32_t period;
    if (!snd_on) return;
    while ((period = snd_irq_period()) != 0) {
        if (snd_irq_in <= 0) snd_irq_in = period;
        if (snd_irq_in > t) { snd_irq_in -= t; return; }
        t -= snd_irq_in; snd_irq_in = 0;
        snd_irq();
    }
}
void ngp_z80_frame(void) { snd_time(515 * 199 / 2); }
void ngp_z80_slice(void) { snd_time(256); }

#ifdef NGP_Z80_INTERP
static uint8_t z80_wrote;
static uint8_t z80_mr(uint16_t a) { return a < 0x1000 ? ngp_ram[0x3000 + a] : a == 0x8000 ? ngp_io[0xbc] : 0xff; }
static void z80_mw(uint16_t a, uint8_t v)
{
    z80_wrote = 1;
    if (a < 0x1000) ngp_ram[0x3000 + a] = v;
    else if (a == 0x4000 || a == 0x4001) ngp_io[0xa0 + (a & 1)] = v;      /* the sound chip */
    else if (a == 0x8000) ngp_io[0xbc] = v;
    else if (a == 0xc000) ipend[6] = 1;                                   /* interrupt to the main CPU */
}
static uint8_t z80_in(uint16_t port);
static void z80_out(uint16_t port, uint8_t v);
#include "z80.h"
static uint8_t z80_in(uint16_t port) { (void)port; return 0xff; }
static void z80_out(uint16_t port, uint8_t v) { (void)port; (void)v; z80.irq_line = 0; z80_wrote = 1; }

/* Run the sound program until it idles: a jump back to the head of a loop in exactly the state it had there the
   last time, with nothing written by either processor since and no interrupt waiting. It would go round until
   something changes. No game knowledge is needed for it. */
static void z80_settle(void)
{
    static Z80 seen; static uint32_t seen_serial; static uint8_t seen_valid;
    long n;
    for (n = 0; n < 2000000; n++) {
        uint16_t pc0;
        int pending = (z80.irq_line && z80.iff1) || z80.nmi_pending;
        if (z80.halted && !pending) return;
        pc0 = z80.pc;
        z80_step(); ngp_z80_steps++;
        if (z80.pc < pc0 && !((z80.irq_line && z80.iff1) || z80.nmi_pending)) {
            if (seen_valid && !z80_wrote && seen_serial == z80_serial && !memcmp(&seen, &z80, sizeof z80)) return;
            memcpy(&seen, &z80, sizeof z80); seen_serial = z80_serial; seen_valid = 1; z80_wrote = 0;
        }
    }
    ngp_fatal(0x7000 + z80.pc, 5);
}
static void snd_start(void) { z80_reset(); z80_settle(); }
static void snd_nmi(void) { z80_settle(); z80.nmi_pending = 1; z80_settle(); }
static void snd_irq(void) { z80.irq_line = 1; z80_settle(); }
#else
/* SNK's sound driver, as the main CPU can see it (read from Neo Turf Masters' sound program, Z80 addresses):
 *  - started, it clears its variables (0x00C0-0x01E5, its tick counter at 0x00DE among them), zeroes its interrupt
 *    count and interrupts the main CPU once (a write to 0xC000);
 *  - its NMI handler (0x0066) queues the command byte from 0x8000 (the main CPU's 0xBC) and writes back its
 *    complement; the command 0xFF restarts the driver (0x062C: a return to address 0);
 *  - its timer interrupt (0x0038) counts; its main loop (0x0218) copies the count to 0x00DE.
 * Nothing else it does is read by the main CPU (tools/ngpref -m lists what the main CPU reads). */
static uint8_t snd_tick;
static void snd_start(void) { snd_tick = 0; ngp_ram[0x70de - 0x4000] = 0; ipend[6] = 1; }
static void snd_nmi(void)
{
    uint8_t c = ngp_io[0xbc];
    ngp_io[0xbc] = (uint8_t)~c;
    if (c == 0xff) snd_start();
}
static void snd_irq(void) { snd_tick++; ngp_ram[0x70de - 0x4000] = snd_tick; }
#endif

/* ---- memory outside RAM and ROM ---- */
uint8_t ngp_rd_slow(uint32_t a)
{
    if (a < 0x100) {
        switch (a) {
        case 0x71: return (uint8_t)((ipend[5] ? 0x08 : 0) | (ipend[6] ? 0x80 : 0));
        case 0x73: return (uint8_t)((ipend[7] ? 0x08 : 0) | (ipend[8] ? 0x80 : 0));
        case 0x74: return (uint8_t)((ipend[9] ? 0x08 : 0) | (ipend[10] ? 0x80 : 0));
        case 0xb0: return ngp_pad;
        case 0xb1: return 0x03;                         /* sub battery fine, power button not pressed */
        }
        return ngp_io[a];
    }
    if (a - 0x8000u < 0x4000u) {
        if (a == 0x8008 || a == 0x8009) return 0;       /* raster position: the frame model has none */
        if (a == 0x8010) return 0;                      /* blanking flag: not modelled (see PLAYBOOK-NGPC.md) */
        return ngp_vid[a - 0x8000];
    }
    if (a - 0x800000u < 0x200000u) return a - 0x600000u < ngp_rom_len ? ngp_rom[a - 0x600000] : 0xff;
    if (a >= 0xfffe00 && a < 0xfffe00 + 27 * 4) return (uint8_t)(sys_addr[(a - 0xfffe00) >> 2] >> ((a & 3) * 8));
    return a >= 0xff0000 ? 0xff : 0;
}
void ngp_wr_slow(uint32_t a, uint8_t v)
{
    if (a < 0x100) {
        switch (a) {
        case 0x25: if ((v & 0xc0) == 0x00) tff3 = !tff3; else if ((v & 0xc0) == 0x40) tff3 = 1; else if ((v & 0xc0) == 0x80) tff3 = 0; break;
        case 0x71: if (!(v & 0x08)) ipend[5] = 0; if (!(v & 0x80)) ipend[6] = 0; break;
        case 0x73: if (!(v & 0x08)) ipend[7] = 0; if (!(v & 0x80)) ipend[8] = 0; break;
        case 0x74: if (!(v & 0x08)) ipend[9] = 0; if (!(v & 0x80)) ipend[10] = 0; break;
        case 0xb9: z80_serial++; if (v == 0x55) { ngp_io[a] = v; snd_on = 1; snd_irq_in = 0; snd_start(); } else if (v == 0xaa) snd_on = 0; break;
        case 0xba: z80_serial++; ngp_io[a] = v; if (snd_on) snd_nmi(); return;
        case 0xbc: z80_serial++; break;
        }
        ngp_io[a] = v; return;
    }
    if (a - 0x7000u < 0x1000u) { ngp_ram[a - 0x4000] = v; z80_serial++; return; }
    if (a - 0x8000u < 0x4000u) {
        uint32_t o = a - 0x8000;
        if (o >= 0x200 && o < 0x400 && (o & 1)) v &= 0x0f;              /* palette: 12 bits */
        if (o == 0x7e2 && ngp_vid[0x7f0] != 0xaa) return;               /* the mode register is locked */
        if (o == 0x10 || o == 0x08 || o == 0x09) return;                /* read-only */
        if (ngp_vid[o] == v) return;
        if (o - 0x200u < 0x200u) ngp_pal_dirty = 1;                       /* a colour: the picture's pens stay */
        else { ngp_vid_gen++; if (o == 0x12) ngp_pal_dirty = 1; }         /* 0x8012: the edge pen and the negative bit */
        ngp_vid[o] = v;
    }                                                                   /* writes to the cartridge (flash commands) are dropped */
}

/* the rare cases of the generated code's accessors (gen900.h) */
uint32_t ngp_rd16_slow(uint32_t a) { return mem_r8(a) | (uint32_t)mem_r8((a + 1) & 0xffffff) << 8; }
uint32_t ngp_rd32_slow(uint32_t a) { return ngp_rd16_slow(a) | ngp_rd16_slow((a + 2) & 0xffffff) << 16; }
void ngp_wr16_slow(uint32_t a, uint32_t v) { mem_w8(a, (uint8_t)v); mem_w8((a + 1) & 0xffffff, (uint8_t)(v >> 8)); }
void ngp_wr32_slow(uint32_t a, uint32_t v) { ngp_wr16_slow(a, v); ngp_wr16_slow((a + 2) & 0xffffff, v >> 16); }

/* ---- the BIOS ---- */
int ngp_bios_at(uint32_t pc)
{
    int i;
    for (i = 0; i < 27; i++) if (sys_addr[i] == pc) return i;
    return -1;
}
void ngp_bios_call(unsigned n)
{
    uint32_t xhl3 = cpu.bank[3][3];
    uint8_t ra3 = (uint8_t)cpu.bank[3][0], rb3 = (uint8_t)(cpu.bank[3][1] >> 8), rc3 = (uint8_t)cpu.bank[3][1];
    unsigned i, j;
    switch (n) {
    case 1: ngp_io[0x80] = rb3; break;                                  /* clock gear: recorded, not acted on */
    case 2: if (xhl3 < 0xc000) for (i = 0; i < 7; i++) mem_w8(xhl3 + i, ngp_io[0x91 + i]); break;        /* date and time */
    case 4: {                                                           /* interrupt level of one source */
        static const uint8_t reg[10] = { 0x70, 0x71, 0x73, 0x73, 0x74, 0x74, 0x79, 0x79, 0x7a, 0x7a };
        if (rc3 < 10) {
            uint8_t r = ngp_io[reg[rc3]];
            r = (rc3 & 1) ? (uint8_t)((r & 0x0f) | ((rb3 & 7) << 4)) : (uint8_t)((r & 0xf0) | (rb3 & 7));
            ngp_wr_slow(reg[rc3], r);
        }
        break; }
    case 5:                                                             /* the system font into character RAM */
        for (i = 0; i < 0x800; i++) {
            uint8_t c = ngp_font ? ngp_font[i] : (uint8_t)((i & 7) == 0 || (i & 7) == 7 ? 0xff : 0x81);
            unsigned w = 0;
            for (j = 0; j < 8; j++, c <<= 1) w = (w << 2) | ((c & 0x80) ? (ra3 & 3u) : (unsigned)(ra3 >> 4) & 3u);
            ngp_vid[0x2000 + i * 2] = (uint8_t)w; ngp_vid[0x2001 + i * 2] = (uint8_t)(w >> 8);
        }
        break;
    case 6: case 7: case 8: case 9: case 11: case 13: case 16: cpu.bank[3][0] &= ~0xffu; break;   /* flash, alarm, serial init: "done" */
    default: break;                                                     /* 0 power off, 3, 10, 12, 14, 15, serial: nothing */
    }
}

void ngp_fatal(uint32_t pc, int why) { if (!ngp_fatal_why) { ngp_fatal_pc = pc; ngp_fatal_why = why; } }

/* ---- interrupts, as the frame model delivers them ---- */
void ngp_frame_start(void)
{
    ngp_frames++;
    ngp_ram[0x6f82 - 0x4000] = ngp_pad;                 /* the BIOS's own vertical blank work: the controller */
    if (ngp_vid[0] & 0x80) ipend[5] = 1;
}
int ngp_irq_next(void)
{
    static const struct { uint8_t idx, reg, shift; } src[6] = {
        { 5, 0x71, 0 }, { 6, 0x71, 4 }, { 7, 0x73, 0 }, { 8, 0x73, 4 }, { 9, 0x74, 0 }, { 10, 0x74, 4 } };
    int i;
    for (i = 0; i < 6; i++) {
        int prio = (ngp_io[src[i].reg] >> src[i].shift) & 7;
        if (ipend[src[i].idx] && prio && prio != 7 && (int)CPU_IFF <= prio) return src[i].idx;
    }
    return -1;
}
uint32_t ngp_irq_enter(int idx)
{
    static const uint8_t reg[11] = { 0, 0, 0, 0, 0, 0x71, 0x71, 0x73, 0x73, 0x74, 0x74 };
    int prio = (ngp_io[reg[idx]] >> ((idx & 1) ? 0 : 4)) & 7;
    ipend[idx] = 0;
    cpu_push(4, cpu.pc); cpu_push(2, cpu.sr);
    cpu.sr = (uint16_t)((cpu.sr & ~0x7000) | ((prio < 7 ? prio + 1 : 7) << 12));
    return cpu_rd(4, 0x6fb8 + idx * 4) & 0xffffff;
}

void ngp_init(const uint8_t *rom, uint32_t len, const uint8_t *font)
{
    int i;
    ngp_rom = rom; ngp_rom_len = len; ngp_font = font;
    memset(ngp_ram, 0, sizeof ngp_ram); memset(ngp_vid, 0, sizeof ngp_vid); memset(ngp_io, 0, sizeof ngp_io);
    memset(&cpu, 0, sizeof cpu); memset(ipend, 0, sizeof ipend);
    snd_on = 0; tff3 = 0; z80_serial = 0; snd_irq_in = 0; ngp_frames = 0; ngp_fatal_why = 0; ngp_pad = 0;
    cpu.pc = (rom[0x1c] | rom[0x1d] << 8 | rom[0x1e] << 16) & 0xffffff;
    cpu.sr = 0xf800; CPU_RFP_SET();
    cpu.gpr[0] = 0xff23c3; cpu.gpr[1] = 0xff23df; cpu.gpr[2] = 0x006480; cpu.gpr[3] = 0x6c00;
#define RAM(a) ngp_ram[(a) - 0x4000]
    for (i = 0; i < 4; i++) RAM(0x6c00 + i) = rom[0x1c + i];
    RAM(0x6c04) = rom[0x20]; RAM(0x6c05) = rom[0x21]; RAM(0x6e82) = rom[0x20]; RAM(0x6e83) = rom[0x21];
    RAM(0x6c06) = rom[0x22]; RAM(0x6e84) = rom[0x22];
    for (i = 0; i < 12; i++) RAM(0x6c08 + i) = rom[0x24 + i];
    RAM(0x6c58) = 1; RAM(0x6c59) = len > 0x200000; RAM(0x6c55) = 1;
    RAM(0x6f80) = 0xff; RAM(0x6f81) = 0x03;             /* battery full */
    RAM(0x6f84) = 0x40;                                 /* started by power on */
    RAM(0x6f87) = 1;                                    /* language: English */
    RAM(0x6f91) = rom[0x23]; RAM(0x6f95) = rom[0x23];   /* colour or mono game */
    for (i = 0; i < 18; i++) { uint8_t *p = &RAM(0x6fb8 + i * 4); p[0] = 0xdf; p[1] = 0x23; p[2] = 0xff; p[3] = 0; }
#undef RAM
    ngp_vid[0x000] = 0xc0; ngp_vid[0x004] = 0xff; ngp_vid[0x005] = 0xff; ngp_vid[0x006] = 0xc6; ngp_vid[0x118] = 0x80;
    ngp_vid[0x3e0] = 0xff; ngp_vid[0x3e1] = 0x0f; ngp_vid[0x3f0] = 0xff; ngp_vid[0x3f1] = 0x0f; ngp_vid[0x400] = 0xff; ngp_vid[0x402] = 0x80;
    ngp_vid[0x7e2] = rom[0x23] ? 0x00 : 0x80;
    /* the CPU's own registers as the BIOS leaves them: timers stopped, vertical blank at level 2, the Z80 at 3 */
    ngp_io[0x20] = 0x80; ngp_io[0x22] = 0x01; ngp_io[0x23] = 0x90; ngp_io[0x24] = 0x03; ngp_io[0x25] = 0xb0; ngp_io[0x26] = 0x90;
    ngp_io[0x27] = 0x62; ngp_io[0x28] = 0x05; ngp_io[0x6f] = 0x4e; ngp_io[0x70] = 0x02; ngp_io[0x71] = 0x32; ngp_io[0xb8] = 0xaa; ngp_io[0xb9] = 0xaa;
    ngp_io[0x91] = 0x99; ngp_io[0x92] = 0x01; ngp_io[0x93] = 0x01; ngp_io[0x94] = 0x12; ngp_io[0x95] = 0x00; ngp_io[0x96] = 0x00; ngp_io[0x97] = 0x05;
}
