/* psg.c - the T6W28's registers (see psg.h). */
#include <string.h>
#include "psg.h"

void psg_reset(Psg *p)
{
    memset(p, 0, sizeof *p);
    memset(p->s.att, 0xff, sizeof p->s.att);
}

void psg_write(Psg *p, unsigned port, uint8_t v)
{
    unsigned idx, per;
    uint8_t *pp;
    if (v & 0x80) p->latch[port] = v;
    idx = (p->latch[port] >> 5) & 3;
    if (p->latch[port] & 0x10) {                        /* a volume */
        if (port) p->s.att[idx] = (uint8_t)((p->s.att[idx] & 0x0f) | (v & 15) << 4);
        else p->s.att[idx] = (uint8_t)((p->s.att[idx] & 0xf0) | (v & 15));
        p->known |= (uint16_t)(1u << (9 + idx));
        return;
    }
    if (port ? idx == 3 : idx < 2) return;              /* left: tones 0-2; right: the noise's period and control */
    if (!port && idx == 3) { p->s.noise_ctl = v & 7; p->known |= 1u << 8; return; }
    pp = port ? p->s.tone[idx] : p->s.noise_per;
    p->known |= (uint16_t)(3u << (port ? 2 * idx : 6));
    per = (unsigned)(pp[0] | pp[1] << 8);
    if (v & 0x80) per = (per & 0x3f0) | (v & 15);        /* the latch byte: the low 4 bits */
    else per = (per & 15) | (v & 0x3f) << 4;            /* the data byte: the high 6 bits */
    pp[0] = (uint8_t)per; pp[1] = (uint8_t)(per >> 8);
}

unsigned psg_period(const PsgState *s, int voice)
{
    static const unsigned fixed[3] = { 16, 32, 64 };
    if (voice < 3) return (unsigned)(s->tone[voice][0] | s->tone[voice][1] << 8);
    if ((s->noise_ctl & 3) < 3) return fixed[s->noise_ctl & 3];
    return (unsigned)(s->noise_per[0] | s->noise_per[1] << 8);
}
