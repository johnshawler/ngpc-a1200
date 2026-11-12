/* gen900.h - what the code tools/recomp900.py generates is written in: direct access to the registers, to memory
 * and to the stack, and the flags. Every primitive here does exactly what the instruction's case in cpu900.h does;
 * tools/verify_ngpc.sh proves the two equal after every tick.
 *
 * Registers: R is the current bank (cpu.cur, reloaded after anything that may change RFP). A register is reached
 * by its address, so a byte or word register is one memory access on either byte order.
 * Flags live in the function's locals fS fZ fH fV fN fC (0 or 1) and go through cpu.sr only where something
 * outside the function can see them; the translator computes only the flags that are read later.
 * Memory is little-endian. RAM (0x4000-0x6FFF) and the cartridge are read and written directly; everything else
 * goes through ngp.c.
 */
#ifndef GEN900_H
#define GEN900_H
#include <stdint.h>
#include "ngp.h"

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define B8(p, k)  (((uint8_t *)(p))[3 - (k)])
#define W16(p, k) (((uint16_t *)(p))[1 - (k)])
#else
#define B8(p, k)  (((uint8_t *)(p))[k])
#define W16(p, k) (((uint16_t *)(p))[k])
#endif
#define PREV() (cpu.bank[((cpu.sr >> 8) - 1) & 3])       /* the bank before the current one (register codes 0xD0-0xDF) */

#define LE16(p) ((uint32_t)((p)[0] | (p)[1] << 8))
#define LE32(p) ((uint32_t)((p)[0] | (p)[1] << 8 | (uint32_t)(p)[2] << 16 | (uint32_t)(p)[3] << 24))

static inline uint32_t RD8(uint32_t a) { return mem_r8(a & 0xffffff); }
static inline uint32_t RD16(uint32_t a)
{
    a &= 0xffffff;
    if (a - 0x4000u < 0x3fffu) return LE16(ngp_ram + a - 0x4000);
    if (a - 0x200000u < 0x1fffffu) return LE16(ngp_rom + a - 0x200000);
    return mem_r8(a) | (uint32_t)mem_r8((a + 1) & 0xffffff) << 8;
}
static inline uint32_t RD32(uint32_t a)
{
    a &= 0xffffff;
    if (a - 0x4000u < 0x3ffdu) return LE32(ngp_ram + a - 0x4000);
    if (a - 0x200000u < 0x1ffffdu) return LE32(ngp_rom + a - 0x200000);
    return RD16(a) | RD16(a + 2) << 16;
}
static inline void WR8(uint32_t a, uint32_t v) { mem_w8(a & 0xffffff, (uint8_t)v); }
static inline void WR16(uint32_t a, uint32_t v)
{
    a &= 0xffffff;
    if (a - 0x4000u < 0x2fffu) { uint8_t *p = ngp_ram + a - 0x4000; p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); return; }
    mem_w8(a, (uint8_t)v); mem_w8((a + 1) & 0xffffff, (uint8_t)(v >> 8));
}
static inline void WR32(uint32_t a, uint32_t v)
{
    a &= 0xffffff;
    if (a - 0x4000u < 0x2ffdu) { uint8_t *p = ngp_ram + a - 0x4000; p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); return; }
    WR16(a, v); WR16(a + 2, v >> 16);
}
#define XSP_ cpu.gpr[3]
#define PUSH8(v)  do { XSP_ -= 1; WR8(XSP_, (v)); } while (0)
#define PUSH16(v) do { XSP_ -= 2; WR16(XSP_, (v)); } while (0)
#define PUSH32(v) do { XSP_ -= 4; WR32(XSP_, (v)); } while (0)
static inline uint32_t POP8(void) { uint32_t v = RD8(XSP_); XSP_ += 1; return v; }
static inline uint32_t POP16(void) { uint32_t v = RD16(XSP_); XSP_ += 2; return v; }
static inline uint32_t POP32(void) { uint32_t v = RD32(XSP_); XSP_ += 4; return v; }

#define EVEN8(v)  ((uint32_t)ngp_even[(v) & 0xff])
#define EVEN16(v) ((uint32_t)(ngp_even[(v) & 0xff] == ngp_even[((v) >> 8) & 0xff]))

/* flags between the locals and cpu.sr: m is a mask of F_S F_Z F_H F_V F_N F_C */
#define SAVEF(m) (cpu.sr = (uint16_t)((cpu.sr & ~(m)) | (((fS << 7) | (fZ << 6) | (fH << 4) | (fV << 2) | (fN << 1) | fC) & (m))))
#define LOADF_S() (fS = (cpu.sr >> 7) & 1)
#define LOADF_Z() (fZ = (cpu.sr >> 6) & 1)
#define LOADF_H() (fH = (cpu.sr >> 4) & 1)
#define LOADF_V() (fV = (cpu.sr >> 2) & 1)
#define LOADF_N() (fN = (cpu.sr >> 1) & 1)
#define LOADF_C() (fC = cpu.sr & 1)
/* conditions from the flags in the locals, as cpu_cond() */
#define CC_LT  (fS ^ fV)
#define CC_LE  (fZ | (fS ^ fV))
#define CC_ULE (fC | fZ)
#endif
