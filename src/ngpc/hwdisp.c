/* hwdisp.c - the Neo Geo Pocket picture on the A1200's hardware: see hwdisp.h and FEASIBILITY.md.
 *
 * Playfield (6 planes, hardware scroll): the back scroll plane, 16 x 16 tiles composed from its 8 x 8 cells, and the
 * background colour. World x = W0 + U + window x, where U adds up the back plane's horizontal scroll steps (the
 * picture slides through memory, as the kit's scrolling map does); world y = the plane's row, and buffer rows
 * 256-407 repeat rows 0-151, so every vertical position is one hw_scroll. Columns that overlap the 160-pixel window
 * hold the course, all others are black (pen 0). Pens: 0 black, 1 the background colour, then slots of three pens,
 * one slot per palette in use (back plane palettes and sprite palettes).
 *
 * Front layer (five 64-pixel sprites, 152 lines): the front scroll plane and the sprite objects that must be in front
 * of it, as one picture of "keys" (which palette entry each pixel shows) in 8 x 8 cells, from window x -16 to 175:
 * three 64-pixel strips at -16, 48 and 112: sprite 4 in strip hwdisp_s4_strip (3 colours: bank entries 9-11), the
 * attached pairs 2+3 and 0+1 in the other two (15 colours), in that order. (Before: sprite 4 at -16..47, pair 2+3 at 48..111 and pair 0+1 at
 * 112..175 (15 colours). The 16 pixels on each side of the window are black: they hide the course tiles that reach
 * past it. The keys used most get bank entries of their own; the others the nearest colour.
 *
 * Objects: sprites that touch form an object. Objects up to 32 x 32 over the course are blitter objects (hw_bob);
 * larger ones, and those over the front plane's pixels, go into the front layer.
 */
#include <string.h>
#include "hwdisp.h"

#define RD16(p) ((unsigned)((p)[0] | (p)[1] << 8))
#define W0      2048                            /* world x of window x 0 when U = 0: a multiple of 256 */
#define NCOL    ((HW_LEVEL_W + 64) / 16 + 2)
#define NROW    (HW_BUF_H / 16 + 1)             /* 26 (the last one half used) */
#define NSLOT   20
#define FCW     24                              /* front cells: 24 x 19 */
#define FCH     19
#define FW      (FCW * 8)
#define KEY_EDGE  97
#define KEY_BLACK 98
#define NKEY      99
#define MAXOBJ    64
#define NCACHE    24

int hwdisp_tiles_per_frame = 4, hwdisp_cells_per_frame = 20, hwdisp_s4_strip = 2;
HwStats hwdisp_last;

static const uint8_t *V;
static int back_sel = -1;                       /* the back plane's map: 0 = 0x9000, 1 = 0x9800 */
static int32_t U, cam_x, cam_y;
static uint8_t last_scx, scroll_known;
static uint8_t tstate[NCOL][NROW];              /* 0 unknown, 1 black, 2 course */
static uint16_t bmap[1024];                     /* the back plane's map as the tiles were made from it */
static int pal_refs[16];                        /* back map cells per palette */
static int8_t key_slot[32], slot_key[NSLOT];    /* key: 0-15 back plane palette, 16-31 sprite palette */
static uint8_t key_alias[32], sprite_used[16];
static uint8_t char_empty[512];
static uint16_t *tilebuf;
static uint32_t pf_rgb[64], bank_rgb[16];
static int pf_rgb_ok, bank_rgb_ok;

/* front layer */
static uint8_t keybuf[HW_VIEW_H][FW];
static uint8_t fdirty[FCH][FCW], fpend[2][FCH][FCW];
static uint32_t cellkeys[FCH][FCW][4];          /* the keys each cell holds */
static int key_count[NKEY], key_count_s4[NKEY];
static uint8_t map_pair[NKEY], map_s4[NKEY], entry_key[16];
static uint8_t key_present[NKEY];
static uint16_t *sprblk[2][5];
static int frame_no;
static uint8_t last_regs[8];
static int front_scx = -1, front_scy = -1;

/* objects */
typedef struct { int16_t x, y; uint16_t e; uint8_t pal, n, pr; } Spr;
static Spr spr[64], fspr[64], fspr_prev[64];
static int nspr, nfspr, nfspr_prev;
typedef struct { int16_t x0, y0, x1, y1; uint8_t front, first, count; } Obj;
static Obj obj[MAXOBJ]; static int nobj;
static uint8_t members[64];
typedef struct { uint16_t sig[64 * 2 + 2]; int nsig; uint16_t *img, *msk; int wwords, h, used; } Bobc;
static Bobc cache[NCACHE];
static int cache_clock;

/* ---- colours ---- */
static unsigned col12(unsigned idx)
{
    unsigned c = RD16(V + 0x200 + idx * 2) & 0xfff;
    return (V[0x12] & 0x80) ? (~c & 0xfff) : c;
}
static uint32_t rgb24(unsigned c)
{
    return ((c & 15) * 17u) << 16 | (((c >> 4) & 15) * 17u) << 8 | ((c >> 8) & 15) * 17u;
}
static int back_palbase(void) { return back_sel ? 128 : 64; }
static int front_palbase(void) { return back_sel ? 64 : 128; }
static unsigned key_col12(int key)            /* front-layer key -> colour */
{
    if (key == KEY_BLACK || key == 0) return 0;
    if (key == KEY_EDGE) return col12(0xf8 + (V[0x12] & 7));
    if (key <= 48) return col12(front_palbase() + (key - 1) / 3 * 4 + (key - 1) % 3 + 1);
    return col12((key - 49) / 3 * 4 + (key - 49) % 3 + 1);
}
static unsigned slotkey_col12(int key, int c) { return col12(key < 16 ? back_palbase() + key * 4 + c : (key - 16) * 4 + c); }
static int coldist(unsigned a, unsigned b)
{
    int d0 = (int)(a & 15) - (int)(b & 15), d1 = (int)((a >> 4) & 15) - (int)((b >> 4) & 15), d2 = (int)(a >> 8) - (int)(b >> 8);
    return d0 * d0 + d1 * d1 + d2 * d2;
}

