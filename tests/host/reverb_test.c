/* SPDX-License-Identifier: GPL-3.0-only */
/* The reverb's ROOM type (fxbus.c).
 *
 * Most of this compares ROOM's output with a float reference written out here: the same network (the
 * high-pass, the diffusers, eight lines with their dampers, the mix and the output filter), with
 * plain float lines and diffusers and fxbus's own coefficients. The two differ only by ROOM's storage
 * (8-bit block-float lines, int16 diffusers), which puts their outputs about 40 dB apart; the check
 * allows 35. Anything else that differs in ROOM's signal path fails it: a read or write in the wrong
 * place, a wrong sign, a wrong damper or filter, even a diffuser's gain 1.4 % off. A wrong coefficient
 * passes, since both sides use the same ones (the loop gains, the dampers, the knob mapping); a
 * separate, coarse check holds ROOM's decay to CLASSIC's, and tools/fit_room.py --check measures it
 * closely.
 *
 * Checked:
 *  - a block of a line comes back from storage (fx_rm_commit, fx_rm_unpack) within half a step, at
 *    the finest step that holds it, and a block under FX_RM_TINY as silence;
 *  - the output against the reference's, at each reverb knob's ends and middle, in calls of
 *    irregular sizes, as the engine's are (split at sequencer events);
 *  - the tail sounds until the reference's is under -105 dB (no gate above ROOM's own, FX_RM_TINY),
 *    then reaches exact silence, and ROOM goes idle;
 *  - ROOM's decay time is roughly CLASSIC's, at settings across the knobs and in three bands;
 *  - L and R are uncorrelated and as loud;
 *  - RvLvl 0 is silent;
 *  - a full-scale send at the longest decay stays finite;
 *  - switching type cuts the tail cleanly;
 *  - turning RvDec or RvTone while ROOM plays sets the coefficients of a fresh ROOM;
 *  - mono is the stereo left.
 * Last, it prints the host time per sample against CLASSIC's (information only).
 *
 * The sends and the test blocks are pseudo-random from a fixed seed for each check, so every run is
 * the same. fxbus.c is included, not linked, for the block format's static functions. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define X0X_DRUM_TABLES_DEFINE              /* no drum909.c in this link: the tables are defined here */
#include "../../firmware/src/dsp/fxbus.c"

#define N 256
#define FS 44100
static int fails;
static void check(int ok, const char *what)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    fails += !ok;
}
static unsigned rs = 12345u;
static float rnd(void) { rs = rs * 1664525u + 1013904223u; return (float)(int)(rs >> 9) / 4194304.0f - 1.0f; }

static fxbus_t F, G;
static int16_t dl1[88200], dl2[88200];
static const float zero[N];

/* ---- the lines' block format (fx_rm_commit, fx_rm_unpack) ---- */
static fx_rv_buf_t rvb;                                     /* line 0's ring starts at block 0 */

/* Store v[0 .. 16) as a block of line 0, decode it into out (at gain 1) and check it. A block whose peak
 * is under FX_RM_TINY must come back as zeros. Any other must come back within half a step of v, at
 * the finest step that holds its peak: 63.75 to 127.5 steps (a peak of 127.5 rounds to 128, past the
 * mantissa's range). Return whether the block passed. */
static int block_ok(const float *v, float *out)
{
    fx_room_t *rm = &rvb.room;
    float peak = 0;
    int ok;
    for (int k = 0; k < FX_RM_BLK; k++) {
        rm->stage[0][k] = v[k];
        peak = fmaxf(peak, fabsf(v[k]));
    }
    const int stored = fx_rm_commit(rm, 0);
    fx_rm_unpack(rm, 0, 0, FX_RM_BLK, 1.0f, out);
    if (peak < FX_RM_TINY) {
        ok = !stored;
        for (int k = 0; k < FX_RM_BLK; k++)
            ok &= out[k] == 0.0f;
        return ok;
    }
    const double step = ldexp(1.0, rm->exp[0] - 7);
    ok = stored && peak >= 63.75 * step && peak < 127.5 * step;
    for (int k = 0; k < FX_RM_BLK; k++)
        ok &= fabs((double)out[k] - v[k]) <= 0.5 * step;
    return ok;
}

