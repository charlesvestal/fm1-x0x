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
    float want;                       /* the static curve's reduction, dB: read every MST_CR samples */
    float g_lin, g_step;              /* makeup x the reduction, linear: set every MST_CR samples, ramped */
    uint8_t cr;                       /* samples to the next control-rate update */
#define MST_CR 4
    float pump_db, pump, pump_tgt;    /* PUMP depth; its envelope (dB) and the target it rises to */
    float a_pump;                     /* PUMP rise: MASTER_PUMP_RISE_MS */
    uint8_t comp_on;                  /* ratio > 1:1 or PUMP: else bypassed, sample for sample */
    float gr_view;                    /* gain reduction for the screen, dB, peak-held */
    /* filter */
    float g, g_t, k;
    float a1;                         /* 1 / (1 + g (g + k)): recomputed only while g moves */
    float ic1, ic2;
    float ic1r, ic2r;                 /* X0X stereo: the right side's filter state */
    float vol_cur;                    /* X0X: master_process_st's volume, gliding to the knob (< 0: unset) */
    /* limiter: looks MST_LA samples ahead, so it can lower the gain before a peak arrives */
#define MST_LA 64
    float env, gain, inv_env;         /* inv_env = 1 / env, kept up as env moves (no divide a sample) */
    float la[MST_LA], la_r[MST_LA];
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
/* X0X stereo: the compressor reads both sides' power, the limiter the louder side, and each moves
 * both together; the filter runs on each. With l == r it is master_process exactly. */
void master_process_st(master_t *m, float *l, float *r, int n, float volume);
/* the value of a pot in its unit, for the screen ("-12 dB", "4:1", "30 ms") */
void master_format(const master_t *m, int i, char *buf);
