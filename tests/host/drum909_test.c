/* SPDX-License-Identifier: GPL-3.0-only */
/* The drum909 / fxbus port against the reference 9W9 engine.
 *
 * Every case builds both engines from scratch, applies the same pot settings
 * (the port through its own parameter table, 9W9 through its key/pot surface),
 * triggers the same voice(s) and renders 1 s through the whole chain (voices,
 * sends, master). The port renders in 256-frame blocks like the device; 9W9 in
 * one call. Reported per case: reference peak / RMS, max |diff|, RMS(diff) /
 * RMS(ref), and the worst 10 ms envelope difference in dB (windows above -60 dB).
 *
 *   drum909_test <9w9 module dir (has samples/)> [demo.wav]
 * Exit status is non-zero if any check fails. Diagnostics for the pattern
 * comparison: D9_ONLY=<voice> plays only that voice, D9_DEBUG=1 lists the 10 ms
 * windows where port and 9W9 differ by more than 3e-3 RMS. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsp/drum909.h"
#include "dsp/fxbus.h"

void *ref_new(const char *module_dir);
void ref_free(void *e);
int ref_set(void *e, const char *key, float pot);
int ref_get(void *e, const char *key, float *pot);
int ref_set_raw(void *e, const char *key, float v);
void ref_trigger(void *e, int which, int vel);
void ref_render(void *e, float *out, int n);
float ref_pot_value(const char *key, int pot);
void ref_noise(float *out, int n);
/* the same engine with double-precision biquads (drum909_refd.c) */
void *refd_new(const char *module_dir);
void refd_free(void *e);
int refd_set(void *e, const char *key, float pot);
void refd_trigger(void *e, int which, int vel);
void refd_render(void *e, float *out, int n);

#define SR 44100
#define NLEN 44100
#define DLY_LEN 88200

static const char *g_dir;
static drum909_t D;
static fxbus_t F;
static int16_t DLY[DLY_LEN];
static float bufp[NLEN * 2], bufr[NLEN * 2], bufd[NLEN * 2];
static int fails;

/* 9W9 trigger ids for DR_BD..DR_RD */
static const int ref_trig[DR_NUM] = { 0, 1, 2, 3, 4, 5, 6, 8, 7, 10, 9 };
static const char *const vname[DR_NUM + 1] = { "BD", "SD", "LT", "MT", "HT", "RS", "CP", "CH", "OH", "CR", "RD", "KIT" };

/* 9W9 keys for each port parameter, in the port's order */
static const char *const dkeys[DR_NUM + 1][DR_MAX_PARAMS] = {
    { "bd_c_tune", "bd_c_attack", "bd_c_decay", "bd_c_level", "bd_c_sweep_depth", "bd_c_pitch_mod", "bd_c_drive", "bd_c_dist_type" },
    { "sd_c_tune", "sd_c_noise_decay", "sd_c_snappy", "sd_c_level", "sd_c_drive", "sd_c_dist_type", "sd_c_rev", "sd_c_dly" },
    { "lt_c_tune", "lt_c_decay", "lt_c_level", "lt_c_attack", "lt_c_drive", "lt_c_dist_type", "lt_c_rev", "lt_c_dly" },
    { "mt_c_tune", "mt_c_decay", "mt_c_level", "mt_c_attack", "mt_c_drive", "mt_c_dist_type", "mt_c_rev", "mt_c_dly" },
    { "ht_c_tune", "ht_c_decay", "ht_c_level", "ht_c_attack", "ht_c_drive", "ht_c_dist_type", "ht_c_rev", "ht_c_dly" },
    { "rs_volume", "rs_tune", "rs_saturation", "rs_dist_type", "rs_rev", "rs_dly" },
    { "hc_volume", "hc_tune", "hc_decay", "hc_drive", "hc_dist_type", "hc_rev", "hc_dly" },
    { "chh_decay", "chh_volume", "chh_pitch", "chh_drive", "chh_dist_type", "chh_rev", "chh_dly" },
    { "ohh_decay", "ohh_volume", "ohh_pitch", "ohh_drive", "ohh_dist_type", "ohh_rev", "ohh_dly" },
    { "cr_pitch", "cr_volume", "cr_decay", "cr_drive", "cr_dist_type", "cr_rev", "cr_dly" },
    { "rc_pitch", "rc_volume", "rc_decay", "rc_drive", "rc_dist_type", "rc_rev", "rc_dly" },
    { "accent", "vel_depth" },
};
static const char *const fkeys[FX_NPARAMS] = {
    "volume", "master_dist", "master_drive", "master_comp", "rev_decay", "rev_tone", "rev_hpf", "rev_level",
    "dly_time", "dly_fdbk", "dly_tone", "dly_level", "dly_hpf", 0, 0,
};

