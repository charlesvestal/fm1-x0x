/* SPDX-License-Identifier: GPL-3.0-only */
/* X0X audio engine; see engine.h. */
#include "engine.h"
#include "plat.h"
#include "../dsp/drum909.h"
#include "../dsp/drum808.h"
#include "../dsp/bass303.h"
#include "../dsp/breaks.h"
#include "../dsp/fxbus.h"
#include "../dsp/master.h"
#include "../dsp/fastmath.h"

#define POOL X0X_POOL

/* time per stage of the render, in plat_cycles() units (the CPU's cycles, or the 24 MHz timer):
 * running totals (wrapping; the PERF screen takes differences) and the frames rendered. */
volatile uint32_t eng_prof[ENG_PROF_N], eng_prof_frames;
#define PROF_T(v) uint32_t v = plat_cycles()
#define PROF_ADD(k, t0) (eng_prof[k] += plat_cycles() - (t0))

/* ---------------------------------------------------------------- state --- */
seq_t seq;
volatile uint8_t eng_step[NTRACKS];
volatile uint16_t eng_peak[NPARTS];
int16_t eng_scope[SCOPE_N];
volatile uint32_t eng_scope_w;

static drum909_t d909 POOL;
static drum808_t d808 POOL;
static bass303_t b303[NBASS] POOL;
static breaks_t brk POOL;
static fxbus_t fx POOL;
static master_t mst;
#define DLY_LEN 88200
static int16_t dly_buf[DLY_LEN] POOL;
static volatile uint8_t brk_hold;      /* main loop is rewriting the break loops: ISR leaves it alone */
static motion_t mot;
static const sound_t *mot_base_snd;    /* the knobs' own values (the project's mirror) */
static uint8_t mot_playing;
static volatile uint8_t mot_on = 1;
static uint8_t mot_was_on = 1;
void engine_motion_enable(int on) { mot_on = (uint8_t)(on != 0); }   /* the ISR releases the knobs */
static volatile uint8_t mot_release_req;
void engine_motion_reset(void) { mot_release_req = 1; }   /* lanes changed under it (undo): knobs back */

/* overload guard: past the CPU (a block over 92%, or 85% on average) the two 303s drop their
 * oversampling (~12% of the CPU: a little aliasing on bright notes) and the 808's voices end at
 * -50 dB instead of -90 (~2%); after 3 s back under 65% both return. The platform reports each
 * block's load after rendering it (engine_load, same context as engine_render); the change is
 * made at the start of the next block. Off during the PERF test, which measures the full cost. */
#define GUARD_ON_LAST 92u
#define GUARD_ON_AVG 85u
#define GUARD_OFF 65u
#define GUARD_CALM_FRAMES (3u * 44100u)
extern float drum808_quiet;
static uint8_t guard_on, guard_applied, guard_enabled = 1;
static uint32_t guard_avg_q8, guard_calm;
volatile uint32_t eng_guard_count;
void engine_guard_enable(int on) { guard_enabled = (uint8_t)(on != 0); }
int engine_guard_active(void) { return guard_on; }
void engine_load(uint32_t pct, uint32_t frames)
{
    guard_avg_q8 = (guard_avg_q8 * 15u + pct * 256u) / 16u;
    if (!guard_enabled) {
        guard_on = 0;
        guard_calm = 0;
    } else if (!guard_on) {
        if (pct > GUARD_ON_LAST || guard_avg_q8 > GUARD_ON_AVG * 256u) {
            guard_on = 1;
            guard_calm = 0;
            eng_guard_count++;
        }
    } else if (pct >= GUARD_OFF || guard_avg_q8 >= GUARD_OFF * 256u) {
        guard_calm = 0;
    } else if ((guard_calm += frames) >= GUARD_CALM_FRAMES) {
        guard_on = 0;
    }
}
static void guard_apply(void)
{
    int k;
    if (guard_on == guard_applied)
        return;
    guard_applied = guard_on;
    for (k = 0; k < NBASS; k++)
        bass303_set_lite(&b303[k], guard_on);
    drum808_quiet = guard_on ? 3.2e-3f : 3.2e-5f;
}

/* mixer: per part Level, Rev send, Dly send (a drum machine's add to its voices' own sends) */
static const x0x_param_t MIX_P[MX_NPARAMS] = {{"LEVEL", 127, 100, 0}, {"REV", 127, 0, 0}, {"DLY", 127, 0, 0},
                                               {"PAN", 127, 64, 0}};