/* ---- palette slots ---- */
static void clear_cache(void) { int i; for (i = 0; i < NCACHE; i++) cache[i].nsig = -1; }
static int key_in_use(int key) { return key < 16 ? pal_refs[key] > 0 : sprite_used[key - 16]; }
static void slot_free(int s)
{
    int k = slot_key[s];
    if (k < 0) return;
    key_slot[k] = -1; slot_key[s] = -1;
    if (k >= 16) clear_cache();
}
static void inval_back_pal(int pal);
static int get_slot(int key)
{
    int s, best = -1, bd = 1 << 30;
    if (key_slot[key] >= 0) return key_slot[key];
    for (s = 0; s < NSLOT; s++) if (slot_key[s] < 0) break;
    if (s == NSLOT) for (s = 0; s < NSLOT; s++) if (!key_in_use(slot_key[s])) { slot_free(s); break; }
    if (s < NSLOT) { slot_key[s] = (int8_t)key; key_slot[key] = (int8_t)s; key_alias[key] = 0; return s; }
    for (s = 0; s < NSLOT; s++) {               /* no slot: the slot with the nearest colours (FEASIBILITY.md cut) */
        int c, d = 0;
        for (c = 1; c < 4; c++) d += coldist(slotkey_col12(key, c), slotkey_col12(slot_key[s], c));
        if (d < bd) { bd = d; best = s; }
    }
    hwdisp_last.slot_misses++;
    key_alias[key] = 1;
    return best;
}
static int slot_of(int key) { int s = key_slot[key]; return s >= 0 && !key_alias[key] ? s : -1; }
static void retry_aliases(void)
{
    int k, s;
    for (k = 0; k < 32; k++) {
        if (!key_alias[k] || !key_in_use(k)) { if (key_alias[k] && !key_in_use(k)) key_alias[k] = 0; continue; }
        for (s = 0; s < NSLOT; s++) if (slot_key[s] < 0 || !key_in_use(slot_key[s])) break;
        if (s == NSLOT) return;
        if (slot_key[s] >= 0) slot_free(s);
        slot_key[s] = (int8_t)k; key_slot[k] = (int8_t)s; key_alias[k] = 0;
        if (k < 16) inval_back_pal(k); else clear_cache();
    }
}
/* the pen of colour c (1-3) of a key; the aliased slot when it has none of its own */
static int pen_of(int key, int c)
{
    int s = slot_of(key);
    if (s < 0) { s = key_slot[key]; if (s < 0 || !key_alias[key]) s = get_slot(key); }
    return 2 + 3 * s + c - 1;
}

/* ---- playfield tiles ---- */
static int col_course(int c)                    /* does world column c overlap the window? */
{
    int32_t x = (int32_t)c * 16, wl = W0 + U;
    return x + 16 > wl && x < wl + 160;
}
/* 4 pixels of a character row (a byte, leftmost in bits 7-6) -> their bits in planes 0-3 (a byte each, low nibble)
   and planes 4-5, for the pens of one back-plane palette; made again when that palette's pens change */
static uint32_t tab4[16][256];
static uint16_t tab2[16][256];
static uint8_t tab_pen[16][3], tab_ok[16], rev8[256];
static void key_table(int key)
{
    int pens[4], b, j, p;
    pens[0] = 1;
    for (j = 1; j < 4; j++) pens[j] = pen_of(key, j);
    if (tab_ok[key] && tab_pen[key][0] == pens[1] && tab_pen[key][1] == pens[2] && tab_pen[key][2] == pens[3]) return;
    for (b = 0; b < 256; b++) {
        uint32_t v4 = 0; unsigned v2 = 0;
        for (j = 0; j < 4; j++) {
            int pen = pens[(b >> (6 - 2 * j)) & 3]; unsigned bit = 8u >> j;
            for (p = 0; p < 4; p++) if (pen >> p & 1) v4 |= bit << (8 * p);
            for (p = 4; p < 6; p++) if (pen >> p & 1) v2 |= bit << (8 * (p - 4));
        }
        tab4[key][b] = v4; tab2[key][b] = (uint16_t)v2;
    }
    tab_ok[key] = 1; tab_pen[key][0] = (uint8_t)pens[1]; tab_pen[key][1] = (uint8_t)pens[2]; tab_pen[key][2] = (uint8_t)pens[3];
}
static void compose_tile(int c, int r, uint16_t *t)
{
    const uint8_t *map = V + (back_sel ? 0x1800 : 0x1000), *chr = V + 0x2000;
    unsigned px = (unsigned)(c * 16 - W0) & 255, py0 = (unsigned)(r * 16) & 255, y, h;
    for (y = 0; y < 16; y++) {
        unsigned py = py0 + y, p;
        uint32_t v4[2]; unsigned v2[2];
        for (h = 0; h < 2; h++) {
            unsigned e = RD16(map + ((py >> 3) * 32 + (((px >> 3) + h) & 31)) * 2), row = (e & 0x4000) ? 7 - (py & 7) : py & 7;
            unsigned bits = RD16(chr + (e & 0x1ff) * 16 + row * 2), key = (e >> 9) & 15;
            if (!bits) { v4[h] = 0xff; v2[h] = 0; continue; }          /* all background: pen 1 */
            if (e & 0x8000) bits = (unsigned)rev8[bits & 255] << 8 | rev8[bits >> 8];
            key_table((int)key);
            v4[h] = tab4[key][bits >> 8] << 4 | tab4[key][bits & 255];
            v2[h] = (unsigned)tab2[key][bits >> 8] << 4 | tab2[key][bits & 255];
        }
        for (p = 0; p < 4; p++) t[p * 16 + y] = (uint16_t)(((v4[0] >> (8 * p)) & 255) << 8 | ((v4[1] >> (8 * p)) & 255));
        for (p = 0; p < 2; p++) t[(4 + p) * 16 + y] = (uint16_t)(((v2[0] >> (8 * p)) & 255) << 8 | ((v2[1] >> (8 * p)) & 255));
    }
}
static void alias_mark(int c, int r, int black)
{
    int d, k;
    for (d = -32; d <= 32; d += 64) {
        int cc = c + d;
        if (cc < 0 || cc >= NCOL) continue;
        for (k = r - 1; k <= r + 1; k++) if (k >= 0 && k < NROW && !(black && tstate[cc][k] == 1)) tstate[cc][k] = 0;
    }
}
static void inval_back_cell(int mx, int my)     /* a cell of the back map changed: the tiles that show it */
{
    int c, c0 = (cam_x - 64) / 16, c1 = (cam_x + 384) / 16, r = my >> 1;
    for (c = c0 < 0 ? 0 : c0; c <= c1 && c < NCOL; c++)
        if ((((unsigned)(c * 16 - W0) & 255) >> 4) == (unsigned)(mx >> 1) && tstate[c][r] == 2) {
            tstate[c][r] = 0;
            if (r < 10 && tstate[c][r + 16] == 2) tstate[c][r + 16] = 0;
        }
}
static void inval_back_pal(int pal)
{
    int i;
    for (i = 0; i < 1024; i++) if (((bmap[i] >> 9) & 15) == pal) inval_back_cell(i & 31, i >> 5);
}