/* Check the lines' block format: blocks stored and decoded come back as block_ok requires, and the
 * decodes of part of a block that fx_rm_fetch makes, at a gain, give exactly those samples of the
 * whole times the gain.
 *
 *  - 20000 blocks of uniform noise, at peaks from about 2^-20 to 8 (the block's scale is a random
 *    power of two), which puts peaks all through each step's range;
 *  - one sample at each edge of the format, the block's other samples smaller: the peak at 127.5 steps
 *    and just under it, at a power of two, negative at both, 15.9, at FX_RM_TINY;
 *  - whole blocks just under FX_RM_TINY, of zeros, and of -0;
 *  - fx_rm_fetch's two ranges of one block, [FX_RM_OFF, 16) and [0, FX_RM_OFF), at gain 0.37 (their
 *    ends must be constants: fx_rm_unpack's loop is unrolled completely). */
static void test_block_format(void)
{
    const float EDGE[] = {127.5f / 128, nextafterf(127.5f / 128, 0.0f), 1.0f, -127.5f / 128, -1.0f, 15.9f,
                          FX_RM_TINY};
    const float WHOLE[] = {nextafterf(FX_RM_TINY, 0.0f), 0.0f, -0.0f};
    const fx_room_t *rm = &rvb.room;
    const int32_t off = FX_RM_OFF(FX_RM_L0);
    float v[FX_RM_BLK], out[FX_RM_BLK], part[FX_RM_BLK];
    int bad = 0, same = 1;
    char what[160];

    /* The noise blocks. */
    rs = 21u;
    for (int b = 0; b < 20000; b++) {
        const float scale = ldexpf(1.0f, -19 + (int)((rnd() + 1.0f) * 11.5f));
        for (int k = 0; k < FX_RM_BLK; k++)
            v[k] = scale * rnd();
        bad += !block_ok(v, out);
    }
    snprintf(what, sizeof what, "the lines' blocks: 20000 noise blocks within half a step, at the finest (%d bad)", bad);
    check(bad == 0, what);

    /* The edges, then the silent blocks. */
    bad = 0;
    rs = 22u;
    for (unsigned c = 0; c < sizeof EDGE / sizeof EDGE[0]; c++) {
        for (int k = 0; k < FX_RM_BLK; k++)
            v[k] = 0.5f * EDGE[c] * rnd();
        v[5] = EDGE[c];
        bad += !block_ok(v, out);
    }
    for (unsigned c = 0; c < sizeof WHOLE / sizeof WHOLE[0]; c++) {
        for (int k = 0; k < FX_RM_BLK; k++)
            v[k] = WHOLE[c];
        bad += !block_ok(v, out);
    }
    snprintf(what, sizeof what, "the lines' blocks: the format's edges and silence (%d bad)", bad);
    check(bad == 0, what);

    /* The partial decodes. */
    for (int k = 0; k < FX_RM_BLK; k++)
        v[k] = 0.3f * rnd();
    block_ok(v, out);
    fx_rm_unpack(rm, 0, off, FX_RM_BLK, 0.37f, part);
    fx_rm_unpack(rm, 0, 0, off, 0.37f, part + FX_RM_BLK - off);
    for (int k = 0; k < FX_RM_BLK; k++)
        same &= part[(k - off + FX_RM_BLK) % FX_RM_BLK] == (float)(rm->man[k] * ldexp((double)0.37f, rm->exp[0] - 7));
    check(same, "the lines' blocks: fx_rm_fetch's two part decodes, at a gain, are those samples times the gain");
}

