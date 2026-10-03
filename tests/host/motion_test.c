/* SPDX-License-Identifier: GPL-3.0-only */
/* X0X motion lanes (firmware/src/seq/motion.c): record a sweep, play it back, ramp between steps,
 * a gap filled, a single touch, the knob winning while it moves and for a pass, a pattern
 * change, stop, and a reused slot. */
#include <stdio.h>
#include <string.h>
#include "../../firmware/src/seq/motion.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static lane_t L[NLANE];
static motion_t M;
static int knob = 64, eng = 64, applies;   /* one parameter: the knob (base) and the engine's value */

static int base(void *x, int t, int v, int i) { (void)x; (void)t; (void)v; (void)i; return knob; }
static void apply(void *x, int t, int v, int i, int val) { (void)x; (void)t; (void)v; (void)i; eng = val; applies++; }

/* one pass of a 16-step part, `per` samples a step; with f, the knob is turned to f(step) before
 * each step, recording (lane k) */
static void pass(int pat, int per, int (*f)(int), int k)
{
    int s;
    for (s = 0; s < 16; s++) {
        if (f) {
            eng = knob = f(s);                         /* the knob turned: the engine has it at once */
            motion_req_rec(&M, k);
        }
        motion_tick(&M, (uint32_t)per / 2u);
        motion_step(&M, 0, pat, s, (s + 1) % 16, 16);
        motion_tick(&M, (uint32_t)per / 2u);
    }
}
static int sweep(int s) { return s * 8; }

int main(void)
{
    int k, k2, v;
    memset(L, 0, sizeof L);
    motion_init(&M, L);
    M.base = base;
    M.apply = apply;

    k = motion_alloc(L, 0, 0, 2, 0, 0);
    CHECK(k >= 0 && motion_count(L) == 1, "allocated");
    pass(0, 1000, 0, k);                               /* an empty lane plays the knob */
    CHECK(eng == 64, "an empty lane leaves the knob's value (%d)", eng);

    pass(0, 1000, sweep, k);                           /* record: a sweep over one pass */
    for (v = 0; v < 16; v++)
        CHECK(L[k].val[v] == v * 8, "recorded step %d = %d", v, L[k].val[v]);
    for (v = 0; v < MOT_GAP; v++)                      /* the gesture lasts MOT_GAP steps: the knob */
        motion_step(&M, 0, 0, v, v + 1, 16);
    CHECK(eng == 120, "just after the gesture the knob is heard (%d)", eng);

    knob = 5;                                          /* let go of the knob elsewhere */
    motion_tick(&M, 500);
    motion_step(&M, 0, 0, 0, 1, 16);
    CHECK(eng == 0, "playback: step 1 plays 0, not the knob (%d)", eng);
    motion_tick(&M, 500);
    CHECK(eng == 4, "ramp: halfway to step 2's 8 (%d)", eng);
    motion_tick(&M, 500);
    motion_step(&M, 0, 0, 1, 2, 16);
    CHECK(eng == 8, "step 2 (%d)", eng);

    L[k].val[2] = MOT_NONE;                            /* a step without a value: the knob */
    motion_step(&M, 0, 0, 2, 3, 16);
    CHECK(eng == 5, "a step with no value plays the knob (%d)", eng);

    motion_req_hold(&M, k);                            /* the knob wins for a pass */
    eng = knob = 99;
    for (v = 3; v < 16; v++) {
        motion_step(&M, 0, 0, v, (v + 1) % 16, 16);
        CHECK(eng == 99, "held: step %d plays the knob (%d)", v, eng);
    }
    for (v = 0; v < 3; v++)
        motion_step(&M, 0, 0, v, v + 1, 16);
    CHECK(eng == 99, "held: the knob's value through the pass (%d)", eng);
    motion_step(&M, 0, 0, 3, 4, 16);
    CHECK(eng == 24, "after the pass the lane again (%d)", eng);

    motion_step(&M, 0, 1, 4, 5, 16);                   /* the part changed pattern */
    CHECK(eng == 99, "another pattern: the knob (%d)", eng);
    motion_step(&M, 0, 0, 5, 6, 16);
    CHECK(eng == 40, "back on the pattern: the lane (%d)", eng);
    motion_step(&M, 1, 0, 6, 7, 16);
    CHECK(eng == 40, "another part's step does not touch it (%d)", eng);

    motion_release(&M);                                /* stop */
    CHECK(eng == 99 && motion_value(&M, k) < 0, "stop: the knob (%d)", eng);

    motion_step(&M, 0, 0, 6, 7, 16);
    CHECK(eng == 48, "playing again (%d)", eng);
    motion_clear(L, k);                                /* cleared while applying; the slot reused */
    k2 = motion_alloc(L, 0, 0, 2, 0, 1);
    CHECK(k2 == k, "the slot is reused");
    L[k2].val[7] = 48;                                 /* the stale runtime applied 48: must still apply */
    applies = 0;
    motion_step(&M, 0, 0, 7, 8, 16);
    CHECK(applies == 1 && eng == 48, "a reused slot starts fresh (applies %d)", applies);

    /* a gap: touches on steps 2 and 5 fill 3 and 4; a lone touch writes one step */
    k2 = motion_alloc(L, 2, 0, 2, 0, 2);
    eng = knob = 30;
    motion_req_rec(&M, k2);
    motion_step(&M, 0, 2, 2, 3, 16);
    motion_step(&M, 0, 2, 3, 4, 16);
    motion_step(&M, 0, 2, 4, 5, 16);
    eng = knob = 60;
    motion_req_rec(&M, k2);
    motion_step(&M, 0, 2, 5, 6, 16);
    CHECK(L[k2].val[2] == 30 && L[k2].val[3] == 40 && L[k2].val[4] == 50 && L[k2].val[5] == 60,
          "gap filled: %d %d %d %d", L[k2].val[2], L[k2].val[3], L[k2].val[4], L[k2].val[5]);
    for (v = 6; v < 6 + MOT_GAP + 2; v++)
        motion_step(&M, 0, 2, v, v + 1, 16);
    eng = knob = 90;
    motion_req_rec(&M, k2);
    motion_step(&M, 0, 2, 13, 14, 16);                 /* a new gesture: no line back to step 5 */
    CHECK(L[k2].val[12] == MOT_NONE && L[k2].val[13] == 90 && L[k2].val[6] == MOT_NONE,
          "a lone touch writes its step only (%d %d %d)", L[k2].val[12], L[k2].val[13], L[k2].val[6]);

    for (v = 0; v < NLANE; v++)                        /* the pool fills, and says so */
        if (motion_alloc(L, 3, 1, 0, v & 7, v >> 3) < 0)
            break;
    CHECK(v == NLANE - 2 && motion_alloc(L, 3, 1, 1, 0, 0) == -1, "pool of %d: full after %d more", NLANE, v);
    CHECK(motion_find(L, 3, 1, 0, 2, 1) >= 0 && motion_find(L, 4, 1, 0, 2, 1) < 0, "find keys on the pattern");
    printf("motion: %s\n", fails ? "FAIL" : "ok");
    return fails != 0;
}