typedef struct { int fx, voice, idx, value; } set_t;

static void port_render(float *out, int n)
{
    float dry[256], rev[256], dly[256];
    for (int pos = 0; pos < n; pos += 256) {
        const int m = n - pos < 256 ? n - pos : 256;
        memset(dry, 0, sizeof(dry));
        memset(rev, 0, sizeof(rev));
        memset(dly, 0, sizeof(dly));
        drum909_render(&D, dry, rev, dly, m);
        fxbus_process(&F, dry, rev, dly, out + pos, m);
    }
}

typedef struct { double peak, rms, maxd, rel, envdb, pport, flipfrac, rel_noflip; } metrics_t;

static metrics_t measure(const float *p, const float *r, int n)
{
    metrics_t m = { 0, 0, 0, 0, 0, 0, 0, 0 };
    double sr = 0, sd = 0;
    for (int i = 0; i < n; ++i) {
        const double a = (double)r[i], b = (double)p[i], d = b - a;
        if (fabs(a) > m.peak) m.peak = fabs(a);
        if (fabs(b) > m.pport) m.pport = fabs(b);
        if (fabs(d) > m.maxd) m.maxd = fabs(d);
        sr += a * a;
        sd += d * d;
    }
    m.rms = sqrt(sr / n);
    m.rel = sr > 0 ? sqrt(sd / sr) : (sd > 0 ? 1.0 : 0.0);
    /* quantizer flips: samples off by more than 1e-3 of the peak */
    {
        int nf = 0;
        double sdn = 0;
        for (int i = 0; i < n; ++i) {
            const double d = (double)p[i] - (double)r[i];
            if (fabs(d) > 1e-3 * m.peak)
                nf++;
            else
                sdn += d * d;
        }
        m.flipfrac = (double)nf / n;
        m.rel_noflip = sr > 0 ? sqrt(sdn / sr) : 0.0;
    }
    const int W = 441;
    for (int w = 0; w + W <= n; w += W) {
        double ea = 0, eb = 0;
        for (int i = w; i < w + W; ++i) {
            ea += (double)r[i] * (double)r[i];
            eb += (double)p[i] * (double)p[i];
        }
        ea = sqrt(ea / W);
        eb = sqrt(eb / W);
        if (ea > m.peak * 1e-3 && ea > 1e-7) {
            const double db = fabs(20.0 * log10((eb + 1e-30) / ea));
            if (db > m.envdb) m.envdb = db;
        }
    }
    return m;
}

/* Pass: RMS(diff)/RMS(ref) < 1e-3. Otherwise the case passes only if the port is
 * within 9W9's own float noise -- rel(port, 9W9) <= 2 x rel(9W9, 9W9 with double
 * biquads) -- with the 10 ms envelope within 0.1 dB. `self` is that second
 * number: how far 9W9 moves when only its filters' rounding changes. */