/* ---- the reference: fxbus.c's ROOM with float lines and diffusers ---- */
static const int RL[8] = {FX_RM_L0, FX_RM_L1, FX_RM_L2, FX_RM_L3, FX_RM_L4, FX_RM_L5, FX_RM_L6, FX_RM_L7};
static const float RIN[8] = {1, 1, -1, 1, 1, -1, -1, 1};
static const int DL[4] = {FX_RM_D0, FX_RM_D1, FX_RM_D2, FX_RM_D3};
static const float DG[4] = {FX_RM_G0, FX_RM_G1, FX_RM_G2, FX_RM_G3};
typedef struct {
    float line[8][FX_RM_LMAX], dif[4][FX_RM_DIF_TOTAL], lp[8], tl, tr;
    int pos[8], dpos[4];
    d9_biquad_t hp;
} ref_t;
static ref_t R;

/* Run the reference over in[0 .. n) into ol and orr, with the coefficients and output gain of f (a
 * ROOM). */
static void ref_run(ref_t *r, const fxbus_t *f, const float *in, float *ol, float *orr, int n)
{
    const fx_room_t *rm = &f->rv.room;
    const float lvl = f->rv_level * rm->out_gain * 0.35355339f * rm->tap_k;
    for (int i = 0; i < n; i++) {
        float x = d9_biquad_tick(&r->hp, in[i]), s[8];
        for (int k = 0; k < 4; k++) {
            float bo = r->dif[k][r->dpos[k]], v = x + DG[k] * bo;
            r->dif[k][r->dpos[k]] = v;
            x = bo - DG[k] * v;
            r->dpos[k] = (r->dpos[k] + 1) % DL[k];
        }
        for (int j = 0; j < 8; j++) {
            float v = r->line[j][r->pos[j]];
            r->lp[j] = rm->a[j] * r->lp[j] + rm->c[j] * v;   /* (c has the loop gain and the mix's 1/sqrt(8)) */
            s[j] = r->lp[j];
        }
        for (int h = 1; h < 8; h <<= 1)
            for (int j = 0; j < 8; j += h << 1)
                for (int k = j; k < j + h; k++) {
                    float u = s[k], w = s[k + h];
                    s[k] = u + w;
                    s[k + h] = u - w;
                }
        /* the outputs: the mix's rows 1 (L: + - + - + - + -) and 6 (R: + + - - - - + +), before the input,
         * the lines' mean damper undone (fxbus's tap_a, tap_k) */
        ol[i] = (s[1] - rm->tap_a * r->tl) * lvl;
        orr[i] = (s[6] - rm->tap_a * r->tr) * lvl;
        r->tl = s[1];
        r->tr = s[6];
        for (int j = 0; j < 8; j++) {
            r->line[j][r->pos[j]] = s[j] + x * 0.35355339f * RIN[j];
            r->pos[j] = (r->pos[j] + 1) % RL[j];
        }
    }
}

/* Set f up fresh, with only the reverb heard (no distortion or glue, full volume): the given type, RvDec
 * and RvTone, and every other knob at its default. */
static void setup(fxbus_t *f, int16_t *dl, int type, int dec, int tone)
{
    fxbus_init(f, dl, 88200);
    fxbus_set(f, FX_DIST, 0);
    fxbus_set(f, FX_COMP, 0);
    fxbus_set(f, FX_VOLUME, 127);
    fxbus_set(f, FX_RV_DECAY, dec);
    fxbus_set(f, FX_RV_TONE, tone);
    fxbus_set(f, FX_RV_TYPE, type);
}

/* Fill x with samples [t0, t0 + n) of the send: noise bursts (snare-like, 50 ms at RMS about 0.5)
 * every half second before sample `on`, then silence. */
static void send(float *x, int t0, int n, int on)
{
    for (int i = 0; i < n; i++) {
        int t = t0 + i;
        x[i] = (t < on && t % (FS / 2) < FS / 20) ? 0.85f * rnd() : 0.0f;
    }
}

/* call sizes: every phase against the lines' 16-sample blocks, and whole blocks */
static const int SZ[] = {256, 13, 256, 1, 37, 256, 200, 3, 256, 64};
#define NSZ ((int)(sizeof SZ / sizeof SZ[0]))

