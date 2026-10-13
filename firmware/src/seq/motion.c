/* SPDX-License-Identifier: GPL-3.0-only */
/* X0X motion; see motion.h. */
#include "motion.h"

void motion_init(motion_t *m, lane_t *lanes)
{
    uint32_t i;
    uint8_t *z = (uint8_t *)m;
    for (i = 0; i < sizeof *m; i++)
        z[i] = 0;
    m->lane = lanes;
    for (i = 0; i < NLANE; i++)
        m->rt[i].applied = m->rt[i].rec_last = MOT_NONE;
}

static void put(motion_t *m, const lane_t *l, lane_rt_t *r, int v)
{
    if (r->applied != v) {
        m->apply(m->ctx, l->t, l->v, l->i, v);
        r->applied = (uint8_t)v;
    }
}

static void to_base(motion_t *m, const lane_t *l, lane_rt_t *r)
{
    if (r->applied != MOT_NONE)
        m->apply(m->ctx, l->t, l->v, l->i, m->base(m->ctx, l->t, l->v, l->i));
    r->applied = MOT_NONE;
    r->ramp = 0;
}

void motion_step(motion_t *m, int part, int pat, int step, int next, int pass)
{
    int k;
    if (part < 0 || part >= MOT_PARTS || step < 0 || step >= NSTEPS)
        return;
    if (m->age[part])
        m->len[part][m->last[part] & 1u] = m->age[part];
    m->age[part] = 0;
    m->last[part] = (uint8_t)step;
    if (next < 0 || next >= NSTEPS)
        next = 0;
    for (k = 0; k < NLANE; k++) {
        lane_t *l = &m->lane[k];
        lane_rt_t *r = &m->rt[k];
        int v, nv;
        if (!l->used || l->part != part)
            continue;
        if (r->gen != l->gen) {                        /* a reused slot: a fresh runtime */
            r->gen = l->gen;
            r->applied = MOT_NONE;
            r->ramp = 0;
            r->rec_last = MOT_NONE;
            r->hold_left = 0;
        }
        if (l->pat != pat) {                           /* not this part's pattern now: the knob */
            to_base(m, l, r);
            r->rec_last = MOT_NONE;
            r->hold_left = 0;
            continue;
        }
        if (r->hold_req) {
            r->hold_req = 0;
            r->hold_left = (uint8_t)pass;
        }
        if (r->rec_req) {                              /* the knob moved: this step takes its value */
            int now = m->base(m->ctx, l->t, l->v, l->i), d, j;
            r->rec_req = 0;
            if (pass < 1)
                pass = 1;
            if (r->rec_last != MOT_NONE) {             /* a short pause in the gesture: a straight line */
                int from = l->val[r->rec_last];
                d = (step - r->rec_last + pass) % pass;
                if (d >= 2 && d <= MOT_GAP && from != MOT_NONE)
                    for (j = 1; j < d; j++)
                        l->val[(r->rec_last + j) % pass] = (uint8_t)(from + (now - from) * j / d);
            }
            l->val[step] = (uint8_t)now;
            r->rec_last = (uint8_t)step;
            r->rec_quiet = 0;
            r->hold_left = 0;
            r->applied = MOT_NONE;
            r->ramp = 0;
            continue;
        }
        if (r->rec_last != MOT_NONE && ++r->rec_quiet > MOT_GAP)
            r->rec_last = MOT_NONE;                    /* the gesture is over */
        if (r->rec_last != MOT_NONE || r->hold_left) { /* the knob has it */
            if (r->hold_left)
                r->hold_left--;
            r->applied = MOT_NONE;
            r->ramp = 0;
            continue;
        }
        v = l->val[step];
        put(m, l, r, v == MOT_NONE ? m->base(m->ctx, l->t, l->v, l->i) : v);
        nv = l->val[next];
        r->ramp = (uint8_t)(v != MOT_NONE && nv != MOT_NONE && nv != v);
        r->from = (uint8_t)v;
        r->to = (uint8_t)nv;
        r->part_parity = (uint8_t)(step & 1);
    }
}

void motion_tick(motion_t *m, uint32_t n)
{
    int k, p;
    for (p = 0; p < MOT_PARTS; p++)
        m->age[p] += n;
    for (k = 0; k < NLANE; k++) {
        const lane_t *l = &m->lane[k];
        lane_rt_t *r = &m->rt[k];
        uint32_t L, a;
        if (!r->ramp || !l->used || r->gen != l->gen || l->part >= MOT_PARTS)
            continue;
        L = m->len[l->part][r->part_parity];
        if (!L)
            continue;                                  /* the first step: no length yet */
        a = m->age[l->part];
        if (a > L)
            a = L;
        put(m, l, r, (int)r->from + ((int)r->to - (int)r->from) * (int)a / (int)L);
    }
}

void motion_release(motion_t *m)
{
    int k, p;
    for (k = 0; k < NLANE; k++) {
        lane_rt_t *r = &m->rt[k];
        if (m->lane[k].used && r->gen == m->lane[k].gen)
            to_base(m, &m->lane[k], r);
        r->rec_last = MOT_NONE;
        r->hold_left = 0;
    }
    for (p = 0; p < MOT_PARTS; p++)
        m->age[p] = m->len[p][0] = m->len[p][1] = 0;
}

int motion_value(const motion_t *m, int k)
{
    if (k < 0 || k >= NLANE || !m->lane[k].used || m->rt[k].gen != m->lane[k].gen)
        return -1;
    return m->rt[k].applied == MOT_NONE ? -1 : m->rt[k].applied;
}

int motion_find(const lane_t *L, int pat, int part, int t, int v, int i)
{
    int k;
    for (k = 0; k < NLANE; k++)
        if (L[k].used && L[k].pat == pat && L[k].part == part && L[k].t == t && L[k].v == v && L[k].i == i)
            return k;
    return -1;
}

int motion_alloc(lane_t *L, int pat, int part, int t, int v, int i)
{
    int k, s;
    for (k = 0; k < NLANE; k++)
        if (!L[k].used)
            break;
    if (k == NLANE)
        return -1;
    L[k].pat = (uint8_t)pat;
    L[k].part = (uint8_t)part;
    L[k].t = (uint8_t)t;
    L[k].v = (uint8_t)v;
    L[k].i = (uint8_t)i;
    for (s = 0; s < NSTEPS; s++)
        L[k].val[s] = MOT_NONE;
    L[k].gen++;
    __asm__ volatile("" ::: "memory");                 /* the fields before `used`: the ISR reads it first */
    L[k].used = 1;
    return k;
}

void motion_clear(lane_t *L, int k)
{
    if (k >= 0 && k < NLANE)
        L[k].used = 0;
}

int motion_count(const lane_t *L)
{
    int k, n = 0;
    for (k = 0; k < NLANE; k++)
        n += L[k].used != 0;
    return n;
}

void motion_req_rec(motion_t *m, int k)
{
    if (k >= 0 && k < NLANE)
        m->rt[k].rec_req = 1;
}

void motion_req_hold(motion_t *m, int k)
{
    if (k >= 0 && k < NLANE)
        m->rt[k].hold_req = 1;
}
