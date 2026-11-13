/* unlzma.c - the LZMA decoder (see unlzma.h): a range decoder with adaptive bit probabilities, literals coded with
 * the previous byte (and the byte at the last distance after a match) as context, match lengths and distances, and
 * four recent distances. The whole output is the dictionary, so a match copies from earlier in dst.
 * The range decoder's state lives in local variables and the bit decoder is a macro, so a 68020 keeps it in
 * registers. */
#include "unlzma.h"

#define PROB_INIT 1024                          /* half of 1 << 11 */
#define STATES 12
#define POS_BITS_MAX 4
#define LEN_TO_POS 4
#define END_POS_MODEL 14
#define FULL_DIST 128
#define ALIGN_BITS 4

/* where each kind of probability lives in the one array */
#define P_ISMATCH   0
#define P_ISREP     (P_ISMATCH + (STATES << POS_BITS_MAX))
#define P_ISREPG0   (P_ISREP + STATES)
#define P_ISREPG1   (P_ISREPG0 + STATES)
#define P_ISREPG2   (P_ISREPG1 + STATES)
#define P_REP0LONG  (P_ISREPG2 + STATES)
#define P_POSSLOT   (P_REP0LONG + (STATES << POS_BITS_MAX))
#define P_POSSPEC   (P_POSSLOT + (LEN_TO_POS << 6))
#define P_ALIGN     (P_POSSPEC + FULL_DIST - END_POS_MODEL)
#define LEN_PROBS   (2 + (16 << 3) + (16 << 3) + 256)   /* choice, choice 2, low and mid per pos state, high */
#define P_LEN       (P_ALIGN + (1 << ALIGN_BITS))
#define P_REPLEN    (P_LEN + LEN_PROBS)
#define P_LIT       (P_REPLEN + LEN_PROBS)

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* the range decoder, on the locals range, code, in, end, bad */
#define NORM() do { if (range < (1u << 24)) { range <<= 8; code = code << 8 | (in < end ? *in++ : (bad = 1, 0u)); } } while (0)
#define BIT(prob, b) do {                                                                      \
        uint16_t *p_ = (prob); uint32_t v_ = *p_, bound_ = (range >> 11) * v_;                  \
        if (code < bound_) { range = bound_; *p_ = (uint16_t)(v_ + ((2048 - v_) >> 5)); (b) = 0; } \
        else { range -= bound_; code -= bound_; *p_ = (uint16_t)(v_ - (v_ >> 5)); (b) = 1; }      \
        NORM();                                                                                \
    } while (0)
#define TREE(probs, nbits, out) do {                    /* nbits bits, highest first */       \
        uint16_t *t_ = (probs); unsigned m_ = 1, k_, b_;                                       \
        for (k_ = 0; k_ < (nbits); k_++) { BIT(&t_[m_], b_); m_ = (m_ << 1) + b_; }           \
        (out) = m_ - (1u << (nbits));                                                          \
    } while (0)
#define RTREE(probs, nbits, out) do {                   /* nbits bits, lowest first */        \
        uint16_t *t_ = (probs); unsigned m_ = 1, k_, b_, s_ = 0;                               \
        for (k_ = 0; k_ < (nbits); k_++) { BIT(&t_[m_], b_); m_ = (m_ << 1) + b_; s_ |= b_ << k_; } \
        (out) = s_;                                                                            \
    } while (0)
#define LENGTH(p, out) do {                             /* a match length less 2 */           \
        uint16_t *l_ = (p); unsigned c_;                                                       \
        BIT(&l_[0], c_);                                                                       \
        if (!c_) TREE(&l_[2 + (pos_state << 3)], 3, out);                                      \
        else { BIT(&l_[1], c_);                                                                \
               if (!c_) { TREE(&l_[2 + (16 << 3) + (pos_state << 3)], 3, out); (out) += 8; }  \
               else { TREE(&l_[2 + (16 << 3) + (16 << 3)], 8, out); (out) += 16; } }          \
    } while (0)

uint32_t unlzma_size(const uint8_t *src, uint32_t srclen)
{
    if (srclen < 13 || src[9] | src[10] | src[11] | src[12]) return 0xffffffffu;
    return le32(src + 5);
}

uint32_t unlzma_work(const uint8_t *src)
{
    unsigned d = src[0], lc = d % 9, lp = (d / 9) % 5;
    return (uint32_t)(P_LIT + (0x300u << (lc + lp))) * 2;
}

