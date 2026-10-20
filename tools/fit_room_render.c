/* SPDX-License-Identifier: GPL-3.0-only */
/* The reverb's response to a 20 ms white-noise burst at RMS 0.6 (the 909 snare's send level), wet
 * only, through this firmware's fxbus (dsp/fxbus.c, included whole for ROOM's fx_room_times), for
 * tools/fit_room.py:
 *   fit_room_render classic DECAY TONE
 *   fit_room_render room DECAY TONE                  ROOM as the knobs set it
 *   fit_room_render room DECAY TONE T_DC T_NY GAIN   ROOM at these decay times (s) and output gain
 * Writes to stdout: the length n (int32), then n samples of L and n of R (float32). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define X0X_DRUM_TABLES_DEFINE              /* no drum909.c in this link: the tables are defined here */
#include "dsp/fxbus.c"

#define LEN 131072                          /* 3 s */
#define BURST 882                           /* 20 ms */

static fxbus_t F;
static int16_t dly[88200];
static float out_l[LEN], out_r[LEN];

int main(int argc, char **argv)
{
    if (argc != 4 && !(argc == 7 && !strcmp(argv[1], "room"))) {
        fprintf(stderr, "usage: fit_room_render classic|room DECAY TONE [T_DC T_NY GAIN]\n");
        return 2;
    }
    fxbus_init(&F, dly, 88200);
    fxbus_set(&F, FX_DIST, 0);
    fxbus_set(&F, FX_COMP, 0);
    fxbus_set(&F, FX_VOLUME, 127);
    fxbus_set(&F, FX_RV_DECAY, atoi(argv[2]));
    fxbus_set(&F, FX_RV_TONE, atoi(argv[3]));
    fxbus_set(&F, FX_RV_TYPE, strcmp(argv[1], "room") ? FX_RV_CLASSIC : FX_RV_ROOM);
    if (argc == 7) {
        fx_room_times(fx_rm(&F), (float)atof(argv[4]), (float)atof(argv[5]));
        fx_rm(&F)->out_gain = (float)atof(argv[6]);
    }
    static const float zero[256];
    uint32_t rng = 12345u;
    for (int b = 0; b < LEN; b += 256) {
        float in[256];
        for (int i = 0; i < 256; i++) {
            rng = rng * 1664525u + 1013904223u;
            in[i] = b + i < BURST ? 0.6f * 1.7320508f * ((float)(rng >> 8) / 8388608.0f - 1.0f) : 0.0f;
        }
        fxbus_process_st(&F, zero, zero, in, zero, out_l + b, out_r + b, 256);
    }
    const int32_t n = LEN;
    fwrite(&n, 4, 1, stdout);
    fwrite(out_l, 4, LEN, stdout);
    fwrite(out_r, 4, LEN, stdout);
    return 0;
}