static const char *const PART_NAMES[NPARTS] = {"909", "808", "303 A", "303 B", "BREAK"};
static float mix_level[NPARTS], mix_rev[NPARTS], mix_dly[NPARTS];
/* pan: a balance law. Centre is full level on both sides (a centred mix is the mono one exactly);
 * turning toward a side fades the other out along a quarter cosine, the near side stays at 1 */
static float mix_pl[NPARTS] = {1, 1, 1, 1, 1}, mix_pr[NPARTS] = {1, 1, 1, 1, 1};

/* ------------------------------------------------------- command queue --- */
/* uint32 commands, single producer (main loop) / single consumer (audio ISR):
 * type:4 | a:8 | b:8 | c:12 */
enum { C_PARAM_T0 = 0, C_DRUM = 8, C_BON, C_BOFF, C_BRK, C_NOP };
#define CQ 1024u
static uint32_t cq[CQ];
static volatile uint32_t cq_w, cq_r;

static void cq_put(uint32_t type, uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t w = cq_w;
    if (w - cq_r >= CQ)
        return;                        /* full: drop (a param flood; the mirror still has it) */
    cq[w % CQ] = (type << 28) | ((a & 0xFFu) << 20) | ((b & 0xFFu) << 12) | (c & 0xFFFu);
    __asm__ volatile("" ::: "memory");
    cq_w = w + 1;
}

/* ------------------------------------------------------------ params --- */
int engine_nvoices(int t)
{
    switch (t) {
    case T_909: return DR_NUM + 1;
    case T_808: return D8_NUM + 1;
    case T_303: return NBASS;
    case T_MIX: return NPARTS;
    default: return 1;
    }
}

int engine_nparams(int t, int v)
{
    switch (t) {
    case T_909: return drum909_nparams(v);
    case T_808: return drum808_nparams(v);
    case T_303: return bass303_nparams();
    case T_BRK: return 2;                                /* Level, Pitch (sends: the mixer's; the rest: the pattern's) */
    case T_FX: return fxbus_nparams();
    case T_MIX: return MX_NPARAMS;                       /* every part: Level, Rev send, Dly send */
    case T_MST: return master_nparams();
    default: return 0;
    }
}

const x0x_param_t *engine_param(int t, int v, int i)
{
    switch (t) {
    case T_909: return drum909_param(v, i);
    case T_808: return drum808_param(v, i);
    case T_303: return bass303_param(i);
    case T_BRK: return breaks_param(BRK_NSET + i);
    case T_FX: return fxbus_param(i);
    case T_MIX: return (i >= 0 && i < MX_NPARAMS) ? &MIX_P[i] : 0;
    case T_MST: return master_param(i);
    default: return 0;
    }
}

static const char *const N909[DR_NUM + 1] = {"BD", "SD", "LT", "MT", "HT", "RS", "CP", "CH", "OH", "CR", "RD", "KIT"};
static const char *const N808[D8_NUM + 1] = {"BD", "SD", "LT", "MT", "HT", "RS", "CP", "CB", "CY", "OH", "CH", "KIT"};

const char *engine_voice_name(int t, int v)
{
    switch (t) {
    case T_909: return (v >= 0 && v <= DR_NUM) ? N909[v] : "";
    case T_808: return (v >= 0 && v <= D8_NUM) ? N808[v] : "";
    case T_303: return v ? "303 B" : "303 A";
    case T_MIX: return (v >= 0 && v < NPARTS) ? PART_NAMES[v] : "";
    case T_BRK: return "BREAK";
    case T_FX: return "FX";
    case T_MST: return "MASTER";
    default: return "";
    }
}

static void apply_param(int t, int v, int i, int val)    /* ISR */
{
    switch (t) {
    case T_909: drum909_set(&d909, v, i, val); break;
    case T_808: drum808_set(&d808, v, i, val); break;
    case T_303: if (v >= 0 && v < NBASS) bass303_set(&b303[v], i, val); break;
    case T_BRK: breaks_set(&brk, BRK_NSET + i, val); break;
    case T_FX: fxbus_set(&fx, i, val); break;
    case T_MIX:
        if (v < 0 || v >= NPARTS)
            break;
        if (i == MX_LEVEL)
            mix_level[v] = (float)val * (float)val / (100.0f * 100.0f);     /* 100 = unity, square law */
        else if (i == MX_REV)
            mix_rev[v] = (float)val / 127.0f;
        else if (i == MX_DLY)
            mix_dly[v] = (float)val / 127.0f;
        else if (i == MX_PAN)
            x0x_pan_gains(val, &mix_pl[v], &mix_pr[v]);
        break;
    case T_MST: master_set(&mst, i, val); break;
    default: break;
    }
}