static double run_case(const char *label, const int *trig, int ntrig, int vel, const set_t *s, int ns,
                       int kind, int n)
{
    void *ref = ref_new(g_dir), *refd = refd_new(g_dir);
    (void)kind;
    drum909_init(&D);
    fxbus_init(&F, DLY, DLY_LEN);
    for (int i = 0; i < ns; ++i) {
        const char *key = s[i].fx ? fkeys[s[i].idx] : dkeys[s[i].voice][s[i].idx];
        if (s[i].fx)
            fxbus_set(&F, s[i].idx, s[i].value);
        else
            drum909_set(&D, s[i].voice, s[i].idx, s[i].value);
        if (key && (!ref_set(ref, key, (float)s[i].value) || !refd_set(refd, key, (float)s[i].value))) {
            printf("unknown reference key %s\n", key);
            fails++;
        }
    }
    for (int t = 0; t < ntrig; ++t) {
        drum909_trigger(&D, trig[t], (float)vel * (1.0f / 127.0f));
        ref_trigger(ref, ref_trig[trig[t]], vel);
        refd_trigger(refd, ref_trig[trig[t]], vel);
    }
    port_render(bufp, n);
    ref_render(ref, bufr, n);
    refd_render(refd, bufd, n);
    ref_free(ref);
    refd_free(refd);
    const metrics_t m = measure(bufp, bufr, n);
    const metrics_t self = measure(bufd, bufr, n);
    int ok = m.rel < 1e-3;
    const char *verdict = ok ? "ok" : "FAIL";
    if (!ok && m.rel <= 2.0 * self.rel && m.envdb <= (self.envdb > 0.05 ? 2.0 * self.envdb : 0.1)) {
        ok = 1;
        verdict = "ok(<=2x self)";
    }
    /* Crush is a quantizer: a 1e-5 input difference moves a sample across a
     * level boundary and the output differs by a whole step for one hold. Pass
     * if those flips are rare (< 1% of samples) and everything else agrees to
     * 1e-3 (a flip in a quiet 10 ms window can move that window's level, so the
     * envelope column is reported, not judged, for these). */
    char flipinfo[96] = "";
    if (!ok && m.flipfrac < 0.01 && m.rel_noflip < 1e-3) {
        ok = 1;
        verdict = "ok(flips)";
    }
    if (m.rel >= 1e-3)
        snprintf(flipinfo, sizeof flipinfo, " [flips %.3f%% of samples, rest rel %.1e, self env %.3f dB]",
                 100.0 * m.flipfrac, m.rel_noflip, self.envdb);
    if (m.peak < 1e-6 && m.pport < 1e-6) {
        ok = 1;
        verdict = "ok(silent)";
    }
    printf("%-26s peak %.4f rms %.5f | port %.4f | maxdiff %.1e rel %.2e (%6.1f dB) self %.2e env %.4f dB %s%s\n",
           label, m.peak, m.rms, m.pport, m.maxd, m.rel, m.rel > 0 ? 20.0 * log10(m.rel) : -999.0, self.rel,
           m.envdb, verdict, flipinfo);
    if (!ok)
        fails++;
    return m.rel;
}

static int pidx(int voice, const char *name)
{
    for (int i = 0; i < drum909_nparams(voice); ++i)
        if (!strcmp(drum909_param(voice, i)->name, name))
            return i;
    return -1;
}

static void voice_cases(void)
{
    char lab[64];
    printf("\n== voices: default pots, then every pot at 0 / 64 / 127 (switches: every position) ==\n");
    for (int v = 0; v < DR_NUM; ++v) {
        const int kind = 0;
        snprintf(lab, sizeof lab, "%s default vel127", vname[v]);
        run_case(lab, &v, 1, 127, 0, 0, kind, NLEN);
        snprintf(lab, sizeof lab, "%s default vel80", vname[v]);
        run_case(lab, &v, 1, 80, 0, 0, kind, NLEN);
        for (int i = 0; i < drum909_nparams(v); ++i) {
            const x0x_param_t *p = drum909_param(v, i);
            const int sends = !strcmp(p->name, "Rev") || !strcmp(p->name, "Dly");
            if (!strcmp(p->name, "Pan"))
                continue;                       /* X0X's (stereo): 9W9 has none; stereo_test covers it */
            if (p->max == 127) {
                static const int pots[3] = { 0, 64, 127 };
                for (int k = 0; k < 3; ++k) {
                    const set_t s = { 0, v, i, pots[k] };
                    snprintf(lab, sizeof lab, "%s %s=%d", vname[v], p->name, pots[k]);
                    run_case(lab, &v, 1, 127, &s, 1, sends ? 1 : 0, NLEN);
                }
            } else {
                for (int k = 0; k <= p->max; ++k) {
                    const set_t s = { 0, v, i, k };
                    snprintf(lab, sizeof lab, "%s %s=%s", vname[v], p->name, p->names[k]);
                    run_case(lab, &v, 1, 127, &s, 1, 0, NLEN);
                }
            }
        }
    }
}

