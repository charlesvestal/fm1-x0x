/* SPDX-License-Identifier: GPL-3.0-only */
/* hal/fm1_input.h's quadrature decoder (fm1__frame) on simulated turns: encoder 0, a full
 * quadrature cycle per detent. A turn must count every detent whether the matrix scan sees each
 * state for 4 scans, 2 or 1, or skips every other state (a fast flick); a one-scan glitch at rest
 * must count nothing. (The scan runs every 1.1 ms: one scan a state is ~225 detents a second.) */
#include <stdio.h>
#include <string.h>
#include "fm1_input.h"

static int fails;
static void frame(uint32_t st)                       /* encoder 0: A = col 0 bit 0, B = col 1 bit 0 */
{
    fm1_in.raw[0] = (uint8_t)((st >> 1) & 1u);
    fm1_in.raw[1] = (uint8_t)(st & 1u);
    fm1__frame();
}
static const uint32_t CW[4] = {1, 3, 2, 0};          /* 0 -> 1 -> 3 -> 2 -> 0: one detent clockwise */

static void reset(void)
{
    int i;
    memset((void *)&fm1_in, 0, sizeof fm1_in);
    for (i = 0; i < (int)FM1_NENC; i++)
        fm1_in.enc_prev[i] = fm1_in.enc_last[i] = 0xFF;
    for (i = 0; i < 60; i++)                         /* at rest on state 0: learned as the detent */
        frame(0);
}

/* `det` detents, `per` scans on each state; skip = 1: only every other state is ever scanned */
static int turn(int det, int per, int skip, int dir)
{
    int d, k, p, steps;
    fm1_in.enc_steps[0] = 0;
    for (d = 0; d < det; d++)
        for (k = skip ? 1 : 0; k < 4; k += skip ? 2 : 1)
            for (p = 0; p < per; p++)
                frame(dir > 0 ? CW[k] : CW[(5 - k) & 3] ^ 0);
    for (k = 0; k < 60; k++)
        frame(0);
    steps = fm1_in.enc_steps[0];
    fm1_in.enc_steps[0] = 0;
    return steps;
}

#define CHECK(c, ...) do { if (!(c)) { printf("FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)
int main(void)
{
    int per, got;
    static const uint32_t CCW[4] = {2, 3, 1, 0};
    for (per = 4; per >= 1; per /= 2) {
        reset();
        got = turn(10, per, 0, 1);
        CHECK(got == 10, "10 detents clockwise, %d scans a state: %d", per, got);
    }
    /* a flick: it starts like any turn (one detent seen whole), then skips every other state;
     * from a standstill straight into skipping, the direction is unknowable and nothing counts */
    {
        int k, d;
        reset();
        for (k = 0; k < 4; k++)
            frame(CW[k]);
        for (d = 0; d < 9; d++)
            for (k = 1; k < 4; k += 2)
                frame(CW[k]);
        for (k = 0; k < 60; k++)
            frame(0);
        got = fm1_in.enc_steps[0];
        CHECK(got == 10, "a flick (1 detent whole, then 9 with every other state skipped): %d", got);
    }
    /* counter-clockwise, one scan a state */
    {
        int d, k;
        reset();
        for (d = 0; d < 10; d++)
            for (k = 0; k < 4; k++)
                frame(CCW[k]);
        for (k = 0; k < 60; k++)
            frame(0);
        got = fm1_in.enc_steps[0];
        CHECK(got == -10, "10 detents counter-clockwise, one scan a state: %d", got);
    }
    /* a bounce at rest: one scan of state 1, back to 0 */
    reset();
    frame(1);
    frame(0);
    {
        int k;
        for (k = 0; k < 60; k++)
            frame(0);
    }
    CHECK(fm1_in.enc_steps[0] == 0, "a one-scan glitch at rest counted %d", fm1_in.enc_steps[0]);
    /* a slow turn that pauses halfway through a click (state 1, ~40 ms), then finishes it, and five
     * more: six steps. The pause must not become the knob's detent (it did: the next clicks then
     * landed between detents and some were lost) */
    reset();
    {
        int k, p;
        for (p = 0; p < 70; p++)
            frame(1);
        for (k = 1; k < 4; k++)
            for (p = 0; p < 4; p++)
                frame(CW[k]);
        for (p = 0; p < 60; p++)
            frame(0);
        got = fm1_in.enc_steps[0];
        got += turn(5, 4, 0, 1);
    }
    CHECK(got == 6, "a pause halfway through a click, then 5 more: %d (6)", got);
    printf(fails ? "encoder: %d FAILED\n" : "encoder: ok\n", fails);
    return fails != 0;
}