void engine_set(int t, int v, int i, int val) { cq_put((uint32_t)t, (uint32_t)v, (uint32_t)i, (uint32_t)val); }

void engine_sound_defaults(sound_t *s)
{
    int t, v, i;
    uint8_t *z = (uint8_t *)s;
    for (i = 0; i < (int)sizeof *s; i++)
        z[i] = 0;
    for (t = 0; t < NTARGETS; t++)
        for (v = 0; v < engine_nvoices(t) && v < NVOICES_MAX; v++)
            for (i = 0; i < engine_nparams(t, v) && i < NPARAMS_MAX; i++) {
                const x0x_param_t *p = engine_param(t, v, i);
                s->v[t][v][i] = p ? p->def : 0;
            }
    /* the factory mix: big beat (Chemical Brothers, Fatboy Slim), balanced by tools/mix_report.py
     * against a funk break, a 909 kick under it and a 303 line. The break leads, the kick sits
     * under it, the 303 screams through its RAT; the master squeezes and pumps. Each engine's
     * own voicing (its pot defaults) is left as its original; only these are X0X's. */
    s->v[T_MIX][PART_909][MX_LEVEL] = 75;            /* 9W9's kit is hot (it drives its own glue): -5 dB */
    s->v[T_909][DR_BD][3] = 61;                      /* its kick under the break, its clap and hats over */
    s->v[T_909][DR_BD][2] = 80;                      /* the kick: a little shorter, and driven. Its Attack */
    s->v[T_909][DR_BD][6] = 30;                      /* stays 9W9's 13: at 50 its tick was 12 dB louder, a click on every kick */
    s->v[T_909][DR_OH][1] = 60;
    s->v[T_MIX][PART_808][MX_LEVEL] = 112;
    s->v[T_808][D8_KIT][0] = 127;                    /* kit level */
    s->v[T_BRK][0][0] = 127;                         /* the break: full level, part a little over unity */
    s->v[T_MIX][PART_BRK][MX_LEVEL] = 120;
    s->v[T_MIX][PART_BRK][MX_REV] = 25;
    for (v = 0; v < NBASS; v++) {                    /* acid: low cutoff, high resonance, a long sweep, RAT */
        s->v[T_303][v][BASS303_CUTOFF] = 48;
        s->v[T_303][v][BASS303_RESO] = 100;
        s->v[T_303][v][BASS303_ENVMOD] = 85;
        s->v[T_303][v][BASS303_ACCENT] = 100;
        s->v[T_303][v][BASS303_VOLUME] = 127;
        s->v[T_303][v][BASS303_DRIVE] = 45;
        s->v[T_303][v][BASS303_DRVTYPE] = 2;
        s->v[T_MIX][PART_303A + v][MX_LEVEL] = 74;   /* the RAT makes it dense: -5 dB (square law) */
        s->v[T_MIX][PART_303A + v][MX_DLY] = v ? 60 : 80;
        s->v[T_MIX][PART_303A + v][MX_REV] = 12;
    }
    s->v[T_909][DR_CP][5] = 90;                      /* the clap in the room */
    s->v[T_FX][0][FX_DL_TYPE] = 1;                   /* tape delay, dotted eighths */
    s->v[T_MST][0][MST_RATIO] = 4;                   /* 4:1 */
    s->v[T_MST][0][MST_THRESH] = 64;                 /* -24 dB */
    s->v[T_MST][0][MST_MAKEUP] = 85;                 /* +16 dB: into the limiter */
    s->v[T_MST][0][MST_ATTACK] = 85;                 /* 10 ms: faster grabbed each kick hard enough to click */
    s->v[T_MST][0][MST_RELEASE] = 58;                /* 100 ms: it breathes with the beat */
    s->v[T_MST][0][MST_PUMP] = 21;                   /* 4 dB of 909-keyed pump */
}

void engine_sound_centre_drum_pans(sound_t *s)
{
    int t, v;
    for (t = T_909; t <= T_808; t++)
        for (v = 0; v < NDRUM && v < NVOICES_MAX; v++) {
            int i = engine_nparams(t, v) - 1;           /* PAN is every voice's last pot */
            const x0x_param_t *p = engine_param(t, v, i);
            if (p && p->def == 64 && p->max == 127)
                s->v[t][v][i] = 64;
        }
}

