/* SPDX-License-Identifier: GPL-3.0-only */
/* X0X master filter + limiter; see master.c. */
#pragma once
#include <stdint.h>
#include "x0x_param.h"

enum { MST_MODE, MST_CUTOFF, MST_RESO, MST_LIMIT, MST_NPARAMS };

typedef struct {
    uint8_t pot[MST_NPARAMS];
    float g, g_t, k;                  /* filter coefficient (gliding to g_t), damping */
    float ic1, ic2;                   /* TPT SVF state */
    float env, gain;                  /* limiter */
} master_t;

void master_init(master_t *m);
int master_nparams(void);
const x0x_param_t *master_param(int i);
void master_set(master_t *m, int i, int value);
int master_get(const master_t *m, int i);
/* in place, mono: filter, volume (0..1+), limiter */
void master_process(master_t *m, float *x, int n, float volume);
