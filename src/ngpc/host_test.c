/* host_test.c - milestone check: run the game logic on the host for N ticks of the bench input and print the
 * game-state checksum. Built twice from the same files:
 *   with run_c.c + gen_*.c    the translation (what the Amiga runs)
 *   with run_interp.c         the interpreter in the same frame model (the proof: tools/verify_ngpc.sh)
 *
 *   host_test N            print the checksum after N ticks
 *   host_test N -v         also print the checksum every 100 ticks (to find the first tick that differs)
 *   host_test N -r file    also write the machine after every tick to file: RAM 0x4000-0x7FFF (16 KB), the K2GE
 *                          (16 KB), the ports (256 bytes), the registers (88 bytes: 20 longs, SR, F', stop reason, pc)
 *   host_test N -d t1,t2   also write the picture after these ticks as host_NNNNN.ppm
 * The cartridge is assets/rom.bin and the BIOS's character set assets/font.bin (tools/make_assets_ngpc.sh).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ngp.h"
#define CPU900_EXTERN
#include "cpu900.h"
#include "logic.h"
#include "k2ge.h"

static uint8_t *load(const char *name, long *len, int must)
{
    FILE *f = fopen(name, "rb"); uint8_t *p;
    if (!f) { if (must) { fprintf(stderr, "cannot read %s (run tools/make_assets_ngpc.sh)\n", name); exit(2); } return 0; }
    fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
    p = malloc((size_t)*len);
    if (fread(p, 1, (size_t)*len, f) != (size_t)*len) exit(2);
    fclose(f);
    return p;
}

static void picture(const char *name)
{
    FILE *f = fopen(name, "wb"); uint16_t row[K2GE_W]; int x, y;
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", K2GE_W, K2GE_H_LINES);
    for (y = 0; y < K2GE_H_LINES; y++) {
        k2ge_line(ngp_vid, y, row);
        for (x = 0; x < K2GE_W; x++) { fputc((row[x] & 15) * 17, f); fputc(((row[x] >> 4) & 15) * 17, f); fputc(((row[x] >> 8) & 15) * 17, f); }
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    unsigned n = argc > 1 ? (unsigned)atoi(argv[1]) : 3000, t;
    int verbose = argc > 2 && !strcmp(argv[2], "-v");
    FILE *ramlog = (argc > 3 && !strcmp(argv[2], "-r")) ? fopen(argv[3], "wb") : 0;
    const char *dumps = (argc > 3 && !strcmp(argv[2], "-d")) ? argv[3] : 0;
    long romlen = 0, fontlen = 0;
    uint8_t *rom = load("assets/rom.bin", &romlen, 1), *font = load("assets/font.bin", &fontlen, 0);
    char key[32], list[4096], name[64];
    game_init(rom, (uint32_t)romlen, fontlen == 0x800 ? font : 0);
    for (t = 0; t < n; t++) {
        game_tick(game_bench_input(t));
        if (verbose && (t + 1) % 100 == 0) printf("tick %u checksum %08x\n", t + 1, (unsigned)game_checksum());
        if (ramlog) {
            uint8_t r[88]; int i, j;
            for (i = 0; i < 20; i++) { uint32_t v = i < 16 ? cpu.bank[i >> 2][i & 3] : cpu.gpr[i - 16]; for (j = 0; j < 4; j++) r[i * 4 + j] = (uint8_t)(v >> (8 * j)); }
            r[80] = (uint8_t)cpu.sr; r[81] = (uint8_t)(cpu.sr >> 8); r[82] = cpu.f2; r[83] = (uint8_t)ngp_fatal_why;
            for (j = 0; j < 4; j++) r[84 + j] = (uint8_t)(cpu.pc >> (8 * j));
            fwrite(ngp_ram, 1, sizeof ngp_ram, ramlog); fwrite(ngp_vid, 1, sizeof ngp_vid, ramlog); fwrite(ngp_io, 1, sizeof ngp_io, ramlog); fwrite(r, 1, 88, ramlog);
        }
        if (dumps) { snprintf(key, sizeof key, ",%u,", t + 1); snprintf(list, sizeof list, ",%s,", dumps); if (strstr(list, key)) { snprintf(name, sizeof name, "host_%05u.ppm", t + 1); picture(name); } }
        if (ngp_fatal_why) { fprintf(stderr, "STOPPED at tick %u: reason %d at %06x\n", t + 1, ngp_fatal_why, (unsigned)ngp_fatal_pc); break; }
    }
    if (ramlog) fclose(ramlog);
    if (verbose) printf("sound program: %u instructions (%u a tick); main CPU: %u instructions (%u a tick; counted by the interpreter only)\n",
                        (unsigned)ngp_z80_steps, (unsigned)(ngp_z80_steps / (n ? n : 1)), (unsigned)ngp_cpu_steps, (unsigned)(ngp_cpu_steps / (n ? n : 1)));
    printf("ticks=%u checksum=%08x\n", n, (unsigned)game_checksum());
    return 0;
}
