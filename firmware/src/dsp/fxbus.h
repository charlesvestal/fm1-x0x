/* SPDX-License-Identifier: GPL-3.0-only */
/* X0X send FX and master, shared by the drum and bass parts. A port of 9W9's
 * (GPL-3.0) send reverb, tempo-synced delay and master stage:
 *
 *   out = volume * glue( dist( dry + reverb(rev) + delay(dly) ) )
 *
 *  - Reverb, two types (FX_RV_TYPE; RvDec, RvTone, RvHPF and RvLvl serve both):
 *    CLASSIC is 9W9's: four combs + two allpasses, the comb loop quantised to 12 bits
 *    (the early-rack grain), damping in the loop, HPF on the send. ROOM (X0X) is
 *    smoother: the HPF, four allpass diffusers, then an 8-line feedback delay network
 *    (Hadamard mixing, a one-pole damper per line) whose lines are kept block-float:
 *    8-bit mantissas sharing an exponent per 16 samples (headroom without clipping,
 *    precision that follows the tail down, in CLASSIC's memory).
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
 * past +-16 full scale saturate) and is passed in by the caller (.pool), as are the
 * reverb's lines (fx_rv_buf_t, shared by the two types: switching clears it). Both
 * effects go idle (no per-sample work) once their input and their lines are silent. */
#pragma once
#include <stdint.h>
#include "x0x_param.h"
#include "drum909_dsp.h"

#define FX_RV_COMB_TOTAL (1116 + 1188 + 1277 + 1356)
#define FX_RV_AP_TOTAL (556 + 441)
#define FX_RV_APR_TOTAL (579 + 457)   /* X0X: the right side's allpasses */
/* ROOM's state (fx_room_t) shares the reverb's buffer with CLASSIC's lines (fx_rv_buf_t), so the two
 * types cost the larger of the two, not the sum. The design and the block format:
 * docs/plans/2026-10-15-room-reverb.md.
 *
 * Each of the 8 lines is a ring of 16-sample blocks. A block is 16 8-bit mantissas and one exponent
 * byte. A line of length L has L / 16 + 1 blocks, the fewest that hold its L samples: at the start of
 * a block of writes, the read's first block is the one the write is about to replace, so the reads
 * are fetched before that block is committed (fx_room). */
#define FX_RM_N 8
#define FX_RM_BLK 16
/* The lines' lengths in samples, in their places in the mix (the order matters: see fxbus.c), and the
 * longest. All must be equal modulo 16 (fx_rm_fetch_all). */
#define FX_RM_L0 1123
#define FX_RM_L1 1459
#define FX_RM_L2 1747
#define FX_RM_L3 1571
#define FX_RM_L4 1283
#define FX_RM_L5 2083
#define FX_RM_L6 2243
#define FX_RM_L7 1907
#define FX_RM_LMAX FX_RM_L6
#define FX_RM_NB(l) ((l) / FX_RM_BLK + 1)
#define FX_RM_BLOCKS (FX_RM_NB(FX_RM_L0) + FX_RM_NB(FX_RM_L1) + FX_RM_NB(FX_RM_L2) + FX_RM_NB(FX_RM_L3) + \
                      FX_RM_NB(FX_RM_L4) + FX_RM_NB(FX_RM_L5) + FX_RM_NB(FX_RM_L6) + FX_RM_NB(FX_RM_L7))
/* The input diffusers' lengths and gains, and their lines' total. */
#define FX_RM_D0 211
#define FX_RM_D1 157
#define FX_RM_D2 397
#define FX_RM_D3 293
#define FX_RM_G0 0.7f
#define FX_RM_G1 0.7f
#define FX_RM_G2 0.65f
#define FX_RM_G3 0.65f
#define FX_RM_DIF_TOTAL (FX_RM_D0 + FX_RM_D1 + FX_RM_D2 + FX_RM_D3)
/* Silent blocks in a row after which every ring holds only silence: ROOM is idle. */
#define FX_RM_QUIET (FX_RM_NB(FX_RM_LMAX) + 2)
typedef struct {
    /* each line's block of writes in progress: the mix is built here in place (fx_rm_damp4, then
     * fx_rm_net), then quantized into the line's ring (fx_rm_commit) */
    float stage[FX_RM_N][FX_RM_BLK];
    /* each line's next 16 reads, decoded and times c (fx_rm_fetch): the dampers' inputs */
    float read[FX_RM_N][FX_RM_BLK];
    /* each line's damper, lp = a lp + c in: c is the loop gain, times the mix's 1/sqrt(8), times 1 - a */
    float c[FX_RM_N], a[FX_RM_N], lp[FX_RM_N];
    /* the outputs' filter, y = (s - tap_a s[-1]) tap_k, and its state for L and R (fx_room_times) */
    float tap_a, tap_k, tap_zl, tap_zr;
    /* each line's ring: its first block in exp / man, its number of blocks, and the block being
     * written (the read's first block too: see above) */
    uint16_t base[FX_RM_N], nblk[FX_RM_N], wblk[FX_RM_N];
    /* the input diffusers: each one's place, and their lines, x2048: CLASSIC's combs' step and range
     * (+-16), which the loudest send X0X can make (every part at level 127 with its send up) leaves
     * about half of */
    int16_t dif_pos[4];
    int16_t dif[FX_RM_DIF_TOTAL];
    /* the lines' blocks: exponents, and 16 mantissas each */
    int8_t exp[FX_RM_BLOCKS];
    int8_t man[FX_RM_BLOCKS * FX_RM_BLK];
} fx_room_t;
_Static_assert(FX_RM_BLOCKS <= 65535, "ROOM's rings: their blocks are numbered in uint16_t");
_Static_assert(FX_RM_D0 <= 32767 && FX_RM_D1 <= 32767 && FX_RM_D2 <= 32767 && FX_RM_D3 <= 32767,
               "ROOM's diffusers: their places are int16_t");
