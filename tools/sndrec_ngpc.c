/* sndrec_ngpc.c - records the game's music and sound effects from its own sound program, on the host.
 *
 * Usage: sndrec_ngpc rom.bin ticks out.bin [compare.txt|-] [script.in ...]
 *
 * The Amiga does not run the Z80: a model of the SNK driver answers the game (src/ngpc/ngp.c). What the driver
 * plays depends on the command byte the game sends and on what the game copied into the sound processor's 4 KB
 * before (the driver, then a song's data before each restart). So:
 *  1. the game runs here with the model, as on the Amiga, for the given ticks of the bench input; every command
 *     is keyed by a hash of the 4 KB (src/ngpc/sndplay.h: sound_key) and the command byte;
 *  2. for each key, the real sound program runs on a Z80 from that 4 KB: started fresh, given the command, timer
 *     interrupts at the game's rate; the sound chip's state (src/ngpc/psg.c) is taken 50 times a second, until the
 *     driver's state repeats (the music loops: the stream loops there) or it goes quiet (the stream ends);
 *  3. out.bin holds the streams (format in src/ngpc/sndplay.h), big-endian, for src/ngpc/sndplay.c.
 * A command byte with no audible output stops everything (command 0x01 of Neo Turf Masters); 0xFF restarts.
 * With compare.txt the tool also writes, per 50 Hz frame of the bench, the state the Amiga's player would
 * produce, for tools/check_music_ngpc.sh to compare with the real driver's. Input scripts after it (tools/ngpref's
 * format: tools/flow_ngpc/<game>_*.in) are played too, each from power-on, for the commands of the scenes they reach.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ngp.h"
#include "logic.h"
#include "psg.h"
#include "sndplay.h"
#include "script_input.h"

/* ---- 1. the keys the game sends ---- */
#define MAXK 512
static struct { uint32_t key; uint8_t cmd; uint8_t ram[0x1000]; int32_t period; } keys[MAXK];
static int nkeys;
static struct { unsigned tick; uint32_t key; uint8_t cmd; } events[8192];
static int nevents;
static unsigned cur_tick;
static int bench_events = 1;                    /* the compare file replays the bench's commands only */

static void on_cmd(uint8_t cmd)
{
    uint32_t k = sound_key(ngp_ram + 0x3000);
    int i;
    if (bench_events && nevents < 8192) { events[nevents].tick = cur_tick; events[nevents].key = k; events[nevents].cmd = cmd; nevents++; }
    if (cmd == 0xff) return;
    for (i = 0; i < nkeys; i++) if (keys[i].key == k && keys[i].cmd == cmd) return;
    if (nkeys == MAXK) { fprintf(stderr, "sndrec: more than %d keys\n", MAXK); return; }
    keys[nkeys].key = k; keys[nkeys].cmd = cmd; keys[nkeys].period = ngp_snd_period();
    memcpy(keys[nkeys].ram, ngp_ram + 0x3000, 0x1000);
    nkeys++;
}

/* ---- 2. the real sound program on a private Z80 ---- */
static uint8_t zram[0x1000], zcmd, zwrote;
static Psg psg;
static uint8_t z80_mr(uint16_t a) { return a < 0x1000 ? zram[a] : a == 0x8000 ? zcmd : 0xff; }
static void z80_mw(uint16_t a, uint8_t v)
{
    zwrote = 1;
    if (a < 0x1000) zram[a] = v;
    else if (a == 0x4000 || a == 0x4001) psg_write(&psg, a & 1, v);
    else if (a == 0x8000) zcmd = v;
}
static uint8_t z80_in(uint16_t port);
static void z80_out(uint16_t port, uint8_t v);
#include "z80.h"
static uint8_t z80_in(uint16_t port) { (void)port; return 0xff; }
static void z80_out(uint16_t port, uint8_t v) { (void)port; (void)v; z80.irq_line = 0; zwrote = 1; }

