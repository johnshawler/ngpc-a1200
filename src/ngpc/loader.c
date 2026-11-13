/* loader.c - starts a program packed with LZMA (Amiga only, AmigaOS 2.0 or later). For floppies the program does not
 * fit on unpacked (tools/make_adf_ngpc.sh): the disk holds this loader under the program's name and the program as
 * <name>.lzma. The loader unpacks it in memory (src/ngpc/unlzma.c), loads it with dos.library's InternalLoadSeg()
 * reading from that memory, runs it with the loader's own arguments, and unloads it again. Nothing of it stays.
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <exec/memory.h>
#include <dos/dosextens.h>
#include <stdio.h>
#include <string.h>
#include "unlzma.h"

static const UBYTE *image;                      /* the unpacked program, read by InternalLoadSeg() */
static ULONG image_len, image_pos;

static LONG lread(BPTR fh __asm("d1"), APTR buf __asm("d2"), LONG len __asm("d3"))
{
    (void)fh;
    if ((ULONG)len > image_len - image_pos) len = (LONG)(image_len - image_pos);
    CopyMem((APTR)(image + image_pos), buf, (ULONG)len);
    image_pos += (ULONG)len;
    return len;
}
static APTR lalloc(ULONG size __asm("d0"), ULONG flags __asm("d1")) { return AllocMem(size, flags); }
static void lfree(APTR mem __asm("a1"), ULONG size __asm("d0")) { FreeMem(mem, size); }

int main(int argc, char **argv)
{
    char name[108], args[256];
    BPTR fh, seg;
    LONG funcs[3], stack = 0, len, rc;
    ULONG plen, n;
    UBYTE *packed, *out;
    UWORD *work;
    int i;
    if (argc < 1) return 20;                    /* started from Workbench: not supported */
    strncpy(name, (char *)FilePart((STRPTR)argv[0]), sizeof name - 6); name[sizeof name - 6] = 0;
    strcat(name, ".lzma");
    if (!(fh = Open((STRPTR)name, MODE_OLDFILE))) { printf("cannot open %s\n", name); return 20; }
    Seek(fh, 0, OFFSET_END);
    plen = (ULONG)Seek(fh, 0, OFFSET_BEGINNING);
    packed = AllocMem(plen, MEMF_ANY);
    if (!packed || Read(fh, packed, (LONG)plen) != (LONG)plen) { Close(fh); printf("cannot read %s\n", name); return 20; }
    Close(fh);
    n = unlzma_size(packed, plen);
    out = n != 0xffffffffUL ? AllocMem(n, MEMF_ANY) : 0;
    work = AllocMem(unlzma_work(packed), MEMF_ANY);
    if (!out || !work || unlzma(packed, plen, out, n, work) != n) { printf("cannot unpack %s\n", name); return 20; }
    FreeMem(work, unlzma_work(packed)); FreeMem(packed, plen);
    image = out; image_len = n; image_pos = 0;
    funcs[0] = (LONG)lread; funcs[1] = (LONG)lalloc; funcs[2] = (LONG)lfree;
    seg = InternalLoadSeg((BPTR)1, 0, funcs, &stack);
    FreeMem(out, n);
    if (!seg) { printf("%s is not a program\n", name); return 20; }
    args[0] = 0;                                /* the arguments, as the Shell gave them to the loader */
    for (i = 1; i < argc; i++) { if (strlen(args) + strlen(argv[i]) + 3 >= sizeof args) break; if (i > 1) strcat(args, " "); strcat(args, argv[i]); }
    strcat(args, "\n");
    len = (LONG)strlen(args);
    rc = RunCommand(seg, 32768, (STRPTR)args, len);
    UnLoadSeg(seg);
    return (int)rc;
}
