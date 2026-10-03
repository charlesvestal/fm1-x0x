/* SPDX-License-Identifier: GPL-3.0-only */
/* X0X 808 drum part: the 8W8 TR-808 engine (Charles Vestal and contributors,
 * GPL-3.0; the rim shot is Yoshinosuke Horiuchi's sc808, MIT) ported to the FM-1.
 *
 * Same circuit models, pot tables, defaults, kit trims, velocity law and hat
 * choke as 8W8's sc808_engine.cpp. What is NOT here: 8W8's own reverb, delay,
 * master drive / glue and note map (the firmware's fxbus does the sends and the
 * master), its mutes (the sequencer's business) and its state blob.
 *
 * ELEVEN TRACKS, sixteen sounds, the way the TR-808 itself does it: LT/MT/HT
 * each switch Tom / Conga, RS switches Rim / Claves and CP switches Clap /
 * Maracas. Every one of the sixteen sounds keeps its own pots (a conga keeps its
 * conga decay when the switch goes back to tom), and a track's knobs edit the
 * sound its switch selects. CH cuts OH (kit Choke switch, as 8W8).
 *
 * Mono, 44.1 kHz, C99, float, no libm, no allocation. Idle voices cost nothing;
 * the shared metal bank (six Schmitt squares feeding CB, CH, OH and CY) runs
 * only while one of those four is sounding.
 *
 * Threading: drum808_trigger / drum808_set are called between render blocks. */
#pragma once
#include <stdint.h>
#include "x0x_param.h"

enum { D8_BD, D8_SD, D8_LT, D8_MT, D8_HT, D8_RS, D8_CP, D8_CB, D8_CY, D8_OH, D8_CH, D8_NUM, D8_KIT = D8_NUM };

/* The velocity an UNACCENTED 808 hit corresponds to: 8W8's velocity line puts
 * the 4 V trigger floor (gain 1.0) at vel 1/1.992 with the kit's Accent pot at
 * its default. vel 1.0 is the accented hit (7.3 V). */
#define D8_VEL_NORMAL 0.502f

#define D8_MAX_BLOCK 256

/* the sixteen sounds, 8W8's lane order */
enum { D8S_BD, D8S_SD, D8S_LT, D8S_MT, D8S_HT, D8S_LC, D8S_MC, D8S_HC,
       D8S_RS, D8S_CL, D8S_MA, D8S_CP, D8S_CB, D8S_CH, D8S_OH, D8S_CY, D8S_NUM };
/* per-sound pot slots */
enum { D8P_LEVEL, D8P_TUNE, D8P_DECAY, D8P_DRIVE, D8P_DIST, D8P_REV, D8P_DLY,
       D8P_X1, D8P_X2, D8P_NUM };   /* X1/X2: BD tone/attack, SD snappy/tone, MA attack */

/* ---- DSP primitives ---------------------------------------------------- */
/* Biquads are transposed direct form II with the denominator stored as its
 * distance from (1 - z^-1)^2: a1 = -2 + e1, a2 = 1 + e2. At a 50 Hz resonance
 * a1 is -1.9999x, and a float a1 keeps only five of those digits; e1 and e2 are
 * computed from sin(w/2) directly and keep all of theirs. */
typedef struct { float b0, e1, e2, z1, z2; } d8_bp_t;               /* constant-peak bandpass */
typedef struct { float n0, n1, n2, e1, e2, z1, z2; } d8_bq_t;       /* general biquad */
typedef struct { float z[3]; } d8_bq3_t;                            /* Werner's 3rd-order BP (fixed coefs) */
typedef struct { float a, z, y; } d8_hp1_t;                         /* one-pole highpass */
typedef struct { uint32_t s1, s2, s3; } d8_rng_t;                   /* SuperCollider taus88 */

typedef struct {                    /* SuperCollider EnvGen.kr, two curve segments */
    float level, grow, a2, b1, prev, cur, start;
    float end[2], dur[2], curve[2];
    int32_t count, seg, counter, phase;
    uint8_t linear, done;
} d8_env_t;

/* ---- voices ------------------------------------------------------------- */
typedef struct {
    float f0, loop_g, forward, tune_trim, atk_amt, atk_env, accent_v;
    float fb, tone_c, tone_z, dc_z, sigh_env, shp;
    d8_bp_t bt;
    int32_t gate, coef_age, quiet;
    uint8_t active;
} d8_bd_t;

typedef struct {
    float mix1, mix2, accent_v, noise_env, noise_decay, shp;
    d8_bp_t bt1, bt2;
    d8_bq_t noise_hp, out_hp;
    d8_rng_t rng;
    int32_t gate, quiet;
    uint8_t active;
} d8_sd_t;