static void kit_cases(void)
{
    char lab[64];
    printf("\n== kit: accent / velocity depth ==\n");
    static const int pots[3] = { 0, 64, 127 };
    for (int i = 0; i < drum909_nparams(DR_KIT); ++i)
        for (int k = 0; k < 3; ++k)
            for (int vel = 60; vel <= 127; vel += 67) {
                const set_t s = { 0, DR_KIT, i, pots[k] };
                const int t = DR_SD;
                snprintf(lab, sizeof lab, "KIT %s=%d SD vel%d", drum909_param(DR_KIT, i)->name, pots[k], vel);
                run_case(lab, &t, 1, vel, &s, 1, 0, NLEN);
            }
    printf("\n== kit: every voice at once, CH then OH (choke) ==\n");
    {
        static const int all[DR_NUM] = { DR_BD, DR_SD, DR_LT, DR_MT, DR_HT, DR_RS, DR_CP, DR_CH, DR_OH, DR_CR, DR_RD };
        run_case("all 11 voices vel127", all, DR_NUM, 127, 0, 0, 0, NLEN);
    }
}

static void fx_cases(void)
{
    char lab[80];
    static const int pots[3] = { 0, 64, 127 };
    const int sd = DR_SD, rs = DR_RS;
    printf("\n== send reverb (SD and CP sends 127; each reverb pot 0/64/127) ==\n");
    for (int i = FX_RV_DECAY; i <= FX_RV_LEVEL; ++i)
        for (int k = 0; k < 3; ++k) {
            const set_t s[3] = { { 0, DR_SD, pidx(DR_SD, "Rev"), 127 }, { 0, DR_CP, pidx(DR_CP, "Rev"), 127 },
                                 { 1, 0, i, pots[k] } };
            static const int tr[2] = { DR_SD, DR_CP };
            snprintf(lab, sizeof lab, "REV %s=%d", fxbus_param(i)->name, pots[k]);
            run_case(lab, tr, 2, 127, s, 3, 1, NLEN);
        }
    printf("\n== send delay (RS send 127; each delay pot 0/64/127, every division) ==\n");
    for (int i = FX_DL_FDBK; i <= FX_DL_HPF; ++i)
        for (int k = 0; k < 3; ++k) {
            const set_t s[2] = { { 0, DR_RS, pidx(DR_RS, "Dly"), 127 }, { 1, 0, i, pots[k] } };
            snprintf(lab, sizeof lab, "DLY %s=%d", fxbus_param(i)->name, pots[k]);
            run_case(lab, &rs, 1, 127, s, 2, 1, NLEN * 2);
        }
    for (int k = 0; k <= 12; ++k) {
        const set_t s[2] = { { 0, DR_SD, pidx(DR_SD, "Dly"), 127 }, { 1, 0, FX_DL_TIME, k } };
        snprintf(lab, sizeof lab, "DLY time=%s", fxbus_param(FX_DL_TIME)->names[k]);
        run_case(lab, &sd, 1, 127, s, 2, 1, NLEN * 2);
    }
    printf("\n== master: every distortion type at drive 0/64/127, comp, volume (kit hit BD+SD+CH) ==\n");
    static const int kit3[3] = { DR_BD, DR_SD, DR_CH };
    for (int t = 0; t <= 7; ++t)
        for (int k = 0; k < 3; ++k) {
            const set_t s[2] = { { 1, 0, FX_DIST, t }, { 1, 0, FX_DRIVE, pots[k] } };
            snprintf(lab, sizeof lab, "MASTER %s drive=%d", fxbus_param(FX_DIST)->names[t], pots[k]);
            run_case(lab, kit3, 3, 127, s, 2, 0, NLEN);
        }
    for (int k = 0; k < 3; ++k) {
        const int cp[3] = { 1, 64, 127 };
        const set_t s = { 1, 0, FX_COMP, cp[k] };
        snprintf(lab, sizeof lab, "MASTER comp=%d", cp[k]);
        run_case(lab, kit3, 3, 127, &s, 1, 0, NLEN);
    }
    for (int k = 0; k < 3; ++k) {
        const set_t s = { 1, 0, FX_VOLUME, pots[k] };
        snprintf(lab, sizeof lab, "MASTER volume=%d", pots[k]);
        run_case(lab, kit3, 3, 127, &s, 1, 0, NLEN);
    }
    {
        /* everything on at once: sends, both FX, distortion and glue */
        const set_t s[6] = { { 0, DR_SD, pidx(DR_SD, "Rev"), 100 }, { 0, DR_SD, pidx(DR_SD, "Dly"), 90 },
                             { 0, DR_CH, pidx(DR_CH, "Dly"), 64 }, { 1, 0, FX_DIST, 2 },
                             { 1, 0, FX_DRIVE, 30 }, { 1, 0, FX_COMP, 80 } };
        run_case("ALL fx on, kit hit", kit3, 3, 127, s, 6, 1, NLEN * 2);
    }
}

