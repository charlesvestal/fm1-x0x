/* SPDX-License-Identifier: GPL-3.0-only */
/* A small LZ77 for the project's patterns, song and motion (project.c), which are mostly empty
 * steps and repeats: 64 steps x 32 patterns plus the lanes are ~30 KB raw and a few KB packed.
 *
 * Stream: a control byte c, then
 *   c < 0x80   c + 1 literal bytes follow (1..128);
 *   c >= 0x80  a match of (c & 0x7F) + LZ_MIN bytes (3..130), then its distance - 1 as a u16
 *              (little endian), copied a byte at a time: a distance shorter than the length
 *              repeats, so a run of one byte is a literal and a match at distance 1.
 *
 * The compressor writes through a sink (a run of bytes at a time), so its output can go to the
 * flash objects one chunk at a time, and reads its input in place. It remembers the last position
 * of each 3-byte hash (lz_head, 4 KB) and tries the byte before too (runs). The decompressor reads
 * a byte at a time from a source and writes into place. No allocation; inputs under 64 KB. */
#include <stdint.h>

#define LZ_MIN 3u
#define LZ_MAXLEN (0x7Fu + LZ_MIN)
#define LZ_MAXLIT 128u
#define LZ_HBITS 11
#define LZ_NONE 0xFFFFu

typedef struct {
    int (*put)(void *ctx, const uint8_t *p, uint32_t n);   /* 0 = taken; else the compression stops */
    void *ctx;
} lz_sink_t;

typedef struct {
    int (*get)(void *ctx);                                  /* the next byte, -1 = none left */
    void *ctx;
} lz_src_t;

static uint16_t lz_head[1u << LZ_HBITS];

static uint32_t lz_hash(const uint8_t *p)
{
    return (((uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]) * 2654435761u) >> (32 - LZ_HBITS);
}

static uint32_t lz_match_len(const uint8_t *in, uint32_t n, uint32_t cand, uint32_t i)
{
    uint32_t len = 0;
    while (len < LZ_MAXLEN && i + len < n && in[cand + len] == in[i + len])
        len++;
    return len;
}

static int lz_literals(lz_sink_t *o, const uint8_t *p, uint32_t n)
{
    while (n) {
        uint32_t k = n > LZ_MAXLIT ? LZ_MAXLIT : n;
        uint8_t c = (uint8_t)(k - 1u);
        if (o->put(o->ctx, &c, 1) || o->put(o->ctx, p, k))
            return -1;
        p += k;
        n -= k;
    }
    return 0;
}

/* 0 = all of in[0..n) went to the sink; -1 = the sink refused (or n is too big) */
static int lz_compress(const uint8_t *in, uint32_t n, lz_sink_t *o)
{
    uint32_t i = 0, lit = 0, k;
    if (n >= LZ_NONE)
        return -1;
    for (k = 0; k < (1u << LZ_HBITS); k++)
        lz_head[k] = LZ_NONE;
    while (i < n) {
        uint32_t best = 0, dist = 0;
        if (i + LZ_MIN <= n) {
            uint32_t h = lz_hash(in + i), c = lz_head[h], len;
            lz_head[h] = (uint16_t)i;
            if (c != LZ_NONE && (len = lz_match_len(in, n, c, i)) > best) {
                best = len;
                dist = i - c;
            }
            if (i && (len = lz_match_len(in, n, i - 1u, i)) > best) {   /* a run */
                best = len;
                dist = 1;
            }
        }
        if (best < LZ_MIN) {
            i++;
            continue;
        }
        if (lz_literals(o, in + lit, i - lit))
            return -1;
        {
            uint8_t m[3] = {(uint8_t)(0x80u | (best - LZ_MIN)), (uint8_t)(dist - 1u), (uint8_t)((dist - 1u) >> 8)};
            if (o->put(o->ctx, m, 3))
                return -1;
        }
        for (k = 1; k < best && i + k + LZ_MIN <= n; k++)       /* the positions inside the match */
            lz_head[lz_hash(in + i + k)] = (uint16_t)(i + k);
        i += best;
        lit = i;
    }
    return lz_literals(o, in + lit, i - lit);
}

/* 0 = exactly n bytes came out; -1 = the stream ended early or does not fit */
static int lz_decompress(lz_src_t *s, uint8_t *out, uint32_t n)
{
    uint32_t w = 0;
    while (w < n) {
        int c = s->get(s->ctx), b;
        uint32_t len, dist;
        if (c < 0)
            return -1;
        if (c < 0x80) {
            for (len = (uint32_t)c + 1u; len; len--) {
                if (w >= n || (b = s->get(s->ctx)) < 0)
                    return -1;
                out[w++] = (uint8_t)b;
            }
            continue;
        }
        len = ((uint32_t)c & 0x7Fu) + LZ_MIN;
        if ((b = s->get(s->ctx)) < 0)
            return -1;
        dist = (uint32_t)b;
        if ((b = s->get(s->ctx)) < 0)
            return -1;
        dist |= (uint32_t)b << 8;
        dist++;
        if (dist > w || len > n - w)
            return -1;
        for (; len; len--, w++)
            out[w] = out[w - dist];
    }
    return 0;
}