static int settle(void)                         /* as z80_settle() in ngp.c: run until the program idles */
{
    static Z80 seen; static int seen_valid;
    long n;
    seen_valid = 0;
    for (n = 0; n < 2000000; n++) {
        uint16_t pc0;
        int pending = (z80.irq_line && z80.iff1) || z80.nmi_pending;
        if (z80.halted && !pending) return 1;
        pc0 = z80.pc;
        z80_step();
        if (z80.pc < pc0 && !((z80.irq_line && z80.iff1) || z80.nmi_pending)) {
            if (seen_valid && !zwrote && !memcmp(&seen, &z80, sizeof z80)) return 1;
            memcpy(&seen, &z80, sizeof z80); seen_valid = 1; zwrote = 0;
        }
    }
    return 0;
}

#define MAXF (50 * 300)                         /* five minutes */
#define MAXI (MAXF * 6)
#define SILENT_END 200                          /* frames of unchanged silence that end a stream */
static PsgState frames[MAXF + 1];
static uint16_t known[MAXF + 1];               /* the bytes of each frame the program had written by then */
static int iframe[MAXI];
static uint8_t prev_ram[0x1000], skip[0x1000];
static uint32_t changes[0x1000];
static int stop_run;

/* The driver from key k's 4 KB: started fresh, given cmd (and cmd2 at frame at2, if at2 >= 0); frames[] gets the
   chip's state at the end of every 50 Hz frame; per_irq(n, f) is called after interrupt n (in frame f). */
static int drive(int k, uint8_t cmd, int nf, int at2, uint8_t cmd2, void (*per_irq)(int n, int f))
{
    const int32_t frame_t = 3072000 / 50;
    int32_t period = keys[k].period > 0 ? keys[k].period : 12545;
    int64_t t = 0, next_irq = period;
    int f, n = 0;
    memcpy(zram, keys[k].ram, sizeof zram);
    z80_reset(); psg_reset(&psg);
    if (!settle()) return -1;                   /* the start-up code */
    psg.known = 0;                              /* what the start-up code leaves is the chip's own state */
    zcmd = cmd; z80.nmi_pending = 1;
    if (!settle()) return -1;
    stop_run = 0;
    for (f = 0; f < nf && !stop_run; f++) {
        if (f == at2) { zcmd = cmd2; z80.nmi_pending = 1; if (!settle()) return -1; }
        t += frame_t;
        while (next_irq <= t) {
            z80.irq_line = 1;
            if (!settle()) return -1;
            next_irq += period;
            if (per_irq) per_irq(n, f);
            if (n < MAXI) iframe[n] = f;
            n++;
        }
        frames[f] = psg.s; known[f] = psg.known;
    }
    return f;
}

static void count_changes(int n, int f)         /* pass 1: which bytes change at most interrupts (accumulators) */
{
    unsigned a;
    (void)f;
    if (n) for (a = 0; a < 0x1000; a++) changes[a] += zram[a] != prev_ram[a];
    memcpy(prev_ram, zram, sizeof prev_ram);
    if (n >= 20000) stop_run = 1;
}

#define HBITS 19
static uint64_t hkey[1 << HBITS]; static int hval[1 << HBITS];
static int loop_i, loop_j, min_loop;
static void find_loop(int n, int f)             /* pass 2: the first state seen again at least min_loop later */
{
    uint64_t h = 1469598103934665603ull;
    unsigned a, b;
    (void)f;
    for (a = 0; a < 0x1000; a++) if (!skip[a]) { h ^= zram[a]; h *= 1099511628211ull; }
    h ^= (uint64_t)z80.sp << 32 ^ z80.pc; h |= 1;
    for (b = (unsigned)(h >> 20) & ((1 << HBITS) - 1); hkey[b]; b = (b + 1) & ((1 << HBITS) - 1))
        if (hkey[b] == h) { if (n - hval[b] >= min_loop) { loop_i = hval[b]; loop_j = n; stop_run = 1; } return; }
    hkey[b] = h; hval[b] = n;
}
static int silent(const PsgState *s) { int v; for (v = 0; v < 4; v++) if (s->att[v] != 0xff) return 0; return 1; }
static int quiet_from;
static void watch_quiet(int n, int f)           /* also in pass 2: silence that does not change any more */
{
    find_loop(n, f);
    if (f > 0 && (!silent(&frames[f - 1]) || memcmp(&frames[f - 1], &psg.s, sizeof psg.s) || !silent(&psg.s))) quiet_from = f;
    if (f - quiet_from >= SILENT_END) stop_run = 1;
}

