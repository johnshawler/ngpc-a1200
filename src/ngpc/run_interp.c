/* run_interp.c - host only, for the proof: runs the original code with the interpreter (cpu900.h) inside the
 * same frame model and the same machine (ngp.c) as the translation. Link it in place of run_c.c and the
 * generated files; the work RAM after every tick must then equal the translation's, byte for byte.
 * The places where the frame ends or the sound processor gets time are the ones the translator found
 * (src/ngpc/gen_waits.h), so both sides stop at the same instructions.
 */
#include "ngp.h"
#define CPU900_EXTERN
#include "cpu900.h"
#include "logic.h"
#include "run.h"
#include "gen_waits.h"
#include <stdio.h>
#include <stdlib.h>                          /* gen_waits[]: { address of the branch back, 1 = frame wait, 2 = poll } */

static uint8_t site[0x400000];                  /* per cartridge address: kind of wait at that branch */
static int sites_ready;

static uint8_t *site_of(uint32_t pc)
{
    if (pc - 0x200000u < 0x200000u) return &site[pc - 0x200000];
    if (pc - 0x800000u < 0x200000u) return &site[pc - 0x600000];
    return 0;
}

/* Host tools (host_test -c): the flow file of what runs, in tools/ngpref's format, so that the translator learns what
   the port itself reaches with an input script: E executed, I and C targets of jumps and calls through registers. */
FILE *interp_flow;
static uint8_t *flow_seen;
static void flow_pair(char kind, uint32_t to, uint32_t from)
{
    static uint32_t pairs[65536][2]; static int npairs; int i;
    for (i = 0; i < npairs; i++) if (pairs[i][0] == to && pairs[i][1] == from) return;
    if (npairs < 65536) { pairs[npairs][0] = to; pairs[npairs][1] = from; npairs++; }
    fprintf(interp_flow, "%c %06x %06x\n", kind, (unsigned)to, (unsigned)from);
}

uint32_t run_call(uint32_t pc)
{
    int depth = 0;
    if (!sites_ready) { unsigned i; for (i = 0; i < sizeof gen_waits / sizeof gen_waits[0]; i++) { uint8_t *s = site_of(gen_waits[i].pc); if (s) *s = gen_waits[i].kind; } sites_ready = 1; }
    cpu.pc = pc;
    for (;;) {
        uint32_t pc0 = cpu.pc, sp0;
        uint8_t f, op = 0, *s;
        int at = -1, call = 0, ret = 0;
        if (ngp_fatal_why) return 0;
        if (pc0 >= 0xff0000) {                  /* a BIOS address: it behaves like a subroutine that returns */
            uint32_t r = ngp_bios_entry(pc0);
            if (!depth) return r;
            depth--; cpu.pc = r; continue;
        }
        f = mem_r8(pc0);
        if (interp_flow) {
            if (!flow_seen) flow_seen = calloc(1 << 21, 1);
            if (!(flow_seen[pc0 >> 3] & (1 << (pc0 & 7)))) { flow_seen[pc0 >> 3] |= (uint8_t)(1 << (pc0 & 7)); fprintf(interp_flow, "E %06x\n", (unsigned)pc0); }
        }
        if (f == 0x05) { cpu.pc = pc0 + 1; ngp_wait(cpu.pc); continue; }                   /* HALT */
        if (f == 0xf9) { cpu.pc = pc0 + 1; ngp_bios_call(cpu.bank[3][0] >> 8 & 0xff); continue; }   /* SWI 1: system call */
        if (f >= 0xf8) { ngp_fatal(pc0, 1); return 0; }                                    /* other SWIs: not done */
        /* is it a call or a return? (the opcode of a "destination" instruction comes after its address bytes) */
        if (f == 0x1c || f == 0x1d || f == 0x1e) call = 1;
        else if (f == 0x0e || f == 0x0f || f == 0x07) ret = 1;
        else if (f >= 0xb0 && f <= 0xb7) at = 1; else if (f >= 0xb8 && f <= 0xbf) at = 2; else if (f == 0xf0 || f == 0xf4 || f == 0xf5) at = 2;
        else if (f == 0xf1) at = 3; else if (f == 0xf2) at = 4; else if (f == 0xf3) at = (mem_r8(pc0 + 1) & 3) == 0 ? 2 : 4;
        if (at > 0) { op = mem_r8(pc0 + (uint32_t)at); if (op >= 0xe0 && op <= 0xef) call = 1; else if (op >= 0xf0 && f == 0xb0) ret = 1; }
        sp0 = cpu.gpr[3];
        cpu_step(); ngp_cpu_steps++;
        if (interp_flow) {                      /* as tools/ngpref: jumps and calls through registers, and where they went */
            uint8_t s2 = mem_r8(pc0 + 1);
            int fat = -1;
            if (f >= 0xb0 && f <= 0xb7) fat = 1; else if (f >= 0xb8 && f <= 0xbf) fat = 2; else if (f == 0xf4 || f == 0xf5) fat = 2;
            else if (f == 0xf3) fat = (s2 & 3) == 0 ? 2 : 4;
            if (fat > 0) {
                uint8_t fop = mem_r8(pc0 + (uint32_t)fat);
                if (fop >= 0xd0 && fop <= 0xef && ((fop & 15) == 8 || cpu.pc != ((pc0 + (uint32_t)fat + 1) & 0xffffff)) && cpu.pc < 0xff0000)
                    flow_pair(fop < 0xe0 ? 'I' : 'C', cpu.pc, pc0);
            }
        }
        if (cpu.bad) { ngp_fatal(pc0, 1); return 0; }
        if (call && cpu.gpr[3] == sp0 - 4) depth++;                                        /* taken */
        else if (ret && cpu.gpr[3] != sp0) { if (!depth) return cpu.pc; depth--; }
        else if ((s = site_of(pc0)) != 0 && *s && cpu.pc < pc0) {                          /* a wait loop's branch back, taken */
            if (*s == 1) ngp_wait(cpu.pc); else ngp_poll(cpu.pc);
        }
    }
}