typedef struct {                    /* one tom/conga channel; the switch is the mode */
    float f0, loop_g, accent_v, fwd, pitch_env, skin_env, skin_lp_c, skin_lp_z;
    float strike_lp_c, strike_lp_z, fb, dc_z, shp;
    d8_bp_t bt;
    d8_rng_t rng;
    int32_t gate, coef_age, quiet;
    uint8_t active, mode;
} d8_tom_t;

typedef struct {
    float g, fb, accent_v, shp;
    d8_bp_t bt;
    int32_t gate, quiet;
    uint8_t active;
} d8_clave_t;

typedef struct {
    float level, atk_c, dec_c, peak;
    d8_hp1_t hpa, hpb;
    d8_rng_t rng;
    int32_t quiet;
    uint8_t rising, active;
} d8_ma_t;

typedef struct {                    /* the sc808 rim shot */
    d8_env_t env;
    float tri_ph, tri_inc;
    uint32_t pul_ph, pul_inc;
    d8_bq_t peak, hp, lp;
    d8_rng_t rng;
    int32_t quiet;
    uint8_t pul_wrap, active;
} d8_rim_t;

typedef struct {
    float tooth, tooth_d, main_level, main_d, main_target, main_atk, floor_level, floor_d;
    d8_bp_t bp;
    d8_bq_t hp;
    d8_rng_t rng;
    int32_t spread, fired, age, main_open, quiet;
    uint8_t active;
} d8_cp_t;

typedef struct {                    /* six Schmitt squares, 0.32 fixed-point phases */
    uint32_t ph[6], inc[6];
    float drift[6], jm1[6];         /* jitter is 1 + jm1 */
    d8_rng_t rng;
    int32_t drift_cnt;
    uint8_t pend_wrap;              /* phases that start at >= 1.0 in 8W8 */
} d8_bank_t;

typedef struct { float v, target, up, down; } d8_menv_t;

typedef struct {
    d8_bq3_t bp1, bp2;
    d8_bp_t sus1, sus2, body_res;
    d8_bq_t body_sk, top_hp, out_hp;
    d8_hp1_t body_hpa, crash_hp, dc;
    float body_lp[2], crash_lp[2], floor_lp[2], top_lp;
    float e1, e2, e3, e1t, d1, d2, d3, atk_c, e1_base;
    int32_t e1_n, quiet;
    uint8_t e1_ramp, active;
} d8_cy_t;

typedef struct {
    d8_bq_t ska, skb;
    d8_hp1_t dc;
    d8_menv_t env;
    float lp_c, lp[5], d1, d2, out_scale;
    float lin_level, lin_step, lin_target, lin_atk_c, lin_base;
    int32_t lin_hold, lin_n, quiet;
    uint8_t mode, linear, ramp, active;
} d8_hat_t;

typedef struct {
    d8_bp_t bp;
    d8_hp1_t hp, bla, blb;
    float clank, body, clank_d, body_d, ratio;
    int32_t quiet;
    uint8_t active;
} d8_cb_t;

/* one output lane: 8W8's per-lane gain, choke fade and Crush state */
typedef struct {
    float hit, cg, cstep, crush[2];
    uint8_t snd;
} d8_lane_t;

typedef struct {                    /* a sound's drive stage, resolved from its pots */
    float drive, k, mk, dn, bm, steps, hold;
} d8_shp_t;

struct drum808 {
    uint8_t pot[D8S_NUM][D8P_NUM];
    uint8_t sw[D8_NUM];               /* LT/MT/HT tom|conga, RS rim|clave, CP clap|maracas */
    uint8_t kpot[3];                  /* kit: level, accent, choke */
    float potv[D8S_NUM][D8P_NUM];     /* engineering values of pot[] */
    d8_shp_t shp[D8S_NUM];
    float vol, vel_depth;
    int32_t choke;

    d8_bd_t bd;
    d8_sd_t sd;
    d8_tom_t tom[3];
    d8_rim_t rs;
    d8_clave_t cl;
    d8_cp_t cp;
    d8_ma_t ma;
    d8_bank_t bank;
    d8_cb_t cb;
    d8_hat_t ch, oh;
    d8_cy_t cy;
    d8_lane_t lane[13];
};
typedef struct drum808 drum808_t;

void drum808_init(drum808_t *d);                          /* 8W8 defaults */
void drum808_trigger(drum808_t *d, int track, float vel); /* vel 0..1, 1.0 = accent */
void drum808_render(drum808_t *d, float *dry, float *rev, float *dly, int n);
     /* ADDS this block (mono, n <= 256) into the dry bus and the reverb / delay send buses */
int drum808_nparams(int track);                           /* D8_KIT = kit-wide */
const x0x_param_t *drum808_param(int track, int i);
void drum808_set(drum808_t *d, int track, int i, int value);
int drum808_get(const drum808_t *d, int track, int i);

/* true while any voice is still sounding */
int drum808_active(const drum808_t *d);
