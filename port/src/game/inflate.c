/*
 * Raw DEFLATE decoder replacing the game's hand written MIPS inflate routines (src/hasm/gzip_asm.s).
 *
 * The game decompresses in place: the compressed data sits at the end of the output buffer, so this decoder
 * reads its input strictly in order and never more than a few bytes ahead of what it has consumed.
 */
#include <ultra64.h>
#include "types.h"
#include "port.h"

#define MAXBITS 15
#define MAXLCODES 286
#define MAXDCODES 30
#define FIXLCODES 288
#define FAST_BITS 9

typedef struct Huffman {
    s16 count[MAXBITS + 1];
    s16 symbol[FIXLCODES];
    /* Fast lookup indexed by the next FAST_BITS input bits: (length << 12) | symbol, or 0 for long codes. */
    u16 fast[1 << FAST_BITS];
} Huffman;

typedef struct InflateState {
    const u8 *in;
    u8 *out;
    u32 bitBuf;
    s32 bitCount;
} InflateState;

static const u16 sLengthBase[29] = { 3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                     31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const u8 sLengthExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const u16 sDistBase[30] = { 1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                   193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const u8 sDistExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static Huffman sFixedLen;
static Huffman sFixedDist;
static s32 sFixedBuilt;

static inline void need_bits(InflateState *s, s32 n) {
    while (s->bitCount < n) {
        s->bitBuf |= (u32) (*s->in++) << s->bitCount;
        s->bitCount += 8;
    }
}

static inline u32 get_bits(InflateState *s, s32 n) {
    u32 val;
    if (n == 0) {
        return 0;
    }
    need_bits(s, n);
    val = s->bitBuf & ((1u << n) - 1);
    s->bitBuf >>= n;
    s->bitCount -= n;
    return val;
}

/* Builds a canonical Huffman decoding table. Returns 0 on success. */
static s32 build_huffman(Huffman *h, const u8 *lengths, s32 n) {
    s16 offs[MAXBITS + 1];
    s32 i, len, left;
    s32 code, sym, reversed, fill, step;

    for (len = 0; len <= MAXBITS; len++) {
        h->count[len] = 0;
    }
    for (sym = 0; sym < n; sym++) {
        h->count[lengths[sym]]++;
    }
    if (h->count[0] == n) {
        bzero(h->fast, sizeof(h->fast));
        return 0;
    }
    left = 1;
    for (len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) {
            return -1;
        }
    }
    offs[1] = 0;
    for (len = 1; len < MAXBITS; len++) {
        offs[len + 1] = offs[len] + h->count[len];
    }
    for (sym = 0; sym < n; sym++) {
        if (lengths[sym] != 0) {
            h->symbol[offs[lengths[sym]]++] = sym;
        }
    }

    // Fast table: walk the canonical codes in order.
    bzero(h->fast, sizeof(h->fast));
    code = 0;
    i = 0;
    for (len = 1; len <= FAST_BITS; len++) {
        s32 c;
        for (c = 0; c < h->count[len]; c++, i++, code++) {
            s32 b;
            reversed = 0;
            for (b = 0; b < len; b++) {
                reversed |= ((code >> b) & 1) << (len - 1 - b);
            }
            step = 1 << len;
            for (fill = reversed; fill < (1 << FAST_BITS); fill += step) {
                h->fast[fill] = (u16) ((len << 12) | h->symbol[i]);
            }
        }
        code <<= 1;
    }
    return 0;
}

static s32 decode_slow(InflateState *s, const Huffman *h) {
    s32 code = 0, first = 0, index = 0, len, count;
    for (len = 1; len <= MAXBITS; len++) {
        code |= get_bits(s, 1);
        count = h->count[len];
        if (code - count < first) {
            return h->symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static inline s32 decode(InflateState *s, const Huffman *h) {
    u32 entry;
    s32 len;

    // Top the bit buffer up byte by byte without reading past what a code could need.
    while (s->bitCount < FAST_BITS) {
        s->bitBuf |= (u32) (*s->in++) << s->bitCount;
        s->bitCount += 8;
    }
    entry = h->fast[s->bitBuf & ((1 << FAST_BITS) - 1)];
    if (entry != 0) {
        len = entry >> 12;
        s->bitBuf >>= len;
        s->bitCount -= len;
        return entry & 0xFFF;
    }
    return decode_slow(s, h);
}

static s32 inflate_codes(InflateState *s, const Huffman *lencode, const Huffman *distcode) {
    s32 sym, len;
    u32 dist;
    u8 *from;

    for (;;) {
        sym = decode(s, lencode);
        if (sym < 0) {
            return -1;
        }
        if (sym < 256) {
            *s->out++ = (u8) sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) {
                return -1;
            }
            len = sLengthBase[sym] + get_bits(s, sLengthExtra[sym]);
            sym = decode(s, distcode);
            if (sym < 0 || sym >= 30) {
                return -1;
            }
            dist = sDistBase[sym] + get_bits(s, sDistExtra[sym]);
            from = s->out - dist;
            while (len--) {
                *s->out++ = *from++;
            }
        }
    }
}

static s32 inflate_stored(InflateState *s) {
    u32 len;
    // Discard the remaining bits of the current byte, and give back any whole bytes that were read ahead.
    s->in -= s->bitCount >> 3;
    s->bitBuf = 0;
    s->bitCount = 0;
    len = s->in[0] | (s->in[1] << 8);
    s->in += 4;
    while (len--) {
        *s->out++ = *s->in++;
    }
    return 0;
}

static s32 inflate_fixed(InflateState *s) {
    if (!sFixedBuilt) {
        u8 lengths[FIXLCODES];
        s32 sym;
        for (sym = 0; sym < 144; sym++) {
            lengths[sym] = 8;
        }
        for (; sym < 256; sym++) {
            lengths[sym] = 9;
        }
        for (; sym < 280; sym++) {
            lengths[sym] = 7;
        }
        for (; sym < FIXLCODES; sym++) {
            lengths[sym] = 8;
        }
        build_huffman(&sFixedLen, lengths, FIXLCODES);
        for (sym = 0; sym < MAXDCODES; sym++) {
            lengths[sym] = 5;
        }
        build_huffman(&sFixedDist, lengths, MAXDCODES);
        sFixedBuilt = TRUE;
    }
    return inflate_codes(s, &sFixedLen, &sFixedDist);
}

static s32 inflate_dynamic(InflateState *s) {
    static const u8 order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    static Huffman lencode, distcode;
    u8 lengths[MAXLCODES + MAXDCODES];
    s32 nlen, ndist, ncode, index, sym, len;

    nlen = get_bits(s, 5) + 257;
    ndist = get_bits(s, 5) + 1;
    ncode = get_bits(s, 4) + 4;
    if (nlen > MAXLCODES || ndist > MAXDCODES) {
        return -1;
    }
    for (index = 0; index < ncode; index++) {
        lengths[order[index]] = get_bits(s, 3);
    }
    for (; index < 19; index++) {
        lengths[order[index]] = 0;
    }
    if (build_huffman(&lencode, lengths, 19) != 0) {
        return -1;
    }
    index = 0;
    while (index < nlen + ndist) {
        sym = decode(s, &lencode);
        if (sym < 0) {
            return -1;
        }
        if (sym < 16) {
            lengths[index++] = sym;
        } else {
            len = 0;
            if (sym == 16) {
                if (index == 0) {
                    return -1;
                }
                len = lengths[index - 1];
                sym = 3 + get_bits(s, 2);
            } else if (sym == 17) {
                sym = 3 + get_bits(s, 3);
            } else {
                sym = 11 + get_bits(s, 7);
            }
            if (index + sym > nlen + ndist) {
                return -1;
            }
            while (sym--) {
                lengths[index++] = len;
            }
        }
    }
    if (build_huffman(&lencode, lengths, nlen) != 0 || build_huffman(&distcode, lengths + nlen, ndist) != 0) {
        return -1;
    }
    return inflate_codes(s, &lencode, &distcode);
}

/* Decompresses a raw deflate stream. Returns the number of bytes written, or -1 on error. */
s32 port_inflate(const u8 *in, u8 *out) {
    InflateState s;
    s32 last, type, err;

    s.in = in;
    s.out = out;
    s.bitBuf = 0;
    s.bitCount = 0;
    do {
        last = get_bits(&s, 1);
        type = get_bits(&s, 2);
        switch (type) {
            case 0:
                err = inflate_stored(&s);
                break;
            case 1:
                err = inflate_fixed(&s);
                break;
            case 2:
                err = inflate_dynamic(&s);
                break;
            default:
                err = -1;
                break;
        }
        if (err != 0) {
            port_log("inflate: corrupt data");
            return -1;
        }
    } while (!last);
    return (s32) (s.out - out);
}
