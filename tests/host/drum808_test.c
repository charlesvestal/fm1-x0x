/* SPDX-License-Identifier: GPL-3.0-only */
/* drum808 (the port) against 8W8's own sc808_engine.cpp (the reference, built
 * from its sources in double with libm), sound by sound and pot by pot, then a
 * four-bar 808 demo rendered by the port.
 *   drum808_test <demo.wav>
 * Each case renders 1 s after one trigger and reports the reference's peak and
 * RMS, the largest sample difference and the RMS of the difference relative to
 * the reference RMS (PASS < 1e-3). The reference's own reverb and delay returns
 * are set to zero, so its output is the dry kit times its Volume; the port's
 * send buses are checked against that times the send pot. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sc808_engine.h"
#include "drum808.h"

#define SRATE 44100
#define LEN SRATE                         /* 1 s per case */
#define TOL 1e-3

static float ref_out[4 * SRATE], port_dry[4 * SRATE], port_rev[4 * SRATE], port_dly[4 * SRATE];
static drum808_t port;
static int n_fail, n_cases;
static double worst_rel;
static char worst_name[96];

typedef struct { const char *name; int ref, track, sw; const char *id; const char *pots[8]; } snd_t;
static const snd_t k_snd[16] = {
    {"BD", SC808_BD, D8_BD, 0, "bd", {"tune", "attack", "decay", "tone", "drive", "level", 0}},
    {"SD", SC808_SD, D8_SD, 0, "sd", {"tune", "decay", "snappy", "drive", "level", "rev", "dly", 0}},
    {"LT", SC808_LT, D8_LT, 0, "lt", {"tune", "decay", "drive", "level", "rev", "dly", 0}},
    {"MT", SC808_MT, D8_MT, 0, "mt", {"tune", "decay", "drive", "level", "rev", "dly", 0}},
    {"HT", SC808_HT, D8_HT, 0, "ht", {"tune", "decay", "drive", "level", "rev", "dly", 0}},
    {"LC", SC808_LC, D8_LT, 1, "lc", {"tune", "decay", "drive", "level", "rev", "dly", 0}},
    {"MC", SC808_MC, D8_MT, 1, "mc", {"tune", "decay", "drive", "level", "rev", "dly", 0}},
    {"HC", SC808_HC, D8_HT, 1, "hc", {"tune", "decay", "drive", "level", "rev", "dly", 0}},
    {"RS", SC808_RS, D8_RS, 0, "rs", {"tune", "decay", "drive", "level", "rev", "dly", 0}},
    {"CL", SC808_CL, D8_RS, 1, "cl", {"tune", "decay", "drive", "level", "rev", "dly", 0}},
    {"MA", SC808_MA, D8_CP, 1, "ma", {"tune", "attack", "decay", "drive", "level", "rev", "dly", 0}},
    {"CP", SC808_CP, D8_CP, 0, "cp", {"tune", "decay", "drive", "level", "rev", "dly", 0}},
    {"CB", SC808_CB, D8_CB, 0, "cb", {"tune", "decay", "drive", "level", "rev", "dly", 0}},
    {"CH", SC808_CH, D8_CH, 0, "ch", {"tune", "decay", "drive", "level", "rev", "dly", 0}},
    {"OH", SC808_OH, D8_OH, 0, "oh", {"tune", "decay", "drive", "level", "rev", "dly", 0}},
    {"CY", SC808_CY, D8_CY, 0, "cy", {"tune", "decay", "drive", "level", "rev", "dly", 0}},
};

/* a port parameter by its (case-insensitive) name on a track */
static int port_index(int track, const char *name)
{
    int i, n = drum808_nparams(track);
    for (i = 0; i < n; i++) {
        const char *a = drum808_param(track, i)->name, *b = name;
        while (*a && *b && (*a | 32) == (*b | 32)) a++, b++;
        if (!*a && !*b) return i;
    }
    fprintf(stderr, "no port param %s on track %d\n", name, track);
    exit(2);
}

typedef struct { int sound; int at; int vel127; } hit_t;

static sc808_engine_t *ref_new(void)
{
    sc808_engine_t *e = sc808_create(44100.0f);
    sc808_set_param(e, "rev_level", "0");      /* keep the reference's own FX returns out */
    sc808_set_param(e, "dly_level", "0");
    return e;
}

static void ref_set(sc808_engine_t *e, const char *key, int v)
{
    char b[16];
    snprintf(b, sizeof b, "%d", v);
    if (!sc808_set_param(e, key, b)) { fprintf(stderr, "ref has no %s\n", key); exit(2); }
}

