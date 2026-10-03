/* SPDX-License-Identifier: GPL-3.0-only */
/* X0X master section, after the send FX: compressor (with kick-keyed PUMP), resonant
 * filter, volume, limiter. See master.c. */
#pragma once
#include <stdint.h>
#include "x0x_param.h"

enum {
    MST_THRESH, MST_RATIO, MST_ATTACK, MST_RELEASE,     /* page COMP */
    MST_MAKEUP, MST_MIX, MST_PUMP, MST_PUMPSRC,         /* page COMP 2 */
    MST_MODE, MST_CUTOFF, MST_RESO, MST_LIMIT,          /* page FILTER (+ the limiter switch) */
    MST_NPARAMS
};
enum { PUMP_909, PUMP_808, PUMP_BOTH };

typedef struct {
    uint8_t pot[MST_NPARAMS];
    /* compressor */
    float thr_db, slope, knee;        /* static curve: slope = 1 - 1/ratio; knee width, dB */
    float a_att, a_rel;               /* one-pole coefficients of the gain-reduction smoother */
    float makeup, mix;                /* linear makeup gain, wet share */
    float gr;                         /* smoothed gain reduction, dB (>= 0) */
    float ms, a_det;                  /* the level it reads: mean square over ~8 ms (not each sample) */
    float pump_db, pump, pump_tgt;    /* PUMP depth; its envelope (dB) and the target it rises to */
    float a_pump;                     /* PUMP rise: MASTER_PUMP_RISE_MS */
    uint8_t comp_on;                  /* ratio > 1:1 or PUMP: else bypassed, sample for sample */
    float gr_view;                    /* gain reduction for the screen, dB, peak-held */
    /* filter */
    float g, g_t, k;
    float ic1, ic2;
    /* limiter: looks MST_LA samples ahead, so it can lower the gain before a peak arrives */
#define MST_LA 64
    float env, gain;
    float la[MST_LA];
    int la_pos, hold;
} master_t;

void master_init(master_t *m);
int master_nparams(void);
const x0x_param_t *master_param(int i);
void master_set(master_t *m, int i, int value);
int master_get(const master_t *m, int i);
/* a kick was triggered (kit 0 = 909, 1 = 808; vel 0..1): the PUMP's key */
void master_key(master_t *m, int kit, float vel);
/* in place, mono: compressor, filter, volume (0..1+), limiter */
void master_process(master_t *m, float *x, int n, float volume);
/* the value of a pot in its unit, for the screen ("-12 dB", "4:1", "30 ms") */
void master_format(const master_t *m, int i, char *buf);
