/* SPDX-License-Identifier: GPL-3.0-only */
/* X0X send FX and master, shared by the drum and bass parts. A port of 9W9's
 * (GPL-3.0) send reverb, tempo-synced delay and master stage:
 *
 *   out = volume * glue( dist( dry + reverb(rev) + delay(dly) ) )
 *
 *  - Reverb: four combs + two allpasses, the comb loop quantised to 12 bits
 *    (the early-rack grain), damping in the loop, HPF on the send.
 *  - Delay: a note division of the tempo, slewed so time changes warp instead of
 *    clicking, feedback through a darkening one-pole, 12-bit write, HPF on the
 *    send. TYPE = DIGI is 9W9's delay exactly. TYPE = TAPE (an X0X addition, for
 *    the 303) reads the same line with wow and flutter, saturates inside the
 *    loop, darkens the loop further and lets feedback run slightly past unity
 *    into self-oscillation that the saturation holds; WEAR scales all of it.
 *  - Master: 9W9's seven distortion characters (or Off) and its one-knob glue
 *    compressor (hard bypass at 0), then volume.
 *
 * The delay line is int16 (9W9 writes it at 12 bits, so nothing is lost; values
 * past +-16 full scale saturate) and is passed in by the caller (.pool). The
 * reverb lines are int16 combs (also 12-bit in 9W9) and float allpasses, inside
 * the struct. Both effects go idle (no per-sample work) once their input and
 * their lines are silent. */
#pragma once
#include <stdint.h>
#include "x0x_param.h"
#include "drum909_dsp.h"

#define FX_RV_COMB_TOTAL (1116 + 1188 + 1277 + 1356)
#define FX_RV_AP_TOTAL (556 + 441)

enum { FX_VOLUME, FX_DIST, FX_DRIVE, FX_COMP,
       FX_RV_DECAY, FX_RV_TONE, FX_RV_HPF, FX_RV_LEVEL,
       FX_DL_TIME, FX_DL_FDBK, FX_DL_TONE, FX_DL_LEVEL,
       FX_DL_HPF, FX_DL_TYPE, FX_DL_WEAR, FX_NPARAMS };

struct fxbus {
    /* reverb */
    float rv_decay, rv_tone, rv_level;
    d9_biquad_t rv_hp;
    int16_t comb[FX_RV_COMB_TOTAL];
    float ap[FX_RV_AP_TOTAL];
    int32_t cpos[4], apos[2];
    float cdmp[4];
    int32_t rv_quiet;
    /* delay */
    float dl_time_ms, dl_fdbk, dl_tone, dl_level, bpm, wear;
    int32_t dl_div, dl_type;
    d9_biquad_t dl_hp;
    int16_t *dbuf;
    int32_t dlen, w, dl_quiet;
    float dcur, lp, tape_hp;
    uint32_t wow_ph, flut_ph;
    /* master */
    float drive, volume, comp;
    int32_t dist_mode;
    d9_shape_t shape;
    float crush_st[2];
    float comp_env_db, comp_det, c_thr, c_slope, c_makeup, c_drel, c_atk, c_rel;
    uint8_t pots[FX_NPARAMS];
};
typedef struct fxbus fxbus_t;

void fxbus_init(fxbus_t *f, int16_t *dly_buf, int dly_len);
void fxbus_set_bpm(fxbus_t *f, float bpm);
void fxbus_process(fxbus_t *f, const float *dry, const float *rev, const float *dly, float *out, int n);
int fxbus_nparams(void);
const x0x_param_t *fxbus_param(int i);
void fxbus_set(fxbus_t *f, int i, int value);
int fxbus_get(const fxbus_t *f, int i);
