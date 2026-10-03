/* SPDX-License-Identifier: GPL-3.0-only */
/* X0X user interface v2 (docs/plans/2026-10-03-ui.md): panel input, screens, the knob strip,
 * lists, questions, the readout, and the key lights.
 *
 * Grammar: ALGORITHM = part, PRESETS = pattern, SELECT = move (page / list row), KNOB 1-4 =
 * the four values in the knob strip (on every screen), SEL = open / run / yes, HOME = back.
 *
 * The screen is four bands (header, main top, main bottom, knob strip), each drawn into
 * Felucca's canvas and blitted only when its pixels changed: SPI time is the scarce thing.
 *
 * Keys: F3..G5; the 16 white keys are steps (HOME: patterns), the 11 black keys a drum
 * machine's tracks or the break's slice pads (HOME: part mutes). */
#include "x0x.h"
#include "../dsp/fastmath.h"
#include "../dsp/master.h"

/* ================================================================ model === */
static const int8_t KEY_WHITE[NKEYS] = {0, -1, 1, -1, 2, -1, 3, 4, -1, 5, -1, 6, 7, -1, 8, -1, 9, -1, 10, 11, -1, 12, -1, 13, 14, -1, 15};
static const int8_t KEY_BLACK[NKEYS] = {-1, 0, -1, 1, -1, 2, -1, -1, 3, -1, 4, -1, -1, 5, -1, 6, -1, 7, -1, -1, 8, -1, 9, -1, -1, 10, -1};
static uint8_t WHITE_KEY[16], BLACK_KEY[11];

enum { V_HOME, V_PART, V_GEN, V_FX, V_MIX, V_GLO, NVIEWS };
static const char *const VIEW_N[NVIEWS] = {"HOME", "", "TB-3PO", "FX", "MIX", "GLOBAL"};
static const uint16_t PART_COL[NPARTS] = {RGB(255, 150, 40), RGB(255, 72, 64), RGB(130, 240, 90), RGB(70, 205, 255),
                                          RGB(214, 120, 255)};
static const char *const PART_N[NPARTS] = {"909", "808", "303A", "303B", "BREAK"};
static const char *const PART_S[NPARTS] = {"909", "808", "303A", "303B", "BRK"};   /* 4 chars: lane labels */

/* overlays drawn over the main area */
enum { O_NONE, O_LIST, O_ASK };
enum { ACT_NONE, ACT_SAVE, ACT_CLEAR_PART, ACT_CLEAR_PAT, ACT_COPY, ACT_RESET, ACT_ABOUT };

typedef struct {
    uint8_t view, part, prev_view;
    uint8_t sel[NKIT];
    uint8_t page[NVIEWS][NPARTS];
    uint8_t spage;
    uint8_t kbd[NBASS];
    int8_t oct;
    uint8_t rec;
    uint8_t wpos[NBASS];
    int8_t held_step;
    uint8_t step_edited;
    uint32_t btn, keys, btn_used;
    int8_t chain_first;
    uint32_t enc_t[NE];
    char msg[28];
    uint32_t msg_until;
    int8_t touched;                    /* knob cell being turned (4 = tempo), -1 none */
    uint32_t touch_until;
    uint8_t overlay;
    uint8_t list_sel, list_top, list_n;
    uint8_t ask_act, ask_arg;
    char ask_q[2][28];
    uint32_t frame;
    uint32_t dirty;
    uint8_t outline[232];              /* the break loop's outline */
    uint8_t outline_ok, outline_slot;
} ui_t;
static ui_t ui;

static pattern_t *cur_pat(void) { return &proj.pat[seq.cur]; }
static int is_303(void) { return ui.part == PART_303A || ui.part == PART_303B; }
static int is_drum(void) { return ui.part == PART_909 || ui.part == PART_808; }
static int bidx(void) { return ui.part == PART_303B ? 1 : 0; }

