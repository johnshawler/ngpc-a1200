/* run68k.c - the Amiga runs the 68k back end of tools/recomp900.py (src/ngpc/gen68k.s, made by tools/back900.py).
 * This is its C side: the registers between the 68k registers and the CPU state, and what the assembly hands to C
 * (the interpreter's step, waits, BIOS calls, memory other than RAM and the cartridge). */
#include <stddef.h>
#include "ngp.h"
#define CPU900_EXTERN
#include "cpu900.h"
#include "gen900.h"
#include "logic.h"
#include "run.h"

/* d3 (flags: C bit 0, V 1, Z 2, S 3, H 5, N 6), d4-d7 (XWA..XHL of the current bank), a2-a5 (XIX..XSP) */
uint32_t asm_regs[9];
/* the assembly reads the four banks and the current bank's address straight from cpu (registers named by their bank) */
typedef char cpu_bank_at_0[offsetof(Cpu900, bank) == 0 ? 1 : -1];
typedef char cpu_cur_at_80[offsetof(Cpu900, cur) == 80 ? 1 : -1];
uint32_t asm_call(uint32_t pc);

static uint8_t sr_of_d3[128], d3_of_sr[256];
static int tabs_ready;
static void make_tabs(void)
{
    unsigned f;
    for (f = 0; f < 256; f++) {
        unsigned d = (f & 1) | ((f >> 2) & 1) << 1 | ((f >> 6) & 1) << 2 | ((f >> 7) & 1) << 3 | ((f >> 4) & 1) << 5 | ((f >> 1) & 1) << 6;
        d3_of_sr[f] = (uint8_t)d;
        if (!(f & 0x28)) sr_of_d3[d] = (uint8_t)f;          /* the SR's bits 3 and 5 are not flags */
    }
    tabs_ready = 1;
}
void asm_regs_out(void)
{
    int i;
    if (!tabs_ready) make_tabs();
    for (i = 0; i < 4; i++) { cpu.cur[i] = asm_regs[1 + i]; cpu.gpr[i] = asm_regs[5 + i]; }
    cpu.sr = (uint16_t)((cpu.sr & 0xff28) | sr_of_d3[asm_regs[0] & 0x7f]);
}
void asm_regs_in(void)
{
    int i;
    if (!tabs_ready) make_tabs();
    for (i = 0; i < 4; i++) { asm_regs[1 + i] = cpu.cur[i]; asm_regs[5 + i] = cpu.gpr[i]; }
    asm_regs[0] = d3_of_sr[cpu.sr & 0xd7];
}
uint32_t asm_step(uint32_t pc)
{
    asm_regs_out();
    cpu.pc = pc;
    do cpu_step(); while (cpu.pc == pc && !cpu.bad);
    if (cpu.bad) ngp_fatal(pc, 1);
    asm_regs_in();
    return cpu.pc;
}
void asm_wait(uint32_t pc) { asm_regs_out(); ngp_wait(pc); asm_regs_in(); }
void asm_poll(uint32_t pc) { asm_regs_out(); ngp_poll(pc); asm_regs_in(); }
void asm_swi(void) { asm_regs_out(); ngp_bios_call(cpu.bank[3][0] >> 8 & 0xff); asm_regs_in(); }
uint32_t asm_bios(uint32_t pc) { uint32_t r; asm_regs_out(); r = ngp_bios_entry(pc); asm_regs_in(); return r; }
uint32_t asm_fatal(uint32_t pc, int why) { asm_regs_out(); ngp_fatal(pc, why); asm_regs_in(); return 0; }
void asm_ei(uint32_t n) { cpu.sr = (uint16_t)((cpu.sr & ~0x7000) | (n << 12)); }
uint32_t asm_rd8(uint32_t a) { return RD8(a); }
uint32_t asm_rd16(uint32_t a) { return RD16(a); }
uint32_t asm_rd32(uint32_t a) { return RD32(a); }
void asm_wr8(uint32_t a, uint32_t v) { WR8(a, v); }
void asm_wr16(uint32_t a, uint32_t v) { WR16(a, v); }
void asm_wr32(uint32_t a, uint32_t v) { WR32(a, v); }
uint32_t run_call(uint32_t pc) { return asm_call(pc); }