static void tiles_work(void)
{
    int pass, c, r, n = hwdisp_tiles_per_frame, c0 = (cam_x - 64) / 16, c1 = (cam_x + 384 + 15) / 16, left = 0, run = -1;
    int vr0 = cam_y >> 4, vr1 = (cam_y + HW_VIEW_H - 1) >> 4;
    if (c0 < 0) c0 = 0;
    if (c1 >= NCOL) c1 = NCOL - 1;
    /* black columns: whole columns cleared with the blitter (hw_clear), neighbours in one rectangle */
    for (c = c0; c <= c1 + 1; c++) {
        int need = c <= c1 && !col_course(c);
        if (need) { for (r = 0; r < NROW; r++) if (tstate[c][r] != 1) break; need = r < NROW; }
        if (need && run < 0) run = c;
        if (!need && run >= 0) {
            int k;
            hw_clear(run * 16, 0, (c - run) * 16, HW_BUF_H);
            for (k = run; k < c; k++) for (r = 0; r < NROW; r++) { tstate[k][r] = 1; alias_mark(k, r, 1); }
            hwdisp_last.clears += c - run; run = -1;
        }
    }
    for (pass = 0; pass < 3; pass++)             /* what is seen now first, then rows and columns about to be seen */
        for (c = c0; c <= c1; c++) {
            int seen_col = c * 16 + 16 > cam_x && c * 16 < cam_x + 320, course = col_course(c);
            if (pass < 2 && !seen_col) continue;
            for (r = 0; r < NROW; r++) {
                int seen = seen_col && r >= vr0 && r <= vr1, want = course ? 2 : 1;
                if ((pass == 0 && !seen) || (pass == 1 && seen) || (pass == 2 && seen_col)) continue;
                if (tstate[c][r] == want) continue;
                if (!n) { left++; continue; }
                if (!course) continue;
                compose_tile(c, r, tilebuf); hw_tile(c * 16, r * 16, tilebuf);
                tstate[c][r] = (uint8_t)want; alias_mark(c, r, !course);
                n--; hwdisp_last.tiles++;
            }
        }
    hwdisp_last.tiles_left = left;
}

/* ---- the back map, characters and scroll ---- */
static void back_cell_changed(int i)
{
    unsigned e = RD16(V + (back_sel ? 0x1800 : 0x1000) + i * 2);
    if (e == bmap[i]) return;
    pal_refs[(bmap[i] >> 9) & 15]--; pal_refs[(e >> 9) & 15]++;
    bmap[i] = (uint16_t)e;
    inval_back_cell(i & 31, i >> 5);
}
static void reset_all(void)
{
    int i, c, r;
    const uint8_t *map = V + (back_sel ? 0x1800 : 0x1000);
    memset(pal_refs, 0, sizeof pal_refs);
    for (i = 0; i < 1024; i++) { bmap[i] = (uint16_t)RD16(map + i * 2); pal_refs[(bmap[i] >> 9) & 15]++; }
    for (c = 0; c < NCOL; c++) for (r = 0; r < NROW; r++) if (tstate[c][r] != 1) tstate[c][r] = 0;
    memset(fdirty, 1, sizeof fdirty);
    clear_cache();
}

