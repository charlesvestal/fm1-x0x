/* SPDX-License-Identifier: GPL-3.0-only */
/* X0X master: bus compressor (+ PUMP), resonant multimode filter, volume, limiter.
 *
 * Compressor: feed-forward, peak detector, gain computed in dB with a 6 dB soft knee
 * (Giannoulis / Massberg / Reiss's "smooth decoupled" design: the gain reduction itself is
 * smoothed, attack and release independent), makeup gain and a dry / wet mix for parallel
 * compression. At RATIO 1:1 with PUMP at 0 the stage is bypassed sample for sample.
 * PUMP: a 909 / 808 kick ducks the whole mix by its depth, rising in 3 ms and swelling back
 * with the release: the sidechain pump of these styles, keyed by the sequencer rather than by
 * audio, so it needs no kick on the master and has no detector lag.
 *
 * Filter: Zavalishin's TPT state-variable filter (stable at any cutoff), the coefficient
 * glides so a fast sweep does not zipper (ReBirth's PCF idea: sweep the whole mix).
 * Limiter: instant-attack peak follower, ~150 ms release, then a tanh knee above -1 dBFS. */
#include "master.h"
#include "fastmath.h"

#define FS 44100.0f

static const char *const RATIO_N[] = {"1:1", "1.5:1", "2:1", "3:1", "4:1", "6:1", "8:1", "12:1", "20:1", "INF"};
static const float RATIO_V[] = {1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f, 12.0f, 20.0f, 1e6f};
static const char *const SRC_N[] = {"909", "808", "BOTH"};
static const char *const MODE_N[] = {"OFF", "LP", "BP", "HP"};
static const char *const ONOFF_N[] = {"OFF", "ON"};
static const x0x_param_t MST_P[MST_NPARAMS] = {
    {"THRESH", 127, 95, 0},           /* -48 .. 0 dB: 95 = -12 dB */
    {"RATIO", 9, 0, RATIO_N},         /* 1:1 = off */
    {"ATTACK", 127, 60, 0},           /* 0.1 .. 100 ms, exponential: 60 = ~2.6 ms */
    {"RELEAS", 127, 60, 0},           /* 10 .. 1500 ms, exponential: 60 = ~110 ms */
    {"MAKEUP", 127, 0, 0},            /* 0 .. 24 dB */
    {"MIX", 127, 127, 0},             /* dry .. wet */
    {"PUMP", 127, 0, 0},              /* 0 .. 24 dB of kick-keyed ducking */
    {"PUMP BY", 2, 0, SRC_N},
    {"FILTER", 3, 0, MODE_N},
    {"CUTOFF", 127, 127, 0},
    {"RESO", 127, 20, 0},
    {"LIMIT", 1, 1, ONOFF_N},
};

int master_nparams(void) { return MST_NPARAMS; }
const x0x_param_t *master_param(int i) { return (i >= 0 && i < MST_NPARAMS) ? &MST_P[i] : 0; }

static float thresh_db(int p) { return -48.0f + 48.0f * (float)p / 127.0f; }
static float attack_ms(int p) { return 0.1f * fm_exp2f((float)p * (9.966f / 127.0f)); }     /* 0.1 .. 100 */
static float release_ms(int p) { return 10.0f * fm_exp2f((float)p * (7.229f / 127.0f)); }   /* 10 .. 1500 */
static float makeup_db(int p) { return 24.0f * (float)p / 127.0f; }
static float pump_db(int p) { return 24.0f * (float)p / 127.0f; }

/* PUMP's duck rises over this: 12 ms lets the kick's attack through and then ducks. At 3 ms it
 * dropped the whole mix 4 dB inside a cycle of the bass: measured, the fast gain movement at the
 * kick was 6 dB louder than at 10 ms, and it was heard as a click on every kick. */
#ifndef MASTER_PUMP_RISE_MS
#define MASTER_PUMP_RISE_MS 12.0f
#endif

/* one-pole coefficient reaching 1 - 1/e of a step in t milliseconds */
static float coef_ms(float t) { return fm_expf(-1000.0f / (t * FS)); }