void engine_apply_sound(const sound_t *s)
{
    int t, v, i;
    for (t = 0; t < NTARGETS; t++)
        for (v = 0; v < engine_nvoices(t) && v < NVOICES_MAX; v++)
            for (i = 0; i < engine_nparams(t, v) && i < NPARAMS_MAX; i++)
                engine_set(t, v, i, s->v[t][v][i]);
}

void engine_drum(int kit, int voice, float vel)
{
    cq_put(C_DRUM, (uint32_t)kit, (uint32_t)voice, (uint32_t)(vel * 4095.0f));
}
void engine_bass_on(int part, int note, int accent, int slide)
{
    cq_put(C_BON, (uint32_t)part, (uint32_t)note, (uint32_t)((accent ? 1 : 0) | (slide ? 2 : 0)));
}
void engine_bass_off(int part) { cq_put(C_BOFF, (uint32_t)part, 0, 0); }
void engine_brk_live(int key, int down) { cq_put(C_BRK, (uint32_t)key, (uint32_t)(down != 0), 0); }

/* ------------------------------------------------------- sequencer sink --- */
static void s_drum(void *x, int kit, int v, float vel)
{
    (void)x;
    if (v == 0)                          /* BD (both kits' track 1): the master's PUMP key */
        master_key(&mst, kit, vel);
    if (kit == 0)
        drum909_trigger(&d909, v, vel);
    else                                 /* the 808's unaccented hit sits at D8_VEL_NORMAL, not at our 909 level */
        drum808_trigger(&d808, v, vel >= 0.999f ? 1.0f : vel * (D8_VEL_NORMAL / (88.0f / 127.0f)));
}
static void s_bon(void *x, int p, int note, int acc, int sl) { (void)x; bass303_note_on(&b303[p], note, acc, sl); }
static void s_boff(void *x, int p) { (void)x; bass303_note_off(&b303[p]); }
static uint8_t brk_applied[BRK_NSET];  /* the pattern settings the break was last given */
static void s_brk(void *x, int s16, int bar, float spb, int en)
{
    const brkpart_t *bp = &seq.pat[seq.ppat[TRK_BRK]].brk;
    int i;
    (void)x;
    if (brk_hold)
        return;
    breaks_set_quiet(&brk, bp->steps == 0 || (seq.mute & (1u << MUTE_BRK)));   /* unheard: stop decoding */
    for (i = 0; i < BRK_NSET; i++)       /* follow the playing pattern's settings (12 compares) */
        if (bp->set[i] != brk_applied[i]) {
            brk_applied[i] = bp->set[i];
            breaks_set(&brk, i, bp->set[i]);
        }
    breaks_step(&brk, s16, bar, spb, en);
}
static void s_brkstop(void *x) { (void)x; breaks_stop(&brk); }
static void s_midi(void *x, uint8_t st, uint8_t d1, uint8_t d2)
{
    uint32_t cin = st >= 0xF0 ? 0x0Fu : (uint32_t)(st >> 4);
    (void)x;
    plat_midi_out(cin | ((uint32_t)st << 8) | ((uint32_t)d1 << 16) | ((uint32_t)d2 << 24));
}
static void s_step(void *x, int t, int p)
{
    (void)x;
    eng_step[t] = (uint8_t)p;
    if (mot_on)
        motion_step(&mot, t, seq.ppat[t], p, seq_next_step(&seq, t, p), seq_part_len(&seq, t));
}

/* --------------------------------------------------------------- motion --- */
static int mot_base(void *x, int t, int v, int i)
{
    (void)x;
    return mot_base_snd ? mot_base_snd->v[t][v][i] : 0;
}
static void mot_apply(void *x, int t, int v, int i, int val) { (void)x; apply_param(t, v, i, val); }

int engine_motion_part(int t, int v)
{
    switch (t) {
    case T_909: return PART_909;
    case T_808: return PART_808;
    case T_303: return PART_303A + (v & 1);
    case T_BRK: return PART_BRK;
    case T_MIX: return v < NPARTS ? v : PART_909;
    default: return PART_909;                     /* FX, master: the bar */
    }
}
int engine_motion_value(int k) { return motion_value(&mot, k); }
void engine_motion_rec(int k) { motion_req_rec(&mot, k); }
void engine_motion_hold(int k) { motion_req_hold(&mot, k); }
static const seq_sink_t SINK = {s_drum, s_bon, s_boff, s_brk, s_brkstop, s_midi, s_step, 0};

