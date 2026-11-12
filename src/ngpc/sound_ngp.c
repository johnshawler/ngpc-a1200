/* sound_ngp.c - the game's music and sound effects on Paula (Amiga only).
 *
 * The Z80 does not run here: tools/sndrec_ngpc.c recorded, on the host, what the game's own sound program does
 * after each command it sends (dist/sound.bin), and src/ngpc/sndplay.c mixes those streams. The driver model in
 * ngp.c hands every command to sound_cmd(); at every vertical blank (50 Hz, in the interrupt) the player gives one
 * frame of the sound chip's state and this file sets Paula from it. Paula plays one period of a waveform in a loop:
 *   tone 0 -> channel 0 (left), tone 1 -> 1 (right), tone 2 -> 2 (right), noise -> 3 (left)
 * each at the louder of its two volumes (the T6W28 has a left and a right volume per voice; Paula's channels are fixed
 * to one side). Nothing here changes the game state: checksums are unaffected. Paula's registers are written
 * directly: the kit's ptplayer plays modules and one-shot samples, not tones (as src/sound.c of the NES port).
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <exec/memory.h>
#include "sys.h"
#include "ngp.h"
#include "sndplay.h"
#include "sound_ngp.h"

#define PAULA_CLOCK 3546895UL                   /* PAL */
#define NOISE_LEN 32768                         /* bytes: the 15-bit white sequence, once */
#define PERIODIC_LEN 30                         /* two periods of the periodic noise (one step in 15) */

static const UBYTE vol_of[16] = { 64, 50, 39, 31, 24, 19, 15, 12, 9, 7, 5, 4, 3, 2, 1, 0 };  /* attenuation -> Paula (2 dB steps) */
static BYTE *wave_sq, *wave_white, *wave_per;
static UBYTE *data;
static ULONG len;
static SoundPlayer pl;
static volatile struct { ULONG key; UBYTE cmd; } queue[16];
static volatile UBYTE q_in, q_out;
static UBYTE on;
static UWORD cur_per[4], cur_vol[4];
static BYTE *cur_ptr[4];
ULONG sound_missing;                            /* commands with no recorded stream (traced too little of the game) */
#ifdef SOUND_TRACE                              /* debug builds: Paula's period and volume per channel, every vertical blank */
static UWORD trace[SOUND_TRACE][8];
ULONG sound_ntrace;
#endif

int sound_load(void)
{
    int i;
    ULONG lfsr = 0x4000;
    data = sys_load("sound.bin", MEMF_ANY, &len);       /* none: the game is silent */
    if (!(wave_sq = sys_alloc(2, MEMF_CHIP)) || !(wave_white = sys_alloc(NOISE_LEN, MEMF_CHIP)) || !(wave_per = sys_alloc(PERIODIC_LEN, MEMF_CHIP))) return 0;
    wave_sq[0] = 96; wave_sq[1] = -96;
    for (i = 0; i < NOISE_LEN; i++) {           /* as the T6W28: feedback from bits 0 and 1 into bit 14, output bit 0 */
        wave_white[i] = (lfsr & 1) ? -80 : 80;
        lfsr = (((lfsr << 14) ^ (lfsr << 13)) & 0x4000) | (lfsr >> 1);
    }
    for (i = 0; i < PERIODIC_LEN; i++) wave_per[i] = (i % 15 == 0) ? 100 : -8;
    sound_player_init(&pl, data, data ? len : 0);
    return 1;
}

static void set_chan(int n, BYTE *ptr, UWORD words, ULONG per, UWORD vol)
{
    if (per < 124) { per = 124; vol = 0; }      /* above Paula's fastest rate: the T6W28 is past hearing there anyway */
    if (per > 65535) per = 65535;
    if (ptr != cur_ptr[n]) { custom->aud[n].ac_ptr = (UWORD *)ptr; custom->aud[n].ac_len = words; cur_ptr[n] = ptr; }
    if (per != cur_per[n]) { custom->aud[n].ac_per = (UWORD)per; cur_per[n] = (UWORD)per; }
    if (vol != cur_vol[n]) { custom->aud[n].ac_vol = vol; cur_vol[n] = vol; }
}

void sound_cmd(UBYTE cmd)                       /* from the logic (ngp_snd_hook): queued for the interrupt */
{
    UBYTE next = (UBYTE)((q_in + 1) & 15);
    if (next == q_out) return;
    queue[q_in].key = sound_key(ngp_ram + 0x3000);
    queue[q_in].cmd = cmd;
    q_in = next;
}

void sound_vbl(void)                            /* every vertical blank, in the interrupt */
{
    PsgState s;
    int v;
    if (!on) return;
    while (q_out != q_in) { sound_player_cmd(&pl, queue[q_out].key, queue[q_out].cmd); q_out = (UBYTE)((q_out + 1) & 15); }
    sound_player_frame(&pl, &s);
    sound_missing = pl.missing;
    for (v = 0; v < 4; v++) {
        UWORD l = s.att[v] >> 4, r = s.att[v] & 15, vol = vol_of[l < r ? l : r];
        ULONG n = psg_period(&s, v);
        if (!n) vol = 0;
        if (v < 3) set_chan(v, wave_sq, 1, (n * 18914UL) >> 10, vol);           /* 2 samples a period: 18.47 x n */
        else if (s.noise_ctl & 4) set_chan(3, wave_white, NOISE_LEN / 2, (n * 37828UL) >> 10, vol);   /* a step per 32 n clocks */
        else set_chan(3, wave_per, PERIODIC_LEN / 2, (n * 37828UL) >> 10, vol);
    }
#ifdef SOUND_TRACE
    if (sound_ntrace < SOUND_TRACE) { for (v = 0; v < 4; v++) { trace[sound_ntrace][v * 2] = cur_per[v]; trace[sound_ntrace][v * 2 + 1] = cur_vol[v]; } sound_ntrace++; }
#endif
}

#ifdef SOUND_TRACE
void sound_trace_save(void)                     /* after sys_give() */
{
    BPTR fh = Open((CONST_STRPTR)"sndtrace.bin", MODE_NEWFILE);
    if (fh) { Write(fh, trace, sound_ntrace * 16); Close(fh); }
}
#endif

void sound_start(void)
{
    int n;
    for (n = 0; n < 4; n++) { cur_ptr[n] = 0; cur_per[n] = 0; cur_vol[n] = 0xffff; set_chan(n, n < 3 ? wave_sq : wave_white, n < 3 ? 1 : NOISE_LEN / 2, 400, 0); }
    custom->dmacon = DMAF_SETCLR | DMAF_AUD0 | DMAF_AUD1 | DMAF_AUD2 | DMAF_AUD3;
    ngp_snd_hook = sound_cmd;
    on = 1;
}

void sound_stop(void)
{
    int n;
    on = 0;
    ngp_snd_hook = 0;
    for (n = 0; n < 4; n++) custom->aud[n].ac_vol = 0;
    custom->dmacon = DMAF_AUD0 | DMAF_AUD1 | DMAF_AUD2 | DMAF_AUD3;
}
