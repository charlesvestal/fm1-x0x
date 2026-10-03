/* SPDX-License-Identifier: GPL-3.0-only */
/* TB-3PO generator; see tb3po.h. Integer and float maths only, no libm. */
#include "tb3po.h"

typedef struct { int8_t deg[7]; uint8_t len; } scale_t;

static const scale_t SCALES[TB3PO_NSCALES] = {     /* schwung-tb3po's SCALES, same order */
    {{0, 2, 3, 5, 7, 8, 10}, 7},                     /* Minor */
    {{0, 1, 3, 5, 7, 8, 10}, 7},                     /* Phrygian */
    {{0, 2, 3, 5, 7, 8, 11}, 7},                     /* HarmMinor */
    {{0, 3, 5, 7, 10, 0, 0}, 5},                     /* MinPent */
    {{0, 2, 3, 5, 7, 9, 10}, 7},                     /* Dorian */
    {{0, 2, 4, 5, 7, 9, 11}, 7},                     /* Major */
};
const char *const TB3PO_SCALE_NAMES[TB3PO_NSCALES] = {"MIN", "PHRY", "HARM", "MPEN", "DOR", "MAJ"};

static uint32_t rng_u32(uint32_t *r)                 /* xorshift32, as schwung-tb3po */
{
    uint32_t x = *r;
    if (x == 0)
        x = 1;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *r = x;
    return x;
}

static float rng_f(uint32_t *r)                      /* [0, 1) */
{
    return (float)(rng_u32(r) & 0xFFFFFFu) / (float)0x1000000;
}

void tb3po_defaults(tb3po_cfg_t *g, uint32_t seed)
{
    g->density = 70;                                 /* schwung-tb3po: 0.7 / 0.4 / 0.25, 2 octaves, A minor */
    g->accent = 40;
    g->slide = 25;
    g->oct_range = 2;
    g->root = 9;
    g->scale = 0;
    g->base_oct = 1;                                 /* note 24 + root: schwung-tb3po's base */
    g->mutate_bars = 0;
    g->seed = seed ? seed : 0xBEEF;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static uint8_t note_of(const tb3po_cfg_t *g, int degree, int oct)
{
    const scale_t *sc = &SCALES[g->scale < TB3PO_NSCALES ? g->scale : 0];
    return (uint8_t)clampi(12 * (g->base_oct + 1) + g->root + sc->deg[degree] + 12 * oct, 0, 127);
}

static int roll_degree(uint32_t *r, const scale_t *sc)
{
    int d = (int)(rng_f(r) * (float)sc->len);
    return d >= sc->len ? sc->len - 1 : d;
}

static int roll_oct(uint32_t *r, int range)
{
    int o = (int)(rng_f(r) * (float)range);
    return o >= range ? range - 1 : o;
}

/* step kind -> flags, schwung-tb3po's STEP_NOTE / ACCENT / SLIDE */
static uint8_t roll_kind(uint32_t *r, const tb3po_cfg_t *g)
{
    uint8_t f = G_NOTE;
    if (rng_f(r) < (float)g->accent / 100.0f)
        f = G_NOTE | BS_ACCENT;
    if (rng_f(r) < (float)g->slide / 100.0f)
        f = G_NOTE | BS_SLIDE;                       /* in schwung-tb3po a SLIDE step is not also an ACCENT */
    return f;
}

void tb3po_generate(bpart_t *b)
{
    tb3po_cfg_t *g = &b->gen;
    const scale_t *sc = &SCALES[g->scale < TB3PO_NSCALES ? g->scale : 0];
    int range = clampi(g->oct_range, 1, 3), len = clampi(b->len, 1, NSTEPS), i, any = 0;
    uint32_t r = g->seed ? g->seed : 1;
    for (i = 0; i < NSTEPS; i++) {
        b->step[i].flags = G_REST;
        b->step[i].note = note_of(g, 0, 0);
    }
    for (i = 0; i < len; i++) {
        int deg, oct;
        if (rng_f(&r) > (float)g->density / 100.0f)
            continue;                                /* rest */
        if (i % 4 == 0 && rng_f(&r) < 0.35f)
            deg = 0;                                 /* on the beat: the root, often */
        else
            deg = roll_degree(&r, sc);
        oct = roll_oct(&r, range);
        b->step[i].note = note_of(g, deg, oct);
        b->step[i].flags = roll_kind(&r, g);
    }
    for (i = 0; i < len; i++) {                      /* a slide into a rest does nothing */
        int nx = (i + 1) % len;
        if ((b->step[i].flags & BS_SLIDE) && bstep_gate(&b->step[nx]) == G_REST)
            b->step[i].flags = G_NOTE;
        if (bstep_gate(&b->step[i]) != G_REST)
            any = 1;
    }
    if (!any) {
        b->step[0].note = note_of(g, 0, 0);
        b->step[0].flags = G_NOTE;
    }
}

void tb3po_mutate(bpart_t *b, uint32_t *rng)
{
    tb3po_cfg_t *g = &b->gen;
    const scale_t *sc = &SCALES[g->scale < TB3PO_NSCALES ? g->scale : 0];
    int range = clampi(g->oct_range, 1, 3), len = clampi(b->len, 1, NSTEPS), i;
    uint32_t r = *rng ? *rng : (g->seed ? g->seed : 1);
    for (i = 0; i < len; i++) {
        if (rng_f(&r) >= 0.25f)
            continue;
        if (rng_f(&r) < 0.5f) {                      /* re-roll the step */
            if (rng_f(&r) > (float)g->density / 100.0f)
                b->step[i].flags = G_REST;
            else
                b->step[i].flags = roll_kind(&r, g);
        } else {                                     /* re-roll the note */
            int deg = roll_degree(&r, sc), oct = roll_oct(&r, range);
            b->step[i].note = note_of(g, deg, oct);
        }
    }
    for (i = 0; i < len; i++) {
        int nx = (i + 1) % len;
        if ((b->step[i].flags & BS_SLIDE) && bstep_gate(&b->step[nx]) == G_REST)
            b->step[i].flags = G_NOTE;
    }
    *rng = r;
}

uint32_t tb3po_new_seed(uint32_t entropy)
{
    uint32_t r = entropy * 2654435761u + 0x9E3779B9u;
    rng_u32(&r);
    rng_u32(&r);
    return r ? r : 1;
}