/* ---------------------------------------------------------------------- */
static void check_tables(void)
{
    printf("\n== tables ==\n");
    /* noise: same xorshift sequence, same float mapping */
    {
        static float rn[200000];
        uint32_t st = 0xC0FFEEu;
        int diff = 0;
        ref_noise(rn, 200000);
        for (int i = 0; i < 200000; ++i)
            diff += d9_noise(&st) != rn[i];
        printf("noise: %d of 200000 samples differ from 9W9's generator\n", diff);
        if (diff) fails++;
    }
    /* pot curves and pot defaults */
    {
        int n = 0, ne = 0, nd = 0, nk = 0;
        double worst = 0;
        void *ref = ref_new(0);
        drum909_init(&D);
        fxbus_init(&F, DLY, DLY_LEN);
        for (int v = 0; v <= DR_KIT; ++v)
            for (int i = 0; i < drum909_nparams(v); ++i) {
                float pv;
                if (!strcmp(drum909_param(v, i)->name, "Pan"))
                    continue;                   /* X0X's: no 9W9 key */
                ref_get(ref, dkeys[v][i], &pv);
                nk++;
                if ((int)pv != drum909_get(&D, v, i)) {
                    printf("  default pot %s: port %d, 9W9 %d\n", dkeys[v][i], drum909_get(&D, v, i), (int)pv);
                    nd++;
                }
            }
        for (int i = 0; i < FX_NPARAMS; ++i) {
            if (!fkeys[i]) continue;
            float pv;
            ref_get(ref, fkeys[i], &pv);
            nk++;
            if ((int)pv != fxbus_get(&F, i)) {
                printf("  default pot %s: port %d, 9W9 %d\n", fkeys[i], fxbus_get(&F, i), (int)pv);
                nd++;
            }
        }
        ref_free(ref);
        static const char *const ek[X0X_EXP_COUNT] = {
            "bd_c_decay", "bd_c_pitch_mod", "bd_c_drive", "sd_c_tune", "sd_c_noise_decay", "lt_c_tune", "lt_c_decay",
            "mt_c_tune", "mt_c_decay", "ht_c_tune", "ht_c_decay", "rs_tune", "hc_tune", "hc_decay",
            "ohh_decay", "chh_decay", "rc_decay", "ohh_pitch", "rev_hpf",
        };
        for (int t = 0; t < X0X_EXP_COUNT; ++t)
            for (int p = 0; p < 128; ++p) {
                const float a = x0x_pot_exp[t][p], b = ref_pot_value(ek[t], p);
                const double r = fabs((double)a - (double)b) / fabs((double)b);
                if (r > worst) worst = r;
                if (a != b) ne++;
                n++;
            }
        printf("pot curves: %d of %d EXP values differ from 9W9's powf (worst rel %.2e)\n", ne, n, worst);
        printf("pot defaults: %d of %d differ from 9W9's seeded positions\n", nd, nk);
        if (nd || worst > 2e-7) fails++;
    }
}

/* TAPE: not in 9W9, so no reference. Check it stays bounded at full feedback
 * (self-oscillation) and that DIGI is untouched by the switch. */