typedef struct { uint32_t key; uint8_t cmd, owned; uint16_t flags; uint32_t nframes, loop, off; PsgState before_loop; uint8_t *data; uint32_t len; } Stream;
static Stream streams[MAXK];

static int record(int k, Stream *st, int *why)
{
    const int32_t frame_t = 3072000 / 50;
    int32_t period = keys[k].period > 0 ? keys[k].period : 12545;
    int f, end, loop_from = -1, v, i;
    unsigned a;
    memset(changes, 0, sizeof changes);
    if ((f = drive(k, keys[k].cmd, MAXF, -1, 0, count_changes)) < 0) { *why = 1; return 0; }
    for (a = 0; a < 0x1000; a++) skip[a] = changes[a] * 2 > 20000 || a == SOUND_TICK || a == SOUND_TICK + 1;
    memset(hkey, 0, sizeof hkey); loop_i = loop_j = -1; quiet_from = 0;
    min_loop = (int)(2LL * 3072000 / period);   /* two seconds */
    if ((f = drive(k, keys[k].cmd, MAXF, -1, 0, watch_quiet)) < 0) { *why = 1; return 0; }
    *why = 0;
    if (loop_j >= 0 && !(f - quiet_from >= SILENT_END)) {
        int len = (int)(((int64_t)(loop_j - loop_i) * period + frame_t / 2) / frame_t);
        loop_from = iframe[loop_i] + 1;
        end = loop_from + (len ? len : 1);
        if (end > MAXF) end = MAXF;
        drive(k, keys[k].cmd, end, -1, 0, 0);   /* the whole of the loop, once */
    } else if (f - quiet_from >= SILENT_END) end = quiet_from;   /* it went quiet and stayed so */
    else { end = f; loop_from = 0; *why = 4; }  /* neither within five minutes: loop the whole */
    if (loop_from >= 0) {                       /* a loop over nothing but silence is an end */
        for (i = loop_from; i < end && silent(&frames[i]); i++) ;
        if (i == end) { end = loop_from; loop_from = -1; *why = 0; }
    }
    if (loop_from < 0) while (end > 0 && silent(&frames[end - 1])) end--;   /* the silence after an end */
    st->owned = 0;
    for (i = 0; i < end; i++) for (v = 0; v < 4; v++) if (frames[i].att[v] != 0xff) st->owned |= (uint8_t)(1 << v);
    st->nframes = (uint32_t)end; st->loop = loop_from < 0 ? 0xffffffffu : (uint32_t)loop_from;
    st->flags = loop_from >= 0;
    {   int nv = 0; for (v = 0; v < 4; v++) nv += st->owned >> v & 1;
        if (loop_from >= 0 || end > 150 || nv >= 3) st->flags |= 8;      /* music: it loops, is long, or uses 3 voices */
    }
    if (loop_from > 0) st->before_loop = frames[loop_from - 1]; else { Psg z; psg_reset(&z); st->before_loop = z.s; }
    {   /* frames as changes: a 16-bit mask of the PsgState bytes that differ from the frame before, then those bytes */
        uint8_t *d = malloc((size_t)end * (2 + PSG_STATE) + 16), *p = d;
        Psg z; psg_reset(&z);
        for (i = 0; i < end; i++) {
            const uint8_t *x = (const uint8_t *)&frames[i], *y = i ? (const uint8_t *)&frames[i - 1] : (const uint8_t *)&z.s;
            unsigned m = 0, j;
            for (j = 0; j < PSG_STATE; j++) if (x[j] != y[j]) m |= 1u << j;
            m = (m | (known[i] & ~(i ? known[i - 1] : 0))) & known[i];   /* bytes it wrote: changed, or new */
            *p++ = (uint8_t)(m >> 8); *p++ = (uint8_t)m;
            for (j = 0; j < PSG_STATE; j++) if (m >> j & 1) *p++ = x[j];
        }
        st->data = d; st->len = (uint32_t)(p - d);
    }
    st->key = keys[k].key; st->cmd = keys[k].cmd;
    return 1;
}

