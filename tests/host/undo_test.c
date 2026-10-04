/* SPDX-License-Identifier: GPL-3.0-only */
/* firmware/src/app/undo.c: random edits committed one by one, then undone to the start and redone
 * to the end, checked against a snapshot of every step; a small pool drops the oldest; an edit
 * larger than the pool empties the history; a new edit after an undo drops the redo. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define UNDO_POOL 1500u
#include "../../firmware/src/app/undo.c"

#define LEN 2000
#define STEPS 40
static uint8_t region[LEN], shadow[LEN], snap[STEPS + 1][LEN];
static undo_t u;
static uint32_t seed = 1;
static uint32_t rnd(void) { seed = seed * 1664525u + 1013904223u; return seed >> 8; }
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static void edit(void)
{
    int k, n = 1 + (int)(rnd() % 6);
    for (k = 0; k < n; k++) {
        uint32_t at = rnd() % LEN, len = 1 + rnd() % 24, j;
        for (j = 0; j < len && at + j < LEN; j++)
            region[at + j] = (uint8_t)rnd();
    }
}

int main(void)
{
    int i, kept, lo_step;
    uint32_t lo, hi;
    for (i = 0; i < LEN; i++)
        region[i] = (uint8_t)rnd();
    undo_init(&u, region, shadow, LEN);
    CHECK(undo_commit(&u) == 0, "no change, no entry");
    memcpy(snap[0], region, LEN);
    for (i = 1; i <= STEPS; i++) {
        edit();
        CHECK(undo_commit(&u) == 1, "step %d committed", i);
        memcpy(snap[i], region, LEN);
    }
    kept = u.n;
    CHECK(kept > 0 && kept <= UNDO_MAX, "kept %d", kept);
    lo_step = STEPS - kept;                       /* the oldest state still reachable */
    for (i = STEPS; i > lo_step; i--) {
        CHECK(undo_undo(&u, 0, &lo, &hi) == 1, "undo %d", i);
        CHECK(!memcmp(region, snap[i - 1], LEN), "undo to step %d", i - 1);
        CHECK(!memcmp(shadow, region, LEN), "shadow follows undo");
    }
    CHECK(undo_undo(&u, 0, &lo, &hi) == 0, "nothing more to undo");
    for (i = lo_step + 1; i <= STEPS; i++) {
        CHECK(undo_redo(&u, 0, &lo, &hi) == 1, "redo %d", i);
        CHECK(!memcmp(region, snap[i], LEN), "redo to step %d", i);
    }
    CHECK(undo_redo(&u, 0, &lo, &hi) == 0, "nothing more to redo");
    /* undo two, edit: the two are gone */
    undo_undo(&u, 0, &lo, &hi);
    undo_undo(&u, 0, &lo, &hi);
    region[5] ^= 0xFF; /* (a known one-byte edit) */
    CHECK(undo_commit(&u) == 1, "edit after undo");
    CHECK(undo_redo(&u, 0, &lo, &hi) == 0, "no redo after a new edit");
    CHECK(undo_undo(&u, 0, &lo, &hi) == 1 && !memcmp(region, snap[STEPS - 2], LEN), "undo the new edit");
    CHECK(lo == 5 && hi == 5, "span of a one-byte edit: %u..%u", lo, hi);
    /* too big for the pool: history emptied, and the region is the new start */
    for (i = 0; i < LEN; i++)
        region[i] ^= 0x5A;
    CHECK(undo_commit(&u) == -1, "too big");
    CHECK(undo_undo(&u, 0, &lo, &hi) == 0, "history emptied");
    CHECK(!memcmp(shadow, region, LEN), "shadow is the new start");
    printf(fails ? "undo: %d FAILED\n" : "undo: ok (%d of %d steps kept in %u bytes)\n", fails ? fails : kept, STEPS, UNDO_POOL);
    return fails != 0;
}
