/* hwdisp.h - the Neo Geo Pocket picture on the A1200's hardware (FEASIBILITY.md): the back scroll plane as a
 * hardware-scrolled playfield of 16 x 16 tiles, the front plane and the sprites in front of it in five hardware
 * sprites, small sprite objects as blitter objects. Only what the game changed is drawn. Plain C, no platform
 * header: the Amiga program and the host model (tools/hwdisp_test.c) run the same code.
 *
 * The caller provides the hw_* functions below: kit calls on the Amiga (src/ngpc/main.c), a model of them on the
 * host. Images are 16-bit words, the leftmost pixel in bit 15, laid out as the kit wants them.
 */
#ifndef HWDISP_H
#define HWDISP_H
#include <stdint.h>
#include "k2ge.h"

#define HW_PLANES   6
#define HW_LEVEL_W  4096                        /* DispCfg level_w: how far the picture can slide */
#define HW_BUF_H    416                         /* 256 rows of the plane, then its rows 0-159 again (vertical wrap; a multiple of 16) */
#define HW_VIEW_H   152
#define HW_WIN_X    80                          /* the Pocket's 160 pixels start here in the 320-pixel view */
#define HW_SPR_H    152                         /* the front-layer sprites: 64 pixels wide, the picture's height */
#define HW_SPR_BYTES ((HW_SPR_H + 2) * 16)      /* one 64-pixel sprite block (kit layout) */
#define HW_SPR_BANK 4                           /* DispCfg sprite_bank: sprite colours 64-79 */

/* what the display must offer (the kit's calls, with world coordinates for tiles and objects) */
void hw_tile(int32_t x, int32_t y, const uint16_t *tile);          /* 6 planes x 16 rows, into every buffer */
void hw_clear(int32_t x, int32_t y, int32_t w, int32_t h);         /* pen 0 into every buffer; x, w multiples of 16 */
void hw_bob(int32_t x, int32_t y, const uint16_t *img, const uint16_t *msk, int wwords, int h);
void hw_sprite(int n, uint16_t *block, int x, int y, int h, int attach);   /* screen pixels */
void hw_sprite_off(int n);
void hw_scroll(int32_t x, int32_t y);
void hw_palette(const uint32_t *rgb, int first, int count);       /* 0x00RRGGBB */
/* memory for images the hardware reads (chip RAM on the Amiga): called once, by hwdisp_init() */
void *hw_chip(uint32_t bytes);

/* per-frame limits on drawing (FEASIBILITY.md section 5); the rest waits for the next frames */
extern int hwdisp_tiles_per_frame, hwdisp_cells_per_frame;
extern uint32_t hwdisp_budget;                  /* and a time limit: colour clocks (0: none); hw_now() is the clock */
uint32_t hw_now(void);
extern int hwdisp_s4_strip;                     /* which 64-pixel strip (0 left, 1 middle, 2 right) has only 3 colours */

int  hwdisp_init(void);                                             /* 0: no memory */
/* bring the hardware picture up to date with the K2GE's memory v; dirty: what the game wrote since the last call
   (cleared here). Calls hw_bob for every object, hw_sprite for the five sprites, hw_scroll and hw_palette. */
void hwdisp_frame(const uint8_t *v, K2geDirty *dirty);

/* what the last hwdisp_frame() did, and totals (for the host test and the bench) */
typedef struct {
    int tiles, tiles_left, clears, cells, cells_left, bobs, front_objs, slot_misses, remaps, rebases, why_scroll, why_chr, why_objs;
    uint32_t t_start, t_tiles, t_front, t_objs, composed, t_draw;     /* hw_now() time of each phase */
} HwStats;
extern HwStats hwdisp_last;
#endif