/* ---------------------------------------------------------------- MIDI in --- */
static uint8_t bass_held[NBASS];       /* note held from MIDI, for legato = slide */

static void midi_in(uint32_t pkt)
{
    uint8_t st = (uint8_t)(pkt >> 8), d1 = (uint8_t)(pkt >> 16) & 0x7F, d2 = (uint8_t)(pkt >> 24) & 0x7F;
    uint8_t ch = st & 0x0F, ty = st & 0xF0;
    int k, v;
    switch (st) {
    case 0xF8: seq_ext_clock(&seq); return;
    case 0xFA: seq_start(&seq); return;
    case 0xFB: seq_continue(&seq); return;
    case 0xFC: seq_stop(&seq); return;
    default: break;
    }
    if (ty == 0x90 && d2 == 0)
        ty = 0x80;
    if ((ch == 9 || ch == 10) && ty == 0x90) {          /* ch 10 = 909, ch 11 = 808: GM drum notes */
        static const uint8_t GM[NDRUM] = {36, 38, 41, 45, 50, 37, 39, 42, 46, 49, 51};
        static const uint8_t GM8[NDRUM] = {36, 38, 41, 45, 50, 37, 39, 56, 49, 46, 42};
        k = ch - 9;
        for (v = 0; v < NDRUM; v++)
            if ((k ? GM8 : GM)[v] == d1)
                s_drum(0, k, v, (float)d2 / 127.0f);
        return;
    }
    if (ch == 1 || ch == 2) {                           /* ch 2 / 3 = 303 A / B */
        int b = ch - 1;
        if (ty == 0x90) {
            bass303_note_on(&b303[b], d1, d2 >= 100, bass_held[b] != 0);
            bass_held[b] = d1;
        } else if (ty == 0x80 && bass_held[b] == d1) {
            bass303_note_off(&b303[b]);
            bass_held[b] = 0;
        }
        return;
    }
    if (ch == 3 && (ty == 0x90 || ty == 0x80) && d1 >= 36 && d1 <= 43)   /* ch 4: break slices */
        breaks_live(&brk, d1 - 36, ty == 0x90);
}

/* ---------------------------------------------------------------- render --- */
static void drain(void)
{
    uint32_t r = cq_r, pkt;
    int guard = 64;
    while (r != cq_w) {
        uint32_t c = cq[r % CQ], type = c >> 28, a = (c >> 20) & 0xFFu, b = (c >> 12) & 0xFFu, v = c & 0xFFFu;
        r++;
        if (type < 8u)
            apply_param((int)type, (int)a, (int)b, (int)v);
        else if (type == C_DRUM)
            s_drum(0, (int)a, (int)b, (float)v / 4095.0f);
        else if (type == C_BON && a < NBASS)
            bass303_note_on(&b303[a], (int)b, (int)(v & 1u), (int)((v >> 1) & 1u));
        else if (type == C_BOFF && a < NBASS)
            bass303_note_off(&b303[a]);
        else if (type == C_BRK && !brk_hold)
            breaks_live(&brk, (int)a, (int)b);
    }
    cq_r = r;
    while (guard-- && plat_midi_in(&pkt))
        midi_in(pkt);
}

static void add_scaled(float *dst, const float *src, float g, uint32_t n)
{
    uint32_t i;
    if (g == 0.0f)
        return;
    for (i = 0; i < n; i++)
        dst[i] += src[i] * g;
}

static float part_peak[NPARTS];
static void meter(int p, const float *x, float g, uint32_t n)
{
    uint32_t i;
    float pk = part_peak[p];
    for (i = 0; i < n; i++) {
        float a = fm_fabsf(x[i]) * g;
        if (a > pk)
            pk = a;
    }
    part_peak[p] = pk;
}

/* dry into L and R by the part's pan; the sends stay mono (the reverb and delay make the width) */
static void add_panned(float *dl, float *dr, const float *src, int p, float g, uint32_t n)
{
    uint32_t i;
    float gl = g * mix_pl[p], gr = g * mix_pr[p];
    if (gl == gr) {                                       /* centred: one product for both sides */
        if (gl == 0.0f)
            return;
        for (i = 0; i < n; i++) {
            float x = src[i] * gl;
            dl[i] += x;
            dr[i] += x;
        }
        return;
    }
    for (i = 0; i < n; i++) {
        dl[i] += src[i] * gl;
        dr[i] += src[i] * gr;
    }
}

