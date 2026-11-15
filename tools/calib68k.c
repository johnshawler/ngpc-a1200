/* calib68k.c - a fixed CPU workload for calibrating the emulator to a CPU it cannot model cycle-exactly
 * (tools/calib_m1230.sh). Writes calib.txt: "calib_cck=<colour clocks the workload took>".
 *
 * The workload is ordinary compiled integer code on data in fast RAM (a table walk, adds, shifts, compares, branches,
 * a short loop that fits the cache), the kind of code the translated game is made of, but none of the game's own code:
 * speeding up the port does not move the calibration.
 *   CFLAGS="-Ikit" tools/build.sh dist/calib68k kit/sys.c tools/calib68k.c     (run as "calib68k")
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <exec/memory.h>
#include <stdio.h>
#include <string.h>
#include "sys.h"

#define N 4096
static ULONG table[N];                          /* BSS: fast RAM when there is some */

static ULONG work(ULONG rounds)
{
    ULONG r, i, x = 12345, sum = 0;
    for (i = 0; i < N; i++) table[i] = i * 2654435761UL;
    for (r = 0; r < rounds; r++)
        for (i = 0; i < N; i++) {
            x = x * 1103515245UL + 12345;
            sum += table[(x >> 16) & (N - 1)] ^ (sum << 3);
            if (sum & 0x80) table[i] += sum >> 5; else sum -= table[i] >> 2;
        }
    return sum;
}

int main(void)
{
    ULONG t, sum;
    char text[64];
    BPTR fh;
    if (!sys_take()) return 20;
    t = sys_now();
    sum = work(24);
    t = sys_now() - t;
    sys_give();
    sys_free_all();
    sprintf(text, "calib_cck=%lu sum=%08lx\n", (unsigned long)t, (unsigned long)sum);
    if ((fh = Open((CONST_STRPTR)"calib.txt", MODE_NEWFILE))) { Write(fh, text, strlen(text)); Close(fh); }
    return 0;
}
