/* SPDX-License-Identifier: GPL-3.0-only */
/* X0X user interface: panel input, pages, the screen and the key lights.
 *
 * One main-loop function, ui_frame(), reads the panel, applies what it means, and
 * redraws. The screen is three regions (header, main, knob strip), each drawn into
 * Felucca's canvas and blitted only when its pixels changed — SPI time is the scarce
 * thing, not drawing time.
 *
 * Views: HOME (patterns, part overview, mutes), PART (the selected part's steps),
 * GEN (TB-3PO, 303 parts), FX (send FX + master), MIX (levels, swing, accent), GLO
 * (MIDI, settings). ALGORITHM picks the part; the view buttons pick the view.
 *
 * Keys: the 27 keys are F3..G5; the 16 white keys are steps / patterns, the 11 black
 * keys are a drum machine's tracks or the break's slice pads. */
#include "x0x.h"

/* ---------------------------------------------------------------- keys --- */
/* key k (0 = F3): semitone 0 = F. White keys: F G A B C D E. */
static const int8_t KEY_WHITE[NKEYS] = {0, -1, 1, -1, 2, -1, 3, 4, -1, 5, -1, 6, 7, -1, 8, -1, 9, -1, 10, 11, -1, 12, -1, 13, 14, -1, 15};
static const int8_t KEY_BLACK[NKEYS] = {-1, 0, -1, 1, -1, 2, -1, -1, 3, -1, 4, -1, -1, 5, -1, 6, -1, 7, -1, -1, 8, -1, 9, -1, -1, 10, -1};
static uint8_t WHITE_KEY[16], BLACK_KEY[11];        /* inverse maps, built at init */

/* ---------------------------------------------------------------- state --- */
enum { V_HOME, V_PART, V_GEN, V_FX, V_MIX, V_GLO, NVIEWS };
static const uint16_t PART_COL[NPARTS] = {RGB(255, 150, 40), RGB(255, 70, 60), RGB(120, 255, 90), RGB(70, 210, 255),
                                          RGB(230, 110, 255)};

typedef struct {
    uint8_t view, part;
    uint8_t sel[NKIT];                 /* selected drum track per kit */
    uint8_t page[NVIEWS][NPARTS];      /* param page per view and part */
    uint8_t spage;                     /* steps 1-16 / 17-32 */
    uint8_t kbd[NBASS];                /* 303 keyboard mode */
    int8_t oct;                        /* keyboard octave */
    uint8_t rec;
    uint8_t wpos[NBASS];               /* 303 step-write position */
    int8_t held_step;                  /* 303: white key held (knobs edit that step), -1 none */
    uint8_t step_edited;               /* the held step was edited: no toggle on release */
    uint32_t btn, keys;                /* last held states */
    uint32_t btn_used;                 /* a held button was used as a modifier: no tap action */
    int8_t chain_first;                /* HOME: first white key held, for a chain */
    uint32_t enc_t[NE];                /* encoder acceleration */
    char msg[28];
    uint32_t msg_until;
    uint32_t frame;
    uint32_t dirty;                    /* project changed since the last save */
} ui_t;
static ui_t ui;

/* ------------------------------------------------------------- messages --- */
static void say(const char *a, const char *b)
{
    int i = 0;
    while (*a && i < 27)
        ui.msg[i++] = *a++;
    while (b && *b && i < 27)
        ui.msg[i++] = *b++;
    ui.msg[i] = 0;
    ui.msg_until = plat_ms() + 1200u;
}

static void ui_fmt_int(char *b, int v)
{
    char t[12];
    int n = 0, neg = v < 0;
    uint32_t u = (uint32_t)(neg ? -v : v);
    do {
        t[n++] = (char)('0' + u % 10u);
        u /= 10u;
    } while (u);
    if (neg)
        *b++ = '-';
    while (n)
        *b++ = t[--n];
    *b = 0;
}