/* ---- the front layer ---- */
static void front_cell_mark(int x0, int y0, int x1, int y1)     /* window pixels, inclusive */
{
    int cx, cy;
    x0 += 16; x1 += 16;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= FW) x1 = FW - 1;
    if (y1 >= HW_VIEW_H) y1 = HW_VIEW_H - 1;
    for (cy = y0 >> 3; cy <= y1 >> 3; cy++) for (cx = x0 >> 3; cx <= x1 >> 3; cx++) fdirty[cy][cx] = 1;
}
static void front_map_cell_changed(int mx, int my)
{
    int sx = (mx * 8 - front_scx) & 255, sy = (my * 8 - front_scy) & 255;
    if (sx > 248) sx -= 256;
    if (sy > 248) sy -= 256;
    if (sx < 160 && sy < HW_VIEW_H) front_cell_mark(sx, sy, sx + 7, sy + 7);
}
static int sprite_pixel(const Spr *s, int x, int y)  /* 0 or the colour 1-3 of sprite s at window pixel (x, y) */
{
    int dx = x - s->x, dy = y - s->y;
    unsigned bits;
    if (dx < 0 || dx > 7 || dy < 0 || dy > 7) return 0;
    bits = RD16(V + 0x2000 + (s->e & 0x1ff) * 16 + ((s->e & 0x4000) ? 7 - dy : dy) * 2);
    return (s->e & 0x8000) ? (bits >> (dx * 2)) & 3 : (bits >> ((7 - dx) * 2)) & 3;
}
static void compose_cell(int cx, int cy)
{
    const uint8_t *map = V + (back_sel ? 0x1000 : 0x1800), *chr = V + 0x2000;
    int winx = last_regs[0], winy = last_regs[1], winr = winx + last_regs[2], winb = winy + last_regs[3], r, i, k;   /* the last window that was not empty */
    uint32_t *km = cellkeys[cy][cx];
    Spr *here[64]; int nh = 0, x0 = cx * 8 - 16, y0 = cy * 8;
    for (k = 0; k < nfspr; k++)                  /* the front sprites over this cell, in sprite order */
        if (fspr[k].x <= x0 + 7 && fspr[k].x + 7 >= x0 && fspr[k].y <= y0 + 7 && fspr[k].y + 7 >= y0) here[nh++] = &fspr[k];
    km[0] = km[1] = km[2] = km[3] = 0;
    for (r = 0; r < 8; r++) {
        int y = cy * 8 + r;
        uint8_t *row;
        if (y >= HW_VIEW_H) break;
        row = keybuf[y] + cx * 8;
        for (i = 0; i < 8; i++) {
            int x = cx * 8 + i - 16, key = 0, old = row[i];
            if (x < 0 || x >= 160) key = KEY_BLACK;
            else if (x < winx || x >= winr || y < winy || y >= winb) key = KEY_EDGE;
            else {
                unsigned px = (unsigned)(x + front_scx) & 255, py = (unsigned)(y + front_scy) & 255;
                unsigned e = RD16(map + ((py >> 3) * 32 + (px >> 3)) * 2), rr = (e & 0x4000) ? 7 - (py & 7) : py & 7;
                unsigned bits = RD16(chr + (e & 0x1ff) * 16 + rr * 2), dx = px & 7;
                int c = (e & 0x8000) ? (bits >> (dx * 2)) & 3 : (bits >> ((7 - dx) * 2)) & 3;
                for (k = nh - 1; k >= 0; k--)     /* priority 2 under the plane: the lower number wins */
                    if (here[k]->pr == 2) { int sc = sprite_pixel(here[k], x, y); if (sc) key = 49 + here[k]->pal * 3 + sc - 1; }
                if (c) key = 1 + ((e >> 9) & 15) * 3 + c - 1;
                for (k = nh - 1; k >= 0; k--)
                    if (here[k]->pr == 3) { int sc = sprite_pixel(here[k], x, y); if (sc) key = 49 + here[k]->pal * 3 + sc - 1; }
            }
            if (key != old) {
                key_count[old]--; key_count[key]++;
                if (cx >> 3 == hwdisp_s4_strip) { key_count_s4[old]--; key_count_s4[key]++; }
                row[i] = (uint8_t)key;
            }
            km[key >> 5] |= 1u << (key & 31);
        }
    }
    fpend[0][cy][cx] = fpend[1][cy][cx] = 1;
}
/* bank entries for the keys: 9 black; 10 and 11 the two most used keys left of window x 48 (sprite 4 has only
   9-11); 1-8 and 12-15 the most used of the rest; the others the nearest colour. 1 when the mapping changed. */
