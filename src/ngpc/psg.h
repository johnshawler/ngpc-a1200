/* psg.h - the Neo Geo Pocket's sound chip (T6W28) as register state. Plain C, no platform header: the host's sound
 * recorder (tools/sndrec_ngpc.c) and the Amiga's player (src/ngpc/sound.c) share it.
 *
 * The T6W28 is an SN76489 with two write ports (after Mednafen's T6W28_Apu): the left one (Z80 0x4001, main CPU
 * 0xA1) sets the three tone periods and the left volumes, the right one (Z80 0x4000, main CPU 0xA0) the right
 * volumes, the noise's own period (a tone 2 write there) and the noise control. Tone frequency = 3,072,000 / (32 x
 * period); a volume is an attenuation, 0 = loudest, 15 = off.
 */
#ifndef PSG_H
#define PSG_H
#include <stdint.h>

#define PSG_STATE 13            /* bytes of PsgState: what a recorded stream holds, frame by frame */
typedef struct {
    uint8_t tone[3][2];         /* tone periods 0-2, low byte and high byte (10 bits) */
    uint8_t noise_per[2];       /* the noise's own period (used when the control selects 3) */
    uint8_t noise_ctl;          /* bits 0-1: rate (0-2 fixed, 3 = noise_per), bit 2: white (else periodic) */
    uint8_t att[4];             /* per voice (0-2 tones, 3 noise): left attenuation << 4 | right attenuation */
} PsgState;

typedef struct { uint8_t latch[2]; uint16_t known; PsgState s; } Psg;   /* known: bit n = byte n of s written */

void psg_reset(Psg *p);                                 /* every voice off */
void psg_write(Psg *p, unsigned port, uint8_t v);       /* port 0 = right (0x4000), 1 = left (0x4001) */
unsigned psg_period(const PsgState *s, int voice);      /* tone period of voice 0-2; the noise's for 3 */
#endif