/* render both engines over len samples with hits at sample positions */
static void render_both(sc808_engine_t *e, const hit_t *h, int nh, int len)
{
    int pos = 0, k = 0;
    memset(port_dry, 0, sizeof(float) * (size_t)len);
    memset(port_rev, 0, sizeof(float) * (size_t)len);
    memset(port_dly, 0, sizeof(float) * (size_t)len);
    while (pos < len) {
        int next = len, n;
        while (k < nh && h[k].at <= pos) {
            const snd_t *s = &k_snd[h[k].sound];
            sc808_trigger(e, s->ref, h[k].vel127);
            if (s->track == D8_LT || s->track == D8_MT || s->track == D8_HT || s->track == D8_RS || s->track == D8_CP)
                drum808_set(&port, s->track, port_index(s->track, "Sound"), s->sw);
            drum808_trigger(&port, s->track, (float)h[k].vel127 * (1.0f / 127.0f));
            k++;
        }
        if (k < nh && h[k].at < next) next = h[k].at;
        n = next - pos;
        if (n > 256) n = 256;
        sc808_render(e, ref_out + pos, n);
        drum808_render(&port, port_dry + pos, port_rev + pos, port_dly + pos, n);
        pos += n;
    }
}

typedef struct { double peak, rms, maxd, rel, env_db; } cmp_t;

static cmp_t compare(const float *ref, const float *got, float scale, int len)
{
    cmp_t c = {0, 0, 0, 0, 0};
    double se = 0, sd = 0, wpk = 0;
    int i, w;
    for (i = 0; i < len; i++) {
        double r = (double)ref[i] * (double)scale, d = (double)got[i] - r;
        if (fabs(r) > c.peak) c.peak = fabs(r);
        if (fabs(d) > c.maxd) c.maxd = fabs(d);
        se += r * r;
        sd += d * d;
    }
    c.rms = sqrt(se / len);
    c.rel = se > 0 ? sqrt(sd / se) : (sd > 0 ? 1.0 : 0.0);
    /* envelope: 10 ms window RMS, max dB difference over windows above -60 dB of the loudest */
    if (scale == 0.0f)
        return c;
    for (w = 0; w + 441 <= len; w += 441) {
        double a = 0;
        for (i = w; i < w + 441; i++) a += (double)ref[i] * (double)ref[i];
        if (a > wpk) wpk = a;
    }
    for (w = 0; w + 441 <= len; w += 441) {
        double a = 0, b = 0, db;
        for (i = w; i < w + 441; i++) {
            a += (double)ref[i] * (double)ref[i];
            b += (double)got[i] * (double)got[i];
        }
        if (a <= 0 || a < wpk * 1e-6)
            continue;
        db = 10.0 * log10((b + 1e-30) / (a * (double)scale * (double)scale));
        if (fabs(db) > fabs(c.env_db)) c.env_db = db;
    }
    return c;
}

static int verbose;
static void report(const char *name, cmp_t c)
{
    int ok = c.rel < TOL;
    n_cases++;
    if (!ok) n_fail++;
    if (c.rel > worst_rel) { worst_rel = c.rel; snprintf(worst_name, sizeof worst_name, "%s", name); }
    if (verbose || !ok)
        printf("  %-26s peak %8.5f  rms %8.5f  maxdiff %9.3e  rel %9.3e  env %+7.4f dB  %s\n", name, c.peak, c.rms,
               c.maxd, c.rel, c.env_db, ok ? "PASS" : "FAIL");
}