/* The reverb's buffer (the caller's: fxbus_init): either type's lines, one type at a time. Each is
 * reached through its own member, and a change of FX_RV_TYPE clears the buffer between them. */
typedef union {
    struct {
        float ap[FX_RV_AP_TOTAL];     /* the left's allpasses (9W9's) */
        float apr[FX_RV_APR_TOTAL];   /* the right's */
        int16_t comb[FX_RV_COMB_TOTAL];
    } classic;
    fx_room_t room;
} fx_rv_buf_t;
enum { FX_RV_CLASSIC, FX_RV_ROOM };   /* FX_RV_TYPE's values (CLASSIC: fxbus_init's); a change cuts the tail */

/* FX parameters. A sound keeps 16 a voice, so the engine stores parameter k at FX's voice k / 16,
 * index k % 16 (engine.h). */
enum { FX_VOLUME, FX_DIST, FX_DRIVE, FX_COMP,
       FX_RV_DECAY, FX_RV_TONE, FX_RV_HPF, FX_RV_LEVEL,
       FX_DL_TIME, FX_DL_FDBK, FX_DL_TONE, FX_DL_LEVEL,
       FX_DL_HPF, FX_DL_TYPE, FX_DL_WEAR, FX_DL_PING,
       FX_DL_TAPE, FX_RV_TYPE, FX_NPARAMS };

struct fxbus {
    /* reverb */
    float rv_decay, rv_tone, rv_level;
    d9_biquad_t rv_hp;
    int16_t *comb;                    /* CLASSIC's lines in rv_buf (fxbus_init). Kept as pointers: read */
    float *ap;                        /* through rv_buf, fx_reverb compiles to a longer loop */
    int32_t cpos[4], apos[2];
    float cdmp[4];
    int32_t rv_quiet;
    float *apr;                       /* X0X stereo: the right side reads the combs with alternating
                                       * signs through its own allpasses (the left is 9W9's) */
    int32_t aposr[2];
    int32_t rv_type;                  /* FX_RV_CLASSIC / FX_RV_ROOM */
    fx_rv_buf_t *rv_buf;              /* the reverb's buffer: CLASSIC's lines, or ROOM's */
    float rm_out;                     /* ROOM: output gain (the energy match to CLASSIC) */
    int32_t rm_quiet;                 /* ROOM: blocks committed silent (input and lines), in a row */
    uint8_t rm_pos;                   /* ROOM: the place in the blocks being filled (all lines together) */
    /* delay */
    float dl_time_ms, dl_fdbk, dl_tone, dl_level, bpm, wear;
    int32_t dl_div, dl_type;
    d9_biquad_t dl_hp;
    int16_t *dbuf;
    int32_t dlen, w, dl_quiet;
    float dcur, lp, tape_hp;
    uint32_t wow_ph, flut_ph;
    int32_t dl_ping;                  /* X0X: ping-pong, a second tap at twice the time */
    /* master */
    float drive, volume, comp;
    int32_t dist_mode;
    d9_shape_t shape;
    float crush_st[2], crush_str[2];
    float comp_env_db, comp_det, c_thr, c_slope, c_makeup, c_drel, c_atk, c_rel;
    uint8_t pots[FX_NPARAMS];
};
typedef struct fxbus fxbus_t;

void fxbus_init(fxbus_t *f, int16_t *dly_buf, int dly_len, fx_rv_buf_t *rv_buf);
void fxbus_set_bpm(fxbus_t *f, float bpm);
void fxbus_process(fxbus_t *f, const float *dry, const float *rev, const float *dly, float *out, int n);
/* X0X stereo: dry in L / R, the sends mono; with everything centred and no reverb, out_l and out_r
 * are fxbus_process's out exactly */
void fxbus_process_st(fxbus_t *f, const float *dry_l, const float *dry_r, const float *rev, const float *dly,
                      float *out_l, float *out_r, int n);
int fxbus_nparams(void);
const x0x_param_t *fxbus_param(int i);
void fxbus_set(fxbus_t *f, int i, int value);
int fxbus_get(const fxbus_t *f, int i);
