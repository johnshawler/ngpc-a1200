/* k2ge.h - the Neo Geo Pocket Color video chip (K2GE) as a function of its memory: one scanline at a time.
 * Plain C, no platform header. Used by the reference emulator (tools/ngpref), by the host test's pictures and
 * by the Amiga port's first, correct-but-slow display. */
#ifndef K2GE_H
#define K2GE_H
#include <stdint.h>
#define K2GE_W 160
#define K2GE_H_LINES 152
/* v = the 16 KB at 0x8000-0xBFFF. out = 160 colours, 12 bits each (0x0BGR as the palette RAM holds them). */
void k2ge_line(const uint8_t *v, int line, uint16_t *out);
/* The same line as palette entries: out = 160 numbers 0..255, the colour's place in the palette RAM at 0x8200
   (0-63 sprites, 64-127 scroll plane 1, 128-191 scroll plane 2, 240-247 background, 248-255 window). */
void k2ge_line_idx(const uint8_t *v, int line, uint8_t *out);
/* All 152 lines into out (160 x 152 bytes), the same as k2ge_line_idx() line by line, several times faster. */
void k2ge_frame_idx(const uint8_t *v, uint8_t *out);
/* What changed in the K2GE's memory since the last cached picture: map cells (plane 1: 0-1023, plane 2: 1024-2047) and
   characters (0-511). Whoever writes the K2GE's memory marks them (ngp.c); k2ge_frame_cached() clears them. */
typedef struct { uint8_t cell[2048], chr[512], any; } K2geDirty;
/* The same picture as k2ge_frame_idx(), from two 256 x 256 buffers that hold the scroll planes already drawn and are
   brought up to date only where cells or characters changed, and only into the lines whose inputs changed: out must
   hold this function's previous picture (a different out pointer redraws every line). Much faster when little changes. */
void k2ge_frame_cached(const uint8_t *v, K2geDirty *dirty, uint8_t *out);
/* +1 every time k2ge_frame_cached() writes that line of out: a line whose number did not change holds the same pens */
extern uint32_t k2ge_line_gen[K2GE_H_LINES];
#endif
