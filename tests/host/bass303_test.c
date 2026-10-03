/* SPDX-License-Identifier: GPL-3.0-only */
/* bass303 (the C / float / polyBLEP port) against schwung-303's Open303 (C++, double, 4x
 * oversampled, mip-mapped wavetables), driven with the same 2-bar acid line.
 *
 * Built by tests/host/run_bass303.sh, once per BASS303_OS. Prints, per setting:
 *   env  - per note, the max |dB| difference of the 5 ms RMS envelope over frames where the
 *          reference is above -40 dBFS; worst note and median note
 *   cent - per note, the relative difference of the spectral centroid; worst and median
 *   glide- per slide, f0 of both tracked every 1 ms (window 1.2 periods of the lower note)
 *          from 5 ms before the slide to the end of the gate: the max difference in cents
 *          (5-frame running median), and the 10-80 % glide times of both
 * then checks two instances interleaved block by block against each rendered alone, and
 * that an idle instance writes exact zeros. Exits 1 if a check fails.
 *   bass303_test [--wav build/]   also writes bass303_demo.wav (port) and bass303_ref.wav
 *   --info   report only (exit 0 whatever the result); -v every note and glide; --dump dir */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../firmware/src/dsp/bass303.h"

/* the reference, tests/host/run_bass303.sh's ref303.cpp */
void *ref_create(void);
void ref_destroy(void *r);
void ref_set(void *r, int idx, int pot);
void ref_note_on(void *r, int note, int accent, int slide);
void ref_note_off(void *r);
void ref_render(void *r, float *out, int n);

/* the same port built with BASS303_CTRL = 1 (control values every sample, as Open303), its
 * symbols renamed by run_bass303.sh; its state lives in an opaque buffer */
void k1_bass303_init(void *b);
void k1_bass303_set(void *b, int i, int v);
void k1_bass303_note_on(void *b, int note, int accent, int slide);
void k1_bass303_note_off(void *b);
void k1_bass303_render(void *b, float *out, int n);
static double k1_mem[sizeof(bass303_t) / sizeof(double) + 64];

#define FS 44100
#define NSTEP 32
#define FRAME 220                          /* 5 ms */
#define TAIL FS

/* ---- the acid line: 130 BPM sixteenths, {note, accent, slide into the next step}; -1 = rest */
static const int pat[NSTEP][3] = {
    { 33, 1, 0 }, { 33, 0, 0 }, { 45, 0, 1 }, { 43, 0, 0 }, { -1, 0, 0 }, { 36, 1, 0 }, { 33, 0, 1 }, { 40, 1, 0 },
    { 33, 0, 0 }, { 45, 1, 0 }, { -1, 0, 0 }, { 38, 0, 1 }, { 50, 0, 0 }, { 33, 1, 0 }, { 31, 0, 0 }, { -1, 0, 0 },
    { 33, 1, 0 }, { 45, 0, 1 }, { 33, 0, 0 }, { 36, 0, 0 }, { 36, 1, 1 }, { 41, 0, 1 }, { 43, 0, 0 }, { -1, 0, 0 },
    { 33, 1, 0 }, { 34, 0, 0 }, { 46, 1, 1 }, { 45, 0, 1 }, { 33, 0, 0 }, { -1, 0, 0 }, { 40, 1, 0 }, { 41, 0, 0 },
};
static const double STEP = FS * 60.0 / 130.0 / 4.0;

typedef struct { int t, type, note, acc, slide; } ev_t;   /* type 0 = on, 1 = off */

static int step_t(int i, int offset) { return offset + (int)(i * STEP + 0.5); }

static int build_events(ev_t *ev, int offset, int transpose)
{
    int n = 0, i;
    for (i = 0; i < NSTEP; i++) {
        int prev_slide = i > 0 && pat[i - 1][0] >= 0 && pat[i - 1][2];
        int next_note = i + 1 < NSTEP && pat[i + 1][0] >= 0;
        if (pat[i][0] < 0)
            continue;
        ev[n].t = step_t(i, offset); ev[n].type = 0; ev[n].note = pat[i][0] + transpose;
        ev[n].acc = pat[i][1]; ev[n].slide = prev_slide; n++;
        if (!(pat[i][2] && next_note)) {                  /* gate: half a step, a full one into a rest */
            ev[n].t = pat[i][2] ? step_t(i + 1, offset) : step_t(i, offset) + (int)(STEP * 0.5);
            ev[n].type = 1; n++;
        }
    }
    return n;
}