static void render_sub(float *out_l, float *out_r, uint32_t n)
{
    static float dry_l[256], dry_r[256], rev[256], dly[256], t0[256], t1[256], t2[256], tr[256];
    uint32_t i;
    int b;
    for (i = 0; i < n; i++)
        dry_l[i] = dry_r[i] = rev[i] = dly[i] = 0.0f;
    /* drum machines: their own per-voice sends; the part level scales all three buses. A silent
     * part is not cleared, mixed or metered (it would add zeros): the 909 is still called, for its
     * shared noise; the 808 does nothing while silent, so it is skipped. Exact either way. */
    for (b = 0; b < NKIT; b++) {
        PROF_T(pt);
        float g = mix_level[b];
        if (!(b == 0 ? drum909_active(&d909) : drum808_active(&d808))) {
            if (b == 0)
                drum909_render(&d909, t0, t1, t2, (int)n);   /* writes nothing; advances the noise */
            PROF_ADD(b, pt);
            continue;
        }
        for (i = 0; i < n; i++)
            t0[i] = t1[i] = t2[i] = tr[i] = 0.0f;
        if (b == 0)                                       /* each voice placed by its own PAN */
            drum909_render_st(&d909, t0, tr, t1, t2, (int)n);
        else
            drum808_render_st(&d808, t0, tr, t1, t2, (int)n);
        {                                                 /* then the part's PAN over the kit */
            float gl = g * mix_pl[b], gr = g * mix_pr[b];
            for (i = 0; i < n; i++) {
                dry_l[i] += t0[i] * gl;
                dry_r[i] += tr[i] * gr;
            }
        }
        add_scaled(rev, t1, g, n);
        add_scaled(dly, t2, g, n);
        for (i = 0; i < n; i++)                           /* the kit send: the whole machine, both sides */
            t0[i] = (t0[i] + tr[i]) * 0.5f;               /* (centred: t0 exactly) */
        add_scaled(rev, t0, g * mix_rev[b], n);
        add_scaled(dly, t0, g * mix_dly[b], n);
        meter(b, t0, g, n);
        PROF_ADD(b, pt);
    }
    for (b = 0; b < NBASS; b++) {
        int p = PART_303A + b;
        PROF_T(pt);
        int was_idle = b303[b].idle;                      /* idle: the render writes zeros */
        bass303_render(&b303[b], t0, (int)n);
        if (was_idle) {
            PROF_ADD(2 + b, pt);
            continue;
        }
        add_panned(dry_l, dry_r, t0, p, mix_level[p], n);
        add_scaled(rev, t0, mix_level[p] * mix_rev[p], n);
        add_scaled(dly, t0, mix_level[p] * mix_dly[p], n);
        meter(p, t0, mix_level[p], n);
        PROF_ADD(2 + b, pt);
    }
    if (!brk_hold) {
        PROF_T(pt);
        float g = mix_level[PART_BRK];
        int silent = breaks_silent(&brk);              /* the render writes zeros: nothing to mix */
        breaks_render(&brk, t0, (int)n);
        if (!silent) {
            add_panned(dry_l, dry_r, t0, PART_BRK, g, n);
            add_scaled(rev, t0, g * mix_rev[PART_BRK], n);
            add_scaled(dly, t0, g * mix_dly[PART_BRK], n);
            meter(PART_BRK, t0, g, n);
        }
        PROF_ADD(4, pt);
    }
    {
        PROF_T(pt);
        fxbus_process_st(&fx, dry_l, dry_r, rev, dly, out_l, out_r, (int)n);
        PROF_ADD(5, pt);
    }
}

