/* SPDX-License-Identifier: GPL-3.0-only */
/* lz.c (the project's packed patterns, song and motion): every input comes back exactly, through
 * the sink and the source a byte at a time; a refusing sink stops it; a short or broken stream is
 * refused, never written past its end. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../firmware/src/app/lz.c"

#define MAXN 60000u
static uint8_t in[MAXN], packed[MAXN + MAXN / 64u + 16u], out[MAXN + 64u];
static uint32_t npacked, cap;

static int put(void *ctx, const uint8_t *p, uint32_t n)
{
    (void)ctx;
    if (npacked + n > cap)
        return -1;
    memcpy(packed + npacked, p, n);
    npacked += n;
    return 0;
}

typedef struct { uint32_t pos, n; } src_t;
static int get(void *ctx)
{
    src_t *s = ctx;
    return s->pos < s->n ? packed[s->pos++] : -1;
}

static uint32_t rng = 12345u;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static int bad;
static void roundtrip(const char *name, uint32_t n)
{
    lz_sink_t sink = {put, 0};
    src_t s = {0, 0};
    lz_src_t src = {get, &s};
    npacked = 0;
    cap = sizeof packed;
    if (lz_compress(in, n, &sink)) {
        printf("FAIL %s: compress refused\n", name);
        bad = 1;
        return;
    }
    s.n = npacked;
    memset(out, 0xA5, sizeof out);
    if (lz_decompress(&src, out, n) || memcmp(in, out, n) || out[n] != 0xA5) {
        printf("FAIL %s: %u bytes do not come back\n", name, n);
        bad = 1;
        return;
    }
    if (n && s.pos != npacked) {
        printf("FAIL %s: %u of %u packed bytes read\n", name, s.pos, npacked);
        bad = 1;
        return;
    }
    if (name[0])
        printf("  ok %-28s %6u -> %6u\n", name, n, npacked);
}

int main(void)
{
    uint32_t i, n;
    roundtrip("empty", 0);
    in[0] = 7;
    roundtrip("one byte", 1);
    memset(in, 0, MAXN);
    roundtrip("zeros", 30000);
    for (i = 0; i < MAXN; i++)
        in[i] = (uint8_t)rnd();
    roundtrip("random (incompressible)", 30000);
    roundtrip("random, near the limit", MAXN);
    for (n = 1; n < 400; n++) {                       /* every literal / match length boundary */
        for (i = 0; i < n; i++)
            in[i] = (uint8_t)(rnd() % 3u);
        npacked = 0;
        roundtrip(n == 399 ? "small alphabet 1..399" : "", n);
        if (bad)
            break;
    }
    for (i = 0; i < 30000; i++)                       /* a pattern-like layout: sparse rows, repeats */
        in[i] = (i % 528u) < 100u ? (uint8_t)((i * 7u) % 5u == 0 ? 0x11 : 0) : (uint8_t)(i % 528u > 400 ? 45 : 0);
    roundtrip("structured", 30000);
    {                                                 /* a sink that fills up stops the compressor */
        lz_sink_t sink = {put, 0};
        for (i = 0; i < 30000; i++)
            in[i] = (uint8_t)rnd();
        npacked = 0;
        cap = 1000;
        if (!lz_compress(in, 30000, &sink)) {
            printf("FAIL a full sink did not stop it\n");
            bad = 1;
        }
    }
    {                                                 /* a short stream: refused, nothing past the end */
        lz_sink_t sink = {put, 0};
        src_t s = {0, 0};
        lz_src_t src = {get, &s};
        for (i = 0; i < 5000; i++)
            in[i] = (uint8_t)(i / 50u);
        npacked = 0;
        cap = sizeof packed;
        lz_compress(in, 5000, &sink);
        s.n = npacked / 2u;
        memset(out, 0xA5, sizeof out);
        if (!lz_decompress(&src, out, 5000) || out[5000] != 0xA5) {
            printf("FAIL a short stream was taken\n");
            bad = 1;
        }
        s.pos = 0;                                    /* a distance before the start */
        s.n = 4;
        packed[0] = 0x80;
        packed[1] = 0x10;
        packed[2] = 0x00;
        packed[3] = 0x00;
        if (!lz_decompress(&src, out, 100)) {
            printf("FAIL a match before the start was taken\n");
            bad = 1;
        }
    }
    {                                                 /* an input too long for 16-bit positions: refused */
        lz_sink_t sink = {put, 0};
        npacked = 0;
        cap = sizeof packed;
        if (!lz_compress(in, 0xFFFFu, &sink)) {
            printf("FAIL 64 KB taken\n");
            bad = 1;
        }
    }
    printf(bad ? "LZ FAILED\n" : "lz: all ok\n");
    return bad;
}