/* ---- engines behind one interface ---- */
typedef struct {
    void *s;
    void (*on)(void *, int, int, int);
    void (*off)(void *);
    void (*render)(void *, float *, int);
} eng_t;

static void p_on(void *s, int n, int a, int sl) { bass303_note_on((bass303_t *)s, n, a, sl); }
static void p_off(void *s) { bass303_note_off((bass303_t *)s); }
static void p_render(void *s, float *o, int n) { bass303_render((bass303_t *)s, o, n); }
static void k1_on(void *s, int n, int a, int sl) { k1_bass303_note_on(s, n, a, sl); }
static void k1_off(void *s) { k1_bass303_note_off(s); }
static void k1_render(void *s, float *o, int n) { k1_bass303_render(s, o, n); }

/* residual a - b relative to b: whole signal, and the worst 5 ms frame above -40 dBFS */
static void residual(const float *a, const float *b, int n, double *whole, double *worst)
{
    double e = 0, sg = 0, w = -400;
    int i, f;
    for (i = 0; i < n; i++) {
        double d = (double)a[i] - (double)b[i];
        e += d * d;
        sg += (double)b[i] * (double)b[i];
    }
    *whole = 10 * log10(e / sg + 1e-30);
    for (f = 0; f + FRAME <= n; f += FRAME) {
        double fe = 0, fs = 0, v;
        for (i = f; i < f + FRAME; i++) {
            double d = (double)a[i] - (double)b[i];
            fe += d * d;
            fs += (double)b[i] * (double)b[i];
        }
        if (fs / FRAME < 1e-4)
            continue;
        v = 10 * log10(fe / fs + 1e-30);
        if (v > w) w = v;
    }
    *worst = w;
}

typedef struct { eng_t e; const ev_t *ev; int nev, iev, pos, len; float *buf; } player_t;

/* render up to maxn samples, splitting at events (the sequencer's job on the device) */
static int player_step(player_t *p, int maxn)
{
    int n, next;
    if (p->pos >= p->len)
        return 0;
    while (p->iev < p->nev && p->ev[p->iev].t <= p->pos) {
        const ev_t *e = &p->ev[p->iev++];
        if (e->type == 0)
            p->e.on(p->e.s, e->note, e->acc, e->slide);
        else
            p->e.off(p->e.s);
    }
    next = p->iev < p->nev ? p->ev[p->iev].t : p->len;
    n = maxn;
    if (n > next - p->pos)
        n = next - p->pos;
    if (n > p->len - p->pos)
        n = p->len - p->pos;
    p->e.render(p->e.s, p->buf + p->pos, n);
    p->pos += n;
    return n;
}

static void play(eng_t e, const ev_t *ev, int nev, float *buf, int len)
{
    player_t p;
    p.e = e; p.ev = ev; p.nev = nev; p.iev = 0; p.pos = 0; p.len = len; p.buf = buf;
    while (player_step(&p, BASS303_MAX_BLOCK))
        ;
}

/* ---- analysis (host only: double and libm are fine here) ---- */
static double rms(const float *x, int n)
{
    double s = 0;
    int i;
    for (i = 0; i < n; i++)
        s += (double)x[i] * (double)x[i];
    return sqrt(s / n);
}

static int best_lag(const float *a, const float *b, int n)    /* b[i + lag] ~ a[i] */
{
    int lag, i, best = 0;
    double bc = -1e300;
    for (lag = -48; lag <= 48; lag++) {
        double c = 0;
        for (i = 64; i < n - 64; i++)
            c += (double)a[i] * (double)b[i + lag];
        if (c > bc) { bc = c; best = lag; }
    }
    return best;
}