/* The level the reference's tail must fall under before ROOM's may go silent: ROOM's blocks go silent
 * under FX_RM_TINY (-120 dB at their peak), about 110 dB under the reference here. A gate ten times
 * higher fails at most settings, and one a hundred times higher at all of them. */
#define TAIL_DB -105.0

/* Check ROOM against the float reference at each reverb knob's ends and middle, and that its tail goes
 * silent and idle, but no sooner than the reference's falls under TAIL_DB, with its sides
 * uncorrelated and as loud.
 *
 * At each setting, send 3 s of bursts and then 11 s of silence through ROOM and the reference, in
 * calls of every size in SZ. The difference must be under -35 dB of the reference. The output must
 * sound until the reference is under TAIL_DB, reach exact silence before the end, and ROOM go idle.
 * L and R must be uncorrelated (|r| < 0.5) and within 3 dB of each other. */
static void test_against_reference(void)
{
    /* Decay, Tone, LowCut, Level (-1: the default) */
    static const int SET[][4] = {{127, 15, -1, -1}, {127, 115, -1, -1}, {73, 57, -1, -1}, {0, 0, -1, -1},
                                 {64, 64, 0, -1}, {64, 64, 64, -1}, {64, 64, 127, -1}, {64, 64, -1, 64},
                                 {64, 64, -1, 127}};
    float x[N], l[N], r[N], rl[N], rr[N];
    for (unsigned c = 0; c < sizeof SET / sizeof SET[0]; c++) {
        double e = 0, d = 0, el = 0, er = 0, elr = 0, ew = 0;
        int last = -1, idle_at = -1, ref_until = -1;
        const int on = 520 * N;                              /* 3 s of bursts, then the tail: 14 s in all */
        char what[160];

        setup(&F, dl1, FX_RV_ROOM, SET[c][0], SET[c][1]);
        if (SET[c][2] >= 0)
            fxbus_set(&F, FX_RV_HPF, SET[c][2]);
        if (SET[c][3] >= 0)
            fxbus_set(&F, FX_RV_LEVEL, SET[c][3]);
        memset(&R, 0, sizeof R);
        R.hp = F.rv_hp;                                      /* the same high-pass, fresh */
        rs = 777u;

        /* Play the bursts and the tail through both, measuring as they go: the difference, each side's
         * energy and their product, the last sample that wasn't silent, when ROOM went idle, and the
         * end of the reference's last 1024-sample window over TAIL_DB (its mean square, L plus R). */
        for (int t0 = 0, k = c; t0 < 2400 * N; k++) {
            const int n = SZ[k % NSZ];
            send(x, t0, n, on);
            fxbus_process_st(&F, zero, zero, x, zero, l, r, n);
            ref_run(&R, &F, x, rl, rr, n);
            for (int i = 0; i < n; i++) {
                float vl = rl[i] * F.volume, vr = rr[i] * F.volume;
                e += (double)vl * vl + (double)vr * vr;
                d += (double)(l[i] - vl) * (l[i] - vl) + (double)(r[i] - vr) * (r[i] - vr);
                el += (double)l[i] * l[i];
                er += (double)r[i] * r[i];
                elr += (double)l[i] * r[i];
                if (l[i] != 0.0f || r[i] != 0.0f)
                    last = t0 + i;
                ew += (double)vl * vl + (double)vr * vr;
                if ((t0 + i + 1) % 1024 == 0) {
                    if (ew > 1024 * pow(10.0, TAIL_DB / 10))
                        ref_until = t0 + i + 1;
                    ew = 0;
                }
            }
            t0 += n;
            if (idle_at < 0 && t0 >= on && F.rv.room.quiet >= FX_RM_QUIET)
                idle_at = t0;
        }

        /* Check the difference, the silence and idle, and the two sides. */
        snprintf(what, sizeof what, "Decay %3d Tone %3d LowCut %3d Level %3d: against the float reference %.1f dB (< -35)",
                 SET[c][0], SET[c][1], fxbus_get(&F, FX_RV_HPF), fxbus_get(&F, FX_RV_LEVEL), 10 * log10(d / e));
        check(10 * log10(d / e) < -35.0, what);
        snprintf(what, sizeof what,
                 "  sounds until %.2f s after the last burst (the reference over %.0f dB until %.2f s), then silent, "
                 "idle at %.2f s",
                 (last - on) / (double)FS, TAIL_DB, (ref_until - on) / (double)FS, (idle_at - on) / (double)FS);
        check(last >= ref_until && last < 2300 * N && idle_at > 0, what);
        snprintf(what, sizeof what, "  L and R: correlation %.2f (|r| < 0.5), R/L %+.1f dB (within 3)",
                 elr / sqrt(el * er), 10 * log10(er / el));
        check(fabs(elr / sqrt(el * er)) < 0.5 && fabs(10 * log10(er / el)) < 3.0, what);
    }
}