static void comp_coefs(master_t *m)
{
    float ratio = RATIO_V[m->pot[MST_RATIO] % 10];
    m->thr_db = thresh_db(m->pot[MST_THRESH]);
    m->slope = 1.0f - 1.0f / ratio;
    m->knee = 6.0f;
    m->a_att = coef_ms(attack_ms(m->pot[MST_ATTACK]));
    m->a_rel = coef_ms(release_ms(m->pot[MST_RELEASE]));
    m->a_det = coef_ms(8.0f);
    m->makeup = fm_db2lin(makeup_db(m->pot[MST_MAKEUP]));
    m->mix = (float)m->pot[MST_MIX] / 127.0f;
    m->pump_db = pump_db(m->pot[MST_PUMP]);
    m->a_pump = coef_ms(MASTER_PUMP_RISE_MS);
    m->comp_on = m->pot[MST_RATIO] != 0 || m->pot[MST_PUMP] != 0;
}

static void filt_coefs(master_t *m)
{
    float fc = 30.0f * fm_exp2f((float)m->pot[MST_CUTOFF] * (9.23f / 127.0f));   /* 30 Hz .. 18 kHz */
    float q = 0.5f + (float)m->pot[MST_RESO] * (11.5f / 127.0f);
    m->g_t = fm_tanf(FM_PI * fm_minf(fc, 19000.0f) / FS);
    m->k = 1.0f / q;
}

void master_init(master_t *m)
{
    int i;
    uint8_t *z = (uint8_t *)m;
    for (i = 0; i < (int)sizeof *m; i++)
        z[i] = 0;
    for (i = 0; i < MST_NPARAMS; i++)
        m->pot[i] = MST_P[i].def;
    m->gain = 1.0f;
    comp_coefs(m);
    filt_coefs(m);
    m->g = m->g_t;
}

void master_set(master_t *m, int i, int v)
{
    if (i < 0 || i >= MST_NPARAMS)
        return;
    m->pot[i] = (uint8_t)(v < 0 ? 0 : v > MST_P[i].max ? MST_P[i].max : v);
    if (i <= MST_PUMPSRC)
        comp_coefs(m);
    else
        filt_coefs(m);
}

int master_get(const master_t *m, int i) { return (i >= 0 && i < MST_NPARAMS) ? m->pot[i] : 0; }

void master_key(master_t *m, int kit, float vel)
{
    int src = m->pot[MST_PUMPSRC];
    if (!m->pot[MST_PUMP] || (src == PUMP_909 && kit != 0) || (src == PUMP_808 && kit != 1))
        return;
    m->pump_tgt = m->pump_db * (0.5f + 0.5f * vel);   /* an accented kick pumps deeper */
}

/* static curve: gain reduction (dB, >= 0) for a level (dB), soft knee */
static float gr_of(const master_t *m, float lv)
{
    float over = lv - m->thr_db, h = 0.5f * m->knee;
    if (over <= -h)
        return 0.0f;
    if (over >= h)
        return m->slope * over;
    over += h;
    return m->slope * over * over / (2.0f * m->knee);
}

