/* sndplay.c - the recorded streams, mixed per voice (see sndplay.h). */
#include <string.h>
#include "sndplay.h"

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

uint32_t sound_key(const uint8_t *ram)
{
    uint32_t h = 2166136261u;
    unsigned a;
    for (a = 0; a < 0x1000; a++) if (a != SOUND_TICK) { h ^= ram[a]; h *= 16777619u; }
    return h;
}

void sound_player_init(SoundPlayer *pl, const uint8_t *data, uint32_t len)
{
    Psg z;
    memset(pl, 0, sizeof *pl);
    psg_reset(&z); pl->chip = z.s;
    if (data && len >= 8 && !memcmp(data, "NGPS", 4)) { pl->data = data; pl->len = len; pl->count = be32(data + 4); }
}

static void stop_all(SoundPlayer *pl) { int i; for (i = 0; i < SOUND_ACTIVE; i++) pl->act[i].on = 0; pl->stop_in = 0; }
static void off(SoundPlayer *pl, SoundStream *s)        /* an effect gives its voices back to the music */
{
    int v;
    if (!s->music) for (v = 0; v < 4; v++) if (s->owned >> v & 1) pl->wait[v] = 1;
    s->on = 0;
}

void sound_player_cmd(SoundPlayer *pl, uint32_t key, uint8_t cmd)
{
    const uint8_t *e = 0;
    SoundStream *s = 0;
    uint32_t i;
    if (cmd == 0xff) { stop_all(pl); return; }
    for (i = 0; i < pl->count; i++) {
        const uint8_t *x = pl->data + 8 + i * SOUND_ENTRY;
        if (be32(x) == key && x[4] == cmd) { e = x; break; }
    }
    if (!e) { pl->missing++; return; }
    if (!e[5]) {                                        /* a command that sounds nothing by itself */
        if (e[7] & 4) return;                           /* it changes nothing that plays */
        pl->stop_in = pl->fade = be32(e + 8);          /* it stops everything, at once or after a fade */
        if (!pl->stop_in) stop_all(pl);
        return;
    }
    for (i = 0; i < SOUND_ACTIVE; i++) {                /* what the new stream replaces */
        SoundStream *o = &pl->act[i];
        if (!o->on) continue;
        if ((e[7] & 8) ? o->music : (!o->music && (o->owned & e[5]))) o->on = 0;
    }
    for (i = 0; i < SOUND_ACTIVE && !s; i++) if (!pl->act[i].on) s = &pl->act[i];
    if (!s) {                                           /* all in use: the oldest goes */
        s = &pl->act[0];
        for (i = 1; i < SOUND_ACTIVE; i++) if (pl->act[i].seq < s->seq) s = &pl->act[i];
    }
    s->e = e; s->p = s->loop_p = pl->data + be32(e + 16);
    s->f = 0; s->n = be32(e + 8); s->loop = be32(e + 12); s->seq = ++pl->seq;
    s->owned = e[5]; s->loops = e[7] & 1; s->music = (e[7] & 8) != 0; s->on = s->n != 0; s->known = s->last = 0;
    { Psg z; psg_reset(&z); s->s = z.s; }
}

static void step(SoundPlayer *pl, SoundStream *s)      /* the stream's next frame */
{
    uint8_t *st = (uint8_t *)&s->s;
    unsigned m, j;
    if (s->f == s->n) {
        if (!s->loops) { off(pl, s); return; }
        s->f = s->loop; s->p = s->loop_p;               /* back to the loop frame, from the state before it */
        memcpy(&s->s, s->e + 20, PSG_STATE);
    }
    if (s->f == s->loop) s->loop_p = s->p;
    m = (unsigned)(s->p[0] << 8 | s->p[1]); s->p += 2;
    for (j = 0; j < PSG_STATE; j++) if (m >> j & 1) st[j] = *s->p++;
    s->known |= (uint16_t)m; s->last = (uint16_t)m;
    s->f++;
}

void sound_player_frame(SoundPlayer *pl, PsgState *out)
{
    static const uint16_t vbytes[4] = { 0x0203, 0x040c, 0x0830, 0x10c0 | 0x100 };   /* each voice's PsgState bytes */
    int v, i;
    unsigned j;
    static const uint16_t vnote[4] = { 0x0003, 0x000c, 0x0030, 0x01c0 };             /* and those that start a note */
    unsigned fade_add = pl->stop_in ? 15 * (pl->fade - pl->stop_in) / pl->fade : 0;
    for (i = 0; i < SOUND_ACTIVE; i++) if (pl->act[i].on) step(pl, &pl->act[i]);
    if (pl->stop_in && !--pl->stop_in) stop_all(pl);
    *out = pl->chip;
    for (v = 0; v < 4; v++) {                           /* who is heard on each voice */
        SoundStream *best = 0;
        for (i = 0; i < SOUND_ACTIVE; i++) {
            SoundStream *s = &pl->act[i];
            if (!s->on || !(s->owned & (1 << v))) continue;
            if (!best || (!s->music && best->music) || (s->music == best->music && s->seq > best->seq)) best = s;
        }
        if (!best) { out->att[v] = 0xff; continue; }   /* nobody: the voice is off, its period stays */
        if (!best->music) pl->wait[v] = 0;
        else if (pl->wait[v] && (best->last & vnote[v])) pl->wait[v] = 0;
        for (j = 0; j < PSG_STATE; j++)
            if ((vbytes[v] >> j & 1) && (best->known >> j & 1)) ((uint8_t *)out)[j] = ((const uint8_t *)&best->s)[j];
        if (pl->wait[v]) out->att[v] = 0xff;
        else if (fade_add) {                            /* fading out: louder parts later */
            unsigned l = (out->att[v] >> 4) + fade_add, r = (out->att[v] & 15) + fade_add;
            out->att[v] = (uint8_t)((l > 15 ? 15 : l) << 4 | (r > 15 ? 15 : r));
        }
    }
    pl->chip = *out;
}