uint32_t unlzma(const uint8_t *src, uint32_t srclen, uint8_t *dst, uint32_t dstlen, uint16_t *probs)
{
    unsigned d = src[0], lc, lp, pb, state = 0, i, b, bad = 0;
    uint32_t rep0 = 0, rep1 = 0, rep2 = 0, rep3 = 0, pos = 0, size, nprobs, range, code;
    const uint8_t *in, *end;
    if (srclen < 18 || d >= 9 * 5 * 5) return 0xffffffffu;
    lc = d % 9; d /= 9; lp = d % 5; pb = d / 5;
    size = unlzma_size(src, srclen);
    if (size == 0xffffffffu || size > dstlen) return 0xffffffffu;   /* this decoder wants the size in the header */
    nprobs = P_LIT + (0x300u << (lc + lp));
    for (i = 0; i < nprobs; i++) probs[i] = PROB_INIT;
    in = src + 13; end = src + srclen;
    if (*in++ != 0) return 0xffffffffu;
    range = 0xffffffffu; code = 0;
    for (i = 0; i < 4; i++) code = code << 8 | *in++;
    while (pos < size) {
        unsigned pos_state = pos & ((1u << pb) - 1), len;
        if (bad) return 0xffffffffu;
        BIT(&probs[P_ISMATCH + (state << POS_BITS_MAX) + pos_state], b);
        if (!b) {                                                                   /* a literal */
            unsigned prev = pos ? dst[pos - 1] : 0, sym = 1;
            uint16_t *lit = &probs[P_LIT + 0x300u * (((pos & ((1u << lp) - 1)) << lc) + (prev >> (8 - lc)))];
            if (state >= 7) {                   /* after a match: the byte at the last distance guides it */
                unsigned mb = dst[pos - rep0 - 1];
                do {
                    unsigned mbit = (mb >> 7) & 1;
                    mb <<= 1;
                    BIT(&lit[((1 + mbit) << 8) + sym], b);
                    sym = (sym << 1) | b;
                    if (mbit != b) break;
                } while (sym < 0x100);
            }
            while (sym < 0x100) { BIT(&lit[sym], b); sym = (sym << 1) | b; }
            dst[pos++] = (uint8_t)sym;
            state = state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
            continue;
        }
        BIT(&probs[P_ISREP + state], b);
        if (b) {                                                                    /* a recent distance */
            if (!pos) return 0xffffffffu;
            BIT(&probs[P_ISREPG0 + state], b);
            if (!b) {
                BIT(&probs[P_REP0LONG + (state << POS_BITS_MAX) + pos_state], b);
                if (!b) {                                                           /* one byte */
                    state = state < 7 ? 9 : 11;
                    dst[pos] = dst[pos - rep0 - 1]; pos++;
                    continue;
                }
            } else {
                uint32_t dist;
                BIT(&probs[P_ISREPG1 + state], b);
                if (!b) dist = rep1;
                else {
                    BIT(&probs[P_ISREPG2 + state], b);
                    if (!b) dist = rep2;
                    else { dist = rep3; rep3 = rep2; }
                    rep2 = rep1;
                }
                rep1 = rep0; rep0 = dist;
            }
            LENGTH(&probs[P_REPLEN], len);
            state = state < 7 ? 8 : 11;
        } else {                                                                    /* a new distance */
            unsigned slot;
            rep3 = rep2; rep2 = rep1; rep1 = rep0;
            LENGTH(&probs[P_LEN], len);
            state = state < 7 ? 7 : 10;
            TREE(&probs[P_POSSLOT + ((len < LEN_TO_POS - 1 ? len : LEN_TO_POS - 1) << 6)], 6, slot);
            if (slot < 4) rep0 = slot;
            else {
                unsigned nd = (slot >> 1) - 1, r;
                rep0 = (2u | (slot & 1)) << nd;
                if (slot < END_POS_MODEL) { RTREE(&probs[P_POSSPEC + rep0 - slot - 1], nd, r); rep0 += r; }
                else {
                    unsigned k = nd - ALIGN_BITS;
                    uint32_t dbits = 0;
                    while (k--) {               /* bits with probability one half */
                        uint32_t t;
                        range >>= 1;
                        code -= range;
                        t = 0u - (code >> 31);  /* all ones if the bit is 0 */
                        code += range & t;
                        NORM();
                        dbits = (dbits << 1) + (t + 1);
                    }
                    rep0 += dbits << ALIGN_BITS;
                    RTREE(&probs[P_ALIGN], ALIGN_BITS, r); rep0 += r;
                }
            }
            if (rep0 == 0xffffffffu) break;     /* the end marker */
            if (rep0 >= pos) return 0xffffffffu;
        }
        len += 2;
        if (len > size - pos) return 0xffffffffu;
        {   uint8_t *o = dst + pos, *f = o - rep0 - 1;
            pos += len;
            while (len--) *o++ = *f++;
        }
    }
    return bad ? 0xffffffffu : pos;
}