static uint32_t remap_keys[4];                  /* the keys whose entry the last remap changed */
static int remap(void)
{
    static const uint8_t free_e[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 12, 13, 14, 15 };
    uint8_t taken[NKEY], ek[16], np[NKEY], ns[NKEY];
    int k, j, changed = 0;
    memset(remap_keys, 0, sizeof remap_keys);
    memset(taken, 0, sizeof taken); memset(ek, 0, sizeof ek);
    taken[0] = 1;
    if (hwdisp_s4_strip != 1) { ek[9] = KEY_BLACK; taken[KEY_BLACK] = 1; }    /* its 16 pixels past the window are black */
    for (j = hwdisp_s4_strip != 1 ? 10 : 9; j <= 11; j++) {
        int best = -1;
        for (k = 1; k < NKEY; k++) if (!taken[k] && key_count_s4[k] > 0 && (best < 0 || key_count_s4[k] > key_count_s4[best])) best = k;
        if (best >= 0) { ek[j] = (uint8_t)best; taken[best] = 1; }
    }
    if (!taken[KEY_BLACK]) { ek[free_e[0]] = KEY_BLACK; taken[KEY_BLACK] = 1; }  /* the pairs' strips have the edges */
    for (j = 0; j < 12; j++) {
        int best = -1;
        if (ek[free_e[j]]) continue;
        for (k = 1; k < NKEY; k++) if (!taken[k] && key_count[k] > 0 && (best < 0 || key_count[k] > key_count[best])) best = k;
        if (best >= 0) { ek[free_e[j]] = (uint8_t)best; taken[best] = 1; }
    }
    for (k = 0; k < NKEY; k++) {
        int e, be = 1, bd = 1 << 30, bs = 9, bsd = 1 << 30;
        unsigned c = key_col12(k);
        np[k] = ns[k] = 0;
        if (!k) continue;
        for (e = 1; e < 16; e++) if (ek[e] == k) { np[k] = (uint8_t)e; if (e >= 9 && e <= 11) ns[k] = (uint8_t)e; }
        for (e = 1; e < 16; e++) if (ek[e]) { int d = coldist(c, key_col12(ek[e])); if (d < bd) { bd = d; be = e; } if (e >= 9 && e <= 11 && d < bsd) { bsd = d; bs = e; } }
        if (!np[k]) np[k] = (uint8_t)be;
        if (!ns[k]) ns[k] = (uint8_t)bs;
        if (key_count[k] && (np[k] != map_pair[k] || ns[k] != map_s4[k])) { changed = 1; remap_keys[k >> 5] |= 1u << (k & 31); }
        map_pair[k] = np[k]; map_s4[k] = ns[k];
    }
    memcpy(entry_key, ek, sizeof ek);
    for (k = 0; k < NKEY; k++) key_present[k] = key_count[k] > 0;
    return changed;
}
static int remap_needed(void)                    /* only when it pays: a free entry, or a key much more used */
{
    int k, e, minc = 1 << 30, maxu = 0, s4min = 1 << 30, s4max = 0;
    uint8_t has[NKEY];
    memset(has, 0, sizeof has);
    for (e = 1; e < 16; e++) {
        int c = entry_key[e] ? key_count[entry_key[e]] : 0;
        has[entry_key[e]] = 1;
        if (c < minc) minc = c;
        if (e >= 9 && e <= 11) { int c4 = entry_key[e] ? key_count_s4[entry_key[e]] : 0; if (c4 < s4min) s4min = c4; }
    }
    for (k = 1; k < NKEY; k++) if (!has[k]) {
        if (key_count[k] > maxu) maxu = key_count[k];
        if (key_count_s4[k] > s4max) s4max = key_count_s4[k];
    }
    if (maxu && (minc == 0 || maxu > 2 * minc + 64)) return 1;
    if (s4max && (s4min == 0 || s4max > 2 * s4min + 64)) return 1;
    return 0;
}
static void convert_cell(int b, int cx, int cy)
{
    int g = cx >> 3, s4 = g == hwdisp_s4_strip, pair = g == (hwdisp_s4_strip ? 0 : 1), byte = cx & 7, r;   /* pair 1: sprites 2+3 (the first strip that is not sprite 4's), pair 0: sprites 0+1 */
    uint16_t *ev = sprblk[b][s4 ? 4 : pair * 2], *od = s4 ? 0 : sprblk[b][pair * 2 + 1];
    int w = byte >> 1, hi = !(byte & 1);
    for (r = 0; r < 8; r++) {
        int y = cy * 8 + r, i;
        unsigned p0 = 0, p1 = 0, p2 = 0, p3 = 0;
        uint16_t *le, *lo;
        if (y >= HW_VIEW_H) break;
        for (i = 0; i < 8; i++) {
            int k = keybuf[y][cx * 8 + i], e = s4 ? (k ? map_s4[k] - 8 : 0) : map_pair[k];
            unsigned bit = 0x80u >> i;
            if (e & 1) p0 |= bit;
            if (e & 2) p1 |= bit;
            if (e & 4) p2 |= bit;
            if (e & 8) p3 |= bit;
        }
        le = ev + 8 + y * 8; lo = od ? od + 8 + y * 8 : 0;
#define PUT(wp, v) ((wp) = (uint16_t)(hi ? ((wp) & 0x00ff) | (v) << 8 : ((wp) & 0xff00) | (v)))
        PUT(le[w], p0); PUT(le[4 + w], p1);
        if (lo) { PUT(lo[w], p2); PUT(lo[4 + w], p3); }
#undef PUT
    }
}
static void front_work(int blank)
{
    int cx, cy, n = hwdisp_cells_per_frame, b = frame_no & 1, left = 0, i;
    for (cy = 0; cy < FCH; cy++) for (cx = 0; cx < FCW; cx++)
        if (fdirty[cy][cx]) { if (n) { compose_cell(cx, cy); fdirty[cy][cx] = 0; n--; } else left++; }
    if (remap_needed() && remap()) {             /* only the cells that hold a key whose entry changed */
        for (cy = 0; cy < FCH; cy++) for (cx = 0; cx < FCW; cx++) {
            uint32_t *km = cellkeys[cy][cx];
            if ((km[0] & remap_keys[0]) | (km[1] & remap_keys[1]) | (km[2] & remap_keys[2]) | (km[3] & remap_keys[3])) fpend[0][cy][cx] = fpend[1][cy][cx] = 1;
        }
        hwdisp_last.remaps++;
    }
    n = hwdisp_cells_per_frame * 2;              /* conversions are cheaper than composing (FEASIBILITY.md: 20 cells) */
    for (cy = 0; cy < FCH; cy++) for (cx = 0; cx < FCW; cx++)
        if (fpend[b][cy][cx]) { if (n) { convert_cell(b, cx, cy); fpend[b][cy][cx] = 0; n--; hwdisp_last.cells++; } else left++; }
    hwdisp_last.cells_left = left;
    for (i = 0; i < 16; i++) {
        uint32_t c = entry_key[i] ? rgb24(key_col12(entry_key[i])) : 0;
        if (blank && entry_key[i] != KEY_BLACK) c = rgb24(col12(0xf8 + (V[0x12] & 7)));   /* an empty window */
        if (!bank_rgb_ok || c != bank_rgb[i]) { bank_rgb[i] = c; bank_rgb_ok = 0; }
    }
    if (!bank_rgb_ok) { hw_palette(bank_rgb, HW_SPR_BANK * 16, 16); bank_rgb_ok = 1; }
    {
        int g, p23 = -1, p01 = -1;                /* the strips of the two pairs: 2+3 first, then 0+1 */
        for (g = 0; g < 3; g++) if (g != hwdisp_s4_strip) { if (p23 < 0) p23 = g; else p01 = g; }
        hw_sprite(4, sprblk[b][4], HW_WIN_X - 16 + 64 * hwdisp_s4_strip, 0, HW_SPR_H, 0);
        hw_sprite(2, sprblk[b][2], HW_WIN_X - 16 + 64 * p23, 0, HW_SPR_H, 0);
        hw_sprite(3, sprblk[b][3], HW_WIN_X - 16 + 64 * p23, 0, HW_SPR_H, 1);
        hw_sprite(0, sprblk[b][0], HW_WIN_X - 16 + 64 * p01, 0, HW_SPR_H, 0);
        hw_sprite(1, sprblk[b][1], HW_WIN_X - 16 + 64 * p01, 0, HW_SPR_H, 1);
    }
}

