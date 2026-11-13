/* unlzma.h - unpacks a file in the .lzma format ("LZMA alone": Python's lzma.FORMAT_ALONE, xz --format=lzma) into
 * memory. Plain C, no platform header. Written for this template from the format's description; used to fit the
 * cartridge image on a floppy (tools/make_adf_ngpc.sh). */
#ifndef UNLZMA_H
#define UNLZMA_H
#include <stdint.h>

/* the unpacked size the header gives, or 0xFFFFFFFF if it does not give one (or more than 4 GB) */
uint32_t unlzma_size(const uint8_t *src, uint32_t srclen);
/* probs: working memory of unlzma_work(src) bytes. Returns the bytes written to dst, or 0xFFFFFFFF if the data is bad */
uint32_t unlzma_work(const uint8_t *src);
uint32_t unlzma(const uint8_t *src, uint32_t srclen, uint8_t *dst, uint32_t dstlen, uint16_t *probs);
#endif