/* one sound, one hit, with the given pot (or none) at a value */
static void sound_case(int si, const char *pot, int val, int vel127, const char *dist_note)
{
    const snd_t *s = &k_snd[si];
    sc808_engine_t *e = ref_new();
    hit_t h = {si, 0, vel127};
    char name[96], key[48];
    int is_send = pot && (!strcmp(pot, "rev") || !strcmp(pot, "dly"));
    drum808_init(&port);
    if (s->sw) drum808_set(&port, s->track, port_index(s->track, "Sound"), 1);
    if (pot) {
        snprintf(key, sizeof key, "%s_%s", s->id, pot);
        ref_set(e, key, val);
        drum808_set(&port, s->track, port_index(s->track, pot), val);
    }
    if (dist_note) {                            /* drive 64 with a distortion type */
        int t = atoi(dist_note);
        snprintf(key, sizeof key, "%s_dist_type", s->id);
        ref_set(e, key, t);
        drum808_set(&port, s->track, port_index(s->track, "Dist"), t);
    }
    render_both(e, &h, 1, LEN);
    if (dist_note)
        snprintf(name, sizeof name, "%s drive=%d dist=%s", s->name, val, drum808_param(s->track, port_index(s->track, "Dist"))->names[atoi(dist_note)]);
    else if (pot)
        snprintf(name, sizeof name, "%s %s=%d", s->name, pot, val);
    else
        snprintf(name, sizeof name, "%s default v%d", s->name, vel127);
    report(name, compare(ref_out, port_dry, 1.0f, LEN));
    if (is_send) {                              /* the bus the pot feeds: dry x amount */
        char n2[96];
        snprintf(n2, sizeof n2, "%s %s=%d (send bus)", s->name, pot, val);
        report(n2, compare(ref_out, !strcmp(pot, "rev") ? port_rev : port_dly, (float)val / 127.0f, LEN));
    }
    sc808_destroy(e);
}

static void seq_case(const char *name, const hit_t *h, int nh, int len)
{
    sc808_engine_t *e = ref_new();
    drum808_init(&port);
    render_both(e, h, nh, len);
    report(name, compare(ref_out, port_dry, 1.0f, len));
    sc808_destroy(e);
}

/* ---- demo ---- */
static void wr32(FILE *f, unsigned v) { fputc((int)(v & 255), f); fputc((int)(v >> 8 & 255), f); fputc((int)(v >> 16 & 255), f); fputc((int)(v >> 24), f); }
static void wr16(FILE *f, unsigned v) { fputc((int)(v & 255), f); fputc((int)(v >> 8 & 255), f); }

static int demo(const char *path)
{
    /* 4 bars at 112 BPM: an electro / hip-hop 808 groove. Bars 1-2: congas on LT/MT,
     * rim; bars 3-4: claves, the cowbell joins, a hi tom answers. Accents (vel 1.0)
     * on the downbeats; everything else at the 808's unaccented level. */
    static const char *pat[][2] = {
        /* track     1...2...3...4...    (x = hit, A = accented hit) */
        {"BD",  "A.....x...x...x."},
        {"SD",  "....A.......A..."},
        {"CP",  "....x.......x..x"},
        {"CH",  "x.x.x.x.x.x.x.x."},
        {"OH",  "..............A."},
        {"LT",  "..x.....x..x...."},
        {"MT",  ".....x.......x.."},
        {"HT",  "................"},
        {"RS",  "...x......x....."},
        {"CB",  "................"},
        {"CY",  "................"},
    };
    static const char *pat2[][2] = {
        {"BD",  "A.....x...x.x..."},
        {"SD",  "....A.......A..."},
        {"CP",  "....x.......x.xx"},
        {"CH",  "x.xxx.x.x.xxx.x."},
        {"OH",  "......A.......A."},
        {"LT",  "..x.....x......."},
        {"MT",  ".....x......x..."},
        {"HT",  "..............Ax"},
        {"RS",  "x..x..x...x..x.."},
        {"CB",  "A..x..x...x..x.."},
        {"CY",  "................"},
    };
    static const int trk[11] = {D8_BD, D8_SD, D8_CP, D8_CH, D8_OH, D8_LT, D8_MT, D8_HT, D8_RS, D8_CB, D8_CY};
    const double step = 60.0 / 112.0 / 4.0 * SRATE;
    const int total = (int)(64 * step) + SRATE;
    float *mono = calloc((size_t)total, sizeof(float)), *junk = calloc(256, sizeof(float));
    int pos = 0, st = 0, i, bar;
    FILE *f;
    double peak = 0;
    drum808_init(&port);
    drum808_set(&port, D8_KIT, 0, 56);         /* kit Level: the dry, accented kit stacks past 0 dBFS at 100 */
    drum808_set(&port, D8_LT, port_index(D8_LT, "Sound"), 1);      /* low conga */
    drum808_set(&port, D8_MT, port_index(D8_MT, "Sound"), 1);      /* mid conga */
    while (pos < total) {
        int next = st < 64 ? (int)(st * step + 0.5) : total, n;
        if (st < 64 && pos >= next) {
            bar = st / 16;
            if (st % 16 == 0) {
                drum808_set(&port, D8_RS, port_index(D8_RS, "Sound"), bar >= 2);   /* rim, then claves */
                if (bar == 0 || bar == 2) drum808_trigger(&port, D8_CY, bar ? 0.6f : 1.0f);
            }
            for (i = 0; i < 11; i++) {
                char c = (bar & 1 ? pat2 : pat)[i][1][st % 16];
                if (trk[i] == D8_CB && bar < 2) continue;                 /* the cowbell joins at bar 3 */
                if (trk[i] == D8_HT && bar < 3) continue;
                if (c == 'x') drum808_trigger(&port, trk[i], D8_VEL_NORMAL);
                if (c == 'A') drum808_trigger(&port, trk[i], 1.0f);
            }
            st++;
            continue;
        }
        n = (st < 64 ? next : total) - pos;
        if (n > 256) n = 256;
        memset(junk, 0, 256 * sizeof(float));
        drum808_render(&port, mono + pos, junk, junk, n);
        pos += n;
    }
    for (i = 0; i < total; i++) if (fabs((double)mono[i]) > peak) peak = fabs((double)mono[i]);
    f = fopen(path, "wb");
    if (!f) { perror(path); return 1; }
    fwrite("RIFF", 1, 4, f); wr32(f, 36u + (unsigned)total * 4u); fwrite("WAVEfmt ", 1, 8, f);
    wr32(f, 16); wr16(f, 1); wr16(f, 2); wr32(f, SRATE); wr32(f, SRATE * 4); wr16(f, 4); wr16(f, 16);
    fwrite("data", 1, 4, f); wr32(f, (unsigned)total * 4u);
    for (i = 0; i < total; i++) {
        double v = (double)mono[i];
        int s = (int)lrint((v > 1.0 ? 1.0 : v < -1.0 ? -1.0 : v) * 32767.0);
        wr16(f, (unsigned)(s & 0xFFFF)); wr16(f, (unsigned)(s & 0xFFFF));
    }
    fclose(f);
    printf("demo: %s, 4 bars at 112 BPM, kit Level 56, %.2f s, peak %.3f (%.1f dBFS)\n", path, (double)total / SRATE, peak,
           20.0 * log10(peak));
    free(mono); free(junk);
    return 0;
}