/* ---- objects ---- */
static int front_plane_under(int x0, int y0, int x1, int y1)    /* any front-plane character with pixels there? */
{
    const uint8_t *map = V + (back_sel ? 0x1000 : 0x1800);
    int x, y;
    for (y = y0 & ~7; y <= y1; y += 8)
        for (x = x0 & ~7; x <= x1; x += 8) {
            unsigned px = (unsigned)(x + front_scx) & 255, py = (unsigned)(y + front_scy) & 255, k;
            for (k = 0; k < 4; k++) {           /* the plane's cells under this 8 x 8 area (it may be scrolled) */
                unsigned qx = (px + (k & 1) * 7) & 255, qy = (py + (k >> 1) * 7) & 255;
                if (!char_empty[RD16(map + ((qy >> 3) * 32 + (qx >> 3)) * 2) & 0x1ff]) return 1;
            }
        }
    return 0;
}
static void find_objects(void)
{
    int i, j, lastx = 0, lasty = 0, par[64], oid[64], m = 0;
    nspr = 0;
    for (i = 0; i < 64; i++) {
        const uint8_t *s = V + 0x0800 + i * 4;
        unsigned e = RD16(s);
        int sx = s[2], sy = s[3];
        if (e & 0x0400) sx += lastx;
        if (e & 0x0200) sy += lasty;
        lastx = sx; lasty = sy;
        if (!((e >> 11) & 3)) continue;
        sx = (sx + V[0x20]) & 255; sy += V[0x21];
        if (sx > 248) sx -= 256;
        if (sy > 248 && sy < 256) sy -= 256; else sy &= 255;
        if (sx >= 160 || sy >= HW_VIEW_H) continue;
        spr[nspr].x = (int16_t)sx; spr[nspr].y = (int16_t)sy; spr[nspr].e = (uint16_t)e;
        spr[nspr].pal = V[0x0c00 + i] & 15; spr[nspr].n = (uint8_t)i; spr[nspr].pr = (uint8_t)((e >> 11) & 3);
        nspr++;
    }
    for (i = 0; i < nspr; i++) par[i] = i;
    for (i = 0; i < nspr; i++) for (j = 0; j < i; j++) {
        int dx = spr[i].x - spr[j].x, dy = spr[i].y - spr[j].y;
        if (dx >= -8 && dx <= 8 && dy >= -8 && dy <= 8) {
            int a = i, b = j;
            while (par[a] != a) a = par[a];
            while (par[b] != b) b = par[b];
            if (a != b) par[a] = b;
        }
    }
    nobj = 0;
    for (i = 0; i < nspr; i++) { int r = i; while (par[r] != r) r = par[r]; par[i] = r; oid[i] = -1; }
    for (i = 0; i < nspr; i++) if (par[i] == i) oid[i] = nobj < MAXOBJ ? nobj++ : -1;
    for (j = 0; j < nobj; j++) {               /* members of each object in sprite order: lower numbers first */
        Obj *o = &obj[j];
        o->x0 = o->y0 = 32767; o->x1 = o->y1 = -32768; o->count = 0; o->first = (uint8_t)m;
        for (i = 0; i < nspr; i++) {
            if (oid[par[i]] != j) continue;
            members[m++] = (uint8_t)i; o->count++;
            if (spr[i].x < o->x0) o->x0 = spr[i].x;
            if (spr[i].y < o->y0) o->y0 = spr[i].y;
            if (spr[i].x + 7 > o->x1) o->x1 = spr[i].x + 7;
            if (spr[i].y + 7 > o->y1) o->y1 = spr[i].y + 7;
        }
        o->front = o->x1 - o->x0 >= 32 || o->y1 - o->y0 >= 32 || front_plane_under(o->x0, o->y0, o->x1, o->y1);
    }
    nfspr = 0;
    for (j = 0; j < nobj; j++) if (obj[j].front)
        for (i = 0; i < obj[j].count; i++) fspr[nfspr++] = spr[members[obj[j].first + i]];
    memset(sprite_used, 0, sizeof sprite_used);
    for (j = 0; j < nobj; j++) if (!obj[j].front)
        for (i = 0; i < obj[j].count; i++) sprite_used[spr[members[obj[j].first + i]].pal] = 1;
}
static void front_objects_changed(void)        /* the front layer's sprites moved or changed: their cells */
{
    int i;
    if (nfspr == nfspr_prev && !memcmp(fspr, fspr_prev, sizeof(Spr) * (size_t)nfspr)) return;
    hwdisp_last.why_objs++;
    for (i = 0; i < nfspr_prev; i++) front_cell_mark(fspr_prev[i].x, fspr_prev[i].y, fspr_prev[i].x + 7, fspr_prev[i].y + 7);
    for (i = 0; i < nfspr; i++) front_cell_mark(fspr[i].x, fspr[i].y, fspr[i].x + 7, fspr[i].y + 7);
    memcpy(fspr_prev, fspr, sizeof(Spr) * (size_t)nfspr); nfspr_prev = nfspr;
}
static Bobc *bob_image(const Obj *o)
{
    uint16_t sig[64 * 2 + 2];
    int ns = 0, i, best = 0, w = o->x1 - o->x0 + 1, h = o->y1 - o->y0 + 1, wwords = (w + 15) >> 4, pr;
    Bobc *b;
    sig[ns++] = (uint16_t)w; sig[ns++] = (uint16_t)h;
    for (i = 0; i < o->count; i++) {
        const Spr *s = &spr[members[o->first + i]];
        sig[ns++] = (uint16_t)((s->x - o->x0) | (s->y - o->y0) << 6 | s->pal << 12);
        sig[ns++] = s->e;
    }
    for (i = 0; i < NCACHE; i++)
        if (cache[i].nsig == ns && !memcmp(cache[i].sig, sig, (size_t)ns * 2)) { cache[i].used = ++cache_clock; return &cache[i]; }
    for (i = 1; i < NCACHE; i++) if (cache[i].used < cache[best].used) best = i;
    b = &cache[best];
    memcpy(b->sig, sig, (size_t)ns * 2); b->nsig = ns; b->used = ++cache_clock; b->wwords = wwords; b->h = h;
    memset(b->img, 0, (size_t)HW_PLANES * 32 * 2 * 2); memset(b->msk, 0, 32 * 2 * 2);
    for (pr = 1; pr <= 3; pr++)                 /* painter's order: priority, then the higher number first */
        for (i = o->count - 1; i >= 0; i--) {
            const Spr *s = &spr[members[o->first + i]];
            int x, y, p;
            if (s->pr != pr) continue;
            for (y = 0; y < 8; y++) for (x = 0; x < 8; x++) {
                int c = sprite_pixel(s, s->x + x, s->y + y), bx = s->x - o->x0 + x, by = s->y - o->y0 + y, pen;
                uint16_t bit = (uint16_t)(0x8000u >> (bx & 15));
                if (!c) continue;
                pen = pen_of(16 + s->pal, c);
                b->msk[by * wwords + (bx >> 4)] |= bit;
                for (p = 0; p < HW_PLANES; p++) {
                    uint16_t *wp = &b->img[(p * h + by) * wwords + (bx >> 4)];
                    if (pen >> p & 1) *wp |= bit; else *wp &= (uint16_t)~bit;
                }
            }
        }
    return b;
}