static const char *const NOTE_N[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static void fmt_note(char *b, int n)
{
    const char *s = NOTE_N[n % 12];
    while (*s)
        *b++ = *s++;
    ui_fmt_int(b, n / 12 - 1);
}

static pattern_t *cur_pat(void) { return &proj.pat[seq.cur]; }

/* ------------------------------------------------------------ param refs --- */
/* Everything a knob can turn: an engine pot (sound_t), or a pattern / sequencer field. */
enum { R_NONE, R_ENG, R_SWING, R_DLEN, R_DRATE, R_BLEN, R_BRATE, R_BDIR, R_BTRANS, R_GEN, R_BRKSET,
       R_BRKSLOT, R_TEMPO, R_ACCENT, R_CLKOUT, R_NOTEOUT, R_PALETTE, R_KEYLED };
typedef struct { uint8_t kind, a, b, c; } pref_t;
#define PR(k, a, b, c) ((pref_t){(k), (a), (b), (c)})

static const char *const RATE_N[] = {"1/16", "1/16T", "1/32", "1/8T"};
static const char *const DIR_N[] = {"FWD", "REV", "PING", "RND"};
static const char *const ONOFF_N[] = {"OFF", "ON"};
static const char *const ROOT_N[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char *const PAL_N[] = {"GREEN", "AMBER", "CYAN", "RED", "MONO"};
enum { G_DENS, G_ACC, G_SLIDE, G_OCTS, G_ROOT, G_SCALE, G_BASE, G_MUT, NGEN };
static const x0x_param_t GEN_P[NGEN] = {
    {"DENS", 100, 70, 0}, {"ACCENT", 100, 40, 0}, {"SLIDE", 100, 25, 0}, {"OCTS", 2, 1, 0},
    {"ROOT", 11, 9, ROOT_N}, {"SCALE", TB3PO_NSCALES - 1, 0, TB3PO_SCALE_NAMES}, {"OCTAVE", 4, 1, 0},
    {"MUTATE", 16, 0, 0},
};
static const x0x_param_t SEQ_P[] = {
    {"SWING", 100, 0, 0}, {"LENGTH", 31, 15, 0}, {"RATE", 3, 0, RATE_N}, {"LENGTH", 31, 15, 0},
    {"RATE", 3, 0, RATE_N}, {"DIR", 3, 0, DIR_N}, {"TRANSP", 48, 24, 0}, {"SLOT", 0, 0, 0},
    {"TEMPO", 255, 0, 0}, {"ACCENT", 127, 88, 0}, {"CLK OUT", 1, 1, ONOFF_N}, {"NOTES", 1, 0, ONOFF_N},
    {"COLOUR", 4, 0, PAL_N}, {"KEYLED", 1, 1, ONOFF_N},
};

static const x0x_param_t *pref_desc(pref_t r)
{
    switch (r.kind) {
    case R_ENG: return engine_param(r.a, r.b, r.c);
    case R_GEN: return &GEN_P[r.c];
    case R_BRKSET: return breaks_param(r.c);
    case R_SWING: return &SEQ_P[0];
    case R_DLEN: return &SEQ_P[1];
    case R_DRATE: return &SEQ_P[2];
    case R_BLEN: return &SEQ_P[3];
    case R_BRATE: return &SEQ_P[4];
    case R_BDIR: return &SEQ_P[5];
    case R_BTRANS: return &SEQ_P[6];
    case R_BRKSLOT: {                                  /* the bank's size is the build's */
        static x0x_param_t d = {"LOOP", 0, 0, 0};
        d.max = (uint8_t)(engine_brk_nslots() - 1);
        d.names = engine_brk_slot_names();
        return &d;
    }
    case R_TEMPO: return &SEQ_P[8];
    case R_ACCENT: return &SEQ_P[9];
    case R_CLKOUT: return &SEQ_P[10];
    case R_NOTEOUT: return &SEQ_P[11];
    case R_PALETTE: return &SEQ_P[12];
    case R_KEYLED: return &SEQ_P[13];
    default: return 0;
    }
}

static int pref_get(pref_t r)
{
    pattern_t *p = cur_pat();
    switch (r.kind) {
    case R_ENG: return proj.sound.v[r.a][r.b][r.c];
    case R_SWING: return p->swing;
    case R_DLEN: return p->drum[r.a].len - 1;
    case R_DRATE: return p->drum[r.a].rate;
    case R_BLEN: return p->bass[r.a].len - 1;
    case R_BRATE: return p->bass[r.a].rate;
    case R_BDIR: return p->bass[r.a].dir;
    case R_BTRANS: return p->bass[r.a].transpose;
    case R_GEN: {
        const tb3po_cfg_t *g = &p->bass[r.a].gen;
        switch (r.c) {
        case G_DENS: return g->density;
        case G_ACC: return g->accent;
        case G_SLIDE: return g->slide;
        case G_OCTS: return g->oct_range - 1;
        case G_ROOT: return g->root;
        case G_SCALE: return g->scale;
        case G_BASE: return g->base_oct;
        default: return g->mutate_bars;
        }
    }
    case R_BRKSET: return p->brk.set[r.c];
    case R_BRKSLOT: return r.a ? p->brk.slot_b : p->brk.slot_a;
    case R_TEMPO: return (int)(seq.bpm + 0.5f) - 20;
    case R_ACCENT: return seq.accent_q7;
    case R_CLKOUT: return seq.send_clock;
    case R_NOTEOUT: return seq.send_notes;
    case R_PALETTE: return proj.set.palette;
    case R_KEYLED: return proj.set.keyled;
    default: return 0;
    }
}

static void pref_set(pref_t r, int v)
{
    pattern_t *p = cur_pat();
    const x0x_param_t *d = pref_desc(r);
    if (!d)
        return;
    v = v < 0 ? 0 : v > d->max ? d->max : v;
    ui.dirty = 1;
    switch (r.kind) {
    case R_ENG:
        proj.sound.v[r.a][r.b][r.c] = (uint8_t)v;
        engine_set(r.a, r.b, r.c, v);
        break;
    case R_SWING: p->swing = (uint8_t)v; break;
    case R_DLEN: p->drum[r.a].len = (uint8_t)(v + 1); break;
    case R_DRATE: p->drum[r.a].rate = (uint8_t)v; break;
    case R_BLEN: p->bass[r.a].len = (uint8_t)(v + 1); break;
    case R_BRATE: p->bass[r.a].rate = (uint8_t)v; break;
    case R_BDIR: p->bass[r.a].dir = (uint8_t)v; break;
    case R_BTRANS: p->bass[r.a].transpose = (uint8_t)v; break;
    case R_GEN: {
        tb3po_cfg_t *g = &p->bass[r.a].gen;
        switch (r.c) {
        case G_DENS: g->density = (uint8_t)v; break;
        case G_ACC: g->accent = (uint8_t)v; break;
        case G_SLIDE: g->slide = (uint8_t)v; break;
        case G_OCTS: g->oct_range = (uint8_t)(v + 1); break;
        case G_ROOT: g->root = (uint8_t)v; break;
        case G_SCALE: g->scale = (uint8_t)v; break;
        case G_BASE: g->base_oct = (uint8_t)v; break;
        default: g->mutate_bars = (uint8_t)v; break;
        }
        if (r.c != G_MUT)
            tb3po_generate(&p->bass[r.a]);           /* the line follows its settings, from the same seed */
        break;
    }
    case R_BRKSET:
        p->brk.set[r.c] = (uint8_t)v;               /* the ISR follows the pattern's settings */
        break;
    case R_BRKSLOT:
        if (r.a)
            p->brk.slot_b = (uint8_t)v;
        else
            p->brk.slot_a = (uint8_t)v;
        engine_brk_loops();
        break;
    case R_TEMPO: seq.bpm = (float)(v + 20); break;
    case R_ACCENT: seq.accent_q7 = (uint8_t)v; break;
    case R_CLKOUT: seq.send_clock = (uint8_t)v; break;
    case R_NOTEOUT: seq.send_notes = (uint8_t)v; break;
    case R_PALETTE: proj.set.palette = (uint8_t)v; palette_set((uint32_t)v); break;
    case R_KEYLED: proj.set.keyled = (uint8_t)v; break;
    default: break;
    }
}

/* the knob's label: a mixer ref names its part ("303 A", "A REV"), anything else its param */
static const char *pref_name(pref_t r)
{
    static const char *const MIXN[NPARTS][3] = {{"909", "909 RV", "909 DL"}, {"808", "808 RV", "808 DL"},
                                                {"303 A", "A REV", "A DLY"}, {"303 B", "B REV", "B DLY"},
                                                {"BREAK", "BRK RV", "BRK DL"}};
    const x0x_param_t *d = pref_desc(r);
    if (r.kind == R_ENG && r.a == T_MIX && r.b < NPARTS && r.c < 3)
        return MIXN[r.b][r.c];
    if (r.kind == R_BRKSLOT)
        return r.a ? "LOOP B" : "LOOP A";
    return d ? d->name : "";
}

static void pref_value(pref_t r, char *b)
{
    const x0x_param_t *d = pref_desc(r);
    int v = pref_get(r);
    if (!d) {
        *b = 0;
        return;
    }
    if (d->names) {
        const char *s = d->names[v];
        while (*s)
            *b++ = *s++;
        *b = 0;
        return;
    }
    switch (r.kind) {
    case R_DLEN:
    case R_BLEN: ui_fmt_int(b, v + 1); return;
    case R_BTRANS: ui_fmt_int(b, v - 24); return;
    case R_GEN:
        if (r.c == G_OCTS)
            ui_fmt_int(b, v + 1);
        else if (r.c == G_MUT && !v) {
            b[0] = 'O', b[1] = 'F', b[2] = 'F', b[3] = 0;
        } else
            ui_fmt_int(b, v);
        return;
    case R_TEMPO: ui_fmt_int(b, v + 20); return;
    default: ui_fmt_int(b, v); return;
    }
}

/* ----------------------------------------------------------------- pages --- */
/* the knob pages of the current view: up to 4 refs per page */
#define MAXPAGES 12
typedef struct { pref_t r[MAXPAGES][4]; char title[MAXPAGES][12]; int n; } pages_t;
static pages_t pg;

static void title(int i, const char *s)
{
    int k = 0;
    while (s[k] && k < 11) {
        pg.title[i][k] = s[k];
        k++;
    }
    pg.title[i][k] = 0;
}

/* engine params of (t, v) as consecutive pages */
static void add_eng_pages(int t, int v, const char *name)
{
    int n = engine_nparams(t, v), i;
    for (i = 0; i < n && pg.n < MAXPAGES; i += 4) {
        int k;
        for (k = 0; k < 4; k++)
            pg.r[pg.n][k] = (i + k < n) ? PR(R_ENG, t, v, i + k) : PR(R_NONE, 0, 0, 0);
        title(pg.n, name);
        pg.n++;
    }
}

static void add_page(const char *t, pref_t a, pref_t b, pref_t c, pref_t d)
{
    if (pg.n >= MAXPAGES)
        return;
    pg.r[pg.n][0] = a;
    pg.r[pg.n][1] = b;
    pg.r[pg.n][2] = c;
    pg.r[pg.n][3] = d;
    title(pg.n, t);
    pg.n++;
}

#define NONE PR(R_NONE, 0, 0, 0)
static void build_pages(void)
{
    int p = ui.part;
    pg.n = 0;
    switch (ui.view) {
    case V_HOME:
        add_page("LEVELS", PR(R_ENG, T_MIX, PART_909, 0), PR(R_ENG, T_MIX, PART_808, 0), PR(R_ENG, T_MIX, PART_303A, 0),
                 PR(R_ENG, T_MIX, PART_303B, 0));
        break;
    case V_PART:
        if (p == PART_909 || p == PART_808) {
            int t = p == PART_909 ? T_909 : T_808, k = p;
            add_eng_pages(t, ui.sel[k], engine_voice_name(t, ui.sel[k]));
            add_page("PART", PR(R_DLEN, k, 0, 0), PR(R_DRATE, k, 0, 0), PR(R_ENG, T_MIX, p, 0), PR(R_SWING, 0, 0, 0));
            add_page("SENDS", PR(R_ENG, T_MIX, p, 1), PR(R_ENG, T_MIX, p, 2), NONE, NONE);
            add_eng_pages(t, NDRUM, "KIT");
        } else if (p == PART_303A || p == PART_303B) {
            int b = p - PART_303A;
            add_eng_pages(T_303, b, engine_voice_name(T_303, b));
            add_page("LINE", PR(R_BLEN, b, 0, 0), PR(R_BRATE, b, 0, 0), PR(R_BDIR, b, 0, 0), PR(R_BTRANS, b, 0, 0));
            add_page("MIX", PR(R_ENG, T_MIX, p, 0), PR(R_ENG, T_MIX, p, 1), PR(R_ENG, T_MIX, p, 2), NONE);
        } else {
            add_page("GROOVE", PR(R_BRKSET, 0, 0, BRK_COMPLEX), PR(R_BRKSET, 0, 0, BRK_ANCHOR),
                     PR(R_BRKSET, 0, 0, BRK_ROLL), PR(R_BRKSET, 0, 0, BRK_FILL));
            add_page("RETRIG", PR(R_BRKSET, 0, 0, BRK_R2), PR(R_BRKSET, 0, 0, BRK_R3), PR(R_BRKSET, 0, 0, BRK_R4),
                     PR(R_BRKSET, 0, 0, BRK_R8));
            add_page("PHRASE", PR(R_BRKSET, 0, 0, BRK_PHRASE), PR(R_BRKSET, 0, 0, BRK_BCHANCE),
                     PR(R_BRKSET, 0, 0, BRK_ALEN), PR(R_BRKSET, 0, 0, BRK_BLEN));
            add_page("LOOPS", PR(R_BRKSLOT, 0, 0, 0), PR(R_BRKSLOT, 1, 0, 0), PR(R_ENG, T_MIX, PART_BRK, 0), NONE);
            add_page("SENDS", PR(R_ENG, T_MIX, PART_BRK, 1), PR(R_ENG, T_MIX, PART_BRK, 2), NONE, NONE);
            add_eng_pages(T_BRK, 0, "BREAK");
        }
        break;
    case V_GEN: {
        int b = p == PART_303B ? 1 : 0;
        add_page("TB-3PO", PR(R_GEN, b, 0, G_DENS), PR(R_GEN, b, 0, G_ACC), PR(R_GEN, b, 0, G_SLIDE),
                 PR(R_GEN, b, 0, G_OCTS));
        add_page("SCALE", PR(R_GEN, b, 0, G_ROOT), PR(R_GEN, b, 0, G_SCALE), PR(R_GEN, b, 0, G_BASE),
                 PR(R_GEN, b, 0, G_MUT));
        add_page("LINE", PR(R_BLEN, b, 0, 0), PR(R_BRATE, b, 0, 0), PR(R_BDIR, b, 0, 0), PR(R_BTRANS, b, 0, 0));
        break;
    }
    case V_FX:
        add_page("SENDS", PR(R_ENG, T_MIX, PART_909, 1), PR(R_ENG, T_MIX, PART_909, 2), PR(R_ENG, T_MIX, PART_808, 1),
                 PR(R_ENG, T_MIX, PART_808, 2));
        add_page("SENDS", PR(R_ENG, T_MIX, PART_303A, 1), PR(R_ENG, T_MIX, PART_303A, 2),
                 PR(R_ENG, T_MIX, PART_303B, 1), PR(R_ENG, T_MIX, PART_303B, 2));
        add_page("SENDS", PR(R_ENG, T_MIX, PART_BRK, 1), PR(R_ENG, T_MIX, PART_BRK, 2), PR(R_ENG, T_FX, 0, FX_DL_TYPE),
                 PR(R_ENG, T_FX, 0, FX_DL_TIME));
        add_eng_pages(T_FX, 0, "FX");
        add_eng_pages(T_MST, 0, "MASTER");
        break;
    case V_MIX:
        add_page("LEVELS", PR(R_ENG, T_MIX, PART_909, 0), PR(R_ENG, T_MIX, PART_808, 0), PR(R_ENG, T_MIX, PART_303A, 0),
                 PR(R_ENG, T_MIX, PART_303B, 0));
        add_page("LEVELS", PR(R_ENG, T_MIX, PART_BRK, 0), NONE, NONE, NONE);
        add_page("GROOVE", PR(R_TEMPO, 0, 0, 0), PR(R_SWING, 0, 0, 0), PR(R_ACCENT, 0, 0, 0), NONE);
        break;
    case V_GLO:
        add_page("MIDI", PR(R_CLKOUT, 0, 0, 0), PR(R_NOTEOUT, 0, 0, 0), PR(R_PALETTE, 0, 0, 0), PR(R_KEYLED, 0, 0, 0));
        break;
    default: break;
    }
    if (pg.n == 0)
        add_page("", NONE, NONE, NONE, NONE);
    if (ui.page[ui.view][ui.part] >= pg.n)
        ui.page[ui.view][ui.part] = 0;
}

static int cur_page(void) { return ui.page[ui.view][ui.part]; }

/* ----------------------------------------------------------------- input --- */
static int32_t accel(int role, int32_t s, int range)
{
    uint32_t now = plat_ms(), dt = now - ui.enc_t[role];
    ui.enc_t[role] = now;
    if (range > 40 && dt < 50u)
        return s * (range > 150 ? 6 : 3);
    return s;
}

static int is_303(void) { return ui.part == PART_303A || ui.part == PART_303B; }
static int is_drum(void) { return ui.part == PART_909 || ui.part == PART_808; }
static int bidx(void) { return ui.part == PART_303B ? 1 : 0; }

static void set_view(int v)
{
    ui.view = (uint8_t)v;
    ui.held_step = -1;
}

/* step under the playhead for live recording: the nearest step (a hit late in a step
 * belongs to the next one) */
static int rec_step(int track)
{
    return eng_step[track];
}

static void drum_key(int black, int down)
{
    int k = ui.part, v = black;
    if (!down)
        return;
    ui.sel[k] = (uint8_t)v;
    engine_drum(k, v, (ui.btn & (1u << B_ENV)) ? 1.0f : 0.75f);
    if (ui.btn & (1u << B_ENV))
        ui.btn_used |= 1u << B_ENV;
    if (ui.rec && seq.playing) {
        int s = rec_step(TRK_DRUM + k);
        cur_pat()->drum[k].hit[v] |= 1u << s;
        if (ui.btn & (1u << B_ENV))
            cur_pat()->drum[k].accent |= 1u << s;
        ui.dirty = 1;
    }
}

static void drum_step(int white)
{
    int k = ui.part, s = ui.spage * 16 + white;
    dpart_t *d = &cur_pat()->drum[k];
    if (s >= NSTEPS)
        return;
    if (ui.btn & (1u << B_ENV)) {
        d->accent ^= 1u << s;
        ui.btn_used |= 1u << B_ENV;
    } else {
        d->hit[ui.sel[k]] ^= 1u << s;
    }
    ui.dirty = 1;
}

/* 303: keyboard note of key k */
static int kbd_note(int k) { return 41 + k + 12 * ui.oct; }   /* key 0 = F2 (41) at octave 0 */

static void bass_write(int b, int note, int gate)
{
    bpart_t *bp = &cur_pat()->bass[b];
    int w = ui.wpos[b] % (bp->len ? bp->len : 16);
    uint8_t f = (uint8_t)gate;
    if (gate == G_NOTE) {
        if (ui.btn & (1u << B_ENV)) {
            f |= BS_ACCENT;
            ui.btn_used |= 1u << B_ENV;
        }
        if (ui.btn & (1u << B_LFO)) {
            f |= BS_SLIDE;
            ui.btn_used |= 1u << B_LFO;
        }
        bp->step[w].note = (uint8_t)note;
    }
    bp->step[w].flags = f;
    ui.wpos[b] = (uint8_t)((w + 1) % (bp->len ? bp->len : 16));
    ui.dirty = 1;
}

static uint8_t kbd_held_note[NBASS];
static void bass_kbd(int key, int down)
{
    int b = bidx(), n = kbd_note(key);
    if (down) {
        engine_bass_on(b, n, (ui.btn & (1u << B_ENV)) != 0, kbd_held_note[b] != 0);
        kbd_held_note[b] = (uint8_t)n;
        if (ui.rec && !seq.playing) {
            bass_write(b, n, G_NOTE);
            say("STEP ", 0);
            {
                char t[8];
                ui_fmt_int(t, ui.wpos[b] ? ui.wpos[b] : cur_pat()->bass[b].len);
                say("WROTE STEP ", t);
            }
        } else if (ui.rec && seq.playing) {
            bpart_t *bp = &cur_pat()->bass[b];
            int s = rec_step(TRK_BASS0 + b);
            bp->step[s].note = (uint8_t)n;
            bp->step[s].flags = (uint8_t)(G_NOTE | ((ui.btn & (1u << B_ENV)) ? BS_ACCENT : 0));
            ui.dirty = 1;
        }
    } else if (kbd_held_note[b] == n) {
        engine_bass_off(b);
        kbd_held_note[b] = 0;
    }
}

static void bass_step_key(int white, int down)
{
    int b = bidx(), s = ui.spage * 16 + white;
    bpart_t *bp = &cur_pat()->bass[b];
    if (s >= NSTEPS)
        return;
    if (down) {
        ui.held_step = (int8_t)s;
        ui.step_edited = 0;
        if (bstep_gate(&bp->step[s]) != G_REST)
            engine_bass_on(b, bp->step[s].note + bp->transpose - 24, (bp->step[s].flags & BS_ACCENT) != 0, 0);
    } else if (ui.held_step == s) {
        if (!ui.step_edited) {                       /* a tap toggles the step */
            bp->step[s].flags = bstep_gate(&bp->step[s]) == G_REST ? G_NOTE : G_REST;
            ui.dirty = 1;
        }
        engine_bass_off(b);
        ui.held_step = -1;
    }
}

static void key_event(int k, int down)
{
    int w = KEY_WHITE[k], bl = KEY_BLACK[k];
    /* SAVE held + white key: copy this pattern there */
    if (down && w >= 0 && (ui.btn & (1u << B_SAVE))) {
        char t[8];
        proj.pat[w] = *cur_pat();
        ui.btn_used |= 1u << B_SAVE;
        ui.dirty = 1;
        ui_fmt_int(t, w + 1);
        say("COPIED TO P", t);
        return;
    }
    /* HOME view (or HOME held): white keys pick patterns */
    if (w >= 0 && (ui.view == V_HOME || (ui.btn & (1u << B_HOME)))) {
        if (ui.btn & (1u << B_HOME))
            ui.btn_used |= 1u << B_HOME;
        if (down) {
            if (ui.chain_first >= 0 && ui.chain_first != w) {
                char t[16], *q = t;
                seq_chain(&seq, ui.chain_first, w);
                ui_fmt_int(q, ui.chain_first + 1);
                while (*q)
                    q++;
                *q++ = '-';
                ui_fmt_int(q, w + 1);
                say("CHAIN P", t);
            } else {
                ui.chain_first = (int8_t)w;
                seq_chain(&seq, 0, 0);
                seq_cue(&seq, w);
                if (!seq.playing)
                    engine_brk_loops();
            }
        } else if (ui.chain_first == w) {
            ui.chain_first = -1;
        }
        return;
    }
    if (ui.view == V_HOME && bl >= 0) {               /* black keys 1-5: part mutes */
        if (down && bl < NPARTS) {
            static const uint32_t MUTE_MASK[NPARTS] = {(1u << NDRUM) - 1u, ((1u << NDRUM) - 1u) << NDRUM,
                                                       1u << MUTE_BASS0, 1u << (MUTE_BASS0 + 1), 1u << MUTE_BRK};
            uint32_t m = MUTE_MASK[bl];
            seq.mute = (seq.mute & m) == m ? seq.mute & ~m : seq.mute | m;
        }
        return;
    }
    if (is_drum()) {
        if (bl >= 0)
            drum_key(bl, down);
        else if (down)
            drum_step(w);
    } else if (is_303()) {
        if (ui.kbd[bidx()])
            bass_kbd(k, down);
        else if (w >= 0)
            bass_step_key(w, down);
    } else {                                          /* BREAK */
        if (bl >= 0)
            engine_brk_live(bl, down);
        else if (down && w >= 0) {
            cur_pat()->brk.steps ^= 1u << w;
            ui.dirty = 1;
        }
    }
}

static void save_project(void)
{
    int rc = project_save();
    say(rc ? "SAVE FAILED " : "SAVED", 0);
    if (!rc)
        ui.dirty = 0;
}

static void button_tap(int b)
{
    switch (b) {
    case B_PLAY:
        if (seq.playing)
            seq_stop(&seq);
        else
            seq_start(&seq);
        break;
    case B_REC:
        if (ui.btn & (1u << B_SAVE)) {               /* SAVE + REC: clear the part in this pattern */
            pattern_t *p = cur_pat();
            ui.btn_used |= 1u << B_SAVE;
            if (is_drum()) {
                uint32_t i;
                for (i = 0; i < NDRUM; i++)
                    p->drum[ui.part].hit[i] = 0;
                p->drum[ui.part].accent = 0;
            } else if (is_303()) {
                int i;
                for (i = 0; i < NSTEPS; i++)
                    p->bass[bidx()].step[i].flags = G_REST;
            } else {
                p->brk.steps = 0;
            }
            ui.dirty = 1;
            say("CLEARED", 0);
            break;
        }
        ui.rec = !ui.rec;
        if (ui.rec && is_303())
            ui.wpos[bidx()] = 0;
        break;
    case B_HOME: set_view(ui.view == V_HOME ? V_PART : V_HOME); break;
    case B_EDIT:
        if (ui.view == V_PART)
            ui.page[V_PART][ui.part] = (uint8_t)((cur_page() + 1) % pg.n);
        else
            set_view(V_PART);
        break;
    case B_ARP:
    case B_SCL:
        if (!is_303()) {
            say("TB-3PO: PICK A 303", 0);
            break;
        }
        if (ui.view != V_GEN) {
            set_view(V_GEN);
            ui.page[V_GEN][ui.part] = b == B_SCL ? 1 : 0;
        } else {
            ui.page[V_GEN][ui.part] = (uint8_t)(b == B_SCL ? 1 : (cur_page() + 1) % pg.n);
        }
        break;
    case B_FX:
        if (ui.view == V_FX)
            ui.page[V_FX][ui.part] = (uint8_t)((cur_page() + 1) % pg.n);
        else
            set_view(V_FX);
        break;
    case B_LFO:
        if (ui.view == V_MIX)
            ui.page[V_MIX][ui.part] = (uint8_t)((cur_page() + 1) % pg.n);
        else
            set_view(V_MIX);
        break;
    case B_GLO: set_view(ui.view == V_GLO ? V_PART : V_GLO); break;
    case B_SEQ:
        if (is_303()) {
            ui.kbd[bidx()] = !ui.kbd[bidx()];
            say(ui.kbd[bidx()] ? "KEYBOARD" : "STEPS", 0);
        }
        if (ui.view != V_PART)
            set_view(V_PART);
        break;
    case B_ENV:                                       /* tapped alone: in 303 step write, a REST */
        if (is_303() && ui.kbd[bidx()] && ui.rec && !seq.playing)
            bass_write(bidx(), 0, G_REST);
        break;
    case B_SAVE: save_project(); break;
    case B_OCTUP:
    case B_OCTDN: {
        int up = b == B_OCTUP;
        if (ui.view == V_GEN) {                       /* TB-3PO: + generate, - mutate */
            bpart_t *bp = &cur_pat()->bass[bidx()];
            if (up) {
                bp->gen.seed = tb3po_new_seed(plat_ms() ^ (ui.frame << 7));
                tb3po_generate(bp);
                say("GENERATED", 0);
            } else {
                uint32_t r = bp->gen.seed ^ plat_ms();
                tb3po_mutate(bp, &r);
                say("MUTATED", 0);
            }
            ui.dirty = 1;
        } else if (is_303() && ui.kbd[bidx()]) {
            if (ui.btn & (1u << B_LFO)) {             /* LFO held + OCT: a TIE in step write */
                ui.btn_used |= 1u << B_LFO;
                bass_write(bidx(), 0, G_TIE);
            } else {
                ui.oct = (int8_t)(ui.oct + (up ? 1 : -1));
                ui.oct = ui.oct < -2 ? -2 : ui.oct > 3 ? 3 : ui.oct;
            }
        } else {
            ui.spage = (uint8_t)up;
        }
        break;
    }
    default: break;
    }
}

static void input(void)
{
    uint32_t btn = plat_buttons(), keys = plat_keys(), ch, i;
    int e;
    /* buttons: press marks, release fires the tap unless the button was a modifier */
    ch = btn ^ ui.btn;
    for (i = 0; i < NB; i++) {
        uint32_t m = 1u << i;
        if (!(ch & m))
            continue;
        if (btn & m) {
            ui.btn_used &= ~m;
            if (i == B_PLAY || i == B_REC)            /* transport acts on the press */
                button_tap((int)i);
        } else if (!(ui.btn_used & m) && i != B_PLAY && i != B_REC) {
            button_tap((int)i);
        }
    }
    ui.btn = btn;
    ch = keys ^ ui.keys;
    ui.keys = keys;
    for (i = 0; i < NKEYS; i++)
        if (ch & (1u << i))
            key_event((int)i, (keys >> i) & 1u);
    build_pages();
    /* encoders */
    if ((e = plat_enc(EN_ALGO)) != 0) {
        int p = ui.part + (e > 0 ? 1 : -1);
        ui.part = (uint8_t)(p < 0 ? 0 : p >= NPARTS ? NPARTS - 1 : p);
        if (ui.view == V_GEN && !is_303())
            set_view(V_PART);
        if (ui.view == V_HOME)
            set_view(V_PART);
        build_pages();
    }
    if ((e = plat_enc(EN_SELECT)) != 0) {
        float bpm = seq.bpm + (float)accel(EN_SELECT, e, 200);
        seq.bpm = bpm < 20.0f ? 20.0f : bpm > 275.0f ? 275.0f : bpm;
        ui.dirty = 1;
    }
    if ((e = plat_enc(EN_PRESET)) != 0) {
        int base = seq.cue < NPAT ? seq.cue : seq.cur, p = base + (e > 0 ? 1 : -1);
        p = p < 0 ? 0 : p >= NPAT ? NPAT - 1 : p;
        seq_chain(&seq, 0, 0);
        seq_cue(&seq, p);
    }
    for (i = 0; i < 4; i++) {
        pref_t r;
        const x0x_param_t *d;
        if ((e = plat_enc(EN_K1 + (int)i)) == 0)
            continue;
        /* 303: a held step's knobs edit the step */
        if (ui.view == V_PART && is_303() && ui.held_step >= 0) {
            bpart_t *bp = &cur_pat()->bass[bidx()];
            bstep_t *st = &bp->step[ui.held_step];
            ui.step_edited = 1;
            ui.dirty = 1;
            if (i == 0) {
                int n = st->note + e;
                st->note = (uint8_t)(n < 12 ? 12 : n > 108 ? 108 : n);
                if (bstep_gate(st) == G_REST)
                    st->flags = (uint8_t)((st->flags & ~BS_GATE_MASK) | G_NOTE);
                engine_bass_on(bidx(), st->note + bp->transpose - 24, 0, 1);
            } else if (i == 1) {
                int g = (bstep_gate(st) + (e > 0 ? 1 : 2)) % 3;
                st->flags = (uint8_t)((st->flags & ~BS_GATE_MASK) | g);
            } else if (i == 2) {
                st->flags ^= BS_ACCENT;
            } else {
                st->flags ^= BS_SLIDE;
            }
            continue;
        }
        r = pg.r[cur_page()][i];
        d = pref_desc(r);
        if (!d)
            continue;
        if (d->names || d->max < 24)
            e = e > 0 ? 1 : -1;                       /* a switch: one detent, one position */
        else
            e = accel(EN_K1 + (int)i, e, d->max);
        pref_set(r, pref_get(r) + e);
    }
}

/* -------------------------------------------------------------- drawing --- */
#define HDR_H 20
#define MAIN_Y 20
#define MAIN_H 144                     /* drawn as two 72-row bands */
#define KNOB_Y 164
#define KNOB_H 76

static uint32_t blit_hash[4];
static uint32_t cv_hash(void)
{
    uint32_t i, h = 2166136261u, n = cv_w * cv_h;
    for (i = 0; i < n; i++)
        h = (h ^ cv_px[i]) * 16777619u;
    return h ^ (cv_w << 16) ^ cv_h;
}
static void cv_commit(int region, uint32_t x, uint32_t y)
{
    uint32_t h = cv_hash();
    if (h != blit_hash[region]) {
        blit_hash[region] = h;
        cv_blit(x, y);
    }
}

static uint16_t part_col(void) { return PART_COL[ui.part]; }
static uint16_t dimmed(uint16_t c)                     /* a quarter of a colour */
{
    return (uint16_t)((((c >> 11) >> 2) << 11) | ((((c >> 5) & 63u) >> 2) << 5) | ((c & 31u) >> 2));
}

static void draw_header(void)
{
    char b[24], t[8];
    static const char *const VIEW_N[NVIEWS] = {"HOME", "", "TB-3PO", "FX", "MIX", "GLOBAL"};
    cv_begin(240, HDR_H, C_BLACK);
    cv_rect(0, HDR_H - 1, 240, 1, C_LINE);
    if (ui.msg_until && (int32_t)(ui.msg_until - plat_ms()) > 0) {
        cv_text(4, 2, &FONT_S, ui.msg, C_WHITE);
        cv_commit(0, 0, 0);
        return;
    }
    ui.msg_until = 0;
    if (ui.view == V_PART || ui.view == V_GEN) {
        cv_rect(0, 0, 4, HDR_H - 1, part_col());
        cv_text(8, 2, &FONT_S, engine_voice_name(T_MIX, ui.part), part_col());
        if (ui.view == V_GEN)
            cv_text(64, 2, &FONT_S, "TB-3PO", C_HI);
    } else {
        cv_text(8, 2, &FONT_S, VIEW_N[ui.view], C_HI);
    }
    /* pattern, cue, chain */
    b[0] = 'P';
    ui_fmt_int(b + 1, seq.cur + 1);
    cv_text(118, 2, &FONT_S, b, C_WHITE);
    if (seq.cue < NPAT) {
        b[0] = '>';
        ui_fmt_int(b + 1, seq.cue + 1);
        cv_text(142, 2, &FONT_S, b, (ui.frame & 16u) ? C_WHITE : C_GRAY);
    } else if (seq.chain_a != seq.chain_b) {
        cv_text(142, 2, &FONT_S, "CH", C_AMB);
    }
    /* tempo + transport */
    ui_fmt_int(t, (int)(seq_tempo(&seq) + 0.5f));
    cv_text(178, 2, &FONT_S, t, seq.ext ? C_AMB : C_HI);
    if (seq.playing)
        cv_rect(216, 5, 8, 9, C_HI);                   /* play: a block (no triangle glyph) */
    if (ui.rec)
        cv_rect(228, 5, 9, 9, RGB(255, 40, 40));
    cv_commit(0, 0, 0);
}

/* the 16 visible steps of a 32-step part */
static int step_of(int col) { return ui.spage * 16 + col; }

#define GX 28                          /* grid x */
#define CW 13                          /* cell width */

static void draw_drum(int band)
{
    int k = ui.part, v, c, len = cur_pat()->drum[k].len;
    const dpart_t *d = &cur_pat()->drum[k];
    int ph = seq.playing && seq.cur < NPAT ? eng_step[TRK_DRUM + k] : -1;
    uint16_t col = part_col();
    /* 11 rows of 11 px + the accent row: band 0 = rows 0-5, band 1 = rows 6-10 + accent */
    cv_begin(240, 72, C_BLACK);
    for (v = band * 6; v < (band ? NDRUM + 1 : 6); v++) {
        int y = (v - band * 6) * 12 + 2, sel = v < NDRUM && v == ui.sel[k];
        const char *nm = v < NDRUM ? engine_voice_name(k == PART_909 ? T_909 : T_808, v) : "AC";
        uint32_t bits = v < NDRUM ? d->hit[v] : d->accent;
        if (sel)
            cv_rect(0, y - 1, 26, 12, col);
        cv_text(2, y - 3, &FONT_S, nm, sel ? C_BLACK : (v < NDRUM ? C_GRAY : C_WHITE));
        for (c = 0; c < 16; c++) {
            int s = step_of(c), x = GX + c * CW;
            uint16_t fc;
            if (s >= len) {
                cv_rect(x + 1, y + 4, CW - 3, 2, C_LINE);
                continue;
            }
            fc = (bits >> s & 1u) ? (v < NDRUM ? col : C_WHITE) : ((c & 3) == 0 ? C_DIM : C_LINE);
            cv_rect(x + 1, y, CW - 3, 10, fc);
            if (s == ph)
                cv_rect(x + 1, y + 10, CW - 3, 1, C_WHITE);
        }
    }
    if (band == 1 && seq.playing)                      /* page dots */
        cv_rect(GX + (ph >= 16 ? 8 : 0) * CW, 70, 8 * CW, 1, C_DIM);
    cv_commit(1 + band, 0, (uint32_t)(MAIN_Y + band * 72));
}

static void draw_303(int band)
{
    int b = bidx(), c;
    const bpart_t *bp = &cur_pat()->bass[b];
    int ph = seq.playing ? eng_step[TRK_BASS0 + b] : -1;
    uint16_t col = part_col();
    cv_begin(240, 72, C_BLACK);
    if (band == 0) {                                   /* pitch lane: note height over 2 octaves */
        int lo = 12 * (bp->gen.base_oct + 1) + bp->gen.root - 2;
        for (c = 0; c < 16; c++) {
            int s = step_of(c), x = GX + c * CW, g;
            const bstep_t *st = &bp->step[s < NSTEPS ? s : 0];
            if (s >= bp->len) {
                cv_rect(x + 1, 66, CW - 3, 2, C_LINE);
                continue;
            }
            cv_rect(x + 1, 69, CW - 3, 1, (c & 3) == 0 ? C_DIM : C_LINE);
            g = bstep_gate(st);
            if (g != G_REST) {
                int y = 64 - (st->note - lo) * 2;
                y = y < 0 ? 0 : y > 64 ? 64 : y;
                cv_rect(x + 1, y, CW - 3, 4, s == ui.held_step ? C_WHITE : col);
                if (g == G_TIE)
                    cv_rect(x - 2, y + 1, 4, 2, col);
                if (st->flags & BS_SLIDE)
                    cv_line(x + CW - 2, y + 2, x + CW + 2, y - 2, C_WHITE);
            }
            if (s == ph)
                cv_rect(x, 0, 1, 70, C_DIM);
        }
        cv_text(2, 0, &FONT_S, ui.kbd[b] ? "KB" : "", C_AMB);
    } else {                                           /* flags rows + the held step */
        static const char *const GN[3] = {".", "N", "T"};
        for (c = 0; c < 16; c++) {
            int s = step_of(c), x = GX + c * CW;
            const bstep_t *st = &bp->step[s < NSTEPS ? s : 0];
            if (s >= bp->len)
                continue;
            cv_text(x + 2, -2, &FONT_S, GN[bstep_gate(st)], bstep_gate(st) ? C_HI : C_DIM);
            cv_rect(x + 1, 21, CW - 3, 4, (st->flags & BS_ACCENT) ? C_WHITE : C_LINE);
            cv_rect(x + 1, 33, CW - 3, 4, (st->flags & BS_SLIDE) ? col : C_LINE);
            if (s == ph)
                cv_rect(x + 1, 40, CW - 3, 2, C_WHITE);
        }
        cv_text(2, -2, &FONT_S, "GT", C_GRAY);
        cv_text(2, 15, &FONT_S, "AC", C_GRAY);
        cv_text(2, 27, &FONT_S, "SL", C_GRAY);
        if (ui.held_step >= 0 || (ui.kbd[b] && ui.rec)) {
            char t[24], *q = t;
            int s = ui.held_step >= 0 ? ui.held_step : ui.wpos[b];
            const bstep_t *st = &bp->step[s];
            const char *p0 = ui.held_step >= 0 ? "STEP " : "WRITE ";
            while (*p0)
                *q++ = *p0++;
            ui_fmt_int(q, s + 1);
            while (*q)
                q++;
            *q++ = ' ';
            fmt_note(q, st->note + bp->transpose - 24);
            cv_text(2, 50, &FONT_S, t, C_WHITE);
        } else {
            char t[24], *q = t;
            const char *sc = TB3PO_SCALE_NAMES[bp->gen.scale % TB3PO_NSCALES], *rn = ROOT_N[bp->gen.root % 12];
            while (*rn)
                *q++ = *rn++;
            *q++ = ' ';
            while (*sc)
                *q++ = *sc++;
            *q = 0;
            cv_text(2, 50, &FONT_S, t, C_GRAY);
        }
    }
    cv_commit(1 + band, 0, (uint32_t)(MAIN_Y + band * 72));
}

static void draw_break(int band)
{
    const brkpart_t *bp = &cur_pat()->brk;
    int c, ph = seq.playing ? eng_step[TRK_BRK] : -1;
    uint16_t col = part_col();
    cv_begin(240, 72, C_BLACK);
    if (band == 0) {
        cv_text(2, 4, &FONT_S, "ON", C_GRAY);
        for (c = 0; c < 16; c++) {
            int x = GX + c * CW;
            cv_rect(x + 1, 6, CW - 3, 14, (bp->steps >> c & 1u) ? col : ((c & 3) == 0 ? C_DIM : C_LINE));
            if (c == ph)
                cv_rect(x + 1, 22, CW - 3, 2, C_WHITE);
        }
        {
            int sl = engine_brk_slice();
            cv_text(2, 34, &FONT_S, "SL", C_GRAY);
            for (c = 0; c < 8; c++)
                cv_rect(GX + c * 26, 36, 24, 14, c == sl ? C_WHITE : C_LINE);
        }
    } else {
        char t[24], *q = t;
        const char *const *sn = engine_brk_slot_names();
        int ns = engine_brk_nslots();
        const char *a = sn[bp->slot_a % ns], *bb = sn[bp->slot_b % ns];
        *q++ = 'A';
        *q++ = ' ';
        while (*a)
            *q++ = *a++;
        *q++ = ' ';
        *q++ = ' ';
        *q++ = 'B';
        *q++ = ' ';
        while (*bb)
            *q++ = *bb++;
        *q = 0;
        cv_text(2, 4, &FONT_S, t, C_GRAY);
        cv_text(2, 26, &FONT_S, "BLACK KEYS: 1-8 SLICES", C_DIM);
        cv_text(2, 44, &FONT_S, "9 REV 10 HALF 11 STUT", C_DIM);
    }
    cv_commit(1 + band, 0, (uint32_t)(MAIN_Y + band * 72));
}

static void draw_home(int band)
{
    int p, c;
    cv_begin(240, 72, C_BLACK);
    if (band == 0) {                                   /* five parts at a glance */
        for (p = 0; p < NPARTS; p++) {
            int y = p * 14 + 1, ph, len;
            uint32_t bits = 0;
            static const uint32_t MUTE_ANY[NPARTS] = {(1u << NDRUM) - 1u, ((1u << NDRUM) - 1u) << NDRUM,
                                                      1u << MUTE_BASS0, 1u << (MUTE_BASS0 + 1), 1u << MUTE_BRK};
            int muted = (seq.mute & MUTE_ANY[p]) == MUTE_ANY[p];
            const pattern_t *pt = cur_pat();
            if (p < NKIT) {
                int v;
                for (v = 0; v < NDRUM; v++)
                    bits |= pt->drum[p].hit[v];
                ph = eng_step[TRK_DRUM + p];
                len = pt->drum[p].len;
            } else if (p < PART_BRK) {
                int s;
                for (s = 0; s < NSTEPS; s++)
                    if (bstep_gate(&pt->bass[p - NKIT].step[s]) != G_REST)
                        bits |= 1u << s;
                ph = eng_step[TRK_BASS0 + p - NKIT];
                len = pt->bass[p - NKIT].len;
            } else {
                bits = pt->brk.steps;
                ph = eng_step[TRK_BRK];
                len = 16;
            }
            cv_text(2, y - 2, &FONT_S, engine_voice_name(T_MIX, p), muted ? C_DIM : PART_COL[p]);
            for (c = 0; c < 16 && c < len; c++) {
                int x = 48 + c * 12;
                uint16_t fc = (bits >> c & 1u) ? (muted ? C_DIM : PART_COL[p]) : C_LINE;
                cv_rect(x, y + 2, 10, 9, fc);
                if (seq.playing && (ph & 15) == c)
                    cv_rect(x, y + 12, 10, 1, C_WHITE);
            }
        }
    } else {                                           /* the 16 patterns */
        for (c = 0; c < NPAT; c++) {
            int x = 4 + c * 14 + (c >= 8 ? 6 : 0), y = 8;
            uint16_t fc = C_LINE;
            const pattern_t *pt = &proj.pat[c];
            int used = pt->brk.steps != 0, v;
            for (v = 0; v < NDRUM; v++)
                used |= (pt->drum[0].hit[v] | pt->drum[1].hit[v]) != 0;
            for (v = 0; v < NSTEPS; v++)
                used |= bstep_gate(&pt->bass[0].step[v]) != G_REST || bstep_gate(&pt->bass[1].step[v]) != G_REST;
            if (used)
                fc = C_DIM;
            if (seq.chain_a != seq.chain_b && c >= (seq.chain_a < seq.chain_b ? seq.chain_a : seq.chain_b) &&
                c <= (seq.chain_a < seq.chain_b ? seq.chain_b : seq.chain_a))
                fc = C_AMB;
            if (c == seq.cue && (ui.frame & 16u))
                fc = C_GRAY;
            if (c == seq.cur)
                fc = C_WHITE;
            cv_rect(x, y, 12, 24, fc);
        }
        cv_text(4, 40, &FONT_S, "WHITE: PATTERN  2 KEYS: CHAIN", C_DIM);
        cv_text(4, 56, &FONT_S, "BLACK 1-5: MUTE PARTS", C_DIM);
    }
    cv_commit(1 + band, 0, (uint32_t)(MAIN_Y + band * 72));
}

static void draw_scope(int band)
{
    int i;
    cv_begin(240, 72, C_BLACK);
    if (band == 0) {
        int prev = 36;
        uint32_t w = eng_scope_w;
        for (i = 0; i < 240; i++) {
            int s = eng_scope[(w + (uint32_t)i) & (SCOPE_N - 1u)], y = 36 - s * 34 / 32768;
            cv_line(i ? i - 1 : 0, prev, i, y, part_col());
            prev = y;
        }
    } else {                                           /* part meters */
        for (i = 0; i < NPARTS; i++) {
            int w = eng_peak[i] * 180 / 32768;
            cv_text(2, i * 14 - 2, &FONT_S, engine_voice_name(T_MIX, i), PART_COL[i]);
            cv_rect(52, i * 14 + 2, w, 8, PART_COL[i]);
            cv_rect(52 + w, i * 14 + 2, 180 - w, 8, C_LINE);
        }
        {
            char t[20], *q = t;
            const char *s = "CPU ";
            while (*s)
                *q++ = *s++;
            ui_fmt_int(q, (int)plat_cpu_pct());
            while (*q)
                q++;
            *q++ = '%';
            *q = 0;
            cv_text(160, 56, &FONT_S, t, plat_cpu_pct() > 85u ? RGB(255, 60, 60) : C_GRAY);
        }
    }
    cv_commit(1 + band, 0, (uint32_t)(MAIN_Y + band * 72));
}

static void draw_main(void)
{
    int band;
    for (band = 0; band < 2; band++) {
        if (ui.view == V_HOME)
            draw_home(band);
        else if (ui.view == V_PART || ui.view == V_GEN) {
            if (is_drum())
                draw_drum(band);
            else if (is_303())
                draw_303(band);
            else
                draw_break(band);
        } else
            draw_scope(band);
    }
}

static void draw_knobs(void)
{
    int i, pgi = cur_page();
    cv_begin(240, KNOB_H, C_BLACK);
    cv_rect(0, 0, 240, 1, C_LINE);
    {
        char t[20], *q = t;
        const char *s = pg.title[pgi];
        while (*s)
            *q++ = *s++;
        if (pg.n > 1) {
            *q++ = ' ';
            ui_fmt_int(q, pgi + 1);
            while (*q)
                q++;
            *q++ = '/';
            ui_fmt_int(q, pg.n);
        } else
            *q = 0;
        cv_text(4, 2, &FONT_S, t, C_GRAY);
    }
    if (ui.view == V_PART && is_303() && ui.held_step >= 0) {   /* the held step's knobs */
        static const char *const SN[4] = {"NOTE", "GATE", "ACCENT", "SLIDE"};
        const bstep_t *st = &cur_pat()->bass[bidx()].step[ui.held_step];
        for (i = 0; i < 4; i++) {
            char v[12];
            int x = i * 60;
            if (i == 0)
                fmt_note(v, st->note);
            else if (i == 1) {
                static const char *const G[3] = {"REST", "NOTE", "TIE"};
                const char *s = G[bstep_gate(st)];
                int k = 0;
                while ((v[k] = s[k]) != 0)
                    k++;
            } else {
                const char *s = (st->flags & (i == 2 ? BS_ACCENT : BS_SLIDE)) ? "ON" : "OFF";
                int k = 0;
                while ((v[k] = s[k]) != 0)
                    k++;
            }
            cv_text(x + 4, 22, &FONT_S, SN[i], C_GRAY);
            cv_text(x + 4, 42, &FONT_S, v, C_WHITE);
        }
        cv_commit(3, 0, KNOB_Y);
        return;
    }
    for (i = 0; i < 4; i++) {
        pref_t r = pg.r[pgi][i];
        const x0x_param_t *d = pref_desc(r);
        char v[12];
        int x = i * 60, val, w;
        if (!d)
            continue;
        val = pref_get(r);
        pref_value(r, v);
        cv_text(x + 4, 22, &FONT_S, pref_name(r), C_GRAY);
        cv_text(x + 4, 42, &FONT_S, v, C_HI);
        w = d->max ? val * 52 / d->max : 0;
        cv_rect(x + 4, 64, 52, 4, C_LINE);
        cv_rect(x + 4, 64, w, 4, part_col());
    }
    cv_commit(3, 0, KNOB_Y);
}

/* ---------------------------------------------------------------- LEDs --- */
static void leds(void)
{
    uint32_t b = 0, k = 0, i;
    int blink = (ui.frame & 8u) != 0;
    if (seq.playing && eng_step[TRK_DRUM] % 4 == 0)
        b |= 1u << B_PLAY;
    if (ui.rec)
        b |= 1u << B_REC;
    if (ui.view == V_HOME)
        b |= 1u << B_HOME;
    if (ui.view == V_PART)
        b |= 1u << B_EDIT;
    if (ui.view == V_GEN)
        b |= 1u << B_ARP;
    if (ui.view == V_FX)
        b |= 1u << B_FX;
    if (ui.view == V_MIX)
        b |= 1u << B_LFO;
    if (ui.view == V_GLO)
        b |= 1u << B_GLO;
    if (is_303() && ui.kbd[bidx()])
        b |= 1u << B_SEQ;
    if (ui.spage)
        b |= 1u << B_OCTUP;
    if (proj.set.keyled) {
        if (ui.view == V_HOME) {
            k |= 1u << WHITE_KEY[seq.cur];
            if (seq.cue < NPAT && blink)
                k |= 1u << WHITE_KEY[seq.cue];
        } else if (is_drum()) {
            const dpart_t *d = &cur_pat()->drum[ui.part];
            uint32_t bits = (ui.btn & (1u << B_ENV)) ? d->accent : d->hit[ui.sel[ui.part]];
            int ph = eng_step[TRK_DRUM + ui.part];
            for (i = 0; i < 16; i++) {
                int s = step_of((int)i), on = (bits >> s) & 1u;
                if (seq.playing && s == ph)
                    on = !on;                          /* the running light */
                if (on)
                    k |= 1u << WHITE_KEY[i];
            }
            k |= 1u << BLACK_KEY[ui.sel[ui.part]];
        } else if (is_303() && !ui.kbd[bidx()]) {
            const bpart_t *bp = &cur_pat()->bass[bidx()];
            int ph = eng_step[TRK_BASS0 + bidx()];
            for (i = 0; i < 16; i++) {
                int s = step_of((int)i), on = bstep_gate(&bp->step[s]) != G_REST && s < bp->len;
                if (seq.playing && s == ph)
                    on = !on;
                if (on)
                    k |= 1u << WHITE_KEY[i];
            }
        } else if (ui.part == PART_BRK) {
            int ph = eng_step[TRK_BRK];
            for (i = 0; i < 16; i++) {
                int on = (cur_pat()->brk.steps >> i) & 1u;
                if (seq.playing && (int)i == ph)
                    on = !on;
                if (on)
                    k |= 1u << WHITE_KEY[i];
            }
        }
    }
    k |= ui.keys;                                      /* a held key is always lit */
    plat_leds(b, k);
}

/* ---------------------------------------------------------------- frame --- */
void ui_init(void)
{
    int i;
    for (i = 0; i < NKEYS; i++) {
        if (KEY_WHITE[i] >= 0)
            WHITE_KEY[KEY_WHITE[i]] = (uint8_t)i;
        if (KEY_BLACK[i] >= 0)
            BLACK_KEY[KEY_BLACK[i]] = (uint8_t)i;
    }
    ui.view = V_PART;
    ui.part = PART_909;
    ui.held_step = -1;
    ui.chain_first = -1;
    palette_set(proj.set.palette);
    for (i = 0; i < 4; i++)
        blit_hash[i] = 0;
    build_pages();
}

void ui_frame(void)
{
    uint32_t i;
    input();
    for (i = 0; i < NPARTS; i++)                     /* meters fall ~ 300 ms */
        eng_peak[i] = (uint16_t)(eng_peak[i] - (eng_peak[i] >> 3));
    draw_header();
    draw_main();
    draw_knobs();
    leds();
    ui.frame++;
}

void ui_input_only(void) { input(); }
int ui_dirty(void) { return ui.dirty != 0; }
void ui_say(const char *a, const char *b) { say(a, b); }
