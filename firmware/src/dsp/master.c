/* SPDX-License-Identifier: GPL-3.0-only */
/* X0X master: a resonant multimode filter (ReBirth's PCF idea: sweep the whole mix)
 * and a soft limiter that keeps the sum of five parts out of the codec's clip.
 *
 * Filter: Zavalishin's TPT state-variable filter (topology-preserving, stable at any
 * cutoff), coefficients recomputed only when the pots move or on a 16-sample glide.
 * Limiter: peak follower with instant attack and ~150 ms release, gain applied
 * smoothly, then a tanh knee above -1 dBFS for whatever the follower lets through. */
#include "master.h"
#include "fastmath.h"

static const char *const MODE_N[] = {"OFF", "LP", "BP", "HP"};
static const char *const ONOFF_N[] = {"OFF", "ON"};
static const x0x_param_t MST_P[MST_NPARAMS] = {
    {"FILTER", 3, 0, MODE_N},
    {"CUTOFF", 127, 127, 0},
    {"RESO", 127, 20, 0},
    {"LIMIT", 1, 1, ONOFF_N},
};

int master_nparams(void) { return MST_NPARAMS; }
const x0x_param_t *master_param(int i) { return (i >= 0 && i < MST_NPARAMS) ? &MST_P[i] : 0; }

static void coefs(master_t *m)
{
    /* cutoff pot -> 30 Hz .. 18 kHz, exponential */
    float fc = 30.0f * fm_exp2f((float)m->pot[MST_CUTOFF] * (9.23f / 127.0f));
    float g = fm_tanf(FM_PI * fm_minf(fc, 19000.0f) / 44100.0f);
    float q = 0.5f + (float)m->pot[MST_RESO] * (11.5f / 127.0f);   /* Q 0.5 .. 12 */
    m->g_t = g;
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
    coefs(m);
    m->g = m->g_t;
}

void master_set(master_t *m, int i, int v)
{
    if (i < 0 || i >= MST_NPARAMS)
        return;
    m->pot[i] = (uint8_t)(v < 0 ? 0 : v > MST_P[i].max ? MST_P[i].max : v);
    coefs(m);
}

int master_get(const master_t *m, int i) { return (i >= 0 && i < MST_NPARAMS) ? m->pot[i] : 0; }

void master_process(master_t *m, float *x, int n, float volume)
{
    int i, mode = m->pot[MST_MODE];
    float k = m->k;
    for (i = 0; i < n; i++) {
        float s = x[i];
        if (mode) {
            float g = m->g += (m->g_t - m->g) * 0.0625f;    /* glide: no zipper on a fast sweep */
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
            float a = fm_fabsf(s);
            m->env = a > m->env ? a : m->env * 0.99985f;     /* ~150 ms release */
            {
                float want = m->env > 0.8f ? 0.8f / m->env : 1.0f;
                m->gain += (want - m->gain) * (want < m->gain ? 0.5f : 0.002f);
            }
            s *= m->gain;
            if (s > 0.89f || s < -0.89f)                     /* above -1 dBFS: soft knee to 1.0 */
                s = s > 0.0f ? 0.89f + 0.11f * fm_tanhf((s - 0.89f) * 9.0f)
                             : -0.89f - 0.11f * fm_tanhf((-s - 0.89f) * 9.0f);
        } else {
            s = fm_clampf(s, -1.0f, 1.0f);
        }
        x[i] = s;
    }
}