/* A biquad band-pass (RBJ, 0 dB at its centre), in double: the decay check's own filter. */
typedef struct { double b0, b2, a1, a2, x1, x2, y1, y2; } bp_t;
static void bp_set(bp_t *f, double fc, double q)
{
    const double w = 2 * M_PI * fc / FS, al = sin(w) / (2 * q), a0 = 1 + al;
    memset(f, 0, sizeof *f);
    f->b0 = al / a0;
    f->b2 = -al / a0;
    f->a1 = -2 * cos(w) / a0;
    f->a2 = (1 - al) / a0;
}
static double bp_tick(bp_t *f, double x)
{
    const double y = f->b0 * x + f->b2 * f->x2 - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1; f->x1 = x;
    f->y2 = f->y1; f->y1 = y;
    return y;
}

#define DLEN (3 * FS)                                       /* the decay check's render: 3 s */
#define DBURST (FS / 50)                                    /* its burst: 20 ms */

/* Return a decay time, in seconds, of h[0 .. n) in the band around fc: band-pass it (two passes), then
 * take the time from -3 to -23 dB of its backward-integrated energy, times 3 (as tools/fit_room.py
 * measures it). */
static double band_t60(const float *h, int n, double fc)
{
    static double e[DLEN];
    bp_t f1, f2;
    double acc = 0;
    int t3 = -1, t23 = -1;
    bp_set(&f1, fc, 1.4);
    bp_set(&f2, fc, 1.4);
    for (int i = 0; i < n; i++)
        e[i] = bp_tick(&f2, bp_tick(&f1, h[i]));
    for (int i = n - 1; i >= 0; i--) {
        acc += e[i] * e[i];
        e[i] = acc;
    }
    for (int i = 0; i < n && t23 < 0; i++) {
        const double db = 10 * log10(e[i] / e[0] + 1e-30);
        if (t3 < 0 && db < -3)
            t3 = i;
        if (db < -23)
            t23 = i;
    }
    return t23 > 0 ? 3.0 * (t23 - t3) / FS : -1.0;
}

/* Fill l and r with the reverb's response, wet only, to a 20 ms white-noise burst at RMS 0.6 (a
 * snare's send level), from a fixed seed: the decay check's material (tools/fit_room_render.c's). */
static void burst_response(int type, int dec, int tone, float *l, float *r)
{
    uint32_t rng = 12345u;
    setup(&F, dl1, type, dec, tone);
    for (int b = 0; b < DLEN; b += N) {
        const int n = DLEN - b < N ? DLEN - b : N;
        float in[N];
        for (int i = 0; i < n; i++) {
            rng = rng * 1664525u + 1013904223u;
            in[i] = b + i < DBURST ? 0.6f * 1.7320508f * ((float)(rng >> 8) / 8388608.0f - 1.0f) : 0.0f;
        }
        fxbus_process_st(&F, zero, zero, in, zero, l + b, r + b, n);
    }
}