int main(int argc, char **argv)
{
    int si, k, v, t;
    static const int vals[3] = {0, 64, 127};
    verbose = argc > 2 && !strcmp(argv[2], "-v");
    printf("drum808 vs 8W8: 1 s per case, rel = rms(port - ref) / rms(ref), PASS < %.0e\n", TOL);
    printf("sizeof(drum808_t) = %u bytes\n", (unsigned)sizeof(drum808_t));

    printf("[defaults and pots at 0/64/127]%s\n", verbose ? "" : " (failures only; -v for all)");
    for (si = 0; si < 16; si++) {
        sound_case(si, 0, 0, 127, 0);
        for (k = 0; k_snd[si].pots[k]; k++)
            for (v = 0; v < 3; v++)
                sound_case(si, k_snd[si].pots[k], vals[v], 127, 0);
    }
    printf("[velocity 0.3 / 0.7 / 1.0 (38, 89, 127 of 127)]\n");
    {
        static const int vs[3] = {38, 89, 127}, which[4] = {0, 1, 11, 13};
        for (si = 0; si < 4; si++)
            for (v = 0; v < 3; v++) {
                int keep = verbose;
                verbose = 1;
                sound_case(which[si], 0, 0, vs[v], 0);
                verbose = keep;
            }
    }
    printf("[drive 64 through each distortion]\n");
    {
        static const int which[3] = {1, 9, 13};
        for (si = 0; si < 3; si++)
            for (t = 0; t < 7; t++) {
                char d[4];
                int keep = verbose;
                snprintf(d, sizeof d, "%d", t);
                verbose = 1;
                sound_case(which[si], "drive", 64, 127, d);
                verbose = keep;
            }
    }
    printf("[sequences: retriggers over a ringing voice, the hat choke]\n");
    {
        int keep = verbose;
        const hit_t a[] = {{0, 0, 127}, {1, 4000, 100}, {0, 11025, 60}, {2, 12000, 127}, {0, 13000, 127},
                           {11, 15000, 127}, {1, 16000, 127}, {2, 20000, 90}, {8, 22000, 127}, {11, 22500, 50},
                           {7, 26000, 127}, {9, 30000, 127}, {10, 31000, 127}, {8, 33000, 70}, {0, 40000, 127}};
        const hit_t b[] = {{14, 0, 127}, {13, 4410, 127}, {14, 8820, 127}, {13, 13230, 127}, {13, 15000, 127}};
        const hit_t c[] = {{15, 0, 127}, {12, 2205, 127}, {13, 6615, 127}, {12, 11025, 127}};
        verbose = 1;
        seq_case("kit sequence (non-metal)", a, (int)(sizeof a / sizeof a[0]), 2 * SRATE);
        seq_case("OH choked by CH, retriggers", b, (int)(sizeof b / sizeof b[0]), SRATE);
        seq_case("CY + CB + CH on one bank", c, (int)(sizeof c / sizeof c[0]), SRATE);
        verbose = keep;
    }
    printf("[port only: the panel]\n");
    {
        /* SD Tone is the 808's (8W8 fixes it at 0.5): 64 must be 8W8's snare exactly */
        static float a[SRATE], b[SRATE], junk[SRATE];
        int tr, i, bad = 0, tone = port_index(D8_SD, "Tone");
        double ra = 0, rb = 0;
        drum808_init(&port);
        drum808_trigger(&port, D8_SD, 1.0f);
        memset(a, 0, sizeof a);
        for (i = 0; i < SRATE; i += 256) drum808_render(&port, a + i, junk + i, junk + i, SRATE - i < 256 ? SRATE - i : 256);
        drum808_init(&port);
        drum808_set(&port, D8_SD, tone, 64);
        drum808_trigger(&port, D8_SD, 1.0f);
        memset(b, 0, sizeof b);
        for (i = 0; i < SRATE; i += 256) drum808_render(&port, b + i, junk + i, junk + i, SRATE - i < 256 ? SRATE - i : 256);
        if (memcmp(a, b, sizeof a)) { printf("  SD Tone=64 differs from the default snare  FAIL\n"); n_fail++; }
        for (v = 0; v < 2; v++) {
            double r = 0;
            drum808_init(&port);
            drum808_set(&port, D8_SD, tone, v ? 127 : 0);
            drum808_trigger(&port, D8_SD, 1.0f);
            memset(b, 0, sizeof b);
            for (i = 0; i < SRATE; i += 256) drum808_render(&port, b + i, junk + i, junk + i, SRATE - i < 256 ? SRATE - i : 256);
            for (i = 0; i < SRATE; i++) r += (double)b[i] * (double)b[i];
            if (!(r == r) || r <= 0) { printf("  SD Tone=%d silent or NaN  FAIL\n", v ? 127 : 0); n_fail++; }
            if (v) rb = r; else ra = r;
        }
        printf("  SD Tone 0 / 64 / 127: rms %.4f / (= default, bit-exact) / %.4f\n", sqrt(ra / SRATE), sqrt(rb / SRATE));
        /* every parameter: names fit, set / get round-trips, the switch picks its sound's own pots */
        drum808_init(&port);
        for (tr = 0; tr <= D8_KIT; tr++)
            for (i = 0; i < drum808_nparams(tr); i++) {
                const x0x_param_t *p = drum808_param(tr, i);
                int want = p->max > 1 ? p->max / 2 + 1 : 1, j;
                if (strlen(p->name) > 6) { printf("  name '%s' > 6 chars  FAIL\n", p->name); bad++; }
                if (p->names)
                    for (j = 0; j <= p->max; j++)
                        if (strlen(p->names[j]) > 5) { printf("  label '%s' > 5 chars  FAIL\n", p->names[j]); bad++; }
                if (drum808_get(&port, tr, i) != p->def) { printf("  track %d %s default %d != %d  FAIL\n", tr, p->name, drum808_get(&port, tr, i), p->def); bad++; }
                drum808_set(&port, tr, i, want);
                if (drum808_get(&port, tr, i) != want) { printf("  track %d %s set/get  FAIL\n", tr, p->name); bad++; }
                drum808_set(&port, tr, i, p->def);
            }
        drum808_init(&port);
        drum808_set(&port, D8_LT, port_index(D8_LT, "Sound"), 1);
        if (drum808_get(&port, D8_LT, port_index(D8_LT, "Decay")) != 58) { printf("  conga keeps its own Decay  FAIL\n"); bad++; }
        n_fail += bad;
        printf("  %d tracks + kit: names <= 6, labels <= 5, defaults, set/get, per-sound pots: %s\n", D8_NUM, bad ? "FAIL" : "ok");
    }
    printf("%d cases, %d over %.0e; worst rel %.3e (%s)\n", n_cases, n_fail, TOL, worst_rel, worst_name);
    if (argc > 1 && demo(argv[1])) return 1;
    return n_fail ? 1 : 0;
}