void engine_render(int32_t *out_lr, uint32_t n)
{
    static float out_l[256], out_r[256];
    uint32_t done = 0, i;
    float vol = (float)plat_master() / 4096.0f;
    int p;
    if (n > 256u)
        n = 256u;
    eng_prof_frames += n;
    if ((mot_was_on && !mot_on) || mot_release_req) {   /* motion off, or its lanes replaced: knobs back */
        mot_release_req = 0;
        motion_release(&mot);
    }
    mot_was_on = mot_on;
    guard_apply();
    {
        PROF_T(pt);
        drain();
        if (mot_on)
            motion_tick(&mot, n);
        fxbus_set_bpm(&fx, seq_tempo(&seq));
        seq_advance(&seq, 0, &SINK);
        PROF_ADD(7, pt);
    }
    while (done < n) {
        uint32_t k = seq_until_event(&seq, n - done);
        render_sub(out_l + done, out_r + done, k);
        {
            PROF_T(pt);
            seq_advance(&seq, k, &SINK);
            PROF_ADD(7, pt);
        }
        done += k;
    }
    if (mot_playing && !seq.playing)                     /* stopped: every knob back to its own value */
        motion_release(&mot);
    mot_playing = seq.playing;
    {
        PROF_T(pt);
        master_process_st(&mst, out_l, out_r, (int)n, vol);
        PROF_ADD(6, pt);
    }
    for (i = 0; i < n; i++) {
        out_lr[2u * i] = (int32_t)(out_l[i] * 4194303.0f);   /* 2^22: Felucca's -6 dBFS ceiling of the 24-bit codec */
        out_lr[2u * i + 1u] = (int32_t)(out_r[i] * 4194303.0f);
        if (i & 1u)
            eng_scope[eng_scope_w++ & (SCOPE_N - 1u)] = (int16_t)((out_l[i] + out_r[i]) * 16383.5f);
    }
    for (p = 0; p < NPARTS; p++) {                      /* meters: peak hold, ~300 ms decay at the UI */
        uint32_t q = (uint32_t)(fm_minf(part_peak[p], 1.0f) * 32767.0f);
        if (q > eng_peak[p])
            eng_peak[p] = (uint16_t)q;
        part_peak[p] = 0.0f;
    }
}

/* -------------------------------------------------------------- loops --- */
/* a user loop's length in bars: 1, 2, 4 or 8, whichever puts its tempo nearest 120 BPM
 * inside 80..180 (breaks are recorded at their own tempo; the player stretches to ours) */
static uint8_t loop_bars(uint32_t ns, uint32_t rate)
{
    float sec = (float)ns / (float)(rate ? rate : 22050u), best = 1e9f;
    uint8_t bars = 1, b;
    for (b = 1; b <= 8; b = (uint8_t)(b * 2)) {
        float bpm = (float)b * 240.0f / sec, d = bpm > 120.0f ? bpm - 120.0f : 120.0f - bpm;
        if (bpm >= 80.0f && bpm <= 180.0f && d < best) {
            best = d;
            bars = b;
        }
    }
    return bars;
}

#include "x0x_break_bank.h"              /* tools/gen_break_bank.py: the built-in loops */

static const brk_loop_t *brk_builtin(int k)
{
    static brk_loop_t L[2];
    static int which;
    brk_loop_t *l = &L[which ^= 1];      /* two: A and B are set one after the other */
    if (k < 0 || k >= X0X_NBREAKS)
        return 0;
    l->adpcm = X0X_BREAKS[k].adpcm;
    l->nsamples = X0X_BREAKS[k].n;
    l->rate = X0X_BREAK_RATE;
    l->bars = X0X_BREAKS[k].bars;
    return l;
}

/* the LOOP choices: the built-in bank, then every loop (zone) of the user slots, in order.
 * The list follows uploads: engine_brk_loops() rescans it. */
#define NLOOPS_MAX (X0X_NBREAKS + PLAT_NSLOTS * PLAT_SLOT_ZONES)
static const char *loop_names[NLOOPS_MAX];
static char user_names[PLAT_NSLOTS * PLAT_SLOT_ZONES][8];
static uint8_t loop_slot[NLOOPS_MAX], loop_zone[NLOOPS_MAX];
static int nloops;

static void scan_loops(void)
{
    int i, k, z;
    nloops = 0;
    for (i = 0; i < X0X_NBREAKS; i++)
        loop_names[nloops++] = X0X_BREAKS[i].name;
    for (k = 0; k < PLAT_NSLOTS; k++) {
        int nz = plat_slot_zones(k);
        for (z = 0; z < nz && z < PLAT_SLOT_ZONES; z++) {
            char *n = user_names[k * PLAT_SLOT_ZONES + z], nm[9];
            uint32_t ns, rate;
            int j = 0;
            plat_slot(k, z, &ns, &rate, nm);
            while (nm[j] && j < (nz > 1 ? 3 : 6)) {   /* "AMEN", or "BRK.3" for a slot of several */
                n[j] = nm[j];
                j++;
            }
            if (!j)
                n[j++] = 'U';
            if (nz > 1) {
                n[j++] = '.';
                if (z + 1 >= 10)
                    n[j++] = (char)('0' + (z + 1) / 10);
                n[j++] = (char)('0' + (z + 1) % 10);
            }
            n[j] = 0;
            loop_slot[nloops] = (uint8_t)k;
            loop_zone[nloops] = (uint8_t)z;
            loop_names[nloops++] = n;
        }
    }
}