/* Check that ROOM decays about as CLASSIC does at the same RvDec and RvTone, so that a grossly wrong
 * knob mapping (fx_room_coeffs) or coefficients (fx_room_times) fail, which the comparison with the
 * reference can't see.
 *
 * At six settings across the knobs, measure both types' decay times in three bands (350, 1400 and
 * 4500 Hz) on the same burst. The tolerances are loose on purpose, well outside the fit's own error
 * (ROOM is within 0.88 to 1.35 of CLASSIC here, at worst at low RvDec), so that only a real fault
 * fails:
 *  - every band at every setting within half and twice CLASSIC's time;
 *  - all of them together (the geometric mean of the ratios) within 0.85 and 1.2: a t_dc off by a
 *    third either way fails this. */
static void test_decay_like_classic(void)
{
    static const int SET[][2] = {{0, 0}, {20, 60}, {73, 57}, {100, 30}, {127, 15}, {127, 115}};
    static const double FC[] = {350.0, 1400.0, 4500.0};
    static float l[DLEN], r[DLEN];
    double lsum = 0, lo = 1e9, hi = 0;
    int nr = 0;
    char what[160];
    for (unsigned c = 0; c < sizeof SET / sizeof SET[0]; c++) {
        double t[2][3];
        for (int type = 0; type < 2; type++) {
            burst_response(type ? FX_RV_ROOM : FX_RV_CLASSIC, SET[c][0], SET[c][1], l, r);
            for (unsigned k = 0; k < 3; k++)
                t[type][k] = (band_t60(l + DBURST, DLEN - DBURST, FC[k]) +
                              band_t60(r + DBURST, DLEN - DBURST, FC[k])) / 2;
        }
        for (unsigned k = 0; k < 3; k++) {
            const double q = t[0][k] > 0 && t[1][k] > 0 ? t[1][k] / t[0][k] : 0.0;   /* 0: no decay measured */
            lo = fmin(lo, q);
            hi = fmax(hi, q);
            lsum += log(q > 0 ? q : 1e-9);
            nr++;
        }
        printf("       Decay %3d Tone %3d: ROOM's decay against CLASSIC's %.2f, %.2f, %.2f (350, 1400, 4500 Hz)\n",
               SET[c][0], SET[c][1], t[1][0] / t[0][0], t[1][1] / t[0][1], t[1][2] / t[0][2]);
    }
    snprintf(what, sizeof what, "decay like CLASSIC's: every band within 0.5 .. 2 (%.2f .. %.2f)", lo, hi);
    check(lo >= 0.5 && hi <= 2.0, what);
    snprintf(what, sizeof what, "decay like CLASSIC's: all together within 0.85 .. 1.2 (%.2f)", exp(lsum / nr));
    check(exp(lsum / nr) >= 0.85 && exp(lsum / nr) <= 1.2, what);
}

/* Check that with RvLvl at 0, ROOM puts out nothing at all, exactly. */
static void test_level_zero(void)
{
    float x[N], l[N], r[N];
    int silent = 1;
    setup(&F, dl1, FX_RV_ROOM, 100, 70);
    fxbus_set(&F, FX_RV_LEVEL, 0);
    rs = 4u;
    for (int b = 0; b < 400; b++) {
        send(x, b * N, N, 300 * N);
        fxbus_process_st(&F, zero, zero, x, zero, l, r, N);
        for (int i = 0; i < N; i++)
            silent &= l[i] == 0.0f && r[i] == 0.0f;
    }
    check(silent, "RvLvl 0: silent");
}

/* Return how many of ROOM's diffuser cells hold the clamp's value (+-32767). */
static int diffusers_clamped(const fxbus_t *f)
{
    const fx_room_t *rm = &f->rv.room;
    int k = 0;
    for (int i = 0; i < FX_RM_DIF_TOTAL; i++)
        k += rm->dif[i] == 32767 || rm->dif[i] == -32767;
    return k;
}

