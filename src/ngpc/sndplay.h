/* sndplay.h - plays the streams tools/sndrec_ngpc.c recorded from the game's sound program. Plain C, no platform
 * header: the recorder's check on the host and the Amiga (src/ngpc/sound_ngp.c) run the same code.
 *
 * A stream is the sound chip's state 50 times a second after one command, from a freshly started driver. It is
 * found by the command byte and a hash of the sound processor's 4 KB when the command came (sound_key), because
 * what a command plays depends on the song data the game copied there.
 *
 * Mixing, per voice (three tones and the noise): a stream owns the voices it ever makes audible. Music is a stream
 * that loops, lasts over 3 s or uses three voices; the rest are sound effects. A voice sounds from the newest effect
 * that owns it, else from the newest music; when an effect ends, the music it covered stays silent on that voice
 * until its next note there (as SNK's driver does). A new music stops the other music, a new effect the effects
 * that share a voice with it. Command 0xFF (the driver restarts) stops everything. A command that sounds nothing by
 * itself was replayed by the recorder over the music the game played when it sent it: it stops everything at once,
 * fades everything out over some frames, or changes nothing.
 *
 * File (big-endian): "NGPS", count, then count entries of SOUND_ENTRY bytes:
 *   key (4), command (1), voices owned (1, bit 0-2 tones, bit 3 noise), flags (2, bit 0 = loops, bit 3 = music;
 *   with no voice: bit 1 = fades everything out over "frames" frames, bit 2 = changes nothing), frames (4),
 *   loop frame (4), offset of the frames from the start of the file (4), state before the loop frame (13), 3 zero
 * and the frames: per frame a 16-bit mask of the PsgState bytes the program wrote that changed (or were written for
 * the first time), then those bytes in order. A byte a stream never writes keeps the value the chip had.
 */
#ifndef SNDPLAY_H
#define SNDPLAY_H
#include <stdint.h>
#include "psg.h"

#define SOUND_ENTRY 36
#define SOUND_TICK 0x00de               /* the driver's tick counter in its 4 KB: not part of the key */
#define SOUND_ACTIVE 8

uint32_t sound_key(const uint8_t *ram4k);

typedef struct {
    const uint8_t *e, *p, *loop_p;      /* its entry, the next frame, the loop frame */
    uint32_t f, n, loop, seq;           /* frame number, frames, loop frame, start order */
    uint8_t owned, loops, music, on;
    uint16_t known, last;               /* last: the bytes its latest frame wrote */                     /* the bytes of s the stream has set: the others are the chip's as it was */
    PsgState s;
} SoundStream;

typedef struct {
    const uint8_t *data;
    uint32_t len, count, seq, missing;  /* missing: commands with no recorded stream */
    uint32_t stop_in, fade;             /* frames until everything stops (a fade-out command), 0 = none; its length */
    uint8_t wait[4];                    /* per voice: an effect ended here, the music waits for its next note */
    PsgState chip;                      /* the last state given out: registers keep their values */
    SoundStream act[SOUND_ACTIVE];
} SoundPlayer;

void sound_player_init(SoundPlayer *pl, const uint8_t *data, uint32_t len);   /* data = the file, 0 = none */
void sound_player_cmd(SoundPlayer *pl, uint32_t key, uint8_t cmd);
void sound_player_frame(SoundPlayer *pl, PsgState *out);                       /* one 50 Hz frame */
#endif