int engine_brk_nslots(void)
{
    if (!nloops)
        scan_loops();
    return nloops;
}
const char *const *engine_brk_slot_names(void)
{
    if (!nloops)
        scan_loops();
    return loop_names;
}

int engine_brk_slice(void) { return brk.running ? brk.st_slice : -1; }

void engine_brk_state(int *slice, int *bank, int *div, int *running)
{
    *slice = brk.st_slice;
    *bank = brk.st_bank;
    *div = brk.st_div;
    *running = brk.running;
}

/* the loop's outline for the screen: n columns of peak level 0..255 (main loop: decodes the
 * whole loop once, ~20k IMA steps; called when the loop changes, never per frame) */
static const int16_t ISTEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767};
static const int8_t IIDX[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
int engine_brk_outline(int which, uint8_t *peaks, int n)
{
    const brk_bank_t *bk = &brk.bank[which & 1];
    uint32_t i, ns = bk->n;
    int32_t pred = 0, idx = 0, col = 0, pk = 0;
    uint32_t per;
    if (!bk->adpcm || !ns || n <= 0)
        return 0;
    per = (ns + (uint32_t)n - 1u) / (uint32_t)n;       /* samples per column (32-bit: no __udivdi3) */
    for (i = 0; i < ns; i++) {
        uint32_t code = (bk->adpcm[i >> 1] >> ((i & 1u) * 4u)) & 15u;
        int32_t st = ISTEP[idx], vd = st >> 3, c;
        if (code & 4u) vd += st;
        if (code & 2u) vd += st >> 1;
        if (code & 1u) vd += st >> 2;
        pred += (code & 8u) ? -vd : vd;
        pred = pred < -32768 ? -32768 : pred > 32767 ? 32767 : pred;
        idx += IIDX[code & 7u];
        idx = idx < 0 ? 0 : idx > 88 ? 88 : idx;
        c = (int32_t)(i / per);
        if (c != col) {
            peaks[col] = (uint8_t)(pk >> 7);
            col = c;
            pk = 0;
        }
        if ((pred < 0 ? -pred : pred) > pk)
            pk = pred < 0 ? -pred : pred;
    }
    peaks[col] = (uint8_t)(pk >> 7);
    for (col++; col < n; col++)
        peaks[col] = 0;
    return 1;
}
float engine_gr_db(void) { return mst.gr_view; }
void engine_master_format(int i, char *buf) { master_format(&mst, i, buf); }

void engine_brk_loops(void)
{
    const brkpart_t *bp = &seq.pat[seq.ppat[TRK_BRK]].brk;
    brk_loop_t L[2];
    int w;
    scan_loops();                                       /* an upload may have changed the user loops */
    brk_hold = 1;                                       /* the ISR skips the break while we rewrite it */
    for (w = 0; w < 2; w++) {
        int slot = w ? bp->slot_b : bp->slot_a;
        uint32_t ns = 0, rate = 0;
        char name[9];
        const uint8_t *d = (slot >= X0X_NBREAKS && slot < nloops)
                               ? plat_slot(loop_slot[slot], loop_zone[slot], &ns, &rate, name) : 0;
        if (d) {
            L[w].adpcm = d;
            L[w].nsamples = ns;
            L[w].rate = rate;
            L[w].bars = loop_bars(ns, rate);
            breaks_set_loop(&brk, w, &L[w]);
        } else {
            breaks_set_loop(&brk, w, brk_builtin(slot < X0X_NBREAKS ? slot : 0));   /* built-in (or an empty user slot: the first) */
        }
    }
    brk_hold = 0;
}

void engine_init(pattern_t *patterns, song_t *song, lane_t *lanes, const sound_t *base)
{
    int p;
    drum909_init(&d909);
    drum808_init(&d808);
    bass303_init(&b303[0]);
    bass303_init(&b303[1]);
    breaks_init(&brk);
    fxbus_init(&fx, dly_buf, DLY_LEN);
    master_init(&mst);
    seq_init(&seq, patterns);
    seq.song = song;
    motion_init(&mot, lanes);
    mot.base = mot_base;
    mot.apply = mot_apply;
    mot_base_snd = base;
    for (p = 0; p < NPARTS; p++) {
        mix_level[p] = 1.0f;
        mix_rev[p] = mix_dly[p] = 0.0f;
    }
}