/* ---- the frame ---- */
int hwdisp_init(void)
{
    int i, b, n;
    tilebuf = hw_chip(HW_PLANES * 16 * 2);
    for (b = 0; b < 2; b++) for (n = 0; n < 5; n++) {
        sprblk[b][n] = hw_chip(HW_SPR_BYTES);
        if (!sprblk[b][n]) return 0;
        memset(sprblk[b][n], 0, HW_SPR_BYTES);
    }
    for (i = 0; i < NCACHE; i++) {
        cache[i].img = hw_chip(HW_PLANES * 32 * 2 * 2); cache[i].msk = hw_chip(32 * 2 * 2);
        if (!cache[i].img || !cache[i].msk) return 0;
        cache[i].nsig = -1;
    }
    if (!tilebuf) return 0;
    for (i = 0; i < 256; i++) rev8[i] = (uint8_t)((i >> 6) | ((i >> 2) & 0x0c) | ((i << 2) & 0x30) | (i << 6 & 0xc0));
    memset(tab_ok, 0, sizeof tab_ok);
    memset(tstate, 1, sizeof tstate);            /* the kit clears the buffers: black */
    memset(key_slot, -1, sizeof key_slot); memset(slot_key, -1, sizeof slot_key);
    memset(keybuf, 0, sizeof keybuf);
    memset(key_count, 0, sizeof key_count); memset(key_count_s4, 0, sizeof key_count_s4);
    key_count[0] = HW_VIEW_H * FW; key_count_s4[0] = HW_VIEW_H * 64;
    memset(map_pair, 0, sizeof map_pair); memset(map_s4, 0, sizeof map_s4); memset(entry_key, 0, sizeof entry_key); memset(key_present, 0, sizeof key_present);
    memset(fdirty, 1, sizeof fdirty);
    back_sel = -1; scroll_known = 0; U = 0; frame_no = 0;
    return 1;
}