/* Check that the hottest sends stay within the diffusers' range and the output finite, at the longest
 * decay with no low cut.
 *
 *  - A full-scale sine sweeping from 60 Hz to 1 kHz at x1.61 (a part's level at 127): the output
 *    stays finite and under 100.
 *  - Noise bursts (20 ms every 125 ms, peaking at 3) that drive the diffusers about as hard as the
 *    loudest send X0X makes does: to about 8.5 of their 16, where every part at level 127 with its
 *    send up and every voice of both kits on the same steps reach 8.4. No diffuser cell reaches the
 *    clamp.
 *  - The same bursts four times as loud, past anything X0X makes: the clamp holds the cells (some reach
 *    it) and the output stays finite and under 100. */
static void test_hot_send(void)
{
    float x[N], l[N], r[N];
    double ph = 0, peak = 0;
    int finite = 1, clamped = 0;
    char what[120];

    /* The sine sweep. */
    setup(&F, dl1, FX_RV_ROOM, 127, 115);
    fxbus_set(&F, FX_RV_HPF, 0);
    for (int b = 0; b < 1200; b++) {
        for (int i = 0; i < N; i++) {
            double fr = 60.0 * pow(1000.0 / 60.0, (b * N + i) / (1200.0 * N));
            ph += 2 * M_PI * fr / FS;
            x[i] = (float)(1.61 * sin(ph));
        }
        fxbus_process_st(&F, zero, zero, x, zero, l, r, N);
        for (int i = 0; i < N; i++) {
            finite &= isfinite(l[i]) && isfinite(r[i]);
            peak = fmax(peak, fmax(fabs(l[i]), fabs(r[i])));
        }
    }
    snprintf(what, sizeof what, "a full-scale sweep at the hottest send stays finite (output peak %.2f)", peak);
    check(finite && peak < 100.0, what);

    /* The bursts, at the loudest send and then past it. */
    for (int k = 0; k < 2; k++) {
        const float amp = k ? 12.0f : 3.0f;
        setup(&F, dl1, FX_RV_ROOM, 127, 115);
        fxbus_set(&F, FX_RV_HPF, 0);
        rs = 11u;
        finite = 1;
        clamped = 0;
        peak = 0;
        for (int b = 0; b < 400; b++) {
            for (int i = 0; i < N; i++)
                x[i] = (b * N + i) % (FS / 8) < FS / 50 ? amp * rnd() : 0.0f;
            fxbus_process_st(&F, zero, zero, x, zero, l, r, N);
            clamped += diffusers_clamped(&F);
            for (int i = 0; i < N; i++) {
                finite &= isfinite(l[i]) && isfinite(r[i]);
                peak = fmax(peak, fmax(fabs(l[i]), fabs(r[i])));
            }
        }
        if (k == 0) {
            snprintf(what, sizeof what, "bursts as hot as the loudest send: no diffuser clamped (%d)", clamped);
            check(clamped == 0 && finite, what);
        } else {
            snprintf(what, sizeof what, "bursts four times louder: the diffusers clamp (%d), output finite (peak %.1f)",
                     clamped, peak);
            check(clamped > 0 && finite && peak < 100.0, what);
        }
    }
}

/* Check that switching the reverb from type `from` to type `to` cuts the tail cleanly: from the switch
 * on, the bus plays exactly as a fresh bus of type `to` does.
 *
 * Play 200 blocks of bursts (from seed) through a bus of type `from`, switch it, then play another
 * burst sequence through it and through a fresh bus of type `to` side by side. */
static void check_switch(int from, int to, unsigned seed, const char *what)
{
    float x[N], l[N], r[N], rl[N], rr[N];
    int same = 1;
    setup(&F, dl1, from, 120, 90);
    setup(&G, dl2, to, 120, 90);
    rs = seed;
    for (int b = 0; b < 200; b++) {
        send(x, b * N, N, 200 * N);
        fxbus_process_st(&F, zero, zero, x, zero, l, r, N);
    }
    fxbus_set(&F, FX_RV_TYPE, to);
    F.rv_hp = G.rv_hp;                                       /* (the high-passes' state is not the tail's) */
    F.dl_hp = G.dl_hp;
    for (int b = 0; b < 400; b++) {
        send(x, b * N, N, 300 * N);
        fxbus_process_st(&F, zero, zero, x, zero, l, r, N);
        fxbus_process_st(&G, zero, zero, x, zero, rl, rr, N);
        same &= !memcmp(l, rl, sizeof l) && !memcmp(r, rr, sizeof r);
    }
    check(same, what);
}