static void tape_checks(void)
{
    printf("\n== delay TYPE=TAPE (no reference: stability) ==\n");
    static const int wears[3] = { 0, 64, 127 };
    for (int k = 0; k < 3; ++k) {
        drum909_init(&D);
        fxbus_init(&F, DLY, DLY_LEN);
        fxbus_set(&F, FX_DL_TYPE, 1);
        fxbus_set(&F, FX_DL_WEAR, wears[k]);
        fxbus_set(&F, FX_DL_FDBK, 127);
        fxbus_set(&F, FX_DL_TONE, 127);
        drum909_set(&D, DR_SD, pidx(DR_SD, "Dly"), 127);
        drum909_trigger(&D, DR_SD, 1.0f);
        double peak_late = 0, peak = 0;
        for (int s = 0; s < 20; ++s) {                 /* 20 s */
            port_render(bufp, NLEN);
            for (int i = 0; i < NLEN; ++i) {
                const double a = fabs((double)bufp[i]);
                if (a != a) { printf("  NaN\n"); fails++; return; }
                if (a > peak) peak = a;
                if (s == 19 && a > peak_late) peak_late = a;
            }
        }
        printf("TAPE wear=%3d fdbk=127: peak over 20 s %.3f, peak in last second %.3f (self-oscillation held)%s\n",
               wears[k], peak, peak_late, peak < 4.0 ? "" : "  FAIL");
        if (!(peak < 4.0)) fails++;
    }
}

/* ---------------------------------------------------------------------- */
/* 4-bar house pattern. Rendered by the port (written to the WAV) and by 9W9
 * with the same hits, velocities and settings, and compared like any case:
 * this is the one that exercises retriggers, the hat choke, overlapping tails,
 * both sends and the glue together. */
static void wr16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }
static void wr32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }

static int demo(const char *path)
{
    const float bpm = 124.0f;
    const double spstep = 60.0 / (double)bpm / 4.0 * SR;
    const int steps = 64, total = (int)(steps * spstep) + SR * 2;  /* + 2 s of tails */
    float *mono = (float *)calloc((size_t)total, sizeof(float));
    float *rmono = (float *)calloc((size_t)total, sizeof(float));
    void *ref = ref_new(g_dir);
    drum909_init(&D);
    fxbus_init(&F, DLY, DLY_LEN);
    fxbus_set_bpm(&F, bpm);
    ref_set_raw(ref, "dly_bpm", bpm);
    /* a little reverb on the clap (and snare fills), delay on the rim, a touch
     * of glue, and the volume pulled back from 9W9's 0.35 so nothing clips */
    const set_t sets[5] = { { 0, DR_CP, pidx(DR_CP, "Rev"), 52 }, { 0, DR_SD, pidx(DR_SD, "Rev"), 30 },
                            { 0, DR_RS, pidx(DR_RS, "Dly"), 70 }, { 1, 0, FX_COMP, 24 }, { 1, 0, FX_VOLUME, 34 } };
    for (int i = 0; i < 5; ++i) {
        if (sets[i].fx) {
            fxbus_set(&F, sets[i].idx, sets[i].value);
            ref_set(ref, fkeys[sets[i].idx], (float)sets[i].value);
        } else {
            drum909_set(&D, sets[i].voice, sets[i].idx, sets[i].value);
            ref_set(ref, dkeys[sets[i].voice][sets[i].idx], (float)sets[i].value);
        }
    }
    /* x = hit, X = accent */
    static const char *const pat[DR_NUM][4] = {
        /* BD */ { "X...x...X...x...", "X...x...X...x...", "X...x...X...x...", "X...x...X...x.x." },
        /* SD */ { "................", "...............x", "................", ".........x.xx.XX" },
        /* LT */ { "................", "................", "................", "..............x." },
        /* MT */ { "................", "................", "................", ".............x.." },
        /* HT */ { "................", "................", "................", "................" },
        /* RS */ { "...x......x..x..", "...x......x..x..", "...x......x..x..", "...x......x....." },
        /* CP */ { "....X.......X...", "....X.......X...", "....X.......X...", "....X.......X..." },
        /* CH */ { "xx.xxx.xxx.xxx.x", "xx.xxx.xxx.xxx.x", "xx.xxx.xxx.xxx.x", "xx.xxx.xxx.xxx.x" },
        /* OH */ { "..X...X...X...X.", "..X...X...X...X.", "..X...X...X...X.", "..X...X...X...X." },
        /* CR */ { "X...............", "................", "X...............", "................" },
        /* RD */ { "................", "................", "x...x...x...x...", "x...x...x...x..." },
    };
    int pos = 0;
    for (int s = 0; s <= steps; ++s) {
        const int at = s < steps ? (int)(s * spstep + 0.5) : total;
        if (at > pos) {
            ref_render(ref, rmono + pos, at - pos);
            while (pos < at) {
                const int m = at - pos < 256 ? at - pos : 256;
                float dry[256] = { 0 }, rev[256] = { 0 }, dly[256] = { 0 };
                drum909_render(&D, dry, rev, dly, m);
                fxbus_process(&F, dry, rev, dly, mono + pos, m);
                pos += m;
            }
        }
        if (s == steps)
            break;
        for (int v = 0; v < DR_NUM; ++v) {
            const char c = pat[v][s / 16][s % 16];
            /* off-accent hits sit lower; the 16th hats breathe with the groove */
            const int vel = c == 'X' ? 127 : (v == DR_CH ? ((s & 1) ? 79 : 99) : 102);
            const char *only = getenv("D9_ONLY");
            if (c != '.' && (!only || atoi(only) == v)) {
                drum909_trigger(&D, v, (float)vel * (1.0f / 127.0f));
                ref_trigger(ref, ref_trig[v], vel);
            }
        }
    }
    ref_free(ref);
    {
        const metrics_t m = measure(mono, rmono, total);
        const int ok = m.rel < 1e-3;
        printf("\n== the demo pattern, port vs 9W9 (4 bars + 2 s tails, sends, choke, glue) ==\n");
        printf("%-26s peak %.4f rms %.5f | port %.4f | maxdiff %.1e rel %.2e (%6.1f dB) env %.4f dB %s\n",
               "demo pattern", m.peak, m.rms, m.pport, m.maxd, m.rel, 20.0 * log10(m.rel + 1e-30), m.envdb,
               ok ? "ok" : "FAIL");
        if (!ok)
            fails++;
        if (getenv("D9_DEBUG")) {
            for (int w = 0; w + 441 <= total; w += 441) {
                double e = 0, a = 0;
                for (int i = w; i < w + 441; ++i) {
                    const double d = (double)mono[i] - (double)rmono[i];
                    e += d * d;
                    a += (double)rmono[i] * (double)rmono[i];
                }
                if (sqrt(e / 441) > 3e-3)
                    printf("  t=%.3f s (step %.2f): rms diff %.2e, ref rms %.2e\n", (double)w / SR,
                           (double)w / spstep, sqrt(e / 441), sqrt(a / 441));
            }
        }
    }
    FILE *f = fopen(path, "wb");
    if (!f) { printf("cannot write %s\n", path); return 1; }
    const uint32_t bytes = (uint32_t)total * 4;
    fwrite("RIFF", 1, 4, f); wr32(f, 36 + bytes); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); wr32(f, 16); wr16(f, 1); wr16(f, 2); wr32(f, SR); wr32(f, SR * 4); wr16(f, 4); wr16(f, 16);
    fwrite("data", 1, 4, f); wr32(f, bytes);
    float peak = 0;
    int clip = 0;
    for (int i = 0; i < total; ++i) {
        float v = mono[i];
        if (fabsf(v) > peak) peak = fabsf(v);
        if (v > 1.0f) { v = 1.0f; clip++; }
        if (v < -1.0f) { v = -1.0f; clip++; }
        const int16_t s = (int16_t)(v * 32767.0f);
        fwrite(&s, 2, 1, f);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
    printf("wrote %s: %d frames (%.2f s, 124 BPM, 4 bars + tails), 16-bit stereo, peak %.3f, %d clipped samples\n",
           path, total, (double)total / SR, (double)peak, clip);
    free(mono);
    free(rmono);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <9w9 module dir> [demo.wav]\n", argv[0]);
        return 2;
    }
    g_dir = argv[1];
    drum909_init(&D);
    printf("sizeof(drum909_t) %u, sizeof(fxbus_t) %u, delay line %u bytes (int16 x %d), samples %u bytes flash\n",
           (unsigned)sizeof(drum909_t), (unsigned)sizeof(fxbus_t), (unsigned)sizeof(DLY), DLY_LEN,
           (unsigned)((D.smp[0].len + D.smp[2].len + D.smp[3].len) * 2));
    check_tables();
    voice_cases();
    kit_cases();
    fx_cases();
    tape_checks();
    if (argc > 2 && demo(argv[2]))
        fails++;
    printf("\n%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
