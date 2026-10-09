/* SPDX-License-Identifier: GPL-3.0-only */
/* Undo and redo by difference.
 *
 * The undoable state is one region of bytes (the project after its settings). A shadow copy holds
 * it as it was at the last commit; a commit (the end of a gesture: ui.c decides when) compares the
 * two and stores the runs of bytes that changed, as one entry, with their previous contents. Undo
 * swaps an entry's bytes with the region's, so the entry then holds what undo took away, and redo
 * swaps them back: one copy serves both directions. Every edit is undoable without any code at the
 * place that makes it.
 *
 * Entries live in a byte pool, oldest first; a new one drops the oldest until it fits, and drops
 * whatever was undone and not redone. An entry bigger than the whole pool cannot be kept: the
 * history is emptied (undo_commit says so).
 *
 * Entry: u16 run count, u16 first changed offset, u16 last, u16 pad, then per run u16 offset,
 * u16 length and its bytes, padded to 2. */
#include <stdint.h>

#ifndef UNDO_POOL
#define UNDO_POOL 20480u                 /* X0X 0.11: 4 KB less, for the 64-step project's shadow (pool headroom) */
#endif
#define UNDO_MAX 32
#define UNDO_GAP 8u                    /* unchanged bytes between changes cheaper to keep than a new run */

typedef struct {
    uint8_t *region, *shadow;
    uint32_t len;
    uint8_t pool[UNDO_POOL];
    uint32_t used;
    uint32_t at[UNDO_MAX + 1];         /* entry k starts at pool[at[k]]; at[n] == used */
    int n, cur;                        /* entries kept; how many of them are applied */
} undo_t;

static uint16_t ud_get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void ud_put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* shadow must have room for len bytes; the region as it is now is the starting point */
static void undo_init(undo_t *u, void *region, void *shadow, uint32_t len)
{
    uint32_t i;
    u->region = region;
    u->shadow = shadow;
    u->len = len;
    for (i = 0; i < len; i++)
        u->shadow[i] = u->region[i];
    u->used = 0;
    u->n = u->cur = 0;
    u->at[0] = 0;
}

/* forget the history and take the region as it is (a load, a factory reset kept as is) */
static void undo_reset(undo_t *u) { undo_init(u, u->region, u->shadow, u->len); }

static void ud_drop_oldest(undo_t *u)
{
    uint32_t sz = u->at[1], i;
    int k;
    for (i = sz; i < u->used; i++)
        u->pool[i - sz] = u->pool[i];
    u->used -= sz;
    for (k = 0; k < u->n; k++)
        u->at[k] = u->at[k + 1] - sz;
    u->n--;
    u->cur--;
}

/* the changes since the last commit, as one entry. 1 = an entry was made, 0 = nothing changed,
 * -1 = too big to keep (the history is emptied; the region is the new starting point) */
static int undo_commit(undo_t *u)
{
    uint32_t i = 0, sz = 8, runs = 0, lo = 0, hi = 0;
    uint8_t *e;
    /* first pass: the runs and the entry's size */
    while (i < u->len) {
        uint32_t s, end, q;
        if (u->region[i] == u->shadow[i]) {
            i++;
            continue;
        }
        s = end = i;
        while (end + 1u < u->len) {                       /* extend over short gaps */
            uint32_t nxt = end + 1u, lim = end + 1u + UNDO_GAP, found = 0;
            for (q = nxt; q < u->len && q < lim; q++)
                if (u->region[q] != u->shadow[q]) {
                    found = q;
                    break;
                }
            if (!found)
                break;
            end = found;
        }
        if (!runs)
            lo = s;
        hi = end;
        runs++;
        sz += 4u + ((end - s + 1u + 1u) & ~1u);
        i = end + 1u;
    }
    if (!runs)
        return 0;
    u->n = u->cur;                                        /* what was undone is gone */
    u->used = u->at[u->n];
    if (sz > UNDO_POOL) {
        undo_reset(u);
        return -1;
    }
    while (u->n > 0 && (u->n >= UNDO_MAX || u->used + sz > UNDO_POOL))
        ud_drop_oldest(u);
    e = u->pool + u->used;
    ud_put16(e, runs);
    ud_put16(e + 2, lo);
    ud_put16(e + 4, hi);
    ud_put16(e + 6, 0);
    e += 8;
    /* second pass, the same runs: the previous bytes into the entry, the shadow brought up to date */
    i = lo;
    while (i <= hi) {
        uint32_t s, end, q, k;
        if (u->region[i] == u->shadow[i]) {
            i++;
            continue;
        }
        s = end = i;
        while (end + 1u < u->len) {
            uint32_t nxt = end + 1u, lim = end + 1u + UNDO_GAP, found = 0;
            for (q = nxt; q < u->len && q < lim; q++)
                if (u->region[q] != u->shadow[q]) {
                    found = q;
                    break;
                }
            if (!found)
                break;
            end = found;
        }
        ud_put16(e, s);
        ud_put16(e + 2, end - s + 1u);
        e += 4;
        for (k = s; k <= end; k++) {
            *e++ = u->shadow[k];
            u->shadow[k] = u->region[k];
        }
        if ((end - s + 1u) & 1u)
            *e++ = 0;
        i = end + 1u;
    }
    u->used += sz;
    u->n++;
    u->cur = u->n;
    u->at[u->n] = u->used;
    return 1;
}

/* swap entry k with the region; changed(off, len) for every run, after the swap */
static void ud_swap(undo_t *u, int k, void (*changed)(uint32_t off, uint32_t len))
{
    uint8_t *e = u->pool + u->at[k];
    uint32_t runs = ud_get16(e), r;
    e += 8;
    for (r = 0; r < runs; r++) {
        uint32_t off = ud_get16(e), n = ud_get16(e + 2), j;
        e += 4;
        for (j = 0; j < n; j++) {
            uint8_t t = u->region[off + j];
            u->region[off + j] = e[j];
            u->shadow[off + j] = e[j];
            e[j] = t;
        }
        e += (n + 1u) & ~1u;
        if (changed)
            changed(off, n);
    }
}

/* the span an entry touches (for naming it) */
static void undo_span(const undo_t *u, int k, uint32_t *lo, uint32_t *hi)
{
    *lo = ud_get16(u->pool + u->at[k] + 2);
    *hi = ud_get16(u->pool + u->at[k] + 4);
}

/* 1 = done (lo / hi: what it touched), 0 = nothing to undo / redo. Commit first: an edit that has
 * not been committed yet is not in the history. */
static int undo_undo(undo_t *u, void (*changed)(uint32_t, uint32_t), uint32_t *lo, uint32_t *hi)
{
    if (u->cur == 0)
        return 0;
    u->cur--;
    undo_span(u, u->cur, lo, hi);
    ud_swap(u, u->cur, changed);
    return 1;
}

static int undo_redo(undo_t *u, void (*changed)(uint32_t, uint32_t), uint32_t *lo, uint32_t *hi)
{
    if (u->cur >= u->n)
        return 0;
    undo_span(u, u->cur, lo, hi);
    ud_swap(u, u->cur, changed);
    u->cur++;
    return 1;
}