/* A command that sounds nothing on a fresh driver acts on what plays: replayed 3 s into the music the game was
   playing when it sent it, it stops it at once, after a while (a fade: the stream says after how many frames),
   or not at all. */
static void classify_silent(int k, uint8_t music, Stream *st)
{
    int f, n = drive(k, music, 150 + 500, 150, keys[k].cmd, 0), q = -1;
    for (f = 150; f < n; f++) { if (!silent(&frames[f])) q = -1; else if (q < 0) q = f; }
    if (n < 0 || q < 0) { st->flags = 4; st->nframes = 0; return; }   /* no effect */
    st->flags = 2; st->nframes = (uint32_t)(q - 150);                  /* stops everything after nframes */
}

static void put32(FILE *f, uint32_t v) { fputc((int)(v >> 24), f); fputc((int)(v >> 16) & 255, f); fputc((int)(v >> 8) & 255, f); fputc((int)v & 255, f); }

int main(int argc, char **argv)
{
    static uint8_t rom[0x400000];
    FILE *f;
    long romlen;
    unsigned n, t;
    int k, ns = 0;
    uint32_t off;
    if (argc < 4) { fprintf(stderr, "usage: sndrec_ngpc rom.bin ticks out.bin [compare.txt|-] [script.in ...]\n"); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    romlen = (long)fread(rom, 1, sizeof rom, f); fclose(f);
    n = (unsigned)atoi(argv[2]);
    game_init(rom, (uint32_t)romlen, 0);
    ngp_snd_hook = on_cmd;
    for (t = 0; t < n; t++) { cur_tick = t; game_tick(game_bench_input(t)); }
    printf("sndrec: the bench: %d commands, %d keys\n", nevents, nkeys);
    bench_events = 0;
    {   int a;
        for (a = 5; a < argc; a++) {            /* the scripts: from power-on, until their last press and 10 s more */
            unsigned last = 36000, k0 = (unsigned)nkeys, i;
            script_n = 0;
            if (!script_load(argv[a])) { fprintf(stderr, "sndrec: cannot read %s\n", argv[a]); return 1; }
            for (i = 0; i < script_n; i++) if (script_lines[i].last + 600 > last) last = script_lines[i].last + 600;
            game_init(rom, (uint32_t)romlen, 0);
            for (t = 0; t < last && !ngp_fatal_why; t++) { cur_tick = t; game_tick(script_input(t)); }
            printf("sndrec: %s: %u ticks%s, %u new keys\n", argv[a], t, ngp_fatal_why ? " (stopped: untranslated code)" : "", (unsigned)nkeys - k0);
        }
    }
    ngp_snd_hook = 0;
    printf("sndrec: %d commands sent in %u ticks, %d different (4 KB, command) keys\n", nevents, n, nkeys);
    for (k = 0; k < nkeys; k++) {
        int why;
        if (!record(k, &streams[ns], &why)) { printf("  key %08x cmd %02x: the sound program did not idle (%d): left out\n", keys[k].key, keys[k].cmd, why); continue; }
        printf("  key %08x cmd %02x: %5u frames (%5.1f s), %s, voices %c%c%c%c, %6u bytes%s\n", streams[ns].key, streams[ns].cmd,
               streams[ns].nframes, streams[ns].nframes / 50.0, streams[ns].flags ? "loops" : "ends", streams[ns].owned & 1 ? '0' : '-',
               streams[ns].owned & 2 ? '1' : '-', streams[ns].owned & 4 ? '2' : '-', streams[ns].owned & 8 ? 'N' : '-', streams[ns].len,
               why == 4 ? " (no repeat within 5 minutes: looped whole)" : "");
        if (streams[ns].flags) printf("      loop from frame %u\n", streams[ns].loop);
        if (!streams[ns].owned) {               /* silent: what it does to the music the game played then */
            int e, m = -1, kk;
            for (e = 0; e < nevents; e++) if (events[e].key == keys[k].key && events[e].cmd == keys[k].cmd) break;
            for (e = e - 1; e >= 0 && m < 0; e--) {
                if (events[e].cmd == 0xff) break;
                for (kk = 0; kk < ns; kk++) if (streams[kk].key == events[e].key && streams[kk].cmd == events[e].cmd && (streams[kk].flags & 1)) { m = events[e].cmd; break; }
            }
            if (m < 0) { streams[ns].flags = 2; streams[ns].nframes = 0; printf("      silent, no music before it: stops everything\n"); }
            else {
                classify_silent(k, (uint8_t)m, &streams[ns]);
                if (streams[ns].flags & 4) printf("      silent, does not stop music %02x: ignored\n", m);
                else printf("      silent, stops music %02x after %u frames\n", m, streams[ns].nframes);
            }
            free(streams[ns].data); streams[ns].data = 0; streams[ns].len = 0;
        }
        ns++;
    }
    f = fopen(argv[3], "wb");
    if (!f) { perror(argv[3]); return 1; }
    fwrite("NGPS", 1, 4, f); put32(f, (uint32_t)ns);
    off = 8 + (uint32_t)ns * SOUND_ENTRY;
    for (k = 0; k < ns; k++) {
        Stream *s = &streams[k];
        put32(f, s->key); fputc(s->cmd, f); fputc(s->owned, f); fputc(s->flags >> 8, f); fputc(s->flags & 255, f);
        put32(f, s->nframes); put32(f, s->loop); put32(f, off);
        fwrite(&s->before_loop, 1, PSG_STATE, f); fputc(0, f); fputc(0, f); fputc(0, f);
        off += (s->len + 3) & ~3u;
    }
    for (k = 0; k < ns; k++) { static const uint8_t z[4]; fwrite(streams[k].data, 1, streams[k].len, f); fwrite(z, 1, ((streams[k].len + 3) & ~3u) - streams[k].len, f); }
    fclose(f);
    printf("sndrec: %d streams, %u bytes in %s\n", ns, (unsigned)off, argv[3]);
    if (argc > 4 && strcmp(argv[4], "-")) {    /* what the player makes of the bench, 50 times a second */
        SoundPlayer pl;
        FILE *c = fopen(argv[4], "w");
        uint8_t *all = malloc(off);
        int e = 0;
        unsigned fr, nf = (unsigned)((double)n / 59.95 * 50);
        f = fopen(argv[3], "rb"); if (fread(all, 1, off, f) != off) return 1; fclose(f);
        sound_player_init(&pl, all, off);
        for (fr = 0; fr < nf; fr++) {
            PsgState out;
            unsigned tick = (unsigned)(fr * 59.95 / 50);
            while (e < nevents && events[e].tick <= tick) { sound_player_cmd(&pl, events[e].key, events[e].cmd); e++; }
            sound_player_frame(&pl, &out);
            fprintf(c, "%u", fr);
            { unsigned j; for (j = 0; j < PSG_STATE; j++) fprintf(c, " %02x", ((uint8_t *)&out)[j]); }
            fputc('\n', c);
        }
        fclose(c);
    }
    return 0;
}