/* Check that turning RvDec, then RvTone, while ROOM plays gives the coefficients of a fresh ROOM at the
 * new setting.
 *
 * One knob at a time, so that each must recompute the coefficients itself. */
static void test_knobs_while_playing(void)
{
    static const int TURN[2][2] = {{30, 70}, {30, 110}};    /* RvDec and RvTone after each turn */
    float x[N], l[N], r[N];
    setup(&F, dl1, FX_RV_ROOM, 100, 70);
    rs = 6u;
    for (int b = 0; b < 50; b++) {
        send(x, b * N, N, 50 * N);
        fxbus_process_st(&F, zero, zero, x, zero, l, r, N);
    }
    for (int k = 0; k < 2; k++) {
        fxbus_set(&F, k ? FX_RV_TONE : FX_RV_DECAY, TURN[k][k]);
        setup(&G, dl2, FX_RV_ROOM, TURN[k][0], TURN[k][1]);
        const fx_room_t *a = &F.rv.room, *g = &G.rv.room;
        check(!memcmp(a->a, g->a, sizeof a->a) && !memcmp(a->c, g->c, sizeof a->c) && a->tap_a == g->tap_a &&
                  a->tap_k == g->tap_k && a->out_gain == g->out_gain,
              k ? "RvTone turned while ROOM plays: its coefficients follow"
                : "RvDec turned while ROOM plays: its coefficients follow");
    }
}

/* Check that ROOM in mono (fxbus_process) is the stereo left, bit for bit. */
static void test_mono(void)
{
    float x[N], l[N], r[N], m[N];
    int same = 1;
    setup(&F, dl1, FX_RV_ROOM, 100, 70);
    setup(&G, dl2, FX_RV_ROOM, 100, 70);
    rs = 5u;
    for (int b = 0; b < 400; b++) {
        send(x, b * N, N, 300 * N);
        fxbus_process_st(&F, zero, zero, x, zero, l, r, N);
        fxbus_process(&G, zero, x, zero, m, N);
        same &= !memcmp(l, m, sizeof l);
    }
    check(same, "mono: the stereo left, bit for bit");
}

/* Print the whole bus's host time per sample with each type at its busiest (information: not
 * checked). */
static void info_host_time(void)
{
    float x[N], l[N], r[N];
    for (int t = 0; t < 2; t++) {
        struct timespec a, b;
        setup(&F, dl1, t ? FX_RV_ROOM : FX_RV_CLASSIC, 127, 115);
        rs = 3u;
        clock_gettime(CLOCK_MONOTONIC, &a);
        for (int k = 0; k < 4000; k++) {
            send(x, (k % 100) * N, N, 100 * N);
            fxbus_process_st(&F, zero, zero, x, zero, l, r, N);
        }
        clock_gettime(CLOCK_MONOTONIC, &b);
        printf("  info %s: %.1f ns per sample (the whole fxbus, host)\n", t ? "ROOM   " : "CLASSIC",
               ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / (4000.0 * N));
    }
}

int main(void)
{
    printf("reverb (ROOM):\n");
    test_block_format();
    test_against_reference();
    test_decay_like_classic();
    test_level_zero();
    test_hot_send();
    check_switch(FX_RV_ROOM, FX_RV_CLASSIC, 99u, "ROOM -> CLASSIC: the tail is cut, then CLASSIC exactly as fresh");
    check_switch(FX_RV_CLASSIC, FX_RV_ROOM, 98u, "CLASSIC -> ROOM: the tail is cut, then ROOM exactly as fresh");
    test_knobs_while_playing();
    test_mono();
    info_host_time();
    printf("reverb: %s\n", fails ? "FAIL" : "ok");
    return fails != 0;
}