/* ============================================================ text helpers === */
static char *put_s(char *b, const char *s)
{
    while (*s)
        *b++ = *s++;
    *b = 0;
    return b;
}
static char *put_i(char *b, int v)
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
    return b;
}
static const char *const NOTE_N[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static char *put_note(char *b, int n)
{
    b = put_s(b, NOTE_N[(n % 12 + 12) % 12]);
    return put_i(b, n / 12 - 1);
}
static char *put_hex(char *b, uint32_t v, int digits)
{
    int i;
    for (i = digits - 1; i >= 0; i--)
        *b++ = "0123456789ABCDEF"[(v >> (4 * i)) & 15u];
    *b = 0;
    return b;
}

static void say(const char *a, const char *b)
{
    char *q = put_s(ui.msg, a);
    if (b)
        put_s(q, b);
    ui.msg_until = plat_ms() + 1300u;
}

/* =============================================================== param refs === */
enum { R_NONE, R_ENG, R_SWING, R_DLEN, R_DRATE, R_BLEN, R_BRATE, R_BDIR, R_BTRANS, R_GEN, R_BRKSET,
       R_BRKSLOT, R_TEMPO, R_ACCENT, R_CLKOUT, R_NOTEOUT, R_PALETTE, R_KEYLED, R_ACT };
typedef struct { uint8_t kind, a, b, c; } pref_t;
#define PR(k, a, b, c) ((pref_t){(k), (a), (b), (c)})
#define NONE PR(R_NONE, 0, 0, 0)

static const char *const RATE_N[] = {"1/16", "1/16T", "1/32", "1/8T"};
static const char *const DIR_N[] = {"FWD", "REV", "PING", "RND"};
static const char *const ONOFF_N[] = {"OFF", "ON"};
static const char *const ROOT_N[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char *const THEME_N[] = {"GREEN", "AMBER", "CYAN", "RED", "MONO"};
enum { G_DENS, G_ACC, G_SLIDE, G_OCTS, G_ROOT, G_SCALE, G_BASE, G_MUT, NGEN };
static const x0x_param_t GEN_P[NGEN] = {
    {"DENS", 100, 70, 0}, {"ACCENT", 100, 40, 0}, {"SLIDE", 100, 25, 0}, {"OCTS", 2, 1, 0},
    {"ROOT", 11, 9, ROOT_N}, {"SCALE", TB3PO_NSCALES - 1, 0, TB3PO_SCALE_NAMES}, {"OCTAVE", 4, 1, 0},
    {"MUTATE", 16, 0, 0},
};
enum { SQ_SWING, SQ_DLEN, SQ_DRATE, SQ_BLEN, SQ_BRATE, SQ_BDIR, SQ_BTRANS, SQ_TEMPO, SQ_ACCENT, SQ_CLK, SQ_NOTES,
       SQ_THEME, SQ_KEYLED, NSQ };
static const x0x_param_t SEQ_P[NSQ] = {
    {"SWING", 100, 0, 0}, {"LENGTH", 31, 15, 0}, {"RATE", 3, 0, RATE_N}, {"LENGTH", 31, 15, 0},
    {"RATE", 3, 0, RATE_N}, {"DIR", 3, 0, DIR_N}, {"TRANSP", 48, 24, 0}, {"TEMPO", 255, 105, 0},
    {"ACCENT", 127, 88, 0}, {"CLOCK OUT", 1, 1, ONOFF_N}, {"NOTES OUT", 1, 0, ONOFF_N}, {"THEME", 4, 1, THEME_N},
    {"KEY LIGHTS", 1, 1, ONOFF_N},
};
static const x0x_param_t ACT_P[] = {
    {"", 0, 0, 0}, {"SAVE PROJECT", 0, 0, 0}, {"CLEAR THIS PART", 0, 0, 0}, {"CLEAR PATTERN", 0, 0, 0},
    {"COPY PATTERN", 0, 0, 0}, {"FACTORY RESET", 0, 0, 0}, {"ABOUT X0X", 0, 0, 0},
};

static const x0x_param_t *pref_desc(pref_t r)
{
    switch (r.kind) {
    case R_ENG: return engine_param(r.a, r.b, r.c);
    case R_GEN: return &GEN_P[r.c % NGEN];
    case R_BRKSET: return breaks_param(r.c);
    case R_SWING: return &SEQ_P[SQ_SWING];
    case R_DLEN: return &SEQ_P[SQ_DLEN];
    case R_DRATE: return &SEQ_P[SQ_DRATE];
    case R_BLEN: return &SEQ_P[SQ_BLEN];
    case R_BRATE: return &SEQ_P[SQ_BRATE];
    case R_BDIR: return &SEQ_P[SQ_BDIR];
    case R_BTRANS: return &SEQ_P[SQ_BTRANS];
    case R_BRKSLOT: {
        static x0x_param_t d = {"LOOP", 0, 0, 0};
        d.max = (uint8_t)(engine_brk_nslots() - 1);
        d.names = engine_brk_slot_names();
        return &d;
    }
    case R_TEMPO: return &SEQ_P[SQ_TEMPO];
    case R_ACCENT: return &SEQ_P[SQ_ACCENT];
    case R_CLKOUT: return &SEQ_P[SQ_CLK];
    case R_NOTEOUT: return &SEQ_P[SQ_NOTES];
    case R_PALETTE: return &SEQ_P[SQ_THEME];
    case R_KEYLED: return &SEQ_P[SQ_KEYLED];
    case R_ACT: return &ACT_P[r.a];
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
    if (!d || r.kind == R_ACT)
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
    case R_BRKSET: p->brk.set[r.c] = (uint8_t)v; break;
    case R_BRKSLOT:
        if (r.a)
            p->brk.slot_b = (uint8_t)v;
        else
            p->brk.slot_a = (uint8_t)v;
        engine_brk_loops();
        ui.outline_ok = 0;
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

/* a knob's label: mixer refs name their part */
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

/* the value, as a number and a unit ("-6" "dB"); a switch gives its name and no unit */
static void pref_value(pref_t r, char *num, char *unit)
{
    const x0x_param_t *d = pref_desc(r);
    int v = pref_get(r);
    *num = *unit = 0;
    if (!d || r.kind == R_ACT)
        return;
    if (d->names) {
        put_s(num, d->names[v]);
        return;
    }
    switch (r.kind) {
    case R_ENG:
        if (r.a == T_MST) {                              /* the master formats itself: "-12dB" */
            char t[16], *q = t;
            engine_master_format(r.c, t);
            while (*q && (*q == '-' || *q == '.' || (*q >= '0' && *q <= '9')))
                q++;
            put_s(unit, q);
            *q = 0;
            put_s(num, t);
            return;
        }
        if (r.a == T_MIX) {
            if (r.c == 0) {                              /* level: 100 = 0 dB, square law */
                if (!v)
                    put_s(num, "OFF");
                else {
                    float db = 12.0412f * fm_log2f((float)v / 100.0f);
                    int t10 = (int)(db * 10.0f + (db < 0 ? -0.5f : 0.5f));
                    char *q = num;
                    if (t10 > 0)
                        *q++ = '+';
                    else if (t10 < 0)
                        *q++ = '-';
                    t10 = t10 < 0 ? -t10 : t10;
                    q = put_i(q, t10 / 10);
                    if (t10 < 100 && t10 % 10) {
                        *q++ = '.';
                        put_i(q, t10 % 10);
                    }
                    put_s(unit, "dB");
                }
                return;
            }
            put_i(num, v * 100 / 127);
            put_s(unit, "%");
            return;
        }
        put_i(num, v);
        return;
    case R_DLEN:
    case R_BLEN: put_i(num, v + 1); put_s(unit, "st"); return;
    case R_BTRANS: {
        char *q = num;
        if (v > 24)
            *q++ = '+';
        put_i(q, v - 24);
        put_s(unit, "st");
        return;
    }
    case R_SWING: put_i(num, 50 + v / 4); put_s(unit, "%"); return;
    case R_GEN:
        if (r.c == G_OCTS || r.c == G_BASE)
            put_i(num, v + 1);
        else if (r.c == G_MUT) {
            if (!v)
                put_s(num, "OFF");
            else {
                put_i(num, v);
                put_s(unit, "bar");
            }
        } else {
            put_i(num, v);
            put_s(unit, "%");
        }
        return;
    case R_BRKSET: put_i(num, v); put_s(unit, "%"); return;
    case R_TEMPO: put_i(num, v + 20); put_s(unit, "bpm"); return;
    default: put_i(num, v); return;
    }
}

/* ===================================================================== pages === */
#define MAXPAGES 14
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

static void add_eng_pages(int t, int v, int from, int to, const char *name)
{
    int n = engine_nparams(t, v), i;
    if (to > n)
        to = n;
    for (i = from; i < to && pg.n < MAXPAGES; i += 4) {
        int k;
        for (k = 0; k < 4; k++)
            pg.r[pg.n][k] = (i + k < to) ? PR(R_ENG, t, v, i + k) : NONE;
        title(pg.n, name);
        pg.n++;
    }
}

static void build_pages(void)
{
    int p = ui.part;
    pg.n = 0;
    switch (ui.view) {
    case V_HOME:
    case V_GLO:
        add_page("PERFORM", PR(R_TEMPO, 0, 0, 0), PR(R_SWING, 0, 0, 0), PR(R_ENG, T_MST, 0, MST_PUMP),
                 PR(R_ENG, T_MST, 0, MST_CUTOFF));
        add_page("LEVELS", PR(R_ENG, T_MIX, PART_909, 0), PR(R_ENG, T_MIX, PART_808, 0), PR(R_ENG, T_MIX, PART_303A, 0),
                 PR(R_ENG, T_MIX, PART_303B, 0));
        add_page("MORE", PR(R_ENG, T_MIX, PART_BRK, 0), PR(R_ACCENT, 0, 0, 0), PR(R_ENG, T_MST, 0, MST_MODE),
                 PR(R_ENG, T_MST, 0, MST_RESO));
        break;
    case V_PART:
        if (is_drum()) {
            int t = p == PART_909 ? T_909 : T_808, k = p;
            add_eng_pages(t, ui.sel[k], 0, 99, engine_voice_name(t, ui.sel[k]));
            add_page("SENDS", PR(R_ENG, T_MIX, p, 1), PR(R_ENG, T_MIX, p, 2), PR(R_ENG, T_MIX, p, 0), NONE);
            add_page("PART", PR(R_DLEN, k, 0, 0), PR(R_DRATE, k, 0, 0), PR(R_SWING, 0, 0, 0), PR(R_ACCENT, 0, 0, 0));
            add_eng_pages(t, NDRUM, 0, 99, "KIT");
        } else if (is_303()) {
            int b = bidx();
            add_eng_pages(T_303, b, 0, 4, "FILTER");
            add_eng_pages(T_303, b, 4, 8, "VOICE");
            add_eng_pages(T_303, b, 8, 12, "DRIVE");
            add_page("SENDS", PR(R_ENG, T_MIX, p, 1), PR(R_ENG, T_MIX, p, 2), PR(R_ENG, T_MIX, p, 0), NONE);
            add_page("LINE", PR(R_BLEN, b, 0, 0), PR(R_BRATE, b, 0, 0), PR(R_BDIR, b, 0, 0), PR(R_BTRANS, b, 0, 0));
        } else {
            add_page("GROOVE", PR(R_BRKSET, 0, 0, BRK_COMPLEX), PR(R_BRKSET, 0, 0, BRK_ANCHOR),
                     PR(R_BRKSET, 0, 0, BRK_ROLL), PR(R_BRKSET, 0, 0, BRK_FILL));
            add_page("RETRIG", PR(R_BRKSET, 0, 0, BRK_R2), PR(R_BRKSET, 0, 0, BRK_R3), PR(R_BRKSET, 0, 0, BRK_R4),
                     PR(R_BRKSET, 0, 0, BRK_R8));
            add_page("PHRASE", PR(R_BRKSET, 0, 0, BRK_PHRASE), PR(R_BRKSET, 0, 0, BRK_BCHANCE),
                     PR(R_BRKSET, 0, 0, BRK_ALEN), PR(R_BRKSET, 0, 0, BRK_BLEN));
            add_page("LOOPS", PR(R_BRKSLOT, 0, 0, 0), PR(R_BRKSLOT, 1, 0, 0), PR(R_ENG, T_BRK, 0, 0),
                     PR(R_ENG, T_BRK, 0, 1));
            add_page("SENDS", PR(R_ENG, T_MIX, PART_BRK, 1), PR(R_ENG, T_MIX, PART_BRK, 2),
                     PR(R_ENG, T_MIX, PART_BRK, 0), NONE);
        }
        break;
    case V_GEN: {
        int b = bidx();
        add_page("GENERATE", PR(R_GEN, b, 0, G_DENS), PR(R_GEN, b, 0, G_ACC), PR(R_GEN, b, 0, G_SLIDE),
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
        add_page("SENDS", PR(R_ENG, T_MIX, PART_BRK, 1), PR(R_ENG, T_MIX, PART_BRK, 2), NONE, NONE);
        add_page("REVERB", PR(R_ENG, T_FX, 0, FX_RV_DECAY), PR(R_ENG, T_FX, 0, FX_RV_TONE),
                 PR(R_ENG, T_FX, 0, FX_RV_HPF), PR(R_ENG, T_FX, 0, FX_RV_LEVEL));
        add_page("DELAY", PR(R_ENG, T_FX, 0, FX_DL_TIME), PR(R_ENG, T_FX, 0, FX_DL_FDBK),
                 PR(R_ENG, T_FX, 0, FX_DL_TONE), PR(R_ENG, T_FX, 0, FX_DL_LEVEL));
        add_page("TAPE", PR(R_ENG, T_FX, 0, FX_DL_TYPE), PR(R_ENG, T_FX, 0, FX_DL_WEAR),
                 PR(R_ENG, T_FX, 0, FX_DL_HPF), NONE);
        add_page("KIT DRIVE", PR(R_ENG, T_FX, 0, FX_VOLUME), PR(R_ENG, T_FX, 0, FX_DIST),
                 PR(R_ENG, T_FX, 0, FX_DRIVE), PR(R_ENG, T_FX, 0, FX_COMP));
        break;
    case V_MIX:
        add_page("LEVELS", PR(R_ENG, T_MIX, PART_909, 0), PR(R_ENG, T_MIX, PART_808, 0), PR(R_ENG, T_MIX, PART_303A, 0),
                 PR(R_ENG, T_MIX, PART_303B, 0));
        add_page("COMP", PR(R_ENG, T_MST, 0, MST_THRESH), PR(R_ENG, T_MST, 0, MST_RATIO),
                 PR(R_ENG, T_MST, 0, MST_ATTACK), PR(R_ENG, T_MST, 0, MST_RELEASE));
        add_page("COMP", PR(R_ENG, T_MST, 0, MST_MAKEUP), PR(R_ENG, T_MST, 0, MST_MIX), PR(R_ENG, T_MST, 0, MST_PUMP),
                 PR(R_ENG, T_MST, 0, MST_PUMPSRC));
        add_page("FILTER", PR(R_ENG, T_MST, 0, MST_MODE), PR(R_ENG, T_MST, 0, MST_CUTOFF),
                 PR(R_ENG, T_MST, 0, MST_RESO), PR(R_ENG, T_MST, 0, MST_LIMIT));
        add_page("MORE", PR(R_ENG, T_MIX, PART_BRK, 0), PR(R_SWING, 0, 0, 0), PR(R_TEMPO, 0, 0, 0),
                 PR(R_ACCENT, 0, 0, 0));
        break;
    default: break;
    }
    if (pg.n == 0)
        add_page("", NONE, NONE, NONE, NONE);
    if (ui.page[ui.view][ui.part] >= pg.n)
        ui.page[ui.view][ui.part] = 0;
}

static int cur_page(void) { return ui.page[ui.view][ui.part]; }

/* ===================================================================== lists === */
/* A list is rows of refs. SEL on a page opens the list of every page's knobs (the deep view);
 * GLOBAL is a list of its own. SELECT moves, ALGORITHM changes, SEL runs an action. */
#define LIST_MAX 64
static pref_t list_rows[LIST_MAX];
static char list_title[16];

static void list_add(pref_t r)
{
    int i;
    if (r.kind == R_NONE || ui.list_n >= LIST_MAX)
        return;
    for (i = 0; i < ui.list_n; i++)                   /* a ref on two pages (LEVEL) lists once */
        if (list_rows[i].kind == r.kind && list_rows[i].a == r.a && list_rows[i].b == r.b && list_rows[i].c == r.c)
            return;
    list_rows[ui.list_n++] = r;
}

static void open_list_of_pages(void)
{
    int p, k;
    ui.list_n = 0;
    for (p = 0; p < pg.n; p++)
        for (k = 0; k < 4; k++)
            list_add(pg.r[p][k]);
    put_s(list_title, ui.view == V_PART ? PART_N[ui.part] : VIEW_N[ui.view]);
    ui.list_sel = ui.list_top = 0;
    ui.overlay = ui.list_n ? O_LIST : O_NONE;
}

static void open_global(void)
{
    ui.list_n = 0;
    list_add(PR(R_CLKOUT, 0, 0, 0));
    list_add(PR(R_NOTEOUT, 0, 0, 0));
    list_add(PR(R_KEYLED, 0, 0, 0));
    list_add(PR(R_PALETTE, 0, 0, 0));
    list_add(PR(R_ACCENT, 0, 0, 0));
    list_add(PR(R_ACT, ACT_SAVE, 0, 0));
    list_add(PR(R_ACT, ACT_CLEAR_PAT, 0, 0));
    list_add(PR(R_ACT, ACT_RESET, 0, 0));
    list_add(PR(R_ACT, ACT_ABOUT, 0, 0));
    put_s(list_title, "GLOBAL");
    ui.list_sel = ui.list_top = 0;
    ui.overlay = O_LIST;
}

/* ================================================================ questions === */
static void ask(int act, int arg, const char *q1, const char *q2)
{
    ui.ask_act = (uint8_t)act;
    ui.ask_arg = (uint8_t)arg;
    put_s(ui.ask_q[0], q1);
    put_s(ui.ask_q[1], q2 ? q2 : "");
    ui.overlay = O_ASK;
}

static int pattern_used(const pattern_t *pt)
{
    int v, s, used = pt->brk.steps != 0;
    for (v = 0; v < NDRUM; v++)
        used |= (pt->drum[0].hit[v] | pt->drum[1].hit[v]) != 0;
    for (s = 0; s < NSTEPS; s++)
        used |= bstep_gate(&pt->bass[0].step[s]) != G_REST || bstep_gate(&pt->bass[1].step[s]) != G_REST;
    return used;
}

static void clear_part(pattern_t *p, int part)
{
    int i;
    if (part < NKIT) {
        for (i = 0; i < NDRUM; i++)
            p->drum[part].hit[i] = 0;
        p->drum[part].accent = 0;
    } else if (part < PART_BRK) {
        for (i = 0; i < NSTEPS; i++)
            p->bass[part - NKIT].step[i].flags = G_REST;
    } else {
        p->brk.steps = 0;
    }
}

static void save_project(void)
{
    int rc = project_save();
    say(rc ? "SAVE FAILED" : "SAVED", 0);
    if (!rc)
        ui.dirty = 0;
}

static void run_action(int act, int arg)
{
    char t[28], *q;
    switch (act) {
    case ACT_SAVE: save_project(); break;
    case ACT_CLEAR_PART:
        clear_part(cur_pat(), arg);
        ui.dirty = 1;
        say("CLEARED ", PART_N[arg]);
        break;
    case ACT_CLEAR_PAT: {
        int i;
        for (i = 0; i < NPARTS; i++)
            clear_part(cur_pat(), i);
        ui.dirty = 1;
        say("PATTERN CLEARED", 0);
        break;
    }
    case ACT_COPY:
        proj.pat[arg] = *cur_pat();
        ui.dirty = 1;
        put_i(put_s(t, "COPIED TO P"), arg + 1);
        say(t, 0);
        break;
    case ACT_RESET:
        project_defaults();
        engine_apply_sound(&proj.sound);
        engine_brk_loops();
        palette_set(proj.set.palette);
        ui.dirty = 1;
        ui.outline_ok = 0;
        say("FACTORY SOUND + PATTERNS", 0);
        break;
    case ACT_ABOUT:
        q = put_s(t, "X0X " X0X_VERSION "  AUDIO ");
        put_s(put_i(q, (int)plat_cpu_pct()), "%");
        say(t, 0);
        break;
    default: break;
    }
}

/* ===================================================================== input === */
static int32_t accel(int role, int32_t s, int range)
{
    uint32_t now = plat_ms(), dt = now - ui.enc_t[role];
    ui.enc_t[role] = now;
    if (range > 40 && dt < 50u)
        return s * (range > 150 ? 6 : 3);
    return s;
}

static void set_view(int v)
{
    if (v != ui.view)
        ui.prev_view = ui.view;
    ui.view = (uint8_t)v;
    ui.held_step = -1;
    ui.overlay = O_NONE;
    build_pages();
}

static void turn_ref(pref_t r, int e, int knob)
{
    const x0x_param_t *d = pref_desc(r);
    if (!d || r.kind == R_ACT)
        return;
    if (d->names || d->max < 24)
        e = e > 0 ? 1 : -1;                          /* a switch: one detent, one position */
    else
        e = accel(knob, e, d->max);
    pref_set(r, pref_get(r) + e);
}

static int rec_step(int track) { return eng_step[track]; }

static void drum_key(int v, int down)
{
    int k = ui.part;
    if (!down)
        return;
    ui.sel[k] = (uint8_t)v;
    build_pages();
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

static int kbd_note(int k) { return 41 + k + 12 * ui.oct; }

static void bass_write(int b, int note, int gate)
{
    bpart_t *bp = &cur_pat()->bass[b];
    int len = bp->len ? bp->len : 16, w = ui.wpos[b] % len;
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
    ui.wpos[b] = (uint8_t)((w + 1) % len);
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
        if (!ui.step_edited) {
            bp->step[s].flags = bstep_gate(&bp->step[s]) == G_REST ? G_NOTE : G_REST;
            ui.dirty = 1;
        }
        engine_bass_off(b);
        ui.held_step = -1;
    }
}

static const uint32_t MUTE_MASK[NPARTS] = {(1u << NDRUM) - 1u, ((1u << NDRUM) - 1u) << NDRUM, 1u << MUTE_BASS0,
                                           1u << (MUTE_BASS0 + 1), 1u << MUTE_BRK};
static int part_muted(int p) { return (seq.mute & MUTE_MASK[p]) == MUTE_MASK[p]; }

static void key_event(int k, int down)
{
    int w = KEY_WHITE[k], bl = KEY_BLACK[k];
    if (ui.overlay == O_ASK)
        return;
    if (down && w >= 0 && (ui.btn & (1u << B_SAVE))) {     /* SAVE held + white key: copy here */
        ui.btn_used |= 1u << B_SAVE;
        if (w == seq.cur)
            return;
        if (pattern_used(&proj.pat[w])) {
            char t[24];
            put_s(put_i(put_s(t, "COPY OVER P"), w + 1), "?");
            ask(ACT_COPY, w, t, "IT HAS NOTES");
        } else
            run_action(ACT_COPY, w);
        return;
    }
    if (w >= 0 && (ui.view == V_HOME || (ui.btn & (1u << B_HOME)))) {
        if (ui.btn & (1u << B_HOME))
            ui.btn_used |= 1u << B_HOME;
        if (down) {
            if (ui.chain_first >= 0 && ui.chain_first != w) {
                char t[16], *q = t;
                seq_chain(&seq, ui.chain_first, w);
                q = put_i(q, (ui.chain_first < w ? ui.chain_first : w) + 1);
                *q++ = '-';
                put_i(q, (ui.chain_first < w ? w : ui.chain_first) + 1);
                say("CHAIN P", t);
            } else {
                ui.chain_first = (int8_t)w;
                seq_chain(&seq, 0, 0);
                seq_cue(&seq, w);
            }
        } else if (ui.chain_first == w) {
            ui.chain_first = -1;
        }
        return;
    }
    if (ui.view == V_HOME && bl >= 0) {
        if (down && bl < NPARTS) {
            seq.mute = part_muted(bl) ? seq.mute & ~MUTE_MASK[bl] : seq.mute | MUTE_MASK[bl];
            say(part_muted(bl) ? "MUTED " : "UNMUTED ", PART_N[bl]);
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
    } else {
        if (bl >= 0)
            engine_brk_live(bl, down);
        else if (down && w >= 0) {
            cur_pat()->brk.steps ^= 1u << w;
            ui.dirty = 1;
        }
    }
}

static void list_move(int d)
{
    int s = ui.list_sel + d;
    s = s < 0 ? 0 : s >= ui.list_n ? ui.list_n - 1 : s;
    ui.list_sel = (uint8_t)s;
    if (ui.list_sel < ui.list_top)
        ui.list_top = ui.list_sel;
    if (ui.list_sel >= ui.list_top + 6)
        ui.list_top = (uint8_t)(ui.list_sel - 5);
}

static void list_enter(void)
{
    pref_t r = list_rows[ui.list_sel];
    if (r.kind != R_ACT)
        return;
    if (r.a == ACT_CLEAR_PAT)
        ask(ACT_CLEAR_PAT, 0, "CLEAR THIS PATTERN?", "ALL FIVE PARTS");
    else if (r.a == ACT_RESET)
        ask(ACT_RESET, 0, "FACTORY RESET?", "SOUND + 16 PATTERNS");
    else
        run_action(r.a, 0);
}

static void close_overlay(void)
{
    if (ui.view == V_GLO) {
        ui.overlay = O_NONE;
        set_view(ui.prev_view == V_GLO ? V_PART : ui.prev_view);
        return;
    }
    ui.overlay = O_NONE;
}

static void button_tap(int b)
{
    if (ui.overlay == O_ASK) {                        /* a question takes SEL or HOME only */
        if (b == B_SEL) {
            ui.overlay = O_NONE;
            run_action(ui.ask_act, ui.ask_arg);
        } else if (b == B_HOME) {
            ui.overlay = O_NONE;
            say("CANCELLED", 0);
        } else
            return;
        if (ui.view == V_GLO)
            ui.overlay = O_LIST;
        return;
    }
    switch (b) {
    case B_PLAY:
        if (seq.playing)
            seq_stop(&seq);
        else
            seq_start(&seq);
        break;
    case B_REC:
        if (ui.btn & (1u << B_SAVE)) {               /* SAVE + REC: clear this part (asks) */
            char t[24];
            ui.btn_used |= 1u << B_SAVE;
            put_s(put_s(t, "CLEAR "), PART_N[ui.part]);
            ask(ACT_CLEAR_PART, ui.part, t, "IN THIS PATTERN?");
            break;
        }
        ui.rec = !ui.rec;
        if (ui.rec && is_303())
            ui.wpos[bidx()] = 0;
        say(ui.rec ? "RECORD ON" : "RECORD OFF", 0);
        break;
    case B_HOME:
        if (ui.overlay != O_NONE || ui.view == V_GLO)
            close_overlay();
        else
            set_view(ui.view == V_HOME ? V_PART : V_HOME);
        break;
    case B_SEL:
        if (ui.overlay == O_LIST)
            list_enter();
        else
            open_list_of_pages();
        break;
    case B_EDIT:
        if (ui.view == V_PART && ui.overlay == O_NONE)
            ui.page[V_PART][ui.part] = (uint8_t)((cur_page() + 1) % pg.n);
        else
            set_view(V_PART);
        break;
    case B_ARP:
        if (!is_303()) {
            say("TB-3PO: PICK 303A OR 303B", 0);
            break;
        }
        if (ui.view != V_GEN)
            set_view(V_GEN);
        else
            ui.page[V_GEN][ui.part] = (uint8_t)((cur_page() + 1) % pg.n);
        break;
    case B_FX:
        if (ui.view == V_FX && ui.overlay == O_NONE)
            ui.page[V_FX][ui.part] = (uint8_t)((cur_page() + 1) % pg.n);
        else
            set_view(V_FX);
        break;
    case B_LFO:
        if (ui.view == V_MIX && ui.overlay == O_NONE)
            ui.page[V_MIX][ui.part] = (uint8_t)((cur_page() + 1) % pg.n);
        else
            set_view(V_MIX);
        break;
    case B_GLO:
        if (ui.view == V_GLO)
            close_overlay();
        else {
            set_view(V_GLO);
            open_global();
        }
        break;
    case B_SEQ:
        if (is_303()) {
            ui.kbd[bidx()] = !ui.kbd[bidx()];
            say(ui.kbd[bidx()] ? "303: KEYBOARD" : "303: STEPS", 0);
        }
        if (ui.view != V_PART)
            set_view(V_PART);
        break;
    case B_ENV:
        if (is_303() && ui.kbd[bidx()] && ui.rec && !seq.playing)
            bass_write(bidx(), 0, G_REST);
        break;
    case B_SAVE: save_project(); break;
    case B_OCTUP:
    case B_OCTDN: {
        int up = b == B_OCTUP;
        if (ui.view == V_GEN) {
            bpart_t *bp = &cur_pat()->bass[bidx()];
            if (up) {
                char t[20];
                bp->gen.seed = tb3po_new_seed(plat_ms() ^ (ui.frame << 7));
                tb3po_generate(bp);
                put_hex(put_s(t, "NEW LINE "), bp->gen.seed & 0xFFFFu, 4);
                say(t, 0);
            } else {
                uint32_t r = bp->gen.seed ^ plat_ms();
                tb3po_mutate(bp, &r);
                say("MUTATED", 0);
            }
            ui.dirty = 1;
        } else if (is_303() && ui.kbd[bidx()]) {
            if (ui.btn & (1u << B_LFO)) {
                ui.btn_used |= 1u << B_LFO;
                bass_write(bidx(), 0, G_TIE);
            } else {
                char t[12];
                ui.oct = (int8_t)(ui.oct + (up ? 1 : -1));
                ui.oct = ui.oct < -2 ? -2 : ui.oct > 3 ? 3 : ui.oct;
                put_i(put_s(t, "OCTAVE "), ui.oct);
                say(t, 0);
            }
        } else {
            ui.spage = (uint8_t)up;
            say(up ? "STEPS 17-32" : "STEPS 1-16", 0);
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
    ch = btn ^ ui.btn;
    for (i = 0; i < NB; i++) {
        uint32_t m = 1u << i;
        if (!(ch & m))
            continue;
        if (btn & m) {
            ui.btn_used &= ~m;
            if (i == B_REC || i == B_PLAY)             /* transport acts on the press: timing */
                button_tap((int)i);
        } else if (!(ui.btn_used & m) && i != B_REC && i != B_PLAY) {
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
    if (ui.overlay == O_ASK) {                         /* a question: the knobs wait */
        for (i = 0; i < NE; i++)
            plat_enc((int)i);
        return;
    }
    if ((e = plat_enc(EN_SELECT)) != 0) {             /* SELECT: move; with HOME held: tempo */
        if (ui.btn & (1u << B_HOME)) {
            float bpm = seq.bpm + (float)accel(EN_SELECT, e, 200);
            seq.bpm = bpm < 20.0f ? 20.0f : bpm > 275.0f ? 275.0f : bpm;
            ui.btn_used |= 1u << B_HOME;
            ui.touched = 4;
            ui.touch_until = plat_ms() + 900u;
            ui.dirty = 1;
        } else if (ui.overlay == O_LIST) {
            list_move(e > 0 ? 1 : -1);
        } else {
            int p = cur_page() + (e > 0 ? 1 : -1);
            ui.page[ui.view][ui.part] = (uint8_t)(p < 0 ? 0 : p >= pg.n ? pg.n - 1 : p);
        }
    }
    if ((e = plat_enc(EN_ALGO)) != 0) {               /* ALGORITHM: the part; in a list, the value */
        if (ui.overlay == O_LIST) {
            turn_ref(list_rows[ui.list_sel], e, EN_ALGO);
        } else {
            int p = ui.part + (e > 0 ? 1 : -1);
            ui.part = (uint8_t)(p < 0 ? 0 : p >= NPARTS ? NPARTS - 1 : p);
            if (ui.view == V_HOME || (ui.view == V_GEN && !is_303()))
                set_view(V_PART);
            ui.outline_ok = 0;
            build_pages();
        }
    }
    if ((e = plat_enc(EN_PRESET)) != 0) {
        int base = seq.cue < NPAT ? seq.cue : seq.cur, p = base + (e > 0 ? 1 : -1);
        p = p < 0 ? 0 : p >= NPAT ? NPAT - 1 : p;
        seq_chain(&seq, 0, 0);
        seq_cue(&seq, p);
    }
    for (i = 0; i < 4; i++) {
        if ((e = plat_enc(EN_K1 + (int)i)) == 0)
            continue;
        if (ui.view == V_PART && is_303() && ui.held_step >= 0) {     /* a held step's knobs edit it */
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
                st->flags = (uint8_t)((st->flags & ~BS_GATE_MASK) | (bstep_gate(st) + (e > 0 ? 1 : 2)) % 3);
            } else if (i == 2) {
                st->flags ^= BS_ACCENT;
            } else {
                st->flags ^= BS_SLIDE;
            }
            continue;
        }
        turn_ref(pg.r[cur_page()][i], e, EN_K1 + (int)i);
        ui.touched = (int8_t)i;
        ui.touch_until = plat_ms() + 900u;
    }
}

/* ================================================================== drawing === */
#define HDR_H 20
#define MAIN_Y 20
#define BAND_H 72
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

static uint16_t blend(uint16_t a, uint16_t b, int t)  /* a..b, t of 16 */
{
    int r = ((a >> 11) * (16 - t) + (b >> 11) * t) >> 4, g = (((a >> 5) & 63) * (16 - t) + ((b >> 5) & 63) * t) >> 4,
        bl = ((a & 31) * (16 - t) + (b & 31) * t) >> 4;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}
static uint16_t part_col(void) { return PART_COL[ui.part]; }
static uint16_t dim(uint16_t c, int t) { return blend(C_BLACK, c, t); }

static int32_t tw(const felucca_font_t *f, const char *s) { return text_w(f, s); }

/* a font's capital line: the first and last rows of 'H' (rounded half coverage), measured
 * once from the glyph bitmap, so text is centred on what the eye sees rather than on the
 * font's cell (which differs per face and size) */
typedef struct { const felucca_font_t *f; int8_t top, bot; } capm_t;
static capm_t capm[6];
static const capm_t *caps(const felucca_font_t *f)
{
    int i;
    for (i = 0; i < 6 && capm[i].f; i++)
        if (capm[i].f == f)
            return &capm[i];
    if (i == 6)
        i = 5;
    {
        uint32_t gi = 'H' - f->first, w = f->bw[gi], bpr = (w + 1u) / 2u, x, y;
        const uint8_t *gd = f->data + f->off[gi];
        int top = -1, bot = 0;
        for (y = 0; y < f->h; y++)
            for (x = 0; x < w; x++) {
                uint32_t a = gd[y * bpr + x / 2u];
                a = (x & 1u) ? (a & 15u) : (a >> 4);
                if (a >= 8u) {
                    if (top < 0)
                        top = (int)y;
                    bot = (int)y;
                }
            }
        capm[i].f = f;
        capm[i].top = (int8_t)(top < 0 ? 0 : top);
        capm[i].bot = (int8_t)bot;
    }
    return &capm[i];
}
/* parts of a line ("A 909 GR", "B 909 FL") with a real gap between them: a proportional face's
 * space is too narrow to separate them. A NULL ends the list. */
static int32_t segs(int32_t x, int32_t y, const felucca_font_t *f, uint16_t c, int32_t gap, const char *a,
                    const char *b, const char *d, const char *e)
{
    const char *p[4] = {a, b, d, e};
    int i;
    for (i = 0; i < 4 && p[i]; i++)
        x = cv_text(x, y, f, p[i], c) + gap;
    return x;
}

/* the y to draw f at so its capitals sit centred in [y0, y0 + h) */
static int32_t vc(const felucca_font_t *f, int32_t y0, int32_t h)
{
    const capm_t *m = caps(f);
    int32_t ch = m->bot - m->top + 1;
    return y0 + (h - ch + 1) / 2 - m->top;
}
/* the y to draw g at so its baseline matches f drawn at y */
static int32_t base_y(const felucca_font_t *f, int32_t y, const felucca_font_t *g)
{
    return y + caps(f)->bot - caps(g)->bot;
}
static void text_c(int32_t cx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    cv_text(cx - tw(f, s) / 2, y, f, s, c);
}
static void text_r(int32_t rx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    cv_text(rx - tw(f, s), y, f, s, c);
}

/* ---- anti-aliased shapes. Coverage is 0..16; a pixel is blended over what is there. ---- */
static void px_blend(int32_t x, int32_t y, uint16_t c, uint32_t a)
{
    uint16_t *q;
    uint32_t bg;
    y += cv_oy;
    if ((uint32_t)x >= cv_w || (uint32_t)y >= cv_h || !a)
        return;
    q = &cv_px[(uint32_t)y * cv_w + (uint32_t)x];
    if (a >= 16u) {
        *q = swap16(c);
        return;
    }
    bg = swap16(*q);
    *q = swap16((uint32_t)(((((c >> 11) * a + (bg >> 11) * (16u - a)) >> 4) << 11) |
                           (((((c >> 5) & 63u) * a + ((bg >> 5) & 63u) * (16u - a)) >> 4) << 5) |
                           (((c & 31u) * a + (bg & 31u) * (16u - a)) >> 4)));
}

/* corner coverage of a radius-r quarter circle, pixel (x, y) of the r x r corner square,
 * 4 x 4 supersampled; and of its 1-px ring (for outlines). Built once. */
#define RMAX 8
static uint8_t corner_fill[RMAX + 1][RMAX][RMAX], corner_ring[RMAX + 1][RMAX][RMAX];
static void corners_init(void)
{
    int r, x, y, sx, sy;
    for (r = 1; r <= RMAX; r++)
        for (y = 0; y < r; y++)
            for (x = 0; x < r; x++) {
                int in = 0, ring = 0;
                for (sy = 0; sy < 4; sy++)
                    for (sx = 0; sx < 4; sx++) {
                        float dx = (float)r - ((float)x + (sx + 0.5f) / 4.0f);
                        float dy = (float)r - ((float)y + (sy + 0.5f) / 4.0f);
                        float d2 = dx * dx + dy * dy;
                        if (d2 <= (float)(r * r)) {
                            in++;
                            if (d2 >= (float)((r - 1) * (r - 1)))
                                ring++;
                        }
                    }
                corner_fill[r][y][x] = (uint8_t)in;
                corner_ring[r][y][x] = (uint8_t)ring;
            }
}

/* a filled rounded rectangle; r is clamped to fit */
static void rbox(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t c)
{
    int32_t i, j;
    if (w <= 0 || h <= 0)
        return;
    if (r > w / 2)
        r = w / 2;
    if (r > h / 2)
        r = h / 2;
    if (r > RMAX)
        r = RMAX;
    if (r < 1) {
        cv_rect(x, y, w, h, c);
        return;
    }
    cv_rect(x + r, y, w - 2 * r, h, c);
    cv_rect(x, y + r, r, h - 2 * r, c);
    cv_rect(x + w - r, y + r, r, h - 2 * r, c);
    for (j = 0; j < r; j++)
        for (i = 0; i < r; i++) {
            uint32_t a = corner_fill[r][j][i];
            px_blend(x + i, y + j, c, a);
            px_blend(x + w - 1 - i, y + j, c, a);
            px_blend(x + i, y + h - 1 - j, c, a);
            px_blend(x + w - 1 - i, y + h - 1 - j, c, a);
        }
}

/* a 1-px rounded outline */
static void rframe(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t c)
{
    int32_t i, j;
    if (r > w / 2)
        r = w / 2;
    if (r > h / 2)
        r = h / 2;
    if (r > RMAX)
        r = RMAX;
    if (r < 1)
        r = 1;
    cv_rect(x + r, y, w - 2 * r, 1, c);
    cv_rect(x + r, y + h - 1, w - 2 * r, 1, c);
    cv_rect(x, y + r, 1, h - 2 * r, c);
    cv_rect(x + w - 1, y + r, 1, h - 2 * r, c);
    for (j = 0; j < r; j++)
        for (i = 0; i < r; i++) {
            uint32_t a = corner_ring[r][j][i];
            px_blend(x + i, y + j, c, a);
            px_blend(x + w - 1 - i, y + j, c, a);
            px_blend(x + i, y + h - 1 - j, c, a);
            px_blend(x + w - 1 - i, y + h - 1 - j, c, a);
        }
}

/* the radius that suits a box of height h: soft, never round */
static int32_t rad_of(int32_t h) { return h >= 30 ? 6 : h >= 18 ? 4 : h >= 9 ? 3 : 2; }
static void box(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c) { rbox(x, y, w, h, rad_of(h < w ? h : w), c); }
static void frame(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c) { rframe(x, y, w, h, rad_of(h < w ? h : w), c); }

/* an anti-aliased disc of radius r (float) at (cx, cy) */
static void disc(float cx, float cy, float r, uint16_t c)
{
    int32_t x0 = (int32_t)fm_floorf(cx - r - 1.0f), x1 = (int32_t)(cx + r + 1.0f);
    int32_t y0 = (int32_t)fm_floorf(cy - r - 1.0f), y1 = (int32_t)(cy + r + 1.0f), x, y;
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy, d = fm_sqrtf(dx * dx + dy * dy);
            float a = fm_clampf(r - d + 0.5f, 0.0f, 1.0f);
            px_blend(x, y, c, (uint32_t)(a * 16.0f + 0.5f));
        }
}
static void dot(int32_t x, int32_t y, int32_t r, uint16_t c) { disc((float)x + 0.5f, (float)y + 0.5f, (float)r + 0.3f, c); }

/* atan2 in turns, 0..1 from +x going clockwise on screen (y down) */
static float turns(float y, float x)
{
    float ax = fm_fabsf(x), ay = fm_fabsf(y), a = ax < ay ? ax / (ay + 1e-9f) : ay / (ax + 1e-9f), s = a * a, r;
    r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;   /* atan on [0, 1] */
    if (ay > ax)
        r = 1.57079637f - r;
    if (x < 0.0f)
        r = FM_PI - r;
    if (y < 0.0f)
        r = FM_TWO_PI - r;
    return r * (1.0f / FM_TWO_PI);
}

/* a knob: a 270-degree track from 7:30 to 4:30, thickness th, anti-aliased, round ends; the
 * value filled from the start (or from the top, for a bipolar value) and a round handle */
static void arc(int32_t cx, int32_t cy, int32_t r, float v, uint16_t track, uint16_t fill, int bipolar)
{
    const float th = 3.4f, a0 = 0.375f, sweep = 0.75f;  /* start at 135 degrees, in turns */
    float ro = (float)r, ri = ro - th, rm = ro - th * 0.5f, fx = (float)cx + 0.5f, fy = (float)cy + 0.5f;
    float lo = bipolar ? (v < 0.5f ? v : 0.5f) : 0.0f, hi = bipolar ? (v < 0.5f ? 0.5f : v) : v;
    int32_t x, y;
    v = fm_clampf(v, 0.0f, 1.0f);
    for (y = -r - 1; y <= r + 1; y++)
        for (x = -r - 1; x <= r + 1; x++) {
            float dx = (float)x + 0.5f - 0.5f, dy = (float)y + 0.5f - 0.5f, d = fm_sqrtf(dx * dx + dy * dy), t, a;
            a = fm_clampf(ro + 0.5f - d, 0.0f, 1.0f) * fm_clampf(d - ri + 0.5f, 0.0f, 1.0f);
            if (a <= 0.0f)
                continue;
            t = turns(dy, dx) - a0;
            if (t < 0.0f)
                t += 1.0f;
            t /= sweep;
            if (t > 1.0f)
                continue;                              /* the gap at the bottom; the caps close it */
            px_blend(cx + x, cy + y, (t >= lo && t <= hi) ? fill : track, (uint32_t)(a * 16.0f + 0.5f));
        }
    {   /* round ends: the track's, then the fill's, then the handle */
        float e0 = (a0) * FM_TWO_PI, e1 = (a0 + sweep) * FM_TWO_PI, ev = (a0 + sweep * v) * FM_TWO_PI;
        float el = (a0 + sweep * lo) * FM_TWO_PI;
        disc(fx + fm_cosf(e0) * rm - 0.5f, fy + fm_sinf(e0) * rm - 0.5f, th * 0.5f, lo <= 0.0f && hi > 0.0f ? fill : track);
        disc(fx + fm_cosf(e1) * rm - 0.5f, fy + fm_sinf(e1) * rm - 0.5f, th * 0.5f, hi >= 1.0f ? fill : track);
        if (hi > lo)
            disc(fx + fm_cosf(el) * rm - 0.5f, fy + fm_sinf(el) * rm - 0.5f, th * 0.5f, fill);
        disc(fx + fm_cosf(ev) * rm - 0.5f, fy + fm_sinf(ev) * rm - 0.5f, th * 0.5f + 1.3f, C_BLACK);
        disc(fx + fm_cosf(ev) * rm - 0.5f, fy + fm_sinf(ev) * rm - 0.5f, th * 0.5f + 0.6f, C_WHITE);
    }
}

static void play_icon(int32_t x, int32_t y, uint16_t c)
{
    int32_t i, j;
    for (j = 0; j < 11; j++)                           /* a triangle, edges anti-aliased */
        for (i = 0; i < 9; i++) {
            float fy = (float)j + 0.5f - 5.5f, fx = (float)i + 0.5f;
            float edge = 8.0f - fx - fm_fabsf(fy) * 1.3f;
            float a = fm_clampf(edge * 0.7f + 0.5f, 0.0f, 1.0f) * fm_clampf(fx + 0.5f, 0.0f, 1.0f);
            px_blend(x + i, y + j, c, (uint32_t)(a * 16.0f + 0.5f));
        }
}

/* -------------------------------------------------------------- header --- */
static void draw_header(void)
{
    char b[24];
    int32_t x;
    cv_begin(240, HDR_H, C_BLACK);
    cv_rect(0, HDR_H - 1, 240, 1, C_LINE);
    if (ui.msg_until && (int32_t)(ui.msg_until - plat_ms()) > 0) {   /* what a button just did */
        box(0, 0, 240, HDR_H - 1, part_col());
        text_c(120, vc(&FONT_B, 0, HDR_H - 1), &FONT_B, ui.msg, C_BLACK);
        cv_commit(0, 0, 0);
        return;
    }
    ui.msg_until = 0;
    if (ui.view == V_PART || ui.view == V_GEN) {      /* the part chip, then where we are */
        int32_t w = tw(&FONT_B, PART_N[ui.part]) + 12;
        box(0, 1, w, 17, part_col());
        cv_text(6, vc(&FONT_B, 1, 17), &FONT_B, PART_N[ui.part], C_BLACK);
        x = w + 6;
        if (ui.view == V_GEN)
            x = cv_text(x, vc(&FONT_S, 1, 17), &FONT_S, "TB-3PO", C_HI) + 6;
        else if (is_drum())
            x = cv_text(x, vc(&FONT_S, 1, 17), &FONT_S, engine_voice_name(ui.part == PART_909 ? T_909 : T_808, ui.sel[ui.part]), C_HI) + 6;
        else if (is_303() && ui.kbd[bidx()])
            x = cv_text(x, vc(&FONT_S, 1, 17), &FONT_S, "KEYS", C_AMB) + 6;
    } else {
        x = cv_text(4, vc(&FONT_B, 1, 17), &FONT_B, VIEW_N[ui.view], C_HI) + 6;
    }
    if (pg.n > 1 && ui.overlay == O_NONE && ui.view != V_GLO) {   /* page dots */
        int i, p = cur_page();
        for (i = 0; i < pg.n && i < 9; i++)
            cv_rect(x + i * 5, 8, 3, 3, i == p ? C_HI : C_LINE);
    }
    b[0] = 'P';
    put_i(b + 1, seq.cur + 1);
    x = cv_text(132, vc(&FONT_B, 1, 17), &FONT_B, b, C_WHITE);
    if (seq.cue < NPAT && seq.cue != seq.cur) {
        b[0] = '>';
        put_i(b + 1, seq.cue + 1);
        cv_text(x + 2, vc(&FONT_S, 1, 17), &FONT_S, b, (ui.frame & 16u) ? C_WHITE : C_GRAY);
    } else if (seq.chain_a != seq.chain_b) {
        cv_text(x + 3, vc(&FONT_XS, 1, 17), &FONT_XS, "CHN", C_AMB);
    }
    put_i(b, (int)(seq_tempo(&seq) + 0.5f));
    text_r(212, vc(&FONT_S, 1, 17), &FONT_S, b, seq.ext ? C_AMB : C_HI);
    if (seq.playing)
        play_icon(217, 4, (eng_step[TRK_DRUM] & 3) == 0 ? C_WHITE : C_HI);
    if (ui.rec)
        dot(231, 9, 4, RGB(255, 50, 50));
    else if (ui.dirty)
        dot(233, 9, 1, C_GRAY);
    cv_commit(0, 0, 0);
}

/* -------------------------------------------------------------- the views --- */
static int step_of(int col) { return ui.spage * 16 + col; }
#define GX 27
static int32_t col_x(int c) { return GX + c * 13 + (c >> 2); }   /* a pixel between beats */

static void draw_drum(int band)
{
    int k = ui.part, v, c, len = cur_pat()->drum[k].len;
    const dpart_t *d = &cur_pat()->drum[k];
    int ph = seq.playing ? eng_step[TRK_DRUM + k] : -1;
    uint16_t col = part_col(), on_dim = dim(col, 8);
    for (v = band * 6; v < (band ? NDRUM + 1 : 6); v++) {
        int y = (v - band * 6) * 12 + 1, sel = v < NDRUM && v == ui.sel[k];
        const char *nm = v < NDRUM ? engine_voice_name(k == PART_909 ? T_909 : T_808, v) : "AC";
        uint32_t bits = v < NDRUM ? d->hit[v] : d->accent;
        int muted = v < NDRUM && (seq.mute & (1u << (k * NDRUM + v)));
        if (sel)
            box(0, y - 1, 24, 12, col);
        cv_text(5, vc(&FONT_XS, y - 1, 12), &FONT_XS, nm, sel ? C_BLACK : muted ? C_LINE : v < NDRUM ? C_GRAY : C_WHITE);
        for (c = 0; c < 16; c++) {
            int s = step_of(c), x = col_x(c), hit = (bits >> s) & 1u;
            if (s >= len) {
                cv_rect(x + 5, y + 4, 2, 2, C_LINE);
                continue;
            }
            if (v == NDRUM) {                          /* the accent row: white marks */
                if (hit)
                    box(x + 2, y + 2, 8, 6, C_WHITE);
                else
                    rbox(x + 2, y + 4, 8, 2, 1, C_LINE);
                continue;
            }
            box(x, y, 12, 10, hit ? (sel ? col : on_dim) : (s == ph ? C_DIM : C_LINE));
            if (hit && s == ph)
                box(x + 2, y + 2, 8, 6, C_WHITE);
        }
    }
    if (band == 1 && len > 16)
        cv_text(2, 60, &FONT_XS, ui.spage ? "17-32" : "1-16", C_GRAY);
}

static int note_y(int n, int lo, int hi) { return 64 - (n - lo) * 58 / (hi - lo); }

static void draw_303(int band, int gen)
{
    int b = bidx(), c;
    const bpart_t *bp = &cur_pat()->bass[b];
    int ph = seq.playing ? eng_step[TRK_BASS0 + b] : -1, lo = 127, hi = 0;
    uint16_t col = part_col();
    for (c = 0; c < bp->len && c < NSTEPS; c++)
        if (bstep_gate(&bp->step[c]) != G_REST) {
            if (bp->step[c].note < lo)
                lo = bp->step[c].note;
            if (bp->step[c].note > hi)
                hi = bp->step[c].note;
        }
    if (lo > hi)
        lo = hi = 12 * (bp->gen.base_oct + 1) + bp->gen.root;
    lo -= 2;
    hi += 2;
    if (hi - lo < 14)
        hi = lo + 14;
    if (band == 0) {                                   /* the line: pitch by height, the root's octaves ruled */
        int n;
        for (n = lo; n <= hi; n++)
            if ((n - bp->gen.root) % 12 == 0) {
                int y = note_y(n, lo, hi);
                char t[6];
                cv_rect(GX, y + 2, 213, 1, C_LINE);
                put_note(t, n);
                cv_text(1, y - 4, &FONT_XS, t, C_DIM);
            }
        for (c = 0; c < 16; c++) {
            int s = step_of(c), x = col_x(c), g;
            const bstep_t *st = &bp->step[s < NSTEPS ? s : 0];
            if (s >= bp->len) {
                cv_rect(x + 5, 64, 2, 2, C_LINE);
                continue;
            }
            g = bstep_gate(st);
            if (s == ph)
                cv_rect(x, 0, 12, 70, dim(col, 2));
            if (g != G_REST) {
                int y = note_y(st->note, lo, hi);
                uint16_t fc = s == ui.held_step ? C_WHITE : (st->flags & BS_ACCENT) ? col : dim(col, 9);
                box(x, y, 12, 5, fc);
                if (g == G_TIE)
                    cv_rect(x - 2, y + 2, 3, 1, fc);
                if ((st->flags & BS_SLIDE) && c < 15) {
                    int nx = s + 1 < bp->len ? s + 1 : 0, ny = note_y(bp->step[nx].note, lo, hi);
                    cv_line(x + 11, y + 2, x + 14, ny + 2, C_WHITE);
                }
            }
        }
    } else {
        char t[40], *q = t;
        for (c = 0; c < 16; c++) {
            int s = step_of(c), x = col_x(c), g;
            const bstep_t *st = &bp->step[s < NSTEPS ? s : 0];
            if (s >= bp->len)
                continue;
            g = bstep_gate(st);
            rbox(x + 1, 2, 10, 4, 2, g && (st->flags & BS_ACCENT) ? C_WHITE : C_LINE);
            rbox(x + 1, 12, 10, 4, 2, g && (st->flags & BS_SLIDE) ? col : C_LINE);
        }
        cv_text(3, vc(&FONT_XS, 0, 8), &FONT_XS, "AC", C_GRAY);
        cv_text(3, vc(&FONT_XS, 10, 8), &FONT_XS, "SL", C_GRAY);
        if (ui.held_step >= 0 || (ui.kbd[b] && ui.rec)) {
            int s = ui.held_step >= 0 ? ui.held_step : ui.wpos[b];
            const bstep_t *st = &bp->step[s];
            q = put_s(q, ui.held_step >= 0 ? "STEP " : "WRITE ");
            q = put_i(q, s + 1);
            q = put_s(q, "  ");
            q = put_note(q, st->note + bp->transpose - 24);
            if (st->flags & BS_ACCENT)
                q = put_s(q, " ACC");
            if (st->flags & BS_SLIDE)
                put_s(q, " SLIDE");
            cv_text(4, 26, &FONT_B, t, C_WHITE);
        } else {
            char t2[16];
            q = put_s(q, ROOT_N[bp->gen.root % 12]);
            q = put_s(q, " ");
            put_s(q, TB3PO_SCALE_NAMES[bp->gen.scale % TB3PO_NSCALES]);
            put_hex(put_s(t2, "SEED "), bp->gen.seed & 0xFFFFu, 4);
            segs(4, 26, &FONT_S, C_GRAY, 18, t, t2, 0, 0);
        }
        if (gen)
            segs(4, 50, &FONT_XS, C_AMB, 16, "OCT+  NEW LINE", "OCT-  MUTATE", 0, 0);
        else if (ui.held_step < 0)
            segs(4, 50, &FONT_XS, C_DIM, 16, ui.kbd[b] ? "KEYS PLAY" : "TAP: STEP", ui.kbd[b] ? "REC: STEP WRITE" : "HOLD + KNOBS: EDIT", 0, 0);
    }
}

static void draw_break(int band)
{
    const brkpart_t *bp = &cur_pat()->brk;
    int c, ph = seq.playing ? eng_step[TRK_BRK] : -1, slice, bank, div, running;
    uint16_t col = part_col();
    engine_brk_state(&slice, &bank, &div, &running);
    if (!ui.outline_ok || ui.outline_slot != bp->slot_a) {
        ui.outline_ok = (uint8_t)engine_brk_outline(0, ui.outline, 232);
        ui.outline_slot = bp->slot_a;
    }
    if (band == 0) {                                   /* loop A, its 8 slices, the one sounding */
        int x;
        for (c = 0; c < 8; c++) {
            int x0 = 4 + c * 29;
            char t[2] = {(char)('1' + c), 0};
            if (running && c == slice && bank == 0)
                box(x0, 1, 28, 56, dim(col, 5));
            text_c(x0 + 14, 59, &FONT_XS, t, running && c == slice && bank == 0 ? C_WHITE : C_DIM);
        }
        for (x = 0; x < 232 && ui.outline_ok; x++) {
            int h = ui.outline[x] * 26 / 255, sl = x / 29;
            if (h)
                cv_rect(4 + x, 29 - h, 1, 2 * h, running && sl == slice && bank == 0 ? C_WHITE : col);
        }
        for (c = 1; c < 8; c++)
            cv_rect(4 + c * 29, 1, 1, 56, C_LINE);
    } else {
        const char *const *sn = engine_brk_slot_names();
        int ns = engine_brk_nslots();
        char t[40], *q = t;
        for (c = 0; c < 16; c++) {
            int x = col_x(c), on = (bp->steps >> c) & 1u;
            box(x, 2, 12, 10, on ? (c == ph ? C_WHITE : col) : (c == ph ? C_DIM : C_LINE));
        }
        cv_text(3, 1, &FONT_XS, "ON", C_GRAY);
        {
            char ta[16], tb[16];
            put_s(put_s(ta, "A  "), sn[bp->slot_a % ns]);
            put_s(put_s(tb, "B  "), sn[bp->slot_b % ns]);
            segs(4, 18, &FONT_S, C_HI, 18, ta, tb, 0, 0);
            (void)q;
        }
        if (running) {
            q = put_s(t, bank ? "PLAYING B" : "PLAYING A");
            if (div > 1)
                put_i(put_s(q, "   RETRIG X"), div);
            cv_text(4, 36, &FONT_S, t, bank ? C_AMB : C_GRAY);
        }
        segs(4, 58, &FONT_XS, C_DIM, 10, "1-8 SLICES", "9 REV", "10 HALF", "11 STUT");
    }
}

static void lane(int y, int p, uint32_t bits, int ph, int len)
{
    int c, muted = part_muted(p);
    cv_text(3, y, &FONT_XS, PART_S[p], muted ? C_DIM : PART_COL[p]);
    for (c = 0; c < 16 && c < len; c++) {
        uint16_t fc = (bits >> c & 1u) ? (muted ? C_DIM : PART_COL[p]) : C_LINE;
        box(col_x(c), y + 2, 12, 9, fc);
        if (seq.playing && (ph & 15) == c)
            rbox(col_x(c) + 2, y + 12, 8, 2, 1, C_WHITE);
    }
}

static void draw_home(int band)
{
    const pattern_t *pt = cur_pat();
    int p, c;
    if (band == 0) {                                   /* the 16 patterns */
        for (c = 0; c < NPAT; c++) {
            int x = 4 + c * 14 + (c >= 8 ? 4 : 0);
            int in_chain = seq.chain_a != seq.chain_b &&
                           c >= (seq.chain_a < seq.chain_b ? seq.chain_a : seq.chain_b) &&
                           c <= (seq.chain_a < seq.chain_b ? seq.chain_b : seq.chain_a);
            box(x, 3, 12, 18, c == seq.cur ? C_WHITE : pattern_used(&proj.pat[c]) ? C_DIM : C_LINE);
            if (c == seq.cue && c != seq.cur && (ui.frame & 16u))
                frame(x - 1, 2, 14, 20, C_WHITE);
            if (in_chain)
                cv_rect(x, 23, 12, 2, C_AMB);
        }
        for (p = 0; p < 2; p++) {
            uint32_t bits = 0;
            int v;
            for (v = 0; v < NDRUM; v++)
                bits |= pt->drum[p].hit[v];
            lane(32 + p * 16, p, bits, eng_step[TRK_DRUM + p], pt->drum[p].len);
        }
    } else {
        for (p = 2; p < NPARTS; p++) {
            uint32_t bits = 0;
            int s;
            if (p < PART_BRK) {
                for (s = 0; s < NSTEPS; s++)
                    if (bstep_gate(&pt->bass[p - NKIT].step[s]) != G_REST)
                        bits |= 1u << s;
                lane((p - 2) * 16 + 1, p, bits, eng_step[TRK_BASS0 + p - NKIT], pt->bass[p - NKIT].len);
            } else {
                lane((p - 2) * 16 + 1, p, pt->brk.steps, eng_step[TRK_BRK], 16);
            }
        }
        segs(4, 50, &FONT_XS, C_DIM, 14, "WHITE: PATTERN", "TWO HELD: CHAIN", 0, 0);
        segs(4, 60, &FONT_XS, C_DIM, 14, "BLACK 1-5: MUTE", "SAVE + WHITE: COPY", 0, 0);
    }
}

static void draw_fx(int band)
{
    int p;
    if (band == 0) {                                   /* five parts x reverb / delay */
        cv_text(52, 0, &FONT_XS, "REVERB", C_GRAY);
        cv_text(148, 0, &FONT_XS, "DELAY", C_GRAY);
        for (p = 0; p < NPARTS; p++) {
            int y = 14 + p * 11, rv = proj.sound.v[T_MIX][p][1], dl = proj.sound.v[T_MIX][p][2];
            cv_text(3, y - 2, &FONT_XS, PART_S[p], PART_COL[p]);
            rbox(52, y + 2, 88, 6, 3, C_LINE);
            if (rv)
                rbox(52, y + 2, 6 + rv * 82 / 127, 6, 3, PART_COL[p]);
            rbox(148, y + 2, 88, 6, 3, C_LINE);
            if (dl)
                rbox(148, y + 2, 6 + dl * 82 / 127, 6, 3, PART_COL[p]);
        }
    } else {
        char a[16], u[8], t[40], *q;
        pref_value(PR(R_ENG, T_FX, 0, FX_DL_TIME), a, u);
        q = put_s(put_s(t, "DELAY "), a);
        pref_value(PR(R_ENG, T_FX, 0, FX_DL_TYPE), a, u);
        put_s(put_s(q, "  "), a);
        cv_text(4, 4, &FONT_B, t, C_HI);
        {
            char t2[20];
            put_s(put_i(put_s(t, "FEEDBACK "), proj.sound.v[T_FX][0][FX_DL_FDBK] * 100 / 127), "%");
            put_i(put_s(t2, "REVERB "), proj.sound.v[T_FX][0][FX_RV_DECAY] * 100 / 127);
            segs(4, 24, &FONT_S, C_GRAY, 18, t, t2, 0, 0);
            (void)q;
        }
        segs(4, 52, &FONT_XS, C_DIM, 14, "FX AGAIN: NEXT PAGE", "SEL: EVERYTHING", 0, 0);
    }
}

static void draw_mix(int band)
{
    int p;
    if (band == 0) {                                   /* channel strips + the master's gain reduction */
        for (p = 0; p < NPARTS; p++) {
            int x = 6 + p * 38, lvl = proj.sound.v[T_MIX][p][0], pk = eng_peak[p] * 64 / 32768;
            rbox(x + 10, 4, 6, 64, 3, C_LINE);
            if (pk > 1)
                rbox(x + 10, 68 - pk, 6, pk, 3, part_muted(p) ? C_DIM : PART_COL[p]);
            rbox(x + 4, 67 - lvl * 64 / 127, 18, 3, 1, C_WHITE);
        }
        {
            int gr = (int)(engine_gr_db() * 64.0f / 24.0f);
            gr = gr > 64 ? 64 : gr;
            rbox(206, 4, 8, 64, 4, C_LINE);
            if (gr > 1)
                rbox(206, 4, 8, gr, 4, RGB(255, 80, 60));
            cv_text(218, 2, &FONT_XS, "GR", C_GRAY);
        }
    } else {
        char t[24], a[12], u[8], *q;
        for (p = 0; p < NPARTS; p++)
            text_c(6 + p * 38 + 13, 0, &FONT_XS, PART_N[p], part_muted(p) ? C_LINE : PART_COL[p]);
        if ((int)(engine_gr_db() + 0.5f))
            put_s(put_i(put_s(t, "COMP -"), (int)(engine_gr_db() + 0.5f)), "dB");
        else
            put_s(t, "COMP 0dB");
        cv_text(4, 20, &FONT_B, t, engine_gr_db() > 0.5f ? RGB(255, 110, 90) : C_GRAY);
        pref_value(PR(R_ENG, T_MST, 0, MST_PUMP), a, u);
        q = put_s(put_s(t, "PUMP "), a);
        put_s(q, u);
        cv_text(124, 20, &FONT_B, t, proj.sound.v[T_MST][0][MST_PUMP] ? C_HI : C_GRAY);
        put_s(put_i(a, (int)plat_cpu_pct()), "%");
        cv_text(4, 50, &FONT_XS, "AUDIO LOAD", C_DIM);
        cv_text(70, 50, &FONT_XS, a, plat_cpu_pct() > 85u ? RGB(255, 60, 60) : C_GRAY);
    }
}

/* the readout: the knob being turned, large, across the bottom band */
static void draw_readout(void)
{
    char num[16], unit[8];
    pref_t r;
    int32_t w, x;
    const char *nm;
    if (ui.touched == 4) {
        r = PR(R_TEMPO, 0, 0, 0);
        nm = "TEMPO";
    } else {
        r = pg.r[cur_page()][ui.touched];
        nm = pref_name(r);
    }
    pref_value(r, num, unit);
    {   /* one row: the name on the left, the value (+ unit) on the right, both on the box's middle */
        const int32_t bx = 8, by = 14, bw = 224, bh = 50;
        int32_t ly = vc(&FONT_L, by, bh), uw = unit[0] ? tw(&FONT_S, unit) + 3 : 0;
        box(bx, by, bw, bh, C_BLACK);
        frame(bx, by, bw, bh, part_col());
        cv_text(bx + 12, vc(&FONT_B, by, bh), &FONT_B, nm, C_GRAY);
        w = tw(&FONT_L, num);
        x = bx + bw - 12 - uw - w;
        cv_text(x, ly, &FONT_L, num, C_WHITE);
        if (unit[0])
            cv_text(bx + bw - 12 - uw + 3, base_y(&FONT_L, ly, &FONT_S), &FONT_S, unit, C_GRAY);
    }
}

/* a list over the main area: six rows of 24 px */
static void draw_list(int band)
{
    int i;
    for (i = 0; i < 3; i++) {
        int row = ui.list_top + band * 3 + i, y = i * 24, sel = row == ui.list_sel;
        pref_t r;
        char num[16], unit[8];
        const x0x_param_t *d;
        if (row >= ui.list_n)
            break;
        r = list_rows[row];
        d = pref_desc(r);
        if (sel)
            box(0, y + 1, 232, 22, dim(part_col(), 6));
        cv_text(8, vc(sel ? &FONT_B : &FONT_S, y + 1, 18), sel ? &FONT_B : &FONT_S, pref_name(r), sel ? C_WHITE : C_GRAY);
        if (r.kind == R_ACT) {                         /* an action: the return arrow */
            uint16_t ac = sel ? C_WHITE : C_DIM;
            cv_rect(212, y + 12, 12, 2, ac);
            cv_rect(222, y + 6, 2, 7, ac);
            cv_line(212, y + 13, 216, y + 9, ac);
            cv_line(212, y + 13, 216, y + 17, ac);
            continue;
        }
        pref_value(r, num, unit);
        {
            const felucca_font_t *nf = sel ? &FONT_B : &FONT_S;
            int32_t ny = vc(nf, y + 1, 18), ux = 226 - tw(&FONT_XS, unit);
            cv_text(ux, base_y(nf, ny, &FONT_XS), &FONT_XS, unit, C_GRAY);
            text_r(unit[0] ? ux - 2 : 226, ny, nf, num, sel ? C_WHITE : C_HI);
        }
        if (d && !d->names && d->max >= 24) {          /* a continuous value: its bar */
            int v = pref_get(r);
            rbox(108, y + 18, 70, 3, 1, C_LINE);
            if (v)
                rbox(108, y + 18, 3 + v * 67 / d->max, 3, 1, sel ? part_col() : C_DIM);
        }
    }
    if (ui.list_n > 6) {                               /* the scrollbar */
        int h = 144 * 6 / ui.list_n, y = 144 * ui.list_top / ui.list_n - band * 72;
        rbox(235, 0, 3, 72, 1, C_LINE);
        rbox(235, y, 3, h, 1, C_HI);
    }
}

static void draw_ask(int band)
{
    uint16_t red = RGB(255, 80, 60);
    if (band == 0) {
        frame(6, 10, 228, 62, red);
        cv_rect(7, 71, 226, 1, C_BLACK);
        text_c(120, 24, &FONT_B, ui.ask_q[0], C_WHITE);
        text_c(120, 46, &FONT_S, ui.ask_q[1], C_GRAY);
    } else {
        cv_rect(6, 0, 1, 44, red);
        cv_rect(233, 0, 1, 44, red);
        cv_rect(7, 44, 226, 1, red);
        box(22, 10, 92, 24, red);
        text_c(68, vc(&FONT_B, 10, 24), &FONT_B, "SEL  YES", C_BLACK);
        frame(126, 10, 92, 24, C_GRAY);
        text_c(172, vc(&FONT_B, 10, 24), &FONT_B, "HOME  NO", C_GRAY);
    }
}

static void draw_main(void)
{
    int band, readout = ui.touched >= 0 && (int32_t)(ui.touch_until - plat_ms()) > 0 && ui.overlay == O_NONE;
    if (!readout)
        ui.touched = -1;
    for (band = 0; band < 2; band++) {
        cv_begin(240, BAND_H, C_BLACK);
        if (ui.overlay == O_ASK)
            draw_ask(band);
        else if (ui.overlay == O_LIST)
            draw_list(band);
        else if (ui.view == V_HOME)
            draw_home(band);
        else if (ui.view == V_FX)
            draw_fx(band);
        else if (ui.view == V_MIX)
            draw_mix(band);
        else if (is_drum())
            draw_drum(band);
        else if (is_303())
            draw_303(band, ui.view == V_GEN);
        else
            draw_break(band);
        if (band == 1 && readout)
            draw_readout();
        cv_commit(1 + band, 0, (uint32_t)(MAIN_Y + band * BAND_H));
    }
}

/* -------------------------------------------------------------- knob strip --- */
static void cell_value(int cx, int y, const char *num, const char *unit, uint16_t c)
{
    int32_t w = tw(&FONT_B, num) + (unit[0] ? tw(&FONT_XS, unit) + 1 : 0), x = cx - w / 2;
    x = cv_text(x, y, &FONT_B, num, c);
    if (unit[0])
        cv_text(x + 1, base_y(&FONT_B, y, &FONT_XS), &FONT_XS, unit, C_GRAY);
}

/* what the knob strip shows, hashed: it is only redrawn when this changes (the arcs cost a
 * square root and an arctangent per pixel; most frames nothing in the strip moves) */
static uint32_t knobs_sig(void)
{
    uint32_t h = 2166136261u, i;
    int pgi = cur_page();
#define MIXIN(v) (h = (h ^ (uint32_t)(v)) * 16777619u)
    MIXIN(ui.view);
    MIXIN(ui.part);
    MIXIN(pgi);
    MIXIN(ui.touched);
    MIXIN(ui.held_step);
    MIXIN(proj.set.palette);
    MIXIN(ui.sel[0] | ui.sel[1] << 8);
    MIXIN(seq.cur);
    for (i = 0; i < 4; i++) {
        pref_t r = pg.r[pgi][i];
        MIXIN(r.kind | r.a << 8 | r.b << 16 | (uint32_t)r.c << 24);
        MIXIN(pref_get(r));
    }
    if (ui.held_step >= 0) {
        const bstep_t *st = &cur_pat()->bass[bidx()].step[ui.held_step];
        MIXIN(st->note | st->flags << 8);
    }
#undef MIXIN
    return h;
}

static void draw_knobs(void)
{
    static uint32_t last_sig;
    int i, pgi = cur_page();
    uint32_t sig = knobs_sig();
    if (sig == last_sig && blit_hash[3])
        return;
    last_sig = sig;
    cv_begin(240, KNOB_H, C_BLACK);
    cv_rect(0, 0, 240, 1, C_LINE);
    if (ui.view == V_PART && is_303() && ui.held_step >= 0) {     /* the held step's own knobs */
        static const char *const SN[4] = {"NOTE", "GATE", "ACCENT", "SLIDE"};
        static const char *const G[3] = {"REST", "NOTE", "TIE"};
        const bpart_t *bp = &cur_pat()->bass[bidx()];
        const bstep_t *st = &bp->step[ui.held_step];
        for (i = 0; i < 4; i++) {
            char v[12];
            int cx = i * 60 + 30;
            if (i)
                cv_rect(i * 60, 6, 1, KNOB_H - 12, C_LINE);
            if (i == 0)
                put_note(v, st->note + bp->transpose - 24);
            else if (i == 1)
                put_s(v, G[bstep_gate(st)]);
            else
                put_s(v, (st->flags & (i == 2 ? BS_ACCENT : BS_SLIDE)) ? "ON" : "OFF");
            text_c(cx, vc(&FONT_XS, 5, 12), &FONT_XS, SN[i], C_GRAY);
            text_c(cx, vc(&FONT_M, 26, 34), &FONT_M, v, C_WHITE);
        }
        cv_commit(3, 0, KNOB_Y);
        return;
    }
    for (i = 0; i < 4; i++) {
        pref_t r = pg.r[pgi][i];
        const x0x_param_t *d = pref_desc(r);
        char num[16], unit[8];
        int cx = i * 60 + 30, val, touched = ui.touched == i;
        uint16_t col = part_col();
        if (i)
            cv_rect(i * 60, 8, 1, KNOB_H - 16, C_LINE);
        if (!d)
            continue;
        if (touched)
            box(i * 60 + 2, 2, 56, KNOB_H - 4, dim(col, 4));
        text_c(cx, vc(&FONT_XS, 6, 12), &FONT_XS, pref_name(r), touched ? C_WHITE : C_GRAY);
        val = pref_get(r);
        if (d->names) {                                /* a switch: its positions as pips */
            int n = d->max + 1, k, pw = n > 6 ? 3 : 6, gap = 2, w0 = n * (pw + gap) - gap;
            if (n <= 12)
                for (k = 0; k < n; k++)
                    box(cx - w0 / 2 + k * (pw + gap), 26, pw, 10, k == val ? col : C_LINE);
        } else {
            arc(cx, 34, 15, d->max ? (float)val / (float)d->max : 0.0f, C_LINE, col, r.kind == R_BTRANS);
        }
        pref_value(r, num, unit);
        cell_value(cx, vc(&FONT_B, 55, 14), num, unit, touched ? C_WHITE : C_HI);
    }
    cv_commit(3, 0, KNOB_Y);
}

/* ===================================================================== LEDs === */
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
    if (ui.overlay != O_NONE)
        b |= 1u << B_SEL;
    if (is_303() && ui.kbd[bidx()])
        b |= 1u << B_SEQ;
    if (ui.spage)
        b |= 1u << B_OCTUP;
    if (ui.dirty && blink && ui.view == V_GLO)
        b |= 1u << B_SAVE;
    if (proj.set.keyled) {
        if (ui.view == V_HOME) {
            k |= 1u << WHITE_KEY[seq.cur];
            if (seq.cue < NPAT && blink)
                k |= 1u << WHITE_KEY[seq.cue];
            for (i = 0; i < NPARTS; i++)
                if (!part_muted((int)i))
                    k |= 1u << BLACK_KEY[i];
        } else if (is_drum()) {
            const dpart_t *d = &cur_pat()->drum[ui.part];
            uint32_t bits = (ui.btn & (1u << B_ENV)) ? d->accent : d->hit[ui.sel[ui.part]];
            int ph = eng_step[TRK_DRUM + ui.part];
            for (i = 0; i < 16; i++) {
                int s = step_of((int)i), on = (bits >> s) & 1u;
                if (seq.playing && s == ph)
                    on = !on;
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
    k |= ui.keys;
    plat_leds(b, k);
}

/* ===================================================================== frame === */
void ui_init(void)
{
    int i;
    for (i = 0; i < NKEYS; i++) {
        if (KEY_WHITE[i] >= 0)
            WHITE_KEY[KEY_WHITE[i]] = (uint8_t)i;
        if (KEY_BLACK[i] >= 0)
            BLACK_KEY[KEY_BLACK[i]] = (uint8_t)i;
    }
    corners_init();
    ui.view = V_PART;
    ui.prev_view = V_PART;
    ui.part = PART_909;
    ui.held_step = -1;
    ui.chain_first = -1;
    ui.touched = -1;
    ui.overlay = O_NONE;
    ui.outline_ok = 0;
    palette_set(proj.set.palette);
    for (i = 0; i < 4; i++)
        blit_hash[i] = 0;
    build_pages();
}

void ui_frame(void)
{
    uint32_t i;
    input();
    for (i = 0; i < NPARTS; i++)
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