static void fft(double *re, double *im, int n)
{
    int i, j, k, m;
    for (i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (m = 2; m <= n; m <<= 1) {
        double a = -2 * M_PI / m;
        for (i = 0; i < n; i += m)
            for (k = 0; k < m / 2; k++) {
                double wr = cos(a * k), wi = sin(a * k);
                double xr = re[i + k + m / 2] * wr - im[i + k + m / 2] * wi;
                double xi = re[i + k + m / 2] * wi + im[i + k + m / 2] * wr;
                re[i + k + m / 2] = re[i + k] - xr; im[i + k + m / 2] = im[i + k] - xi;
                re[i + k] += xr; im[i + k] += xi;
            }
    }
}

#define NFFT 8192
static double centroid(const float *x, int n)
{
    static double re[NFFT], im[NFFT];
    double num = 0, den = 0;
    int i;
    for (i = 0; i < NFFT; i++) {
        re[i] = i < n ? (double)x[i] * (0.5 - 0.5 * cos(2 * M_PI * i / n)) : 0.0;
        im[i] = 0;
    }
    fft(re, im, NFFT);
    for (i = 1; i < NFFT / 2; i++) {
        double f = (double)i * FS / NFFT, m = sqrt(re[i] * re[i] + im[i] * im[i]);
        if (f < 20 || f > 20000)
            continue;
        num += f * m;
        den += m;
    }
    return den > 0 ? num / den : 0;
}

/* period by a normalised squared-difference function: its minimum over [pmin, pmax] (the
 * caller tracks, so the range excludes octave errors), parabolic refinement */
static double f0_at(const float *x, int c, int w, int pmin, int pmax)
{
    static double d[2048];
    int tau, i, best = -1;
    double bv = 1e300, a, b2, cc, off;
    if (pmin < 16) pmin = 16;
    if (pmax > pmin + 2000) pmax = pmin + 2000;
    if (pmax < pmin + 2) pmax = pmin + 2;
    for (tau = pmin - 1; tau <= pmax + 1; tau++) {
        double s = 0, e = 0;
        for (i = 0; i < w; i++) {
            double u = (double)x[c + i], v = (double)x[c + i + tau];
            s += (u - v) * (u - v);
            e += u * u + v * v;
        }
        d[tau - pmin + 1] = e > 0 ? s / e : 1;
    }
    for (tau = pmin; tau <= pmax; tau++)
            if (d[tau - pmin + 1] < bv) { bv = d[tau - pmin + 1]; best = tau; }
    a = d[best - pmin]; b2 = d[best - pmin + 1]; cc = d[best - pmin + 2];
    off = (a - 2 * b2 + cc) > 0 ? 0.5 * (a - cc) / (a - 2 * b2 + cc) : 0;
    if (off > 0.5) off = 0.5;
    if (off < -0.5) off = -0.5;
    return FS / (best + off);
}

static double nfreq(int note) { return 440.0 * pow(2.0, (note - 69) / 12.0); }

typedef struct {
    double env_max, env_med, cen_max, cen_med, glide_max_cents, glide_dt_max_ms;
    int lag;
    double peak_ref, peak_port;
} result_t;

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* glide time from 10 % to 80 % of the (log-frequency) way from note a to note b */
static double glide_time(const double *f, int nf, double hop_ms, double fa, double fb)
{
    int i, t10 = -1, t90 = -1;
    double la = log(fa), lb = log(fb);
    for (i = 0; i < nf; i++) {
        double p = (log(f[i]) - la) / (lb - la);
        if (t10 < 0 && p >= 0.1) t10 = i;
        if (t90 < 0 && p >= 0.8) { t90 = i; break; }
    }
    return (t10 < 0 || t90 < 0) ? -1 : (t90 - t10) * hop_ms;
}

static void compare(const float *ref, const float *port, int len, int offset, int verbose, result_t *r)
{
    double envd[NSTEP], cend[NSTEP];
    int ne = 0, i, f, lag;
    r->lag = lag = best_lag(ref, port, len);
    r->glide_max_cents = 0;
    r->glide_dt_max_ms = 0;
    r->peak_ref = r->peak_port = 0;
    for (i = 0; i < len; i++) {
        if (fabs((double)ref[i]) > r->peak_ref) r->peak_ref = fabs((double)ref[i]);
        if (fabs((double)port[i]) > r->peak_port) r->peak_port = fabs((double)port[i]);
    }
    for (i = 0; i < NSTEP; i++) {
        int t0, n, nf;
        double mx = 0, cr, cp;
        if (pat[i][0] < 0)
            continue;
        t0 = step_t(i, offset);
        n = (int)STEP;
        for (f = 0; f + FRAME <= n; f += FRAME) {
            double a = rms(ref + t0 + f, FRAME), b = rms(port + t0 + f + lag, FRAME), d;
            if (a < 0.01)                              /* -40 dBFS */
                continue;
            d = fabs(20 * log10((b + 1e-12) / a));
            if (d > mx) mx = d;
        }
        cr = centroid(ref + t0, n);
        cp = centroid(port + t0 + lag, n);
        envd[ne] = mx;
        cend[ne] = fabs(cp - cr) / cr;
        if (verbose)
            printf("      step %2d note %2d%s%s  env %.2f dB  centroid ref %6.0f port %6.0f Hz (%+.1f%%)\n", i,
                   pat[i][0], pat[i][1] ? " acc" : "    ", (i > 0 && pat[i - 1][2]) ? " sld" : "    ", mx, cr, cp,
                   100 * (cp - cr) / cr);
        ne++;
        /* glide: this note was slid into */
        if (i > 0 && pat[i - 1][0] >= 0 && pat[i - 1][2]) {
            double fa = nfreq(pat[i - 1][0]), fb = nfreq(pat[i][0]);
            double flo = fa < fb ? fa : fb, fhi = fa < fb ? fb : fa;
            int pmin = (int)(0.8 * FS / fhi), pmax = (int)(1.25 * FS / flo) + 1;
            int w = (int)(1.2 * FS / flo), hop = FS / 1000;          /* 1 ms */
            int gate_end = t0 + (pat[i][2] ? (int)STEP : (int)(0.5 * STEP)) - FS / 500;
            double fr[200], fp[200], mc = 0, gr, gp, c;
            nf = 0;
            (void)pmin; (void)pmax;
            for (f = t0 - FS / 200 - w / 2; f + w + pmax < gate_end && nf < 200; f += hop) {
                /* track: search +-25 % around the last estimate, starting from note a */
                double lr = nf ? FS / fr[nf - 1] : FS / fa, lp = nf ? FS / fp[nf - 1] : FS / fa;
                fr[nf] = f0_at(ref, f, w, (int)(0.8 * lr), (int)(1.25 * lr) + 1);
                fp[nf] = f0_at(port, f + lag, w, (int)(0.8 * lp), (int)(1.25 * lp) + 1);
                nf++;
            }
            for (f = 2; f < nf - 2; f++) {            /* 5-frame running median: estimator glitches out */
                double m[5];
                int q;
                for (q = 0; q < 5; q++)
                    m[q] = fabs(1200 * log2(fp[f - 2 + q] / fr[f - 2 + q]));
                qsort(m, 5, sizeof(double), cmp_d);
                c = m[2];
                if (c > mc) mc = c;
            }
            gr = glide_time(fr, nf, 1.0, fa, fb);
            gp = glide_time(fp, nf, 1.0, fa, fb);
            if (verbose)
                printf("      glide %2d->%2d: max f0 diff %.1f cents, 10-80%% time ref %.0f ms port %.0f ms\n",
                       pat[i - 1][0], pat[i][0], mc, gr, gp);
            if (mc > r->glide_max_cents) r->glide_max_cents = mc;
            if (fabs(gr - gp) > r->glide_dt_max_ms) r->glide_dt_max_ms = fabs(gr - gp);
        }
    }
    {
        double se[NSTEP], sc[NSTEP];
        memcpy(se, envd, sizeof(double) * (size_t)ne);
        memcpy(sc, cend, sizeof(double) * (size_t)ne);
        qsort(se, (size_t)ne, sizeof(double), cmp_d);
        qsort(sc, (size_t)ne, sizeof(double), cmp_d);
        r->env_max = se[ne - 1]; r->env_med = se[ne / 2];
        r->cen_max = sc[ne - 1]; r->cen_med = sc[ne / 2];
    }
}

/* inharmonic (alias) power above 500 Hz relative to all harmonic power, dB: Blackman-Harris
 * window, bins within 12 Hz of a harmonic of f0 are harmonic. Below 500 Hz both engines carry
 * envelope-modulation sidebands at ~-85 dB that are not aliasing, so they are left out. */
#define NAL 16384
static double alias_db(const float *x, double f0)
{
    static double re[NAL], im[NAL];
    double ph = 0, pa = 0;
    int i;
    for (i = 0; i < NAL; i++) {
        double t = 2 * M_PI * i / (NAL - 1);
        re[i] = (double)x[i] * (0.35875 - 0.48829 * cos(t) + 0.14128 * cos(2 * t) - 0.01168 * cos(3 * t));
        im[i] = 0;
    }
    fft(re, im, NAL);
    for (i = 1; i < NAL / 2; i++) {
        double f = (double)i * FS / NAL, p = re[i] * re[i] + im[i] * im[i];
        double k = floor(f / f0 + 0.5);
        if (f < 20)
            continue;
        if (k >= 1 && fabs(f - k * f0) < 12.0) ph += p;
        else if (f >= 500) pa += p;
    }
    return 10 * log10(pa / ph);
}

/* ---- settings ---- */
typedef struct { const char *name; int pots[BASS303_NPARAMS]; int check; } setting_t;
#define DEF 64, 64, 64, 64, 64, 0, 64, 96, 0, 1, 21, 7          /* the power-on pots */
static const setting_t settings[] = {
    { "default", { DEF }, 1 },
    { "reso+envmod", { 40, 120, 120, 90, 64, 0, 64, 102, 0, 1, 21, 7 }, 1 },
    { "square+accent", { 64, 80, 80, 64, 127, 1, 64, 102, 0, 1, 21, 7 }, 1 },
    { "drive soft", { 64, 64, 64, 64, 64, 0, 64, 90, 80, 1, 21, 7 }, 0 },
    { "drive RAT", { 64, 64, 64, 64, 64, 0, 64, 90, 80, 2, 21, 7 }, 0 },
};
#define NSET ((int)(sizeof(settings) / sizeof(settings[0])))

static void apply(bass303_t *b, void *ref, const setting_t *s)
{
    int i;
    for (i = 0; i < BASS303_NPARAMS; i++) {
        if (b) bass303_set(b, i, s->pots[i]);
        if (ref) ref_set(ref, i, s->pots[i]);
    }
}

static void wav_write(const char *path, const float *x, int n)
{
    FILE *f = fopen(path, "wb");
    unsigned char h[44];
    int i;
    unsigned data = (unsigned)n * 4u;
    if (!f) { perror(path); exit(2); }
    memcpy(h, "RIFF", 4);
    h[4] = (unsigned char)(data + 36); h[5] = (unsigned char)((data + 36) >> 8);
    h[6] = (unsigned char)((data + 36) >> 16); h[7] = (unsigned char)((data + 36) >> 24);
    memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16; h[17] = h[18] = h[19] = 0; h[20] = 1; h[21] = 0; h[22] = 2; h[23] = 0;
    h[24] = 0x44; h[25] = 0xAC; h[26] = 0; h[27] = 0;          /* 44100 */
    h[28] = 0x10; h[29] = 0xB1; h[30] = 0x02; h[31] = 0;       /* 176400 */
    h[32] = 4; h[33] = 0; h[34] = 16; h[35] = 0;
    memcpy(h + 36, "data", 4);
    h[40] = (unsigned char)data; h[41] = (unsigned char)(data >> 8);
    h[42] = (unsigned char)(data >> 16); h[43] = (unsigned char)(data >> 24);
    fwrite(h, 1, 44, f);
    for (i = 0; i < n; i++) {
        double v = (double)x[i] * 32767.0;
        short s = (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        unsigned char o[4];
        o[0] = o[2] = (unsigned char)(s & 0xFF);
        o[1] = o[3] = (unsigned char)((s >> 8) & 0xFF);
        fwrite(o, 1, 4, f);
    }
    fclose(f);
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}

/* targets (see the report in run_bass303.sh's header) */
#define ENV_MAX_DB 1.0
#define CEN_MAX 0.10
#define GLIDE_CENTS 25.0
/* control rate BASS303_CTRL against 1 (every sample), same metrics, held to a fraction of
 * the port-vs-reference tolerance (half, a tenth, a fifth). The waveform residual is
 * reported, not checked: near self-oscillation the phase of the resonant ringing is
 * sensitive to the last bit of the coefficient trajectory, so it measures phase, not sound
 * (measured: magnitude spectra agree to 0.01-0.2 dB on average where it is largest). */
#define CTRL_ENV_DB 0.5
#define CTRL_CEN 0.01
#define CTRL_GLIDE 5.0

int main(int argc, char **argv)
{
    static ev_t ev[2 * NSTEP];
    static bass303_t port, pa, pb, pa2, pb2;
    const char *wavdir = 0, *dumpdir = 0;
    int nev, len = step_t(NSTEP, 0) + TAIL, s, fails = 0, i, verbose = 0, info = 0;
    float *ref_out = malloc(sizeof(float) * (size_t)len), *port_out = malloc(sizeof(float) * (size_t)len);
    float *demo = 0, *demo_ref = 0;
    int demo_len = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--wav") && i + 1 < argc) wavdir = argv[++i];
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "--info")) info = 1;
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dumpdir = argv[++i];
    }
    if (wavdir) {
        demo = calloc((size_t)(len * 3), sizeof(float));
        demo_ref = calloc((size_t)(len * 3), sizeof(float));
    }

    {
        static const int def[BASS303_NPARAMS] = { DEF };
        for (i = 0; i < BASS303_NPARAMS; i++)
            if (bass303_param(i)->def != def[i]) {
                printf("  test's DEF disagrees with bass303_param(%d)->def\n", i);
                return 1;
            }
    }
    nev = build_events(ev, 0, 0);
    printf("BASS303_OS=%d  sizeof(bass303_t)=%u bytes\n", BASS303_OS, (unsigned)sizeof(bass303_t));
    printf("  %-14s %4s  %-17s %-17s %-12s %-14s %s\n", "setting", "lag", "env dB max/med",
           "centroid max/med", "glide cents", "glide dt ms", "peak ref/port");
    for (s = 0; s < NSET; s++) {
        void *ref = ref_create();
        eng_t er, ep;
        result_t r;
        int ok;
        bass303_init(&port);
        apply(&port, ref, &settings[s]);
        er.s = ref; er.on = ref_note_on; er.off = ref_note_off; er.render = ref_render;
        ep.s = &port; ep.on = p_on; ep.off = p_off; ep.render = p_render;
        play(er, ev, nev, ref_out, len);
        play(ep, ev, nev, port_out, len);
        if (verbose)
            printf("    %s\n", settings[s].name);
        if (dumpdir) {                                   /* raw float32 for offline analysis */
            char path[512];
            FILE *f;
            snprintf(path, sizeof path, "%s/set%d_ref.f32", dumpdir, s);
            if ((f = fopen(path, "wb"))) { fwrite(ref_out, sizeof(float), (size_t)len, f); fclose(f); }
            snprintf(path, sizeof path, "%s/set%d_port.f32", dumpdir, s);
            if ((f = fopen(path, "wb"))) { fwrite(port_out, sizeof(float), (size_t)len, f); fclose(f); }
        }
        compare(ref_out, port_out, len, 0, verbose, &r);
        ok = r.env_max <= ENV_MAX_DB && r.cen_max <= CEN_MAX && r.glide_max_cents <= GLIDE_CENTS;
        printf("  %-14s %4d  %6.2f / %-8.2f %5.1f%% / %-7.1f%% %8.1f    %8.0f      %.3f / %.3f %s\n",
               settings[s].name, r.lag, r.env_max, r.env_med, 100 * r.cen_max, 100 * r.cen_med,
               r.glide_max_cents, r.glide_dt_max_ms, r.peak_ref, r.peak_port,
               settings[s].check ? (ok ? "ok" : "FAIL") : "(info)");
        {
            eng_t ek;
            float *k1 = malloc(sizeof(float) * (size_t)len);
            double rw, rf;
            result_t rk;
            int q;
            k1_bass303_init(k1_mem);
            for (q = 0; q < BASS303_NPARAMS; q++)
                k1_bass303_set(k1_mem, q, settings[s].pots[q]);
            ek.s = k1_mem; ek.on = k1_on; ek.off = k1_off; ek.render = k1_render;
            play(ek, ev, nev, k1, len);
            residual(port_out, k1, len, &rw, &rf);
            compare(k1, port_out, len, 0, 0, &rk);
            if (dumpdir) {
                char path[512];
                FILE *f;
                snprintf(path, sizeof path, "%s/set%d_k1.f32", dumpdir, s);
                if ((f = fopen(path, "wb"))) { fwrite(k1, sizeof(float), (size_t)len, f); fclose(f); }
            }
            {
                int kok = rk.env_max <= CTRL_ENV_DB && rk.cen_max <= CTRL_CEN && rk.glide_max_cents <= CTRL_GLIDE;
                printf("    ctrl %d vs 1 %4d  %6.2f / %-8.2f %5.1f%% / %-7.1f%% %8.1f    %8.0f      %s"
                       "  (waveform residual %.1f dB, worst frame %.1f)\n",
                       BASS303_CTRL, rk.lag, rk.env_max, rk.env_med, 100 * rk.cen_max, 100 * rk.cen_med,
                       rk.glide_max_cents, rk.glide_dt_max_ms, settings[s].check ? (kok ? "ok" : "FAIL") : "(info)",
                       rw, rf);
                if (settings[s].check && !kok)
                    ok = 0;
            }
            free(k1);
        }
        if (settings[s].check && !ok)
            fails++;
        if (wavdir && s < 3) {
            memcpy(demo + demo_len, port_out, sizeof(float) * (size_t)len);
            memcpy(demo_ref + demo_len, ref_out, sizeof(float) * (size_t)len);
            demo_len += len;
        }
        ref_destroy(ref);
    }

    /* aliasing: a sustained note, filter wide open, analysed 0.5 s in */
    {
        static const int notes[] = { 48, 72, 84 };
        float *r1 = malloc(sizeof(float) * FS), *p1 = malloc(sizeof(float) * FS);
        int w, k;
        printf("  alias floor (inharmonic > 500 Hz), sustained note, cutoff/decay max, reso/envmod 0:\n");
        for (w = 0; w < 2; w++)
            for (k = 0; k < 3; k++) {
                setting_t st = { "alias", { 127, 0, 0, 127, 0, 0, 64, 102, 0, 1, 21, 7 }, 0 };
                void *ref = ref_create();
                st.pots[BASS303_WAVE] = w;
                bass303_init(&port);
                apply(&port, ref, &st);
                ref_note_on(ref, notes[k], 0, 0);
                bass303_note_on(&port, notes[k], 0, 0);
                for (i = 0; i < FS; i += 256) {
                    ref_render(ref, r1 + i, FS - i < 256 ? FS - i : 256);
                    bass303_render(&port, p1 + i, FS - i < 256 ? FS - i : 256);
                }
                printf("    %s note %d (%4.0f Hz): ref %6.1f dB  port %6.1f dB\n", w ? "square" : "saw   ", notes[k],
                       nfreq(notes[k]), alias_db(r1 + FS / 2 - NAL / 2, nfreq(notes[k])),
                       alias_db(p1 + FS / 2 - NAL / 2, nfreq(notes[k])));
                ref_destroy(ref);
            }
        free(r1);
        free(p1);
    }

    /* a note after a long silence: the port has gone idle (cleared filters), Open303 has not.
     * The pattern twice with 1 s between; the second pass's first note starts cold. Reported,
     * not checked: it is the documented cost of idling (bass303.h, BASS303_IDLE_HOLD). */
    {
        static ev_t ev2[4 * NSTEP];
        int off2 = step_t(NSTEP, 0) + FS, len2 = off2 + step_t(NSTEP, 0) + TAIL, n2;
        float *r2 = malloc(sizeof(float) * (size_t)len2), *p2 = malloc(sizeof(float) * (size_t)len2);
        n2 = build_events(ev2, 0, 0);
        n2 += build_events(ev2 + n2, off2, 0);
        printf("  after 1 s of silence (second pass; its first note starts from idle):\n");
        for (s = 0; s < 3; s++) {
            void *ref = ref_create();
            eng_t er, ep;
            result_t r;
            bass303_init(&port);
            apply(&port, ref, &settings[s]);
            er.s = ref; er.on = ref_note_on; er.off = ref_note_off; er.render = ref_render;
            ep.s = &port; ep.on = p_on; ep.off = p_off; ep.render = p_render;
            play(er, ev2, n2, r2, len2);
            play(ep, ev2, n2, p2, len2);
            compare(r2, p2, len2, off2, 0, &r);
            printf("  %-14s %4d  %6.2f / %-8.2f %5.1f%% / %-7.1f%% %8.1f    %8.0f      (info: cold onset)\n",
                   settings[s].name, r.lag, r.env_max, r.env_med, 100 * r.cen_max, 100 * r.cen_med,
                   r.glide_max_cents, r.glide_dt_max_ms);
            ref_destroy(ref);
        }
        free(r2);
        free(p2);
    }

    /* two instances interleaved block by block == each alone */
    {
        static ev_t evb[2 * NSTEP];
        float *a1 = calloc((size_t)len, sizeof(float)), *b1 = calloc((size_t)len, sizeof(float));
        float *a2 = calloc((size_t)len, sizeof(float)), *b2 = calloc((size_t)len, sizeof(float));
        int nevb = build_events(evb, 1234, 7), more;
        player_t A, B;
        eng_t e;
        e.on = p_on; e.off = p_off; e.render = p_render;
        bass303_init(&pa); apply(&pa, 0, &settings[0]);
        bass303_init(&pb); apply(&pb, 0, &settings[2]);
        bass303_init(&pa2); apply(&pa2, 0, &settings[0]);
        bass303_init(&pb2); apply(&pb2, 0, &settings[2]);
        e.s = &pa; play(e, ev, nev, a1, len);
        e.s = &pb; play(e, evb, nevb, b1, len);
        A.e = e; A.e.s = &pa2; A.ev = ev; A.nev = nev; A.iev = 0; A.pos = 0; A.len = len; A.buf = a2;
        B.e = e; B.e.s = &pb2; B.ev = evb; B.nev = nevb; B.iev = 0; B.pos = 0; B.len = len; B.buf = b2;
        do {
            more = player_step(&A, BASS303_MAX_BLOCK);
            more |= player_step(&B, BASS303_MAX_BLOCK);
        } while (more);
        i = memcmp(a1, a2, sizeof(float) * (size_t)len) == 0 && memcmp(b1, b2, sizeof(float) * (size_t)len) == 0
            && memcmp(a1, b1, sizeof(float) * (size_t)len) != 0;
        printf("  two instances interleaved == alone (bit-exact): %s\n", i ? "ok" : "FAIL");
        if (!i) fails++;
        free(a1); free(b1); free(a2); free(b2);
    }

    /* idle: after the line and its tail, exact zeros; host time active vs idle */
    {
        float blk[BASS303_MAX_BLOCK];
        int k, nz = 0, reps = 2000;
        double t0, ta, ti;
        bass303_init(&port);
        apply(&port, 0, &settings[0]);
        {
            eng_t ep;
            ep.s = &port; ep.on = p_on; ep.off = p_off; ep.render = p_render;
            play(ep, ev, nev, port_out, len);
        }
        for (k = 0; k < 64; k++) {
            bass303_render(&port, blk, BASS303_MAX_BLOCK);
            for (i = 0; i < BASS303_MAX_BLOCK; i++)
                nz += blk[i] != 0.0f;
        }
        printf("  idle after release: %s (%d non-zero samples, idle=%d)\n", nz == 0 && port.idle ? "ok" : "FAIL", nz,
               port.idle);
        if (nz || !port.idle) fails++;
        t0 = now_s();
        for (k = 0; k < reps; k++)
            bass303_render(&port, blk, BASS303_MAX_BLOCK);
        ti = (now_s() - t0) / (reps * BASS303_MAX_BLOCK);
        bass303_note_on(&port, 33, 0, 0);
        t0 = now_s();
        for (k = 0; k < reps; k++)
            bass303_render(&port, blk, BASS303_MAX_BLOCK);
        ta = (now_s() - t0) / (reps * BASS303_MAX_BLOCK);
        printf("  host time per output sample: active %.1f ns, idle %.2f ns\n", ta * 1e9, ti * 1e9);
    }

    if (wavdir) {
        char path[512];
        snprintf(path, sizeof path, "%s/bass303_demo.wav", wavdir);
        wav_write(path, demo, demo_len);
        snprintf(path, sizeof path, "%s/bass303_ref.wav", wavdir);
        wav_write(path, demo_ref, demo_len);
        printf("  wrote %s/bass303_demo.wav and bass303_ref.wav (default, reso+envmod, square+accent)\n", wavdir);
    }
    printf("  %s%s\n", fails ? "FAILED" : "all checks passed", info ? " (informational: not part of the exit status)" : "");
    return fails && !info ? 1 : 0;
}