void hwdisp_frame(const uint8_t *v, K2geDirty *dirty)
{
    int i, sel = (v[0x30] & 0x80) ? 0 : 1, fsx, fsy, blank;
    int32_t want_cam;
    V = v;
    memset(&hwdisp_last, 0, sizeof hwdisp_last);
    if (!scroll_known) { memset(char_empty, 0, sizeof char_empty); for (i = 0; i < 512; i++) dirty->chr[i] = 1; }
    for (i = 0; i < 512; i++) if (dirty->chr[i]) {
        const uint8_t *c = v + 0x2000 + i * 16; int k, e = 1;
        for (k = 0; k < 16; k++) if (c[k]) e = 0;
        char_empty[i] = (uint8_t)e;
    }
    if (sel != back_sel) { back_sel = sel; reset_all(); }
    /* back plane scroll: U adds up the horizontal steps; vertical is the row */
    {
        uint8_t scx = v[back_sel ? 0x34 : 0x32];
        if (!scroll_known) { U = scx; scroll_known = 1; }
        else U += (int8_t)(uint8_t)(scx - last_scx);
        last_scx = scx;
        cam_y = v[back_sel ? 0x35 : 0x33];
        want_cam = W0 + U - HW_WIN_X;
        if (want_cam < 64 || want_cam > HW_LEVEL_W - 320 - 64) {    /* the end of the slide: start again near W0 */
            U &= 255; want_cam = W0 + U - HW_WIN_X;
            for (i = 0; i < NCOL; i++) memset(tstate[i], 0, NROW);
            hwdisp_last.rebases++;
        }
        cam_x = want_cam;
    }
    /* what the game wrote */
    {
        int front_base = back_sel ? 0 : 1024, back_base = back_sel ? 1024 : 0, any_chr = 0;
        fsx = v[back_sel ? 0x32 : 0x34]; fsy = v[back_sel ? 0x33 : 0x35];
        if (fsx != front_scx || fsy != front_scy) {
            front_scx = fsx; front_scy = fsy; memset(fdirty, 1, sizeof fdirty); hwdisp_last.why_scroll++;
        }
        if (memcmp(last_regs, v + 2, 4) && v[4] && v[5]) {     /* the window moved: the cells on its old and new edges */
            int ax0 = last_regs[0], ay0 = last_regs[1], ax1 = ax0 + last_regs[2], ay1 = ay0 + last_regs[3];
            int bx0 = v[2], by0 = v[3], bx1 = bx0 + v[4], by1 = by0 + v[5], cx, cy;
            if (!last_regs[2] || !last_regs[3]) ax0 = ax1 = ay0 = ay1 = 0;
            for (cy = 0; cy < FCH; cy++) for (cx = 2; cx < FCW - 2; cx++) {
                int x0 = cx * 8 - 16, y0 = cy * 8, x1 = x0 + 8, y1 = y0 + 8;
                int ina = x0 >= ax0 && x1 <= ax1 && y0 >= ay0 && y1 <= ay1, outa = x1 <= ax0 || x0 >= ax1 || y1 <= ay0 || y0 >= ay1;
                int inb = x0 >= bx0 && x1 <= bx1 && y0 >= by0 && y1 <= by1, outb = x1 <= bx0 || x0 >= bx1 || y1 <= by0 || y0 >= by1;
                if (!((ina && inb) || (outa && outb))) fdirty[cy][cx] = 1;
            }
            memcpy(last_regs, v + 2, 4);
        }
        for (i = 0; i < 512; i++) if (dirty->chr[i]) { any_chr = 1; break; }
        if (any_chr) {                          /* changed characters: the cells and objects that show them */
            const uint8_t *fm = v + (back_sel ? 0x1000 : 0x1800);
            clear_cache();
            for (i = 0; i < 1024; i++) {
                if (dirty->chr[bmap[i] & 0x1ff]) inval_back_cell(i & 31, i >> 5);
                if (dirty->chr[RD16(fm + i * 2) & 0x1ff]) front_map_cell_changed(i & 31, i >> 5);
            }
            for (i = 0; i < nfspr_prev; i++) if (dirty->chr[fspr_prev[i].e & 0x1ff]) front_cell_mark(fspr_prev[i].x, fspr_prev[i].y, fspr_prev[i].x + 7, fspr_prev[i].y + 7);
        }
        for (i = 0; i < 1024; i++) {
            if (dirty->cell[back_base + i]) back_cell_changed(i);
            if (dirty->cell[front_base + i]) front_map_cell_changed(i & 31, i >> 5);
        }
    }
    memset(dirty->cell, 0, sizeof dirty->cell); memset(dirty->chr, 0, sizeof dirty->chr); dirty->any = 0;
    find_objects();
    front_objects_changed();
    retry_aliases();
    tiles_work();
    blank = !v[4] || !v[5] || v[2] >= 160 || v[3] >= HW_VIEW_H;  /* an empty window: the edge colour everywhere */
    front_work(blank);
    for (i = 0; i < nobj && !blank; i++) if (!obj[i].front) {
        Bobc *b = bob_image(&obj[i]);
        hw_bob(W0 + U + obj[i].x0, cam_y + obj[i].y0, b->img, b->msk, b->wwords, b->h);
        hwdisp_last.bobs++;
    }
    for (i = 0; i < nobj; i++) hwdisp_last.front_objs += obj[i].front;
    /* playfield colours: black, the background, the slots */
    {
        uint32_t c[64]; int lo = 64, hi = -1, s, k;
        c[0] = 0; c[1] = rgb24(col12(0xf0 + (v[0x118] & 7)));
        for (s = 0; s < NSLOT; s++) for (k = 1; k < 4; k++) c[2 + 3 * s + k - 1] = slot_key[s] >= 0 ? rgb24(slotkey_col12(slot_key[s], k)) : 0;
        for (s = 2 + 3 * NSLOT; s < 64; s++) c[s] = 0;
        if (blank) { uint32_t e = rgb24(col12(0xf8 + (v[0x12] & 7))); for (s = 1; s < 64; s++) c[s] = e; }
        for (s = 0; s < 64; s++) if (!pf_rgb_ok || c[s] != pf_rgb[s]) { if (s < lo) lo = s; hi = s; pf_rgb[s] = c[s]; }
        if (hi >= 0) hw_palette(pf_rgb + lo, lo, hi - lo + 1);
        pf_rgb_ok = 1;
    }
    hw_scroll(cam_x, cam_y);
    frame_no++;
}