void master_process(master_t *m, float *x, int n, float volume)
{
    int i, mode = m->pot[MST_MODE];
    float k = m->k, grmax = 0.0f;
    for (i = 0; i < n; i++) {
        float s = x[i];
        if (m->comp_on) {
            float lv, want, total, wet;
            /* the level: mean square over ~8 ms, x2 so a sine reads its peak. Read sample by sample
             * (|x|), the reduction chased every cycle of a bass note and grabbed each kick within a
             * millisecond: audio-rate gain movement, heard as clicks */
            m->ms = fm_flush(m->a_det * m->ms + (1.0f - m->a_det) * s * s);
            lv = 2.0f * m->ms;
            want = (lv > 1e-12f && m->slope > 0.0f) ? gr_of(m, 3.0103f * fm_log2f(lv)) : 0.0f;
            m->gr = want > m->gr ? m->a_att * m->gr + (1.0f - m->a_att) * want
                                 : m->a_rel * m->gr + (1.0f - m->a_rel) * want;
            if (m->pump_tgt > m->pump) {                     /* PUMP: rise to the kick's depth ... */
                m->pump = m->a_pump * m->pump + (1.0f - m->a_pump) * m->pump_tgt;
                if (m->pump_tgt - m->pump < 0.05f)
                    m->pump_tgt = 0.0f;                      /* ... then let go: the release swells back */
            } else {
                m->pump = fm_flush(m->a_rel * m->pump);
            }
            total = m->gr + m->pump;
            if (total > grmax)
                grmax = total;
            wet = s * m->makeup * fm_db2lin(-total);
            s = s + (wet - s) * m->mix;
        }
        if (mode) {
            float g = m->g += (m->g_t - m->g) * 0.0625f;
            float a1 = 1.0f / (1.0f + g * (g + k));
            float v3 = s - m->ic2;
            float v1 = a1 * m->ic1 + g * a1 * v3;
            float v2 = m->ic2 + g * v1;
            m->ic1 = fm_flush(2.0f * v1 - m->ic1);
            m->ic2 = fm_flush(2.0f * v2 - m->ic2);
            s = mode == 1 ? v2 : mode == 2 ? v1 : s - k * v1 - v2;
        }
        s *= volume;
        if (m->pot[MST_LIMIT]) {
            /* look ahead MST_LA samples: a peak is seen when it enters, the gain eases down over the
             * MST_LA samples it takes to come out, and is held for them. (It used to halve the
             * distance every sample: a step in the waveform, a click on every limited kick.) */
            float a = fm_fabsf(s), d;
            if (a >= m->env) {
                m->env = a;
                m->hold = MST_LA;
            } else if (m->hold) {
                m->hold--;
            } else {
                m->env *= 0.99985f;
            }
            {
                float want = m->env > 0.8f ? 0.8f / m->env : 1.0f;
                m->gain += (want - m->gain) * (want < m->gain ? (5.0f / MST_LA) : 0.002f);
            }
            d = m->la[m->la_pos];
            m->la[m->la_pos] = s;
            m->la_pos = (m->la_pos + 1) % MST_LA;
            s = d * m->gain;
            if (s > 0.89f || s < -0.89f)
                s = s > 0.0f ? 0.89f + 0.11f * fm_tanhf((s - 0.89f) * 9.0f)
                             : -0.89f - 0.11f * fm_tanhf((-s - 0.89f) * 9.0f);
        } else {
            s = fm_clampf(s, -1.0f, 1.0f);
        }
        x[i] = s;
    }
    m->gr_view = grmax > m->gr_view ? grmax : m->gr_view * 0.93f;   /* the meter: peak, falling */
}

static void put_num(char *b, float v, int decimals, const char *unit)
{
    int neg = v < 0.0f, n = 0, i;
    char t[16];
    uint32_t scale = decimals ? 10u : 1u, u = (uint32_t)((neg ? -v : v) * (float)scale + 0.5f);
    if (decimals) {
        t[n++] = (char)('0' + u % 10u);
        t[n++] = '.';
        u /= 10u;
    }
    do {
        t[n++] = (char)('0' + u % 10u);
        u /= 10u;
    } while (u);
    i = 0;
    if (neg)
        b[i++] = '-';
    while (n)
        b[i++] = t[--n];
    while (*unit)
        b[i++] = *unit++;
    b[i] = 0;
}

void master_format(const master_t *m, int i, char *b)
{
    int p = m->pot[i];
    switch (i) {
    case MST_THRESH: put_num(b, thresh_db(p), thresh_db(p) > -10.0f ? 1 : 0, "dB"); return;
    case MST_ATTACK: { float t = attack_ms(p); put_num(b, t, t < 10.0f ? 1 : 0, "ms"); return; }
    case MST_RELEASE: put_num(b, release_ms(p), 0, "ms"); return;
    case MST_MAKEUP: put_num(b, makeup_db(p), 1, "dB"); return;
    case MST_PUMP: put_num(b, pump_db(p), 1, "dB"); return;
    case MST_MIX: put_num(b, (float)p * (100.0f / 127.0f), 0, "%"); return;
    case MST_CUTOFF: {
        float fc = 30.0f * fm_exp2f((float)p * (9.23f / 127.0f));
        if (fc >= 1000.0f)
            put_num(b, fc / 1000.0f, 1, "k");
        else
            put_num(b, fc, 0, "Hz");
        return;
    }
    default: {
        const x0x_param_t *d = &MST_P[i];
        if (d->names) {
            const char *s = d->names[p];
            while ((*b = *s++) != 0)
                b++;
        } else
            put_num(b, (float)p, 0, "");
        return;
    }
    }
}
