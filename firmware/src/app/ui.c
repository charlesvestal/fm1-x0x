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
#include "../dsp/bass303.h"                    /* BASS303_* (the 303's pages) */
#include "undo.c"

/* ================================================================ model === */
static const int8_t KEY_WHITE[NKEYS] = {0, -1, 1, -1, 2, -1, 3, 4, -1, 5, -1, 6, 7, -1, 8, -1, 9, -1, 10, 11, -1, 12, -1, 13, 14, -1, 15};
static const int8_t KEY_BLACK[NKEYS] = {-1, 0, -1, 1, -1, 2, -1, -1, 3, -1, 4, -1, -1, 5, -1, 6, -1, 7, -1, -1, 8, -1, 9, -1, -1, 10, -1};
static uint8_t WHITE_KEY[16], BLACK_KEY[11];

enum { V_HOME, V_PART, V_GEN, V_FX, V_MIX, V_GLO, V_SONG, V_PERF, NVIEWS };
static const char *const VIEW_N[NVIEWS] = {"HOME", "", "TB-3PO", "FX", "MIX", "GLOBAL", "SONG", "PERF"};
static const uint16_t PART_COL[NPARTS] = {RGB(255, 150, 40), RGB(255, 72, 64), RGB(130, 240, 90), RGB(70, 205, 255),
                                          RGB(214, 120, 255)};
static const char *const PART_N[NPARTS] = {"909", "808", "303A", "303B", "BREAK"};
static const char *const PART_S[NPARTS] = {"909", "808", "303A", "303B", "BRK"};   /* 4 chars: lane labels */

/* overlays drawn over the main area */
enum { O_NONE, O_LIST, O_ASK };
enum { ACT_NONE, ACT_SAVE, ACT_CLEAR_PART, ACT_CLEAR_PAT, ACT_COPY, ACT_RESET, ACT_ABOUT, ACT_SONG_INS,
       ACT_SONG_DEL, ACT_SONG_CLR, ACT_PERF, ACT_PERF_TEST };

typedef struct {
    uint8_t view, part, prev_view;
    uint8_t sel[NKIT];
    uint8_t page[NVIEWS][NPARTS];
    uint8_t spage;                     /* the 16 steps the white keys show: 0 = 1-16 .. 3 = 49-64 */
    uint8_t pbank;                     /* the 16 patterns HOME's white keys choose: 0 = 1-16, 1 = 17-32 */
    uint8_t mixsel;                    /* MIX's PARTS: the part the knobs set (black keys 1-5) */
    int8_t oct;
    uint8_t rec;
    uint8_t wpos[NBASS];
    int8_t held_step;
    uint8_t step_edited;
    uint32_t step_t0;                  /* when the held 303 step went down: a tap toggles, a hold does not */
    uint8_t step_preview;              /* the held step is sounding (only while stopped) */
    uint8_t step_created;              /* the press turned the held step on (its release keeps it) */
    int8_t brk_held;                   /* BREAK: the white key (16th) held, -1 none */
    uint16_t brk_pinkeys;              /* BREAK: black keys used to give a held step its slice (their release is not live) */
    uint8_t gen_stale[NBASS];          /* TB-3PO's knobs changed since the line was written */
    uint32_t btn, keys, btn_used;
    int8_t chain_first;
    uint32_t enc_t[NE];
    char msg[28];
    uint32_t msg_until;
    int8_t touched;                    /* knob cell being turned (4 = tempo), -1 none */
    uint32_t touch_until;
    uint8_t overlay;
    uint8_t list_sel, list_top, list_n;
    uint8_t ask_act;
    uint16_t ask_arg;
    char ask_q[2][28];
    uint32_t frame;
    uint32_t dirty;
    uint32_t act_t;                    /* the last time anything was touched (autosave waits for quiet) */
    uint32_t saved_t;                  /* the last autosave */
    uint8_t last_autosave;             /* AUTOSAVE was just switched off: one more, which keeps that */
    uint8_t help;                      /* the help card on screen: its button + 1, 0 none */
    uint32_t down_t[NB];               /* when each button went down */
    uint32_t turn_t;                   /* the last encoder turn */
    uint8_t outline[232];              /* the break loop's outline */
    uint8_t outline_ok, outline_slot;
    uint8_t song_sel;                  /* the SONG screen's bar */
} ui_t;
static ui_t ui;
static uint8_t any_button;             /* a button has been pressed since power-on (the footer's first line) */
#define FOOT_GAP 7                     /* between the footer's parts */
static uint16_t footer_text(const char **a, const char **b, const char **c);

/* something the project keeps changed: SAVE (or the autosave) has work to do */
static void mark_dirty(void) { ui.dirty = 1; }

/* the encoders, read for the UI: a turn counts as being touched (the autosave waits for quiet) */
static int32_t enc(int role)
{
    int32_t v = plat_enc(role);
    if (v)
        ui.act_t = ui.turn_t = plat_ms();
    return v;
}

/* PERF (the performance screen and its test; see "performance" below) */
enum { PT_IDLE = -1, PT_DONE = 3 };
static struct {
    uint32_t t_ms, ticks, cyc, frames, prof[ENG_PROF_N], st[3];
    uint16_t mhz10;                    /* the CPU clock x 10; 0 = not known (no cycle counter) */
    uint8_t load, peak, stalls_on, have;
    uint16_t stage[ENG_PROF_N];        /* each stage's share of the CPU, x 10 (%) */
    uint16_t cps;                      /* the render's cycles per sample (cycle counter only) */
    uint8_t stall[3];                  /* fetch / read / write stall cycles, % */
    int8_t test, phase;                /* test: PT_IDLE, a scenario 0..2, PT_DONE (when phase is 4);
                                        * phase: 0 stop, 1 settle, 2 measure, 3 put back, 4 done */
    uint32_t test_t0;
    uint16_t res_load[3], res_cps[3];  /* per scenario: the render's share x 10, cycles per sample */
    uint8_t res_peak[3];
    pattern_t *pat;                    /* what the test changes, to put back */
    uint8_t ppat[NPARTS], song_on, ca, cb;
    uint32_t mute;
    float bpm;
} perf = {.test = PT_IDLE};

static pattern_t *pat_of(int part) { return &proj.pat[seq.ppat[part]]; }
static pattern_t *cur_pat(void) { return pat_of(ui.part); }    /* the pattern the part on screen plays */
static int part_view(void) { return ui.view == V_PART || ui.view == V_GEN; }
static song_t *song(void) { return &proj.arr.song; }
static int is_303(void) { return ui.part == PART_303A || ui.part == PART_303B; }
static int is_drum(void) { return ui.part == PART_909 || ui.part == PART_808; }
static int bidx(void) { return ui.part == PART_303B ? 1 : 0; }
static int held_note_of(const bpart_t *bp, int s);

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
       R_BRKSLOT, R_TEMPO, R_ACCENT, R_CLKOUT, R_NOTEOUT, R_PALETTE, R_KEYLED, R_KEYSOUND, R_AUTOSAVE, R_ACT, R_SBAR, R_SPAT, R_SMODE,
       R_SLEN, R_STALLS, R_HDR, R_TAPE };   /* R_TAPE: FX's one-knob TAPE (DIGI .. worn tape) */          /* R_HDR: a list's section header (a = its group), never selected */
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
       SQ_THEME, SQ_KEYLED, SQ_KEYSOUND, SQ_AUTOSAVE, NSQ };
static const char *const KEYSOUND_N[] = {"STOPPED", "ALWAYS"};
static const char *const LIGHTS_N[] = {"OFF", "ON", "KEYS"};   /* ON: the keys, and the unlit buttons glow */
static const x0x_param_t SEQ_P[NSQ] = {
    {"SWING", 100, 0, 0}, {"LENGTH", NSTEPS - 1, 15, 0}, {"RATE", 3, 0, RATE_N}, {"LENGTH", NSTEPS - 1, 15, 0},
    {"RATE", 3, 0, RATE_N}, {"DIR", 3, 0, DIR_N}, {"TRANSP", 48, 24, 0}, {"TEMPO", 255, 105, 0},
    {"ACCENT", 127, 88, 0}, {"CLOCK OUT", 1, 1, ONOFF_N}, {"NOTES OUT", 1, 0, ONOFF_N}, {"THEME", 4, 1, THEME_N},
    {"LIGHTS", 2, 1, LIGHTS_N}, {"KEY SOUND", 1, 0, KEYSOUND_N},
    {"AUTOSAVE", 1, 1, ONOFF_N},
};
static const x0x_param_t ACT_P[] = {
    {"", 0, 0, 0}, {"SAVE PROJECT", 0, 0, 0}, {"CLEAR THIS PART", 0, 0, 0}, {"CLEAR PATTERN", 0, 0, 0},
    {"COPY PATTERN", 0, 0, 0}, {"FACTORY RESET", 0, 0, 0}, {"ABOUT X0X", 0, 0, 0}, {"INSERT BAR", 0, 0, 0},
    {"DELETE BAR", 0, 0, 0}, {"CLEAR SONG", 0, 0, 0}, {"PERFORMANCE", 0, 0, 0}, {"RUN PERF TEST", 0, 0, 0},
};
static const x0x_param_t STALLS_P = {"STALLS", 1, 0, ONOFF_N};
static const char *const MODE_N[] = {"PATTERN", "SONG"};
static const x0x_param_t SONG_P[] = {
    {"909", NPAT - 1, 0, 0}, {"808", NPAT - 1, 0, 0}, {"303A", NPAT - 1, 0, 0}, {"303B", NPAT - 1, 0, 0},
    {"BREAK", NPAT - 1, 0, 0},
    {"MODE", 1, 0, MODE_N}, {"LENGTH", NSONG, 0, 0},
};

/* the song's bar k exists from here on: a new bar repeats the one before (the first: what plays) */
static void song_fill(int k)
{
    song_t *sg = song();
    while (sg->len <= k && sg->len < NSONG) {
        song_bar_t *b = &sg->bar[sg->len];
        int p;
        if (sg->len)
            *b = sg->bar[sg->len - 1];
        else {
            for (p = 0; p < NPARTS; p++)
                b->pat[p] = seq.ppat[p];
            b->mute = 0;
        }
        sg->len++;
    }
}

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
    case R_KEYSOUND: return &SEQ_P[SQ_KEYSOUND];
    case R_AUTOSAVE: return &SEQ_P[SQ_AUTOSAVE];
    case R_ACT: return &ACT_P[r.a];
    case R_SBAR: {
        static x0x_param_t d = {"BAR", 0, 0, 0};
        d.max = (uint8_t)(song()->len < NSONG ? song()->len : NSONG - 1);
        return &d;
    }
    case R_SPAT: return &SONG_P[r.a % NPARTS];
    case R_SMODE: return &SONG_P[5];
    case R_SLEN: return &SONG_P[6];
    case R_STALLS: return &STALLS_P;
    case R_TAPE: {
        static const x0x_param_t d = {"TAPE", 127, 72, 0};
        return &d;
    }
    default: return 0;
    }
}

static int pref_get(pref_t r)
{
    pattern_t *p = cur_pat();
    switch (r.kind) {
    case R_ENG: return proj.sound.v[r.a][r.b][r.c];
    case R_SWING: return pat_of(PART_909)->swing;
    case R_DLEN: return pat_of(r.a)->drum[r.a].len - 1;
    case R_DRATE: return pat_of(r.a)->drum[r.a].rate;
    case R_BLEN: return pat_of(NKIT + r.a)->bass[r.a].len - 1;
    case R_BRATE: return pat_of(NKIT + r.a)->bass[r.a].rate;
    case R_BDIR: return pat_of(NKIT + r.a)->bass[r.a].dir;
    case R_BTRANS: return pat_of(NKIT + r.a)->bass[r.a].transpose;
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
    case R_KEYSOUND: return proj.set.keysound;
    case R_AUTOSAVE: return !proj.set.autosave_off;
    case R_SBAR: return ui.song_sel;
    case R_SPAT: {
        const song_t *sg = song();
        int k = ui.song_sel < sg->len ? ui.song_sel : sg->len - 1;
        return k >= 0 ? sg->bar[k].pat[r.a] : seq.ppat[r.a];
    }
    case R_SMODE: return seq.song_on;
    case R_SLEN: return song()->len;
    case R_STALLS: return perf.stalls_on;
    case R_TAPE: return proj.sound.v[T_FX][1][0];       /* (kept in an engine slot nothing reads) */
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
    mark_dirty();
    switch (r.kind) {
    case R_ENG:
        proj.sound.v[r.a][r.b][r.c] = (uint8_t)v;
        engine_set(r.a, r.b, r.c, v);
        if (r.a == T_MST && r.c == MST_COMP1)          /* the macros: the details they set, for the list */
            master_comp1_pots(v, &proj.sound.v[T_MST][0][MST_THRESH], &proj.sound.v[T_MST][0][MST_RATIO],
                              &proj.sound.v[T_MST][0][MST_MAKEUP]);
        else if (r.a == T_MST && r.c == MST_DJF)
            master_djf_pots(v, &proj.sound.v[T_MST][0][MST_MODE], &proj.sound.v[T_MST][0][MST_CUTOFF]);
        break;
    case R_SWING: pat_of(PART_909)->swing = (uint8_t)v; break;
    case R_DLEN: pat_of(r.a)->drum[r.a].len = (uint8_t)(v + 1); break;
    case R_DRATE: pat_of(r.a)->drum[r.a].rate = (uint8_t)v; break;
    case R_BLEN: pat_of(NKIT + r.a)->bass[r.a].len = (uint8_t)(v + 1); break;
    case R_BRATE: pat_of(NKIT + r.a)->bass[r.a].rate = (uint8_t)v; break;
    case R_BDIR: pat_of(NKIT + r.a)->bass[r.a].dir = (uint8_t)v; break;
    case R_BTRANS: pat_of(NKIT + r.a)->bass[r.a].transpose = (uint8_t)v; break;
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
        if (r.c != G_MUT)                            /* the line stays: the settings are the next NEW / MUTATE's */
            ui.gen_stale[r.a] = 1;
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
    case R_KEYSOUND: proj.set.keysound = (uint8_t)v; break;
    case R_AUTOSAVE: proj.set.autosave_off = (uint8_t)!v; ui.last_autosave = (uint8_t)!v; break;
    case R_SBAR: ui.song_sel = (uint8_t)v; break;
    case R_SPAT:
        song_fill(ui.song_sel);
        song()->bar[ui.song_sel].pat[r.a] = (uint8_t)v;
        break;
    case R_SMODE: seq.song_on = (uint8_t)v; break;
    case R_TAPE: {                                    /* 0..15 the clean digital delay; above, tape, wearing */
        uint8_t ty = (uint8_t)(v >= 16), we = (uint8_t)(ty ? (v - 16) * 127 / 111 : proj.sound.v[T_FX][0][FX_DL_WEAR]);
        proj.sound.v[T_FX][1][0] = (uint8_t)v;
        proj.sound.v[T_FX][0][FX_DL_TYPE] = ty;
        proj.sound.v[T_FX][0][FX_DL_WEAR] = we;
        engine_set(T_FX, 0, FX_DL_TYPE, ty);
        engine_set(T_FX, 0, FX_DL_WEAR, we);
        break;
    }
    case R_STALLS:
        perf.stalls_on = (uint8_t)v;
        plat_stalls_enable(v);
        perf.have = 0;                                /* the next window starts the counts again */
        break;
    case R_SLEN:
        if (v > song()->len)
            song_fill(v - 1);
        else
            song()->len = (uint16_t)v;
        if (ui.song_sel > song()->len)
            ui.song_sel = (uint8_t)(song()->len < NSONG ? song()->len : NSONG - 1);
        break;
    default: break;
    }
}

/* a name in capitals (the engines' own are mixed case): two at a time */
static const char *caps_of(const char *n)
{
    static char t[2][24];
    static int w;
    char *q = t[w ^= 1];
    int i;
    for (i = 0; n[i] && i < 23; i++)
        q[i] = n[i] >= 'a' && n[i] <= 'z' ? (char)(n[i] - 32) : n[i];
    q[i] = 0;
    return q;
}

/* the value belongs to the pattern (it changes when the pattern does), not to the sound */
static int in_pattern(pref_t r)
{
    switch (r.kind) {
    case R_SWING: case R_DLEN: case R_DRATE: case R_BLEN: case R_BRATE: case R_BDIR: case R_BTRANS: case R_GEN:
    case R_BRKSET: case R_BRKSLOT:
        return 1;
    default:
        return 0;
    }
}

/* a knob's label: mixer refs name their part */
static const char *pref_name(pref_t r)
{
    static const char *const MIXN[NPARTS][MX_NPARAMS] = {{"909", "909 RV", "909 DL", "909 PAN"},
                                                         {"808", "808 RV", "808 DL", "808 PAN"},
                                                         {"303 A", "A REV", "A DLY", "A PAN"},
                                                         {"303 B", "B REV", "B DLY", "B PAN"},
                                                         {"BREAK", "BRK RV", "BRK DL", "BRK PAN"}};
    static const char *const PATN[6][2] = {{"909 LEN", "808 LEN"}, {"909 RATE", "808 RATE"}, {"A LEN", "B LEN"},
                                           {"A RATE", "B RATE"}, {"A DIR", "B DIR"}, {"A TRANSP", "B TRANSP"}};
    static const char *const CHN[MX_NPARAMS] = {"LEVEL", "REV", "DLY", "PAN"};
    const x0x_param_t *d = pref_desc(r);
    if (r.kind >= R_DLEN && r.kind <= R_BTRANS)       /* HOME: whose */
        return PATN[r.kind - R_DLEN][r.a & 1];
    if (r.kind == R_ENG && r.a == T_MIX && ui.view == V_MIX && r.c < MX_NPARAMS)
        return CHN[r.c];                               /* MIX's PARTS: the picked part's */
    if (r.kind == R_ENG && (r.a == T_909 || r.a == T_808) && r.b == NDRUM)
        return r.c == 1 ? "ACCENT" : "CHOKE";          /* the kits' depth of accents; the 808's choke */
    if (r.kind == R_ENG && r.a == T_MIX && r.b < NPARTS && r.c < MX_NPARAMS)
        return MIXN[r.b][r.c];
    if (r.kind == R_ENG && r.a == T_FX) {               /* the page says which effect: the knob, what */
        switch (r.c) {
        case FX_RV_DECAY: return "DECAY";
        case FX_RV_TONE: case FX_DL_TONE: return "TONE";
        case FX_DL_TIME: return "TIME";
        case FX_DL_FDBK: return "FEEDBACK";
        case FX_RV_HPF: return "REV LOW CUT";
        case FX_DL_HPF: return "DLY LOW CUT";
        case FX_RV_LEVEL: return "REV RETURN";
        case FX_DL_LEVEL: return "DLY RETURN";
        case FX_DL_PING: return "PING-PONG";
        case FX_COMP: return "GLUE";
        default: break;
        }
    }
    if (r.kind == R_BRKSET && r.c == BRK_BCHANCE)
        return "B CHANCE";
    if (r.kind == R_ENG && (r.a == T_909 || r.a == T_808) && r.b < NDRUM && ui.view == V_MIX && ui.overlay == O_LIST) {
        static char t[24];                             /* MIX's list: whose track ("909 BD LEVEL") */
        put_s(put_s(put_s(put_s(t, r.a == T_909 ? "909 " : "808 "), engine_voice_name(r.a, r.b)), " "), d ? d->name : "");
        return t;
    }
    if (r.kind == R_BRKSLOT)
        return r.a ? "LOOP B" : "LOOP A";
    return d ? d->name : "";
}

/* a pan: a part's (MIX) or a drum voice's ("Pan", its last pot): drawn L / C / R, its arc from the middle */
static int is_pan(pref_t r)
{
    const x0x_param_t *d;
    if (r.kind != R_ENG)
        return 0;
    if (r.a == T_MIX)
        return r.c == MX_PAN;
    d = pref_desc(r);
    return (r.a == T_909 || r.a == T_808) && d && d->name[0] == 'P' && d->name[1] == 'a' && d->name[2] == 'n' &&
           !d->name[3];
}

/* the value, as a number and a unit ("-6" "dB"); a switch gives its name and no unit */
static void pref_value_of(pref_t r, int v, char *num, char *unit)
{
    const x0x_param_t *d = pref_desc(r);
    *num = *unit = 0;
    if (!d || r.kind == R_ACT)
        return;
    if (d->names) {
        put_s(num, d->names[v]);
        return;
    }
    if (r.kind == R_TAPE) {
        if (v < 16)
            put_s(num, "DIGI");
        else {
            put_i(num, (v - 16) * 100 / 111);
            put_s(unit, "%");
        }
        return;
    }
    if (is_pan(r)) {                                     /* L64 .. C .. R63 */
        if (v == 64)
            put_s(num, "C");
        else
            put_i(put_s(num, v < 64 ? "L" : "R"), v < 64 ? 64 - v : v - 64);
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
    case R_SBAR: put_i(num, v + 1); return;
    case R_SPAT: num[0] = 'P'; put_i(num + 1, v + 1); return;
    case R_SLEN: put_i(num, v); put_s(unit, "bar"); return;
    default: put_i(num, v); return;
    }
}
static void pref_value(pref_t r, char *num, char *unit) { pref_value_of(r, pref_get(r), num, unit); }

/* ===================================================================== pages === */
#define MAXPAGES 14
/* page groups: whose values a page holds (the SEL list's headers, the header's colour) */
enum { GR_NONE, GR_TRACK, GR_TMIX, GR_KIT, GR_PAT, GR_SOUND, GR_MORE };
#define MAXMORE 14
typedef struct {
    pref_t r[MAXPAGES][4];
    char title[MAXPAGES][12];
    uint8_t group[MAXPAGES];
    int n;
    pref_t more[MAXMORE];              /* the screen's other knobs: only in SEL's list, under MORE */
    int nmore;
} pages_t;
static pages_t pg;
static uint8_t pg_grp;                 /* the group pages added now belong to */
static void more(pref_t r)
{
    if (r.kind != R_NONE && pg.nmore < MAXMORE)
        pg.more[pg.nmore++] = r;
}

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
    pg.group[pg.n] = pg_grp;
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
        pg.group[pg.n] = pg_grp;
        pg.n++;
    }
}

static int name_is(const x0x_param_t *d, const char *n)
{
    const char *a = d ? d->name : "";
    while (*a && *a == *n) {
        a++;
        n++;
    }
    return !*a && !*n;
}
static pref_t find_ref(int t, int v, const char *n)
{
    int i;
    for (i = 0; i < engine_nparams(t, v); i++)
        if (name_is(engine_param(t, v, i), n))
            return PR(R_ENG, t, v, i);
    return NONE;
}

/* a drum track: its sound, four knobs a page (its DRIVE and DIST: SEL's list; its level, pan and sends:
 * MIX's 909 MIX / 808 MIX page) */
static void add_track_pages(int t, int v)
{
    const char *nm = engine_voice_name(t, v);
    pref_t snd[NPARAMS_MAX];
    int n = 0, i;
    for (i = 0; i < engine_nparams(t, v) && n < NPARAMS_MAX; i++) {
        const x0x_param_t *d = engine_param(t, v, i);
        if (name_is(d, "Drive") || name_is(d, "Dist"))
            more(PR(R_ENG, t, v, i));
        else if (!name_is(d, "Level") && !name_is(d, "Pan") && !name_is(d, "Rev") && !name_is(d, "Dly"))
            snd[n++] = PR(R_ENG, t, v, i);
    }
    pg_grp = GR_TRACK;
    for (i = 0; i < n; i += 4)
        add_page(nm, snd[i], i + 1 < n ? snd[i + 1] : NONE, i + 2 < n ? snd[i + 2] : NONE, i + 3 < n ? snd[i + 3] : NONE);
}

/* where a screen's page is kept: per part on the part's screens (and TB-3PO), else one per screen */
static uint8_t *page_of(int view)
{
    return &ui.page[view][view == V_PART || view == V_GEN ? ui.part : 0];
}

static void build_pages(void)
{
    int p = ui.part, b = bidx();
    pg.n = pg.nmore = 0;
    pg_grp = GR_NONE;
    switch (ui.view) {
    case V_HOME:                                       /* the pattern: how the parts run through it */
    case V_GLO:
        add_page("PERFORM", PR(R_TEMPO, 0, 0, 0), PR(R_SWING, 0, 0, 0), PR(R_ENG, T_MST, 0, MST_DJF),
                 PR(R_ENG, T_MST, 0, MST_PUMP));
        add_page("LENGTH", PR(R_DLEN, 0, 0, 0), PR(R_DLEN, 1, 0, 0), PR(R_BLEN, 0, 0, 0), PR(R_BLEN, 1, 0, 0));
        more(PR(R_DRATE, 0, 0, 0));
        more(PR(R_DRATE, 1, 0, 0));
        more(PR(R_BRATE, 0, 0, 0));
        more(PR(R_BRATE, 1, 0, 0));
        more(PR(R_BDIR, 0, 0, 0));
        more(PR(R_BTRANS, 0, 0, 0));
        more(PR(R_BDIR, 1, 0, 0));
        more(PR(R_BTRANS, 1, 0, 0));
        break;
    case V_PART:                                       /* the part's sound */
        if (is_drum()) {
            int t = p == PART_909 ? T_909 : T_808;
            add_track_pages(t, ui.sel[p]);
            pg_grp = GR_KIT;
            if (p == PART_909)                         /* (the kit's gain: the mixer's level does it) */
                add_page("KIT", PR(R_ENG, T_909, NDRUM, 1), NONE, NONE, NONE);
            else
                add_page("KIT", PR(R_ENG, T_808, NDRUM, 1), PR(R_ENG, T_808, NDRUM, 2), NONE, NONE);
        } else if (is_303()) {
            pg_grp = GR_SOUND;
            add_page("TONE", PR(R_ENG, T_303, b, BASS303_CUTOFF), PR(R_ENG, T_303, b, BASS303_RESO),
                     PR(R_ENG, T_303, b, BASS303_ENVMOD), PR(R_ENG, T_303, b, BASS303_DECAY));
            add_page("VOICE", PR(R_ENG, T_303, b, BASS303_ACCENT), PR(R_ENG, T_303, b, BASS303_WAVE),
                     PR(R_ENG, T_303, b, BASS303_DRIVE), PR(R_ENG, T_303, b, BASS303_SLIDE));
            more(PR(R_ENG, T_303, b, BASS303_TUNE));
            more(PR(R_ENG, T_303, b, BASS303_VOLUME));
            more(PR(R_ENG, T_303, b, BASS303_DRVTYPE));
            more(PR(R_ENG, T_303, b, BASS303_ACCDEC));
        } else {
            pg_grp = GR_PAT;
            add_page("GROOVE", PR(R_BRKSET, 0, 0, BRK_COMPLEX), PR(R_BRKSET, 0, 0, BRK_ANCHOR),
                     PR(R_BRKSET, 0, 0, BRK_ROLL), PR(R_BRKSET, 0, 0, BRK_FILL));
            add_page("RETRIG", PR(R_BRKSET, 0, 0, BRK_R2), PR(R_BRKSET, 0, 0, BRK_R3), PR(R_BRKSET, 0, 0, BRK_R4),
                     PR(R_BRKSET, 0, 0, BRK_R8));
            add_page("LOOPS", PR(R_BRKSLOT, 0, 0, 0), PR(R_BRKSLOT, 1, 0, 0), PR(R_BRKSET, 0, 0, BRK_BCHANCE),
                     PR(R_ENG, T_BRK, 0, 1));
            more(PR(R_BRKSET, 0, 0, BRK_PHRASE));
            more(PR(R_BRKSET, 0, 0, BRK_ALEN));
            more(PR(R_BRKSET, 0, 0, BRK_BLEN));
            more(PR(R_ENG, T_BRK, 0, 0));
        }
        break;
    case V_GEN:
        pg_grp = GR_PAT;
        add_page("GENERATE", PR(R_GEN, b, 0, G_DENS), PR(R_GEN, b, 0, G_ACC), PR(R_GEN, b, 0, G_SLIDE),
                 PR(R_GEN, b, 0, G_OCTS));
        add_page("SCALE", PR(R_GEN, b, 0, G_ROOT), PR(R_GEN, b, 0, G_SCALE), PR(R_GEN, b, 0, G_BASE),
                 PR(R_GEN, b, 0, G_MUT));
        break;
    case V_FX:                                         /* (each part's sends: MIX's PARTS) */
        add_page("REVERB", PR(R_ENG, T_FX, 0, FX_RV_DECAY), PR(R_ENG, T_FX, 0, FX_RV_TONE), NONE, NONE);
        add_page("DELAY", PR(R_ENG, T_FX, 0, FX_DL_TIME), PR(R_ENG, T_FX, 0, FX_DL_FDBK),
                 PR(R_ENG, T_FX, 0, FX_DL_TONE), PR(R_TAPE, 0, 0, 0));
        more(PR(R_ENG, T_FX, 0, FX_RV_HPF));
        more(PR(R_ENG, T_FX, 0, FX_RV_LEVEL));
        more(PR(R_ENG, T_FX, 0, FX_DL_HPF));
        more(PR(R_ENG, T_FX, 0, FX_DL_LEVEL));
        more(PR(R_ENG, T_FX, 0, FX_DL_PING));
        break;
    case V_MIX:                                        /* a channel picked with a black key, its four knobs */
        add_page("PARTS", PR(R_ENG, T_MIX, ui.mixsel, MX_LEVEL), PR(R_ENG, T_MIX, ui.mixsel, MX_PAN),
                 PR(R_ENG, T_MIX, ui.mixsel, MX_REV), PR(R_ENG, T_MIX, ui.mixsel, MX_DLY));
        for (p = 0; p < NKIT; p++) {                    /* each drum machine's tracks: LEVEL, PAN, REV, DLY */
            int t = p ? T_808 : T_909, v = ui.sel[p];
            add_page(p ? "808 MIX" : "909 MIX", find_ref(t, v, "Level"), find_ref(t, v, "Pan"), find_ref(t, v, "Rev"),
                     find_ref(t, v, "Dly"));
        }
        add_page("MASTER", PR(R_ENG, T_FX, 0, FX_DRIVE), PR(R_ENG, T_MST, 0, MST_COMP1), PR(R_ENG, T_MST, 0, MST_PUMP),
                 PR(R_ENG, T_MST, 0, MST_DJF));
        more(PR(R_ENG, T_MST, 0, MST_THRESH));
        more(PR(R_ENG, T_MST, 0, MST_RATIO));
        more(PR(R_ENG, T_MST, 0, MST_ATTACK));
        more(PR(R_ENG, T_MST, 0, MST_RELEASE));
        more(PR(R_ENG, T_MST, 0, MST_MAKEUP));
        more(PR(R_ENG, T_MST, 0, MST_MIX));
        more(PR(R_ENG, T_MST, 0, MST_PUMPSRC));
        more(PR(R_ENG, T_MST, 0, MST_RESO));
        more(PR(R_ENG, T_MST, 0, MST_LIMIT));
        more(PR(R_ENG, T_FX, 0, FX_DIST));
        more(PR(R_ENG, T_FX, 0, FX_COMP));
        more(PR(R_ENG, T_FX, 0, FX_VOLUME));
        break;
    case V_PERF:
        add_page("PERF", PR(R_STALLS, 0, 0, 0), NONE, NONE, NONE);
        break;
    case V_SONG:
        add_page("BAR", PR(R_SBAR, 0, 0, 0), PR(R_SPAT, PART_909, 0, 0), PR(R_SPAT, PART_808, 0, 0),
                 PR(R_SPAT, PART_303A, 0, 0));
        add_page("BAR", PR(R_SBAR, 0, 0, 0), PR(R_SPAT, PART_303B, 0, 0), PR(R_SPAT, PART_BRK, 0, 0),
                 PR(R_SMODE, 0, 0, 0));
        break;
    default: break;
    }
    if (pg.n == 0)
        add_page("", NONE, NONE, NONE, NONE);
    if (*page_of(ui.view) >= pg.n)
        *page_of(ui.view) = 0;
}

static int cur_page(void) { return *page_of(ui.view); }

/* MIX's page of a drum machine's tracks: which machine (0 / 1), -1 = not one */
static int drum_mix_page(void)
{
    const char *t = pg.title[cur_page()];
    return ui.view == V_MIX && t[4] == 'M' && t[5] == 'I' && t[6] == 'X' ? (t[0] == '8') : -1;
}

/* a drum machine's page of the selected track's own sound (not the whole kit's: SENDS, PART, KIT) */
static int track_page(void)
{
    int g = pg.group[cur_page()];
    return ui.view == V_PART && is_drum() && (g == GR_TRACK || g == GR_TMIX);
}

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

/* a list section's name: whose values follow */
static void hdr_name(int g, char *t)
{
    const char *who = is_drum() ? engine_voice_name(ui.part == PART_909 ? T_909 : T_808, ui.sel[ui.part]) : PART_N[ui.part];
    switch (g) {
    case GR_TRACK: put_s(put_s(t, who), ": THIS TRACK"); break;
    case GR_TMIX: put_s(put_s(t, who), ": TRACK MIX"); break;
    case GR_KIT: put_s(put_s(t, PART_N[ui.part]), ": ALL TRACKS"); break;
    case GR_PAT: put_s(put_i(put_s(t, "P"), seq.ppat[ui.part] + 1), ": THE PATTERN"); break;
    case GR_MORE: put_s(t, "MORE"); break;
    default: put_s(put_s(t, PART_N[ui.part]), ": THE SOUND"); break;
    }
}

static int list_skip(int i) { return i < ui.list_n && list_rows[i].kind == R_HDR; }

static void open_list_of_pages(void)
{
    int p, k, last = -1, groups = 0;
    ui.list_n = 0;
    for (p = 0; p < pg.n; p++)
        if (pg.group[p] != (p ? pg.group[p - 1] : GR_NONE))
            groups++;
    for (p = 0; p < pg.n; p++) {
        if (groups > 1 && pg.group[p] != last && ui.list_n < LIST_MAX)
            list_rows[ui.list_n++] = PR(R_HDR, pg.group[p], 0, 0);
        last = pg.group[p];
        for (k = 0; k < 4; k++)
            list_add(pg.r[p][k]);
    }
    if (pg.nmore && ui.list_n < LIST_MAX)              /* the knobs no page has */
        list_rows[ui.list_n++] = PR(R_HDR, GR_MORE, 0, 0);
    for (k = 0; k < pg.nmore; k++)
        list_add(pg.more[k]);
    if (ui.view == V_PERF)
        list_add(PR(R_ACT, ACT_PERF_TEST, 0, 0));
    if (ui.view == V_SONG) {
        list_add(PR(R_SLEN, 0, 0, 0));
        list_add(PR(R_ACT, ACT_SONG_INS, 0, 0));
        list_add(PR(R_ACT, ACT_SONG_DEL, 0, 0));
        list_add(PR(R_ACT, ACT_SONG_CLR, 0, 0));
    }
    put_s(list_title, ui.view == V_PART ? PART_N[ui.part] : VIEW_N[ui.view]);
    ui.list_sel = ui.list_top = 0;
    while (list_skip(ui.list_sel))
        ui.list_sel++;
    ui.overlay = ui.list_n ? O_LIST : O_NONE;
}

static void open_global(void)
{
    ui.list_n = 0;
    list_add(PR(R_CLKOUT, 0, 0, 0));
    list_add(PR(R_NOTEOUT, 0, 0, 0));
    list_add(PR(R_KEYLED, 0, 0, 0));
    list_add(PR(R_KEYSOUND, 0, 0, 0));
    list_add(PR(R_PALETTE, 0, 0, 0));
    list_add(PR(R_SMODE, 0, 0, 0));
    list_add(PR(R_ACT, ACT_SAVE, 0, 0));
    list_add(PR(R_AUTOSAVE, 0, 0, 0));
    list_add(PR(R_ACT, ACT_CLEAR_PAT, 0, 0));
    list_add(PR(R_ACT, ACT_RESET, 0, 0));
    list_add(PR(R_ACT, ACT_PERF, 0, 0));
    list_add(PR(R_ACT, ACT_ABOUT, 0, 0));
    put_s(list_title, "GLOBAL");
    ui.list_sel = ui.list_top = 0;
    ui.overlay = O_LIST;
}

/* ================================================================ questions === */
static void ask(int act, int arg, const char *q1, const char *q2)
{
    ui.ask_act = (uint8_t)act;
    ui.ask_arg = (uint16_t)arg;
    put_s(ui.ask_q[0], q1);
    put_s(ui.ask_q[1], q2 ? q2 : "");
    ui.overlay = O_ASK;
}

/* part `part` of a pattern has something to play */
static int part_used(const pattern_t *pt, int part)
{
    int v, s;
    if (part < NKIT) {
        for (v = 0; v < NDRUM; v++)
            if (sm_any(pt->drum[part].hit[v]))
                return 1;
        return 0;
    }
    if (part == PART_BRK)
        return pt->brk.steps != 0;
    for (s = 0; s < NSTEPS; s++)
        if (bstep_gate(&pt->bass[part - NKIT].step[s]) != G_REST)
            return 1;
    return 0;
}

static int pattern_used(const pattern_t *pt)
{
    int v, s, used = pt->brk.steps != 0;
    for (v = 0; v < NDRUM; v++)
        used |= sm_any(pt->drum[0].hit[v]) | sm_any(pt->drum[1].hit[v]);
    for (s = 0; s < NSTEPS; s++)
        used |= bstep_gate(&pt->bass[0].step[s]) != G_REST || bstep_gate(&pt->bass[1].step[s]) != G_REST;
    return used;
}

/* motion lanes of one part pattern: gone, and the knobs back to their own values */
static void clear_lanes(int pat, int part)
{
    int k;
    for (k = 0; k < NLANE; k++) {
        lane_t *l = &proj.arr.lane[k];
        if (l->used && l->pat == pat && l->part == part) {
            motion_clear(proj.arr.lane, k);
            engine_set(l->t, l->v, l->i, proj.sound.v[l->t][l->v][l->i]);
        }
    }
}

/* part `part` of pattern src onto pattern dst, its lanes too; 0 = done, -1 = lanes did not fit */
static int copy_part(int dst, int src, int part)
{
    pattern_t *d = &proj.pat[dst];
    const pattern_t *a = &proj.pat[src];
    int k, rc = 0;
    if (part < NKIT) {
        d->drum[part] = a->drum[part];
        if (part == PART_909)
            d->swing = a->swing;                      /* the 909 is the bar: its groove goes with it */
    } else if (part < PART_BRK)
        d->bass[part - NKIT] = a->bass[part - NKIT];
    else
        d->brk = a->brk;
    clear_lanes(dst, part);
    for (k = 0; k < NLANE; k++) {                     /* a copy lands on pattern dst: never copied again */
        const lane_t *l = &proj.arr.lane[k];
        int n, i;
        if (!l->used || l->pat != src || l->part != part)
            continue;
        if ((n = motion_alloc(proj.arr.lane, dst, part, l->t, l->v, l->i)) < 0) {
            rc = -1;
            break;
        }
        for (i = 0; i < NSTEPS; i++)
            proj.arr.lane[n].val[i] = l->val[i];
    }
    return rc;
}

static void clear_part(pattern_t *p, int part)
{
    int i;
    if (part < NKIT) {
        int w;
        for (w = 0; w < NSW; w++) {
            for (i = 0; i < NDRUM; i++)
                p->drum[part].hit[i][w] = 0;
            p->drum[part].accent[w] = 0;
        }
    } else if (part < PART_BRK) {
        for (i = 0; i < NSTEPS; i++)
            p->bass[part - NKIT].step[i].flags = G_REST;
    } else {
        p->brk.steps = 0;
        for (i = 0; i < 16; i++)
            p->brk.slice[i] = 0;
    }
}

/* ============================================================ help cards === */
/* Hold one button for HELP_HOLD ms without using it with anything: a card says what it does here,
 * its combinations included. A key, a turn or another button puts the card away (and the
 * combination works as always); letting go of the button after its card does nothing else. */
#define HELP_HOLD 900u
#define HELP_HOLD_SAVE 400u                    /* SAVE is held to copy: its card says what will be copied */
static int perf_testing(void);
#define HELP_LINES 6
static const char *const BTN_LABEL[NB] = {"FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ",
                                          "PLAY / STOP", "REC", "OCT-", "OCT+"};

static int help_lines(int b, const char *l[HELP_LINES])
{
    int n = 0, gen = ui.view == V_GEN, kbd = is_303() && ui.rec;
#define L(s) (l[n++] = (s))
    switch (b) {
    case B_FX: L("THE EFFECTS: REVERB, DELAY,"); L("TAPE AND KIT DRIVE"); L("AGAIN: THE NEXT PAGE"); break;
    case B_SEL: L("THIS SCREEN AS A LIST"); L("IN A LIST: RUN THE ACTION"); L("IN A QUESTION: YES"); break;
    case B_ENV:
        if (is_303()) { L("HOLD + KEY: AN ACCENT"); if (kbd) L("WRITING: + OCT: A REST"); }
        else if (is_drum()) { L("HOLD + WHITE KEY: ACCENT"); L("HOLD + BLACK KEY: LOUD HIT"); }
        else L("HOLD + KEY: AN ACCENT");
        break;
    case B_LFO:
        L("THE MIXER AND THE MASTER"); L("AGAIN: THE NEXT PAGE");
        if (is_303()) { L("HOLD + KEY: A SLIDE"); if (kbd) L("HOLD + OCT: A TIE"); }
        break;
    case B_EDIT: L("THE PART: ITS STEPS, ITS SOUND"); L("AGAIN: THE NEXT PAGE"); break;
    case B_GLO: L("SETTINGS, SAVE, CLEAR,"); L("FACTORY RESET, PERFORMANCE"); L("AGAIN: CLOSE"); break;
    case B_HOME:
        L("THE PATTERN: ALL FIVE PARTS"); L("AGAIN: THE NEXT PAGE   + SELECT: TEMPO"); L("+ WHITE KEY: PATTERN");
        if (is_drum() && part_view()) L("+ BLACK KEY: MUTE THE TRACK");
        L("+ REC: UNDO   + PLAY: REDO"); L("LIST: BACK   QUESTION: NO");
        break;
    case B_SAVE: {                                   /* says what a copy takes: this part, or all five */
        static char cp[40], cl[32];
        if (part_view())
            put_i(put_s(put_s(put_s(cp, "+ WHITE KEY: COPY "), PART_N[ui.part]), " P"), seq.ppat[ui.part] + 1);
        else
            put_s(cp, "+ WHITE KEY: COPY ALL FIVE PARTS");
        put_s(put_s(cl, "+ REC: CLEAR "), PART_N[ui.part]);
        L("TAP: SAVE EVERYTHING"); L(cp); L(ui.pbank ? "    TO P17-32 (+ OCT-: P1-16)" : "    TO P1-16 (+ OCT+: P17-32)"); L(cl);
        L("+ KNOB: FORGET ITS MOTION"); L(proj.set.autosave_off ? "AUTOSAVE IS OFF" : "STOPPED: SAVES BY ITSELF");
        break;
    }
    case B_ARP:
        L("TB-3PO: THE 303 LINE"); L("GENERATOR (303A, 303B)");
        if (gen) { L("OCT+: A NEW LINE"); L("OCT-: MUTATE IT"); }
        L("AGAIN: THE NEXT PAGE");
        break;
    case B_SEQ:
        L("THE SONG: PATTERNS, BAR BY BAR"); L("AGAIN: THE NEXT PAGE");
        break;
    case B_PLAY: L("START / STOP"); L("HOME + PLAY: REDO"); break;
    case B_REC:
        L("RECORD ON / OFF:"); L(is_303() ? "THE KEYS PLAY AND WRITE NOTES" : "BLACK KEYS, KNOB MOVES");
        L("HOME + REC: UNDO"); L("SAVE + REC: CLEAR PART");
        break;
    case B_OCTDN:
    case B_OCTUP:
        if (gen) L(b == B_OCTUP ? "A NEW 303 LINE" : "MUTATE THE LINE");
        else if (kbd) { L("THE KEYBOARD'S OCTAVE"); L("LFO + OCT: A TIE  ENV + OCT: A REST"); }
        else if (ui.view == V_HOME || ui.view == V_SONG) L(b == B_OCTUP ? "PATTERNS 17-32" : "PATTERNS 1-16");
        else { L(b == B_OCTUP ? "THE NEXT 16 STEPS" : "THE 16 STEPS BEFORE"); L("PAGES: 1-16 17-32 33-48 49-64"); }
        break;
    default: break;
    }
#undef L
    return n;
}

static void help_tick(uint32_t btn, uint32_t keys)
{
    uint32_t now = plat_ms();
    int i;
    if (ui.help) {
        uint32_t m = 1u << (ui.help - 1);
        if (btn != m || keys || (int32_t)(ui.turn_t - ui.down_t[ui.help - 1]) > 0 || ui.overlay == O_ASK)
            ui.help = 0;                                 /* used with something, or let go */
        return;
    }
    if (!btn || (btn & (btn - 1u)) || keys || ui.overlay == O_ASK || perf_testing())
        return;                                          /* exactly one button, nothing else */
    for (i = 0; i < NB; i++)
        if (btn == 1u << i && !(ui.btn_used & btn) && now - ui.down_t[i] >= (i == B_SAVE ? HELP_HOLD_SAVE : HELP_HOLD) &&
            (int32_t)(ui.turn_t - ui.down_t[i]) <= 0) {
            ui.help = (uint8_t)(i + 1);
            ui.btn_used |= btn;                          /* its release now does nothing */
        }
}

/* ================================================================ undo === */
/* HOME + REC undoes, HOME + PLAY redoes (undo.c): everything after the settings is undoable, the
 * sound, the patterns, the song and the knob motion. A step is one gesture: what changed between
 * two quiet moments (UNDO_QUIET ms with nothing touched or held), or a whole recording pass. */
#define UNDO_QUIET 400u
#define UNDO_OFF __builtin_offsetof(project_t, sound)
static undo_t undo X0X_POOL;
static uint8_t undo_shadow[sizeof(project_t) - UNDO_OFF] X0X_POOL;
static uint8_t undo_lanes;             /* the last undo / redo touched the knob motion */

static void undo_start(void) { undo_init(&undo, (uint8_t *)&proj + UNDO_OFF, undo_shadow, sizeof undo_shadow); }

static void undo_tick(void)
{
    if (perf_testing() || (ui.rec && seq.playing) || ui.btn || ui.keys || plat_ms() - ui.act_t < UNDO_QUIET ||
        (ui.frame & 3u))
        return;
    undo_commit(&undo);
}

/* a run of the project changed under the engine: the sound's pots go to the engine again */
static void undo_changed(uint32_t off, uint32_t len)
{
    const uint32_t s0 = __builtin_offsetof(project_t, sound), s1 = s0 + sizeof(sound_t);
    const uint32_t l0 = __builtin_offsetof(project_t, arr) + __builtin_offsetof(arrange_t, lane);
    uint32_t a = off + UNDO_OFF, k;
    for (k = a; k < a + len; k++) {
        if (k >= s0 && k < s1) {
            uint32_t x = k - s0, t = x / (NVOICES_MAX * NPARAMS_MAX), v = (x / NPARAMS_MAX) % NVOICES_MAX,
                     i = x % NPARAMS_MAX;
            if ((int)v < engine_nvoices((int)t) && (int)i < engine_nparams((int)t, (int)v))
                engine_set((int)t, (int)v, (int)i, proj.sound.v[t][v][i]);
        } else if (k >= l0) {
            undo_lanes = 1;
        }
    }
}

/* what an undo step touched, in words: "909 P3", "SOUND", "SONG", "MOTION" */
static void undo_name(uint32_t lo, uint32_t hi, char *t)
{
    const uint32_t p0 = __builtin_offsetof(project_t, pat), a0 = __builtin_offsetof(project_t, arr),
                   l0 = a0 + __builtin_offsetof(arrange_t, lane);
    uint32_t a = lo + UNDO_OFF, b = hi + UNDO_OFF;
    if (b < p0) {
        put_s(t, "SOUND");
    } else if (a >= p0 && b < a0 && (a - p0) / sizeof(pattern_t) == (b - p0) / sizeof(pattern_t)) {
        uint32_t pi = (a - p0) / sizeof(pattern_t), x = (a - p0) % sizeof(pattern_t), y = (b - p0) % sizeof(pattern_t);
        int pa = (int)(x < __builtin_offsetof(pattern_t, bass) ? x / sizeof(dpart_t)
                       : x < __builtin_offsetof(pattern_t, brk) ? NKIT + (x - __builtin_offsetof(pattern_t, bass)) / sizeof(bpart_t)
                       : PART_BRK);
        int pb = (int)(y < __builtin_offsetof(pattern_t, bass) ? y / sizeof(dpart_t)
                       : y < __builtin_offsetof(pattern_t, brk) ? NKIT + (y - __builtin_offsetof(pattern_t, bass)) / sizeof(bpart_t)
                       : PART_BRK);
        char *q = put_s(t, pa == pb ? PART_N[pa] : "PATTERN");
        put_i(put_s(q, " P"), (int)pi + 1);
    } else if (a >= p0 && b < a0) {
        put_s(t, "PATTERNS");
    } else if (a >= a0 && b < l0) {
        put_s(t, "SONG");
    } else if (a >= l0) {
        put_s(t, "MOTION");
    } else {
        put_s(t, "CHANGES");
    }
}

static void do_undo(int redo)
{
    uint32_t lo, hi;
    char t[24];
    undo_commit(&undo);                              /* what was just done is a step too */
    undo_lanes = 0;
    if (!(redo ? undo_redo : undo_undo)(&undo, undo_changed, &lo, &hi)) {
        say(redo ? "NOTHING TO REDO" : "NOTHING TO UNDO", 0);
        return;
    }
    if (undo_lanes)
        engine_motion_reset();
    engine_brk_loops();
    ui.outline_ok = 0;
    if (ui.song_sel > song()->len)
        ui.song_sel = song()->len;
    build_pages();
    mark_dirty();
    undo_name(lo, hi, t);
    say(redo ? "REDO " : "UNDO ", t);
}

static void save_project(void)
{
    int rc = project_save();
    say(rc == -7 ? "MEMORY FULL: NOT SAVED" : rc ? "SAVE FAILED" : "SAVED", 0);
    if (!rc)
        ui.dirty = 0;
}

/* ============================================================ performance === */
/* PERF: what the FM-1 measures of itself, a window a second: the clock (the core's cycle counter
 * against the 24 MHz timer), the audio load and its peak, dropouts, stall cycles (when switched on),
 * and each stage's share of the CPU (the engine times them: engine.h ENG_PROF_N). PERF TEST plays
 * three patterns of its own (the factory loop, all five parts, a dense worst case) on the factory
 * sound, 1 s to settle and 3 s measured each, and keeps the table; then it puts back the project's
 * patterns, sound, tempo, mutes, chain and song mode. The project itself is never written. */
static const char *const PT_N[3] = {"FACTORY LOOP", "ALL FIVE", "WORST CASE"};
static pattern_t perf_bench[3];
static sound_t perf_snd;

static void perf_window(int force)
{
    uint32_t now = plat_ms(), t = plat_ticks24(), c = plat_cycles(), f = eng_prof_frames, st[3];
    uint32_t dt = t - perf.ticks, dc = c - perf.cyc, df = f - perf.frames, sum = 0;
    float ups;
    int k;
    if (!force && now - perf.t_ms < 1000u)
        return;
    perf.t_ms = now;
    if (perf.have && df) {
        if (plat_cycles_cpu() && dt >= 240u)
            perf.mhz10 = (uint16_t)(dc / (dt / 240u));          /* cycles per (dt / 24) us, x 10 */
        ups = plat_cycles_cpu() ? (float)perf.mhz10 * (100000.0f / 44100.0f) : (float)plat_cycles_hz() / 44100.0f;
        for (k = 0; k < ENG_PROF_N; k++) {
            uint32_t d = eng_prof[k] - perf.prof[k];
            sum += d;
            perf.stage[k] = (uint16_t)((float)d * 1000.0f / ((float)df * ups) + 0.5f);
        }
        perf.cps = plat_cycles_cpu() ? (uint16_t)(sum / df) : 0;
        perf.load = (uint8_t)plat_cpu_pct();
        perf.peak = (uint8_t)plat_cpu_peak_pct();
        if (plat_stalls(st) == 0 && dc)
            for (k = 0; k < 3; k++)
                perf.stall[k] = (uint8_t)((float)(st[k] - perf.st[k]) * 100.0f / (float)dc + 0.5f);
    } else {
        plat_cpu_peak_pct();
    }
    perf.ticks = t;
    perf.cyc = c;
    perf.frames = f;
    for (k = 0; k < ENG_PROF_N; k++)
        perf.prof[k] = eng_prof[k];
    if (plat_stalls(st) == 0)
        for (k = 0; k < 3; k++)
            perf.st[k] = st[k];
    perf.have = 1;
}

/* the test's patterns: 0 the factory loop (909 kick, clap, open hats; 303 A; the break), 1 all five,
 * 2 everything dense */
static void perf_build(void)
{
    /* the tracks in key order (pattern.h): 909 BD SD LT MT HT RS CP CH OH CR RD, 808 ... CB CY OH CH */
    enum { DR_BD, DR_SD, DR_LT, DR_MT, DR_HT, DR_RS, DR_CP, DR_CH, DR_OH, DR_CR, DR_RD };
    enum { D8_BD, D8_SD, D8_LT, D8_MT, D8_HT, D8_RS, D8_CP, D8_CB, D8_CY, D8_OH, D8_CH };
    int i;
    for (i = 0; i < 3; i++) {
        pattern_t *p = &perf_bench[i];
        dpart_t *a = &p->drum[0], *b = &p->drum[1];
        pattern_init(p, 0x3B0u + 977u * (uint32_t)i, 0x5A1u + 613u * (uint32_t)i);
        tb3po_generate(&p->bass[0]);
        if (i)
            tb3po_generate(&p->bass[1]);
        p->brk.steps = 0xFFFFu;
        a->hit[DR_BD][0] = 0x1111u;
        a->hit[DR_CP][0] = 0x1010u;
        a->hit[DR_OH][0] = 0x4444u;
        a->accent[0] = 0x0101u;
        if (i >= 1) {
            b->hit[D8_BD][0] = 0x1111u;
            b->hit[D8_CP][0] = 0x1010u;
            b->hit[D8_CH][0] = 0x1111u;
            b->hit[D8_OH][0] = 0x4444u;
            b->hit[D8_CB][0] = 0x0808u;
        }
        if (i == 2) {
            a->hit[DR_SD][0] = 0x1010u;
            a->hit[DR_CH][0] = 0xFFFFu;
            a->hit[DR_CR][0] = 0x0001u;
            a->hit[DR_RD][0] = 0x1111u;
            a->hit[DR_LT][0] = 0x4000u;
            a->accent[0] = 0x1111u;
            b->hit[D8_SD][0] = 0x1010u;
            b->hit[D8_CH][0] = 0xFFFFu;
            b->hit[D8_CY][0] = 0x0101u;
            b->hit[D8_LT][0] = 0x8080u;
        }
    }
}

static void perf_restore(void)
{
    int p;
    seq.pat = perf.pat;
    for (p = 0; p < NPARTS; p++) {
        seq.ppat[p] = perf.ppat[p];
        seq.pcue[p] = 0xFF;
    }
    seq.mute = perf.mute;
    seq.song_on = perf.song_on;
    seq_chain(&seq, perf.ca, perf.cb);
    seq.bpm = perf.bpm;
    engine_apply_sound(&proj.sound);
    engine_motion_enable(1);
    engine_guard_enable(1);
    engine_brk_loops();
}

static void perf_test_start(void)
{
    int p;
    if (perf.test >= 0 && perf.phase != 4)
        return;
    perf.pat = seq.pat;
    for (p = 0; p < NPARTS; p++)
        perf.ppat[p] = seq.ppat[p];
    perf.mute = seq.mute;
    perf.song_on = seq.song_on;
    perf.ca = seq.chain_a;
    perf.cb = seq.chain_b;
    perf.bpm = seq.bpm;
    perf_build();
    if (seq.playing)
        seq_stop(&seq);
    perf.test = 0;
    perf.phase = 0;
    for (p = 0; p < 3; p++)
        perf.res_load[p] = perf.res_cps[p] = perf.res_peak[p] = 0;
}

static void perf_test_abort(void)
{
    seq_stop(&seq);
    perf.phase = 3;
    perf.test = 0;
    say("PERF TEST STOPPED", 0);
}

/* the test, one step a frame (the main loop): the transport changes take a block to land */
static void perf_test_tick(void)
{
    uint32_t now = plat_ms();
    int p;
    if (perf.test < 0 || perf.phase == 4)
        return;
    switch (perf.phase) {
    case 0:                                           /* stopped: swap in the test's patterns and sound */
        if (seq.playing || seq.req_stop)              /* (a pending stop would cancel the start below) */
            return;
        for (p = 0; p < NPARTS; p++) {
            seq.ppat[p] = 0;
            seq.pcue[p] = 0xFF;
        }
        seq.pat = perf_bench;
        seq.mute = 0;
        seq.song_on = 0;
        seq_chain(&seq, 0, 0);
        seq.bpm = 125.0f;
        engine_sound_defaults(&perf_snd);
        engine_apply_sound(&perf_snd);
        engine_motion_enable(0);
        engine_guard_enable(0);
        engine_brk_loops();
        seq_start(&seq);
        perf.phase = 1;
        perf.test_t0 = now;
        break;
    case 1:                                           /* settle on the scenario's pattern */
        if (!seq.playing && now - perf.test_t0 > 200u) {
            perf_test_abort();
            return;
        }
        if (now - perf.test_t0 >= 1000u && seq.ppat[PART_909] == perf.test) {
            perf_window(1);
            perf.phase = 2;
            perf.test_t0 = now;
        }
        break;
    case 2:                                           /* measure 3 s */
        if (!seq.playing) {
            perf_test_abort();
            return;
        }
        if (now - perf.test_t0 >= 3000u) {
            int k;
            uint32_t sum = 0;
            perf_window(1);
            for (k = 0; k < ENG_PROF_N; k++)
                sum += perf.stage[k];
            perf.res_load[perf.test] = (uint16_t)sum;
            perf.res_peak[perf.test] = perf.peak;
            perf.res_cps[perf.test] = perf.cps;
            if (++perf.test < 3) {
                seq_cue(&seq, perf.test);
                perf.phase = 1;
                perf.test_t0 = now;
            } else {
                seq_stop(&seq);
                perf.phase = 3;
            }
        }
        break;
    default:                                          /* put everything back once stopped */
        if (seq.playing || seq.req_stop)
            return;
        perf_restore();
        perf.phase = 4;
        if (perf.test == 3) {
            perf.test = PT_DONE;
            say("PERF TEST DONE", 0);
        } else {
            perf.test = PT_IDLE;
        }
        break;
    }
}

static void set_view(int v);

static void run_action(int act, int arg)
{
    char t[28], *q;
    switch (act) {
    case ACT_SAVE: save_project(); break;
    case ACT_CLEAR_PART:
        clear_part(pat_of(arg), arg);
        clear_lanes(seq.ppat[arg], arg);
        mark_dirty();
        say("CLEARED ", PART_N[arg]);
        break;
    case ACT_CLEAR_PAT: {
        int i;
        for (i = 0; i < NPARTS; i++) {
            clear_part(pat_of(i), i);
            clear_lanes(seq.ppat[i], i);
        }
        mark_dirty();
        say("PATTERN CLEARED", 0);
        break;
    }
    case ACT_COPY: {                                  /* arg: pattern | (part + 1) << 8, part 0 = all five */
        int dst = arg & 0xFF, part = (arg >> 8) - 1, i, rc = 0;
        for (i = 0; i < NPARTS; i++)
            if ((part < 0 || i == part) && seq.ppat[i] != dst)
                rc |= copy_part(dst, seq.ppat[i], i);
        mark_dirty();
        q = put_s(t, part < 0 ? "ALL FIVE" : PART_N[part]);
        if (part >= 0)
            q = put_i(put_s(q, " P"), seq.ppat[part] + 1);
        put_i(put_s(q, " COPIED TO P"), dst + 1);
        say(rc ? "MOTION FULL: NOT ALL COPIED" : t, 0);
        break;
    }
    case ACT_SONG_INS:
    case ACT_SONG_DEL: {
        song_t *sg = song();
        int k = ui.song_sel, i;
        if (k >= sg->len)
            break;
        if (act == ACT_SONG_INS && sg->len < NSONG) {
            for (i = sg->len; i > k; i--)
                sg->bar[i] = sg->bar[i - 1];
            sg->len++;
        } else if (act == ACT_SONG_DEL) {
            for (i = k; i + 1 < sg->len; i++)
                sg->bar[i] = sg->bar[i + 1];
            sg->len--;
        }
        mark_dirty();
        say(act == ACT_SONG_INS ? "BAR INSERTED" : "BAR DELETED", 0);
        break;
    }
    case ACT_SONG_CLR:
        song()->len = 0;
        ui.song_sel = 0;
        mark_dirty();
        say("SONG CLEARED", 0);
        break;
    case ACT_RESET:
        project_defaults();
        engine_apply_sound(&proj.sound);
        engine_brk_loops();
        palette_set(proj.set.palette);
        mark_dirty();
        ui.outline_ok = 0;
        say("FACTORY SOUND + PATTERNS", 0);
        break;
    case ACT_PERF:
        set_view(V_PERF);
        break;
    case ACT_PERF_TEST:
        ui.overlay = O_NONE;                          /* the table is under the list */
        perf_test_start();
        say("PERF TEST: 15 SECONDS", 0);
        break;
    case ACT_ABOUT:
        q = put_s(t, "X0X " X0X_VERSION "  MEM ");
        q = put_s(put_i(q, project_mem_pct()), "%  CPU ");
        put_s(put_i(q, (int)plat_cpu_pct()), "%");
        say(t, 0);
        break;
    default: break;
    }
}

/* ===================================================================== input === */
/* knob acceleration by speed: a slow turn is one step a detent (fine detail), a quick one up to 8,
 * so a fast half turn sweeps 0-127. Several detents in one read are a fast turn too. Small ranges
 * stay gentler; switches and lists never get here (turn_ref steps them one at a time). */
static uint8_t accel_off;              /* the host simulator's "spin": exact steps */
static int32_t accel(int role, int32_t s, int range)
{
    uint32_t now = plat_ms(), dt = now - ui.enc_t[role], a = (uint32_t)(s < 0 ? -s : s), m;
    ui.enc_t[role] = now;
    if (range <= 24 || !a || accel_off)
        return s;
    if (a > 1)
        dt /= a;                                      /* per detent */
    m = dt < 12u ? 8u : dt < 25u ? 5u : dt < 45u ? 3u : dt < 80u ? 2u : 1u;
    if (range < 100 && m > 3u)
        m = 3u;
    if (range > 150 && m > 1u)
        m *= 2u;                                      /* tempo and the other wide ones, when quick */
    return s * (int32_t)m;
}

static void set_view(int v)
{
    if (v != ui.view)
        ui.prev_view = ui.view;
    ui.view = (uint8_t)v;
    ui.held_step = -1;
    ui.brk_held = -1;
    ui.overlay = O_NONE;
    build_pages();
}

/* a sound knob turned: record its lane (REC, playing), or let the knob win over the lane for a pass */
static void motion_touch(pref_t r)
{
    int part, k;
    if (r.kind != R_ENG || !seq.playing)
        return;
    part = engine_motion_part(r.a, r.b);
    k = motion_find(proj.arr.lane, seq.ppat[part], part, r.a, r.b, r.c);
    if (ui.rec) {
        if (k < 0 && (k = motion_alloc(proj.arr.lane, seq.ppat[part], part, r.a, r.b, r.c)) < 0) {
            say("MOTION FULL", 0);
            return;
        }
        engine_motion_rec(k);
    } else if (k >= 0) {
        engine_motion_hold(k);
    }
}

/* SAVE held + a sound knob: forget its motion in this pattern */
static void motion_forget(pref_t r)
{
    int part, k;
    if (r.kind != R_ENG)
        return;
    part = engine_motion_part(r.a, r.b);
    k = motion_find(proj.arr.lane, seq.ppat[part], part, r.a, r.b, r.c);
    if (k < 0) {
        say("NO MOTION ON ", pref_name(r));
        return;
    }
    motion_clear(proj.arr.lane, k);
    engine_set(r.a, r.b, r.c, proj.sound.v[r.a][r.b][r.c]);
    mark_dirty();
    say("MOTION CLEARED: ", pref_name(r));
}

/* what a knob's lane plays now in this pattern, -1 none (the knob strip draws it) */
static int motion_now(pref_t r)
{
    int part;
    if (r.kind != R_ENG)
        return -1;
    part = engine_motion_part(r.a, r.b);
    return engine_motion_value(motion_find(proj.arr.lane, seq.ppat[part], part, r.a, r.b, r.c));
}
static int has_motion(pref_t r)
{
    int part;
    if (r.kind != R_ENG)
        return 0;
    part = engine_motion_part(r.a, r.b);
    return motion_find(proj.arr.lane, seq.ppat[part], part, r.a, r.b, r.c) >= 0;
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
    motion_touch(r);
}

static int rec_step(int track) { return eng_step[track]; }

static void drum_key(int v, int down)
{
    int k = ui.part;
    if (!down)
        return;
    ui.sel[k] = (uint8_t)v;
    build_pages();
    /* KEY SOUND STOPPED (the default): while the pattern plays a black key only selects its track,
     * so choosing one to edit does not add a hit to the groove; stopped, it plays it, and with REC
     * on it always does (that is how drums are recorded) */
    if (proj.set.keysound || !seq.playing || ui.rec)
        engine_drum(k, v, (ui.btn & (1u << B_ENV)) ? 1.0f : 0.75f);
    if (ui.btn & (1u << B_ENV))
        ui.btn_used |= 1u << B_ENV;
    if (ui.rec && seq.playing) {
        int s = rec_step(TRK_DRUM + k);
        sm_set(cur_pat()->drum[k].hit[v], s);
        if (ui.btn & (1u << B_ENV))
            sm_set(cur_pat()->drum[k].accent, s);
        mark_dirty();
    }
}

static void drum_step(int white)
{
    int k = ui.part, s = ui.spage * 16 + white;
    dpart_t *d = &cur_pat()->drum[k];
    if (s >= NSTEPS)
        return;
    if (ui.btn & (1u << B_ENV)) {
        sm_flip(d->accent, s);
        ui.btn_used |= 1u << B_ENV;
    } else {
        sm_flip(d->hit[ui.sel[k]], s);
    }
    mark_dirty();
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
    mark_dirty();
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
            mark_dirty();
        }
    } else if (kbd_held_note[b] == n) {
        engine_bass_off(b);
        kbd_held_note[b] = 0;
    }
}

/* an edit sounds as the drum keys do (KEY SOUND): while stopped or recording, or always */
static int key_sounds(void) { return proj.set.keysound || !seq.playing || ui.rec; }
#define STEP_TAP_MS 300u
static void bass_step_key(int white, int down)
{
    int b = bidx(), s = ui.spage * 16 + white;
    bpart_t *bp = &cur_pat()->bass[b];
    if (s >= NSTEPS)
        return;
    if (down) {                                       /* an empty step is on at once: hold it to edit it */
        ui.held_step = (int8_t)s;
        ui.step_edited = 0;
        ui.step_t0 = plat_ms();
        ui.step_created = bstep_gate(&bp->step[s]) == G_REST;
        if (ui.step_created) {
            bp->step[s].flags = G_NOTE;
            mark_dirty();
        }
        ui.step_preview = key_sounds();               /* as the drum keys */
        if (ui.step_preview)
            engine_bass_on(b, bp->step[s].note + bp->transpose - 24, (bp->step[s].flags & BS_ACCENT) != 0, 0);
    } else if (ui.held_step == s) {
        if (!ui.step_created && !ui.step_edited && plat_ms() - ui.step_t0 < STEP_TAP_MS) {   /* a tap takes it off */
            bp->step[s].flags = G_REST;
            mark_dirty();
        }
        if (ui.step_preview)
            engine_bass_off(b);
        ui.step_preview = 0;
        ui.held_step = -1;
    }
}

static int all_on(int w)                               /* every part plays pattern w */
{
    int p;
    for (p = 0; p < NPARTS; p++)
        if (seq.ppat[p] != w)
            return 0;
    return 1;
}

static const uint32_t MUTE_MASK[NPARTS] = {(1u << NDRUM) - 1u, ((1u << NDRUM) - 1u) << NDRUM, 1u << MUTE_BASS0,
                                           1u << (MUTE_BASS0 + 1), 1u << MUTE_BRK};
static int part_muted(int p) { return (seq.mute & MUTE_MASK[p]) == MUTE_MASK[p]; }

static int perf_testing(void) { return perf.test >= 0 && perf.phase != 4; }

static void key_event(int k, int down)
{
    int w = KEY_WHITE[k], bl = KEY_BLACK[k], wp = w >= 0 ? ui.pbank * 16 + w : -1;   /* wp: a white key's pattern */
    if (ui.overlay == O_ASK || ui.view == V_PERF || perf_testing())
        return;
    if (down && w >= 0 && (ui.btn & (1u << B_SAVE))) {     /* SAVE held + white key: copy here */
        int part = part_view() ? ui.part : -1, arg = wp | (part + 1) << 8;   /* a part's screen: that part */
        ui.btn_used |= 1u << B_SAVE;
        if (part >= 0 ? seq.ppat[part] == wp : all_on(wp))
            return;
        if (pattern_used(&proj.pat[wp])) {
            char t[24];
            put_s(put_i(put_s(t, "COPY OVER P"), wp + 1), "?");
            ask(ACT_COPY, arg, t, "IT HAS NOTES");
        } else
            run_action(ACT_COPY, arg);
        return;
    }
    if (ui.view == V_SONG && !(ui.btn & (1u << B_HOME))) {   /* SONG: white = the bar's pattern, black = its mutes */
        int k = ui.song_sel;
        if (!down || k >= NSONG || (w < 0 && bl >= NPARTS))
            return;
        song_fill(k);
        if (w >= 0) {
            int p;
            for (p = 0; p < NPARTS; p++)
                song()->bar[k].pat[p] = (uint8_t)wp;
            if (k + 1 < NSONG)
                ui.song_sel = (uint8_t)(k + 1);
        } else {
            song()->bar[k].mute ^= (uint8_t)(1u << bl);
        }
        mark_dirty();
        return;
    }
    if (w >= 0 && (ui.view == V_HOME || (ui.btn & (1u << B_HOME)))) {
        if (ui.btn & (1u << B_HOME))
            ui.btn_used |= 1u << B_HOME;
        if (down) {
            if (ui.chain_first >= 0 && ui.chain_first != wp) {
                char t[16], *q = t;
                seq_chain(&seq, ui.chain_first, wp);
                q = put_i(q, (ui.chain_first < wp ? ui.chain_first : wp) + 1);
                *q++ = '-';
                put_i(q, (ui.chain_first < wp ? wp : ui.chain_first) + 1);
                say("CHAIN P", t);
            } else {
                ui.chain_first = (int8_t)wp;
                seq_chain(&seq, 0, 0);
                seq_cue(&seq, wp);
            }
        } else if (ui.chain_first == wp) {
            ui.chain_first = -1;
        }
        return;
    }
    if (bl >= 0 && ui.view == V_MIX && cur_page() == 0) {   /* MIX's PARTS: black keys 1-5 pick the part */
        if (down && bl < NPARTS) {
            ui.mixsel = (uint8_t)bl;
            build_pages();
        }
        return;
    }
    if (bl >= 0 && drum_mix_page() >= 0) {            /* MIX, a drum machine's page: black keys pick the track */
        if (down) {
            int km = drum_mix_page();
            ui.sel[km] = (uint8_t)bl;
            if (proj.set.keysound || !seq.playing)
                engine_drum(km, bl, 0.75f);
            build_pages();
        }
        return;
    }
    if (ui.view == V_HOME && bl >= 0) {
        if (down && bl < NPARTS) {
            seq.mute = part_muted(bl) ? seq.mute & ~MUTE_MASK[bl] : seq.mute | MUTE_MASK[bl];
            if (seq.song_rec && seq.playing && seq.song_pos < song()->len) {   /* song recording: this bar too */
                song_bar_t *b = &song()->bar[seq.song_pos];
                b->mute = (uint8_t)(part_muted(bl) ? b->mute | 1u << bl : b->mute & ~(1u << bl));
            }
            say(part_muted(bl) ? "MUTED " : "UNMUTED ", PART_N[bl]);
        }
        return;
    }
    if (is_drum() && bl >= 0 && (ui.btn & (1u << B_HOME))) {   /* HOME + black key: mute that track */
        if (down) {
            uint32_t m = 1u << (ui.part * NDRUM + bl);
            const char *nm = engine_voice_name(ui.part == PART_909 ? T_909 : T_808, bl);
            ui.btn_used |= 1u << B_HOME;
            seq.vmute ^= m;
            say((seq.vmute & m) ? "MUTED " : "UNMUTED ", nm);
        }
        return;
    }
    if (is_drum()) {
        if (bl >= 0)
            drum_key(bl, down);
        else if (down)
            drum_step(w);
    } else if (is_303()) {
        if (ui.rec)                                    /* REC: the keys play and write notes */
            bass_kbd(k, down);
        else if (w >= 0)
            bass_step_key(w, down);
    } else {
        brkpart_t *bp = &cur_pat()->brk;
        if (bl >= 0 && bl < 8 && down && ui.brk_held >= 0) {     /* a step held + black 1-8: its own slice */
            int st = ui.brk_held;
            bp->slice[st] = (uint8_t)(bp->slice[st] == bl + 1 ? 0 : bl + 1);
            bp->steps |= 1u << st;
            ui.brk_pinkeys |= (uint16_t)(1u << bl);
            mark_dirty();
            if (bp->slice[st]) {
                char t[24];
                put_i(put_s(put_i(put_s(t, "STEP "), st + 1), ": SLICE "), bl + 1);
                say(t, 0);
            } else
                say("STEP: THE GENERATOR'S SLICE", 0);
        } else if (bl >= 0 && !down && (ui.brk_pinkeys & (1u << bl))) {
            ui.brk_pinkeys &= (uint16_t)~(1u << bl);
        } else if (bl >= 0)
            engine_brk_live(bl, down);
        else if (w >= 0 && down) {
            bp->steps ^= 1u << w;
            ui.brk_held = (int8_t)w;
            mark_dirty();
        } else if (w >= 0 && ui.brk_held == w)
            ui.brk_held = -1;
    }
}

static void list_move(int d)
{
    int s = ui.list_sel + d;
    while (s > 0 && s < ui.list_n - 1 && list_skip(s))   /* over a section header */
        s += d;
    s = s < 0 ? 0 : s >= ui.list_n ? ui.list_n - 1 : s;
    if (list_skip(s))
        s = ui.list_sel;
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
        ask(ACT_RESET, 0, "FACTORY RESET?", "SOUND, PATTERNS, SONG");
    else if (r.a == ACT_SONG_CLR)
        ask(ACT_SONG_CLR, 0, "CLEAR THE SONG?", 0);
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
    if (perf_testing()) {                             /* the test owns the transport: HOME or PLAY stop it */
        if ((b == B_HOME || b == B_PLAY) && perf.phase != 3)
            perf_test_abort();
        return;
    }
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
        if (ui.btn & (1u << B_HOME)) {               /* HOME + PLAY: redo */
            ui.btn_used |= 1u << B_HOME;
            do_undo(1);
            break;
        }
        if (seq.playing)
            seq_stop(&seq);
        else {
            seq.song_start = ui.song_sel < song()->len ? ui.song_sel : 0;
            seq_start(&seq);
        }
        break;
    case B_REC:
        if (ui.btn & (1u << B_HOME)) {               /* HOME + REC: undo */
            ui.btn_used |= 1u << B_HOME;
            do_undo(0);
            break;
        }
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
        say(ui.rec ? "RECORD ON: KEYS + KNOBS" : "RECORD OFF", 0);
        break;
    case B_HOME:                                      /* the pattern; again: its next page (a list: back) */
        if (ui.overlay != O_NONE || ui.view == V_GLO)
            close_overlay();
        else if (ui.view == V_HOME)
            *page_of(V_HOME) = (uint8_t)((cur_page() + 1) % pg.n);
        else
            set_view(V_HOME);
        break;
    case B_SEL:
        if (ui.overlay == O_LIST)
            list_enter();
        else
            open_list_of_pages();
        break;
    case B_EDIT:
        if (ui.view == V_PART && ui.overlay == O_NONE)
            *page_of(V_PART) = (uint8_t)((cur_page() + 1) % pg.n);
        else
            set_view(V_PART);
        break;
    case B_ARP:
        if (!is_303()) {
            say("TB-3PO: PICK 303A OR 303B", 0);
            break;
        }
        if (ui.view == V_GEN && ui.overlay == O_NONE)  /* the generator; again: its next page */
            *page_of(V_GEN) = (uint8_t)((cur_page() + 1) % pg.n);
        else
            set_view(V_GEN);
        break;
    case B_FX:
        if (ui.view == V_FX && ui.overlay == O_NONE)
            *page_of(V_FX) = (uint8_t)((cur_page() + 1) % pg.n);
        else
            set_view(V_FX);
        break;
    case B_LFO:
        if (ui.view == V_MIX && ui.overlay == O_NONE)
            *page_of(V_MIX) = (uint8_t)((cur_page() + 1) % pg.n);
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
    case B_SEQ:                                       /* the song; again: its next page */
        if (ui.view == V_SONG && ui.overlay == O_NONE)
            *page_of(V_SONG) = (uint8_t)((cur_page() + 1) % pg.n);
        else
            set_view(V_SONG);
        break;
    case B_SAVE: save_project(); break;
    case B_OCTUP:
    case B_OCTDN: {
        int up = b == B_OCTUP;
        if (ui.btn & (1u << B_SAVE)) {               /* SAVE + OCT: the bank a copy goes to */
            ui.btn_used |= 1u << B_SAVE;
            ui.pbank = (uint8_t)up;
            say(up ? "COPY TO P17-32" : "COPY TO P1-16", 0);
            ui.help = 0;
        } else if (ui.view == V_GEN) {
            bpart_t *bp = &cur_pat()->bass[bidx()];
            if (up) {
                char t[20];
                bp->gen.seed = tb3po_new_seed(plat_ms() ^ (ui.frame << 7));
                tb3po_generate(bp);
                ui.gen_stale[bidx()] = 0;
                put_hex(put_s(t, "NEW LINE "), bp->gen.seed & 0xFFFFu, 4);
                say(t, 0);
            } else {
                uint32_t r = bp->gen.seed ^ plat_ms();
                tb3po_mutate(bp, &r);
                ui.gen_stale[bidx()] = 0;
                say("MUTATED", 0);
            }
            mark_dirty();
        } else if (is_303() && ui.rec && ui.view == V_PART) {   /* pitch entry: the keyboard's octave */
            if (ui.btn & (1u << B_LFO)) {                 /* LFO + OCT: a tie; ENV + OCT: a rest */
                ui.btn_used |= 1u << B_LFO;
                bass_write(bidx(), 0, G_TIE);
            } else if (ui.btn & (1u << B_ENV)) {
                ui.btn_used |= 1u << B_ENV;
                bass_write(bidx(), 0, G_REST);
            } else {
                char t[12];
                ui.oct = (int8_t)(ui.oct + (up ? 1 : -1));
                ui.oct = ui.oct < -2 ? -2 : ui.oct > 3 ? 3 : ui.oct;
                put_i(put_s(t, "OCTAVE "), ui.oct);
                say(t, 0);
            }
        } else if (ui.view == V_HOME || ui.view == V_SONG) {   /* the pattern bank: 1-16, 17-32 */
            ui.pbank = (uint8_t)up;
            say(up ? "PATTERNS 17-32" : "PATTERNS 1-16", 0);
        } else {
            char t[20];
            int pg_ = ui.spage + (up ? 1 : -1);
            ui.spage = (uint8_t)(pg_ < 0 ? 0 : pg_ > NSTEPS / 16 - 1 ? NSTEPS / 16 - 1 : pg_);
            put_i(put_s(put_i(put_s(t, "STEPS "), ui.spage * 16 + 1), "-"), ui.spage * 16 + 16);
            say(t, 0);
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
            any_button = 1;
            ui.btn_used &= ~m;
            ui.down_t[i] = plat_ms();
            if (i == B_REC || i == B_PLAY)             /* transport acts on the press: timing */
                button_tap((int)i);
        } else if (!(ui.btn_used & m) && i != B_REC && i != B_PLAY) {
            button_tap((int)i);
        }
    }
    if (btn || keys || btn != ui.btn || keys != ui.keys)
        ui.act_t = plat_ms();                         /* held or changed: being played */
    help_tick(btn, keys);
    ui.btn = btn;
    ch = keys ^ ui.keys;
    ui.keys = keys;
    for (i = 0; i < NKEYS; i++)
        if (ch & (1u << i))
            key_event((int)i, (keys >> i) & 1u);
    build_pages();
    seq.song_rec = (uint8_t)(seq.song_on && ui.rec);  /* SONG mode + REC: the song is being written */
    if (ui.overlay == O_ASK) {                         /* a question: the knobs wait */
        for (i = 0; i < NE; i++)
            enc((int)i);
        return;
    }
    if (perf_testing()) {                              /* the test owns the knobs too */
        for (i = 0; i < NE; i++)
            enc((int)i);
        return;
    }
    if ((e = enc(EN_SELECT)) != 0) {             /* SELECT: move; with HOME held: tempo */
        if (ui.btn & (1u << B_HOME)) {
            float bpm = seq.bpm + (float)accel(EN_SELECT, e, 200);
            seq.bpm = bpm < 20.0f ? 20.0f : bpm > 275.0f ? 275.0f : bpm;
            ui.btn_used |= 1u << B_HOME;
            ui.touched = 4;
            ui.touch_until = plat_ms() + 900u;
            mark_dirty();
        } else if (ui.overlay == O_LIST) {
            list_move(e > 0 ? 1 : -1);
        } else {
            int p = cur_page() + (e > 0 ? 1 : -1);
            *page_of(ui.view) = (uint8_t)(p < 0 ? 0 : p >= pg.n ? pg.n - 1 : p);
        }
    }
    if ((e = enc(EN_ALGO)) != 0) {               /* ALGORITHM: the part; in a list, the value */
        if (ui.overlay == O_LIST) {
            turn_ref(list_rows[ui.list_sel], e, EN_ALGO);
        } else {
            int p = ui.part + (e > 0 ? 1 : -1);
            ui.part = (uint8_t)(p < 0 ? 0 : p >= NPARTS ? NPARTS - 1 : p);
            if (ui.view == V_GEN && !is_303())         /* TB-3PO is a 303's */
                set_view(V_PART);
            ui.outline_ok = 0;
            build_pages();
            if (ui.view == V_MIX) {                   /* MIX follows: a drum machine's tracks, else that part on PARTS */
                int k;
                ui.mixsel = ui.part;
                build_pages();
                for (k = 0; k < pg.n; k++)
                    if (is_drum() ? pg.title[k][0] == PART_N[ui.part][0] && pg.title[k][4] == 'M' : !k)
                        *page_of(V_MIX) = (uint8_t)k;
            }
        }
    }
    if ((e = enc(EN_PRESET)) != 0) {             /* PRESETS: a part's screen, that part; else all five */
        int part = part_view() ? ui.part : PART_909, c = seq_cue_of(&seq, part);
        int p = (c >= 0 ? c : seq.ppat[part]) + (e > 0 ? 1 : -1);
        p = p < 0 ? 0 : p >= NPAT ? NPAT - 1 : p;
        seq_chain(&seq, 0, 0);
        if (part_view())
            seq_cue_part(&seq, part, p);
        else
            seq_cue(&seq, p);
        ui.pbank = (uint8_t)(p / 16);                 /* HOME shows the bank it is in */
    }
    for (i = 0; i < 4; i++) {
        if ((e = enc(EN_K1 + (int)i)) == 0)
            continue;
        if (ui.view == V_PART && is_303() && ui.held_step >= 0) {     /* a held step's knobs edit it */
            bpart_t *bp = &cur_pat()->bass[bidx()];
            bstep_t *st = &bp->step[ui.held_step];
            ui.step_edited = 1;
            mark_dirty();
            if (i == 0) {                               /* a rest or a tie becomes a note of its own */
                int n = held_note_of(bp, ui.held_step) + e;
                st->note = (uint8_t)(n < 12 ? 12 : n > 108 ? 108 : n);
                if (bstep_gate(st) != G_NOTE)
                    st->flags = (uint8_t)((st->flags & ~BS_GATE_MASK) | G_NOTE);
                if (key_sounds()) {                     /* as the drum keys: playing, the line is heard */
                    engine_bass_on(bidx(), st->note + bp->transpose - 24, 0, 1);
                    ui.step_preview = 1;
                }
            } else if (i == 1) {
                st->flags = (uint8_t)((st->flags & ~BS_GATE_MASK) | (bstep_gate(st) + (e > 0 ? 1 : 2)) % 3);
            } else if (i == 2) {
                st->flags ^= BS_ACCENT;
            } else {
                st->flags ^= BS_SLIDE;
            }
            continue;
        }
        if (ui.btn & (1u << B_SAVE)) {                  /* SAVE + knob: forget that knob's motion */
            ui.btn_used |= 1u << B_SAVE;
            motion_forget(pg.r[cur_page()][i]);
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
#define BAND_H 69                     /* the main area: two bands of 69 (the knob row takes the 6 below) */
#define KNOB_Y 158
#define KNOB_H 82

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

/* the main area sends only the rectangle that changed: the panel has no tearing-effect line the
 * FM-1 can see, so a transfer the panel's refresh overtakes shows half old, half new. The whole
 * area is 69 KB, 18 ms at 30 MHz, longer than a refresh, so every playhead step sheared; the two
 * columns a step changes are ~3 ms. Rows and 8-pixel column blocks are hashed against the last
 * frame sent; the changed rectangle is packed to the front of the canvas (it is rebuilt every
 * frame) and sent as one transfer. blit_hash[region] == 0 (a reset) sends it all. */
#define MAIN_ROWS (2 * BAND_H)
#define MAIN_COLB (240 / 8)
static uint32_t main_rowh[MAIN_ROWS], main_colh[MAIN_COLB];
static void cv_commit_rect(int region, uint32_t x, uint32_t y)
{
    uint32_t rowh[MAIN_ROWS], colh[MAIN_COLB], r, c, r0 = MAIN_ROWS, r1 = 0, c0 = MAIN_COLB, c1 = 0, w, h, k;
    for (c = 0; c < MAIN_COLB; c++)
        colh[c] = 2166136261u;
    for (r = 0; r < MAIN_ROWS; r++) {
        const uint16_t *q = cv_px + r * 240u;
        uint32_t hr = 2166136261u;
        for (c = 0; c < MAIN_COLB; c++) {
            uint32_t hc = colh[c];
            for (k = 0; k < 8u; k++) {
                hr = (hr ^ q[k]) * 16777619u;
                hc = (hc ^ q[k]) * 16777619u;
            }
            colh[c] = hc;
            q += 8;
        }
        rowh[r] = hr;
    }
    if (!blit_hash[region]) {                         /* after a reset: all of it */
        r0 = c0 = 0;
        r1 = MAIN_ROWS - 1;
        c1 = MAIN_COLB - 1;
    } else {
        for (r = 0; r < MAIN_ROWS; r++)
            if (rowh[r] != main_rowh[r]) {
                if (r0 == MAIN_ROWS)
                    r0 = r;
                r1 = r;
            }
        for (c = 0; c < MAIN_COLB; c++)
            if (colh[c] != main_colh[c]) {
                if (c0 == MAIN_COLB)
                    c0 = c;
                c1 = c;
            }
        if (r0 == MAIN_ROWS || c0 == MAIN_COLB)
            return;                                     /* nothing changed */
    }
    blit_hash[region] = 1;
    for (r = 0; r < MAIN_ROWS; r++)
        main_rowh[r] = rowh[r];
    for (c = 0; c < MAIN_COLB; c++)
        main_colh[c] = colh[c];
    w = (c1 - c0 + 1u) * 8u;
    h = r1 - r0 + 1u;
    for (r = 0; r < h; r++) {                         /* pack: the destination never passes the source */
        const uint16_t *src = cv_px + (r0 + r) * 240u + c0 * 8u;
        uint16_t *dst = cv_px + r * w;
        for (k = 0; k < w; k++)
            dst[k] = src[k];
    }
    lcd_blit(x + c0 * 8u, y + r0, w, h, cv_px);
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
        uint32_t gi = ((uint32_t)'H' <= f->last ? 'H' : '0') - f->first, w = f->bw[gi], bpr = (w + 1u) / 2u, x, y;
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
    if ((uint32_t)x >= cv_w || y < cv_y0 || y >= cv_y1 || !a)
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

/* the length of the part on screen, when its keys are steps (0: they are not) */
static int steps_len(void)
{
    if (is_drum())
        return cur_pat()->drum[ui.part].len;
    if (is_303() && !ui.rec)
        return cur_pat()->bass[bidx()].len;
    return 0;
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
        else if (is_303() && ui.rec)
            x = cv_text(x, vc(&FONT_S, 1, 17), &FONT_S, "KEYS", C_AMB) + 6;
    } else {
        x = cv_text(4, vc(&FONT_B, 1, 17), &FONT_B, VIEW_N[ui.view], C_HI) + 6;
        if (drum_mix_page() >= 0)                     /* MIX: 909 / MIX: 808 (whose tracks these are) */
            x = cv_text(x - 6, vc(&FONT_B, 1, 17), &FONT_B, drum_mix_page() ? ": 808" : ": 909",
                        PART_COL[drum_mix_page()]) + 6;
    }
    {   /* the pattern of the part on screen (else the 909's), its cue; in SONG mode the bar */
        int part = part_view() ? ui.part : PART_909, cue = seq_cue_of(&seq, part);
        if (seq.song_on) {
            b[0] = 'S';
            put_i(b + 1, (seq.playing ? seq.song_pos : ui.song_sel) + 1);
            x = cv_text(132, vc(&FONT_B, 1, 17), &FONT_B, b, seq.song_rec ? RGB(255, 90, 80) : C_AMB);
        } else {
            b[0] = 'P';
            put_i(b + 1, seq.ppat[part] + 1);
            x = cv_text(132, vc(&FONT_B, 1, 17), &FONT_B, b, C_WHITE);
        }
        if (cue >= 0 && cue != seq.ppat[part] && !seq.song_on) {
            b[0] = '>';
            put_i(b + 1, cue + 1);
            cv_text(x + 2, vc(&FONT_S, 1, 17), &FONT_S, b, (ui.frame & 16u) ? C_WHITE : C_GRAY);
        } else if (seq.chain_a != seq.chain_b && !seq.song_on) {
            cv_text(x + 3, vc(&FONT_XS, 1, 17), &FONT_XS, "CHN", C_AMB);
        } else if (ui.view == V_PART && steps_len() > 16) {   /* the 16 steps the keys show */
            put_i(put_s(put_i(b, ui.spage * 16 + 1), "-"), ui.spage * 16 + 16);
            cv_text(x + 4, vc(&FONT_XS, 1, 17), &FONT_XS, b, C_GRAY);
        }
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

#define DROW 11                                    /* drum rows: 12 of them over both bands, a gap under */
static void draw_drum(int band)
{
    int k = ui.part, v, c, len = cur_pat()->drum[k].len;
    const dpart_t *d = &cur_pat()->drum[k];
    int ph = seq.playing ? eng_step[TRK_DRUM + k] : -1;
    uint16_t col = part_col(), on_dim = dim(col, 8);
    for (v = 0; v <= NDRUM; v++) {
        int y = v * DROW + 1 - band * BAND_H, sel = v < NDRUM && v == ui.sel[k];
        const char *nm = v < NDRUM ? engine_voice_name(k == PART_909 ? T_909 : T_808, v) : "AC";
        const uint32_t *bits = v < NDRUM ? d->hit[v] : d->accent;
        int muted = v < NDRUM && ((seq.mute | seq.vmute) & (1u << (k * NDRUM + v)));
        if (y < -DROW || y >= BAND_H)
            continue;
        if (sel)
            box(0, y - 1, 24, DROW, col);
        cv_text(5, vc(&FONT_XS, y - 1, DROW - 1), &FONT_XS, nm, sel ? C_BLACK : muted ? C_LINE : v < NDRUM ? C_GRAY : C_WHITE);
        for (c = 0; c < 16; c++) {
            int s = step_of(c), x = col_x(c), hit = s < NSTEPS && sm_get(bits, s);
            if (s >= len) {
                cv_rect(x + 5, y + 3, 2, 2, C_LINE);
                continue;
            }
            if (v == NDRUM) {                          /* the accent row: white marks */
                if (hit)
                    box(x + 2, y + 1, 8, 6, C_WHITE);
                else
                    rbox(x + 2, y + 3, 8, 2, 1, C_LINE);
                continue;
            }
            box(x, y, 12, DROW - 2, hit ? (muted ? C_DIM : sel ? col : on_dim) : (s == ph ? C_DIM : C_LINE));
            if (hit && s == ph)
                box(x + 2, y + 2, 8, DROW - 6, C_WHITE);
        }
    }
}

#define ROLL_TOP 4                                 /* the 303's line: from here (both bands) ... */
#define ROLL_BOT 92                                /* ... to here; then AC, SL and the step's line */
static int note_y(int n, int lo, int hi) { return ROLL_BOT - (n - lo) * (ROLL_BOT - ROLL_TOP) / (hi - lo); }

/* the note step s sounds: a TIE holds the note of the NOTE it follows (its own is not played) */
static int held_note_of(const bpart_t *bp, int s)
{
    int k;
    for (k = 0; k < bp->len && bstep_gate(&bp->step[s]) == G_TIE; k++)
        s = s ? s - 1 : bp->len - 1;
    return bp->step[s].note;
}

static void draw_303(int band, int gen)
{
    int b = bidx(), c, o = band * BAND_H, n;       /* o: drawn in the screen's rows, minus the band's */
    const bpart_t *bp = &cur_pat()->bass[b];
    int ph = seq.playing ? eng_step[TRK_BASS0 + b] : -1, lo = 127, hi = 0;
    uint16_t col = part_col();
    char t[40], *q = t;
    (void)gen;
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
    for (n = lo; n <= hi; n++)                     /* the line: pitch by height, the root's octaves ruled */
        if ((n - bp->gen.root) % 12 == 0) {
            int y = note_y(n, lo, hi) - o;
            put_note(t, n);
            cv_rect(GX, y + 2, 213, 1, C_LINE);
            cv_text(1, y - 4, &FONT_XS, t, C_DIM);
        }
    for (c = 0; c < 16; c++) {
        int s = step_of(c), x = col_x(c), g;
        const bstep_t *st = &bp->step[s < NSTEPS ? s : 0];
        if (s >= bp->len) {
            cv_rect(x + 5, ROLL_BOT + 4 - o, 2, 2, C_LINE);
            continue;
        }
        g = bstep_gate(st);
        if (s == ph)
            cv_rect(x, ROLL_TOP - 4 - o, 12, ROLL_BOT - ROLL_TOP + 10, dim(col, 2));
        if (g != G_REST) {
            int hn = held_note_of(bp, s), y = note_y(hn, lo, hi) - o;
            uint16_t fc = s == ui.held_step ? C_WHITE : (st->flags & BS_ACCENT) ? col : dim(col, 9);
            box(x, y, 12, 5, fc);
            if (g == G_TIE && c > 0)                 /* a tie: one brick with the note it holds */
                cv_rect(col_x(c - 1) + 6, y, x - col_x(c - 1), 5, fc);
            if ((st->flags & BS_SLIDE) && c < 15) {
                int nx = s + 1 < bp->len ? s + 1 : 0, ny = note_y(bp->step[nx].note, lo, hi) - o;
                cv_line(x + 11, y + 2, x + 14, ny + 2, C_WHITE);
            }
        }
        rbox(x + 1, 101 - o, 10, 4, 2, g && (st->flags & BS_ACCENT) ? C_WHITE : C_LINE);   /* its accent, its slide */
        rbox(x + 1, 113 - o, 10, 4, 2, g && (st->flags & BS_SLIDE) ? col : C_LINE);
    }
    cv_text(3, vc(&FONT_XS, 99, 8) - o, &FONT_XS, "AC", C_GRAY);
    cv_text(3, vc(&FONT_XS, 111, 8) - o, &FONT_XS, "SL", C_GRAY);
    if (ui.held_step >= 0 || ui.rec) {   /* the step held (or being written) */
        int s = ui.held_step >= 0 ? ui.held_step : ui.wpos[b];
        const bstep_t *st = &bp->step[s];
        q = put_s(q, ui.held_step >= 0 ? "STEP " : "WRITE ");
        q = put_i(q, s + 1);
        q = put_s(q, "  ");
        q = put_note(q, held_note_of(bp, s) + bp->transpose - 24);
        if (bstep_gate(st) == G_TIE)
            q = put_s(q, " TIE");
        if (st->flags & BS_ACCENT)
            q = put_s(q, " ACC");
        if (st->flags & BS_SLIDE)
            put_s(q, " SLIDE");
        cv_text(4, vc(&FONT_B, 122, 16) - o, &FONT_B, t, C_WHITE);
    } else {                                           /* else the line's scale and seed */
        char t2[16];
        q = put_s(q, ROOT_N[bp->gen.root % 12]);
        q = put_s(q, " ");
        put_s(q, TB3PO_SCALE_NAMES[bp->gen.scale % TB3PO_NSCALES]);
        put_hex(put_s(t2, "SEED "), bp->gen.seed & 0xFFFFu, 4);
        segs(4, vc(&FONT_S, 122, 16) - o, &FONT_S, C_GRAY, 18, t, t2, 0, 0);
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
            if (bp->slice[c]) {                        /* the step's own slice */
                char d[2] = {(char)('0' + bp->slice[c]), 0};
                text_c(x + 6, vc(&FONT_XS, 2, 10), &FONT_XS, d, on ? C_BLACK : C_GRAY);
            }
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
    }
}

/* a part's lane on HOME: the 16 steps of the page its playhead is on (bits: those 16, from base) */
static void lane(int y, int p, uint32_t bits, int ph, int len, int base)
{
    int c, muted = part_muted(p);
    cv_text(3, y, &FONT_XS, PART_S[p], muted ? C_DIM : PART_COL[p]);
    for (c = 0; c < 16 && base + c < len; c++) {
        uint16_t fc = (bits >> c & 1u) ? (muted ? C_DIM : PART_COL[p]) : C_LINE;
        box(col_x(c), y + 2, 12, 9, fc);
        if (seq.playing && (ph & 15) == c)
            rbox(col_x(c) + 2, y + 12, 8, 2, 1, C_WHITE);
    }
}

static void draw_home(int band)
{
    int p, c;
    if (band == 0) {                                   /* the 16 patterns; in each, a bar per part playing it */
        int cue = seq_cue_of(&seq, PART_909), c0 = ui.pbank * 16;
        for (c = c0; c < c0 + 16; c++) {
            int x = 4 + (c - c0) * 14 + (c - c0 >= 8 ? 4 : 0);
            int in_chain = seq.chain_a != seq.chain_b &&
                           c >= (seq.chain_a < seq.chain_b ? seq.chain_a : seq.chain_b) &&
                           c <= (seq.chain_a < seq.chain_b ? seq.chain_b : seq.chain_a);
            box(x, 3, 12, 18, pattern_used(&proj.pat[c]) ? C_DIM : C_LINE);
            for (p = 0; p < NPARTS; p++)                 /* the parts playing it: solid if they have notes here */
                if (seq.ppat[p] == c)
                    rbox(x + 2, 5 + p * 3, 8, 2, 1, part_used(&proj.pat[c], p) ? PART_COL[p] : dim(PART_COL[p], 5));
            if (c == cue && c != seq.ppat[PART_909] && (ui.frame & 16u))
                frame(x - 1, 2, 14, 20, C_WHITE);
            if (in_chain)
                cv_rect(x, 30, 12, 2, C_AMB);
            if ((c & 3) == 0) {                         /* every fourth: its number (and so the bank) */
                char t[4];
                put_i(t, c + 1);
                cv_text(x + 1, 21, &FONT_XS, t, ui.pbank ? C_AMB : C_GRAY);
            }
        }
        for (p = 0; p < 2; p++) {
            const dpart_t *d = &pat_of(p)->drum[p];
            uint32_t bits = 0;
            int v, base = seq.playing ? (eng_step[TRK_DRUM + p] & ~15) : 0;
            for (v = 0; v < NDRUM; v++)
                bits |= d->hit[v][base >> 5] >> (base & 31);
            lane(32 + p * 16, p, bits & 0xFFFFu, eng_step[TRK_DRUM + p], d->len, base);
        }
    } else {
        for (p = 2; p < NPARTS; p++) {
            uint32_t bits = 0;
            int s;
            if (p < PART_BRK) {
                const bpart_t *bp = &pat_of(p)->bass[p - NKIT];
                int base = seq.playing ? (eng_step[TRK_BASS0 + p - NKIT] & ~15) : 0;
                for (s = 0; s < 16; s++)
                    if (bstep_gate(&bp->step[base + s]) != G_REST)
                        bits |= 1u << s;
                lane((p - 2) * 16 + 1, p, bits, eng_step[TRK_BASS0 + p - NKIT], bp->len, base);
            } else {
                lane((p - 2) * 16 + 1, p, pat_of(p)->brk.steps, eng_step[TRK_BRK], 16, 0);
            }
        }
    }
}

/* SONG: a table of bars, twelve rows across the two bands (the first is the heading) */
static void draw_song(int band)
{
    const song_t *sg = song();
    int r, p, top = ui.song_sel > 5 ? ui.song_sel - 5 : 0;
    for (r = 0; r < 6; r++) {
        int row = band * 6 + r, y = r * 11, k = top + row - 1;
        char t[8];
        if (row == 0) {                                /* the heading */
            cv_text(4, vc(&FONT_XS, y, 11), &FONT_XS, "BAR", C_GRAY);
            for (p = 0; p < NPARTS; p++)
                text_c(58 + p * 36, vc(&FONT_XS, y, 11), &FONT_XS, PART_S[p], PART_COL[p]);
            continue;
        }
        if (k > sg->len || k >= NSONG)
            break;
        if (k == ui.song_sel)
            box(0, y, 236, 11, C_LINE);
        put_i(t, k + 1);
        cv_text(4, vc(&FONT_XS, y, 11), &FONT_XS, t, k == ui.song_sel ? C_WHITE : C_GRAY);
        if (seq.song_on && seq.playing && k == seq.song_pos)
            play_icon(26, y + 1, seq.song_rec ? RGB(255, 80, 70) : C_WHITE);
        if (k == sg->len) {                            /* one past the end: where the song grows */
            cv_text(50, vc(&FONT_XS, y, 11), &FONT_XS, k ? "END" : "EMPTY: WHITE KEYS ADD BARS", C_DIM);
            continue;
        }
        for (p = 0; p < NPARTS; p++) {
            int muted = sg->bar[k].mute >> p & 1u;
            t[0] = 'P';
            put_i(t + 1, sg->bar[k].pat[p] + 1);
            text_c(58 + p * 36, vc(&FONT_XS, y, 11), &FONT_XS, muted ? "-" : t, muted ? C_DIM : PART_COL[p]);
        }
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
    }
}

/* MIX, a drum machine's page: its eleven tracks' levels (and pans), the selected one lit */
static void draw_drum_mix(int band, int k)
{
    int t = k ? T_808 : T_909, v, o = band * BAND_H;
    uint16_t col = PART_COL[k];
    for (v = 0; v < NDRUM; v++) {
        int x = 4 + v * 21, sel = v == ui.sel[k];
        pref_t lv = find_ref(t, v, "Level"), pn = find_ref(t, v, "Pan");
        int l = lv.kind ? proj.sound.v[t][v][lv.c] : 0, pa = pn.kind ? proj.sound.v[t][v][pn.c] : 64;
        int muted = (seq.mute | seq.vmute) & (1u << (k * NDRUM + v)), fh = 96, f = l * fh / 127;
        if (sel)
            box(x, 0 - o, 19, 134, dim(col, 3));
        rbox(x + 7, 4 - o, 5, fh, 2, C_LINE);
        if (f)
            rbox(x + 7, 4 + fh - f - o, 5, f, 2, muted ? C_DIM : sel ? col : dim(col, 9));
        text_c(x + 10, vc(&FONT_XS, 104, 12) - o, &FONT_XS, engine_voice_name(t, v), sel ? C_WHITE : muted ? C_LINE : C_GRAY);
        rbox(x + 3, 123 - o, 13, 3, 1, C_LINE);          /* the pan, a mark from the middle */
        rbox(x + 3 + (pa * 10) / 127, 121 - o, 3, 7, 1, sel ? C_WHITE : C_GRAY);
    }
}

static void draw_mix(int band)
{
    int p;
    if (drum_mix_page() >= 0) {
        draw_drum_mix(band, drum_mix_page());
        return;
    }
    if (band == 0) {                                   /* channel strips + the master's gain reduction */
        for (p = 0; p < NPARTS; p++) {
            int x = 6 + p * 38, lvl = proj.sound.v[T_MIX][p][0], pk = eng_peak[p] * 64 / 32768;
            if (cur_page() == 0 && p == ui.mixsel)       /* PARTS: the part the knobs set */
                box(x - 2, 0, 30, BAND_H, dim(PART_COL[p], 3));
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
        if (engine_guard_active())                    /* the overload guard is saving the CPU */
            cv_text(100, 50, &FONT_XS, "GUARD", C_AMB);
    }
}

/* PERF: the live window, or the test's progress and table */
static char *put_pct10(char *q, int v)                /* 123 -> "12.3%" */
{
    q = put_i(q, v / 10);
    *q++ = '.';
    q = put_i(q, v % 10);
    return put_s(q, "%");
}
static void draw_perf(int band)
{
    static const char *const SN[ENG_PROF_N] = {"909", "808", "303A", "303B", "BREAK", "FX", "MASTER", "SEQ"};
    char t[40], *q;
    int k;
    if (band == 0) {
        q = put_s(t, "CLOCK ");
        if (perf.mhz10) {
            q = put_i(q, perf.mhz10 / 10);
            put_s(q, " MHz");
        } else
            put_s(q, plat_cycles_cpu() ? "..." : "? (NO CYCLE COUNTER)");
        cv_text(4, 2, &FONT_B, t, C_WHITE);
        if (perf.test >= 0) {
            int done = perf.phase == 4;
            q = put_s(t, done ? "PERF TEST DONE" : "PERF TEST ");
            if (!done) {
                q = put_i(q, perf.test + 1);
                q = put_s(q, "/3  ");
                put_s(q, PT_N[perf.test < 3 ? perf.test : 2]);
            }
            cv_text(4, 24, &FONT_S, t, done ? C_HI : C_AMB);
            if (!done)
                cv_text(4, 46, &FONT_XS, "HOME OR PLAY: STOP", C_DIM);
            else
                cv_text(4, 46, &FONT_XS, "SHARE OF THE CPU (PEAK)  CYCLES / SAMPLE", C_DIM);
            return;
        }
        q = put_s(t, "LOAD ");
        q = put_i(q, perf.load);
        q = put_s(q, "%  PEAK ");
        q = put_i(q, perf.peak);
        q = put_s(q, "%  DROPOUTS ");
        put_i(q, (int)plat_xruns());
        cv_text(4, 24, &FONT_S, t, C_HI);
        if (perf.stalls_on) {
            q = put_s(t, "STALLS  FETCH ");
            q = put_i(q, perf.stall[0]);
            q = put_s(q, "%  READ ");
            q = put_i(q, perf.stall[1]);
            q = put_s(q, "%  WRITE ");
            put_s(put_i(q, perf.stall[2]), "%");
        } else
            put_s(t, "STALLS OFF (KNOB 1)");
        cv_text(4, 46, &FONT_XS, t, perf.stalls_on ? C_GRAY : C_DIM);
        if (!perf.stalls_on && plat_irq_stack_size()) {   /* the interrupts' 8 KiB: the deepest so far */
            q = put_s(t, "IRQ STACK ");
            q = put_i(q, (int)plat_irq_stack_used());
            q = put_s(q, " / ");
            put_s(put_i(q, (int)plat_irq_stack_size()), " B");
            text_r(236, 46, &FONT_XS, t, plat_irq_stack_used() * 4u > plat_irq_stack_size() * 3u ? RGB(255, 80, 60) : C_GRAY);
        }
        return;
    }
    if (perf.test >= 0) {                             /* the table, so far */
        for (k = 0; k < 3; k++) {
            int y = k * 14 + 2;
            cv_text(4, y, &FONT_S, PT_N[k], perf.test == k && perf.phase != 4 ? C_AMB : C_GRAY);
            if (!perf.res_load[k])
                continue;
            q = put_pct10(t, perf.res_load[k]);
            q = put_s(q, " (");
            q = put_i(q, perf.res_peak[k]);
            put_s(q, "%)");
            cv_text(112, y, &FONT_S, t, C_WHITE);
            if (perf.res_cps[k]) {
                put_i(t, perf.res_cps[k]);
                text_r(236, y, &FONT_S, t, C_HI);
            }
        }
        if (perf.phase == 4)
            cv_text(4, 50, &FONT_XS, "SEL: RUN IT AGAIN", C_DIM);
        if (plat_irq_stack_size()) {                 /* the interrupts' stack, the deepest so far (after the test too) */
            q = put_s(t, "IRQ STACK ");
            q = put_i(q, (int)plat_irq_stack_used());
            q = put_s(q, " / ");
            put_s(put_i(q, (int)plat_irq_stack_size()), " B");
            text_r(236, 50, &FONT_XS, t, plat_irq_stack_used() * 4u > plat_irq_stack_size() * 3u ? RGB(255, 80, 60) : C_GRAY);
        }
        return;
    }
    for (k = 0; k < ENG_PROF_N; k++) {                /* each stage's share of the CPU */
        int x = (k & 1) * 118 + 4, y = (k >> 1) * 12;
        cv_text(x, y, &FONT_XS, SN[k], C_GRAY);
        put_pct10(t, perf.stage[k]);
        text_r(x + 108, y, &FONT_XS, t, C_WHITE);
    }
    if (perf.cps) {
        q = put_s(t, "RENDER ");
        q = put_i(q, perf.cps);
        put_s(q, " CYCLES / SAMPLE");
        cv_text(4, 44, &FONT_XS, t, C_GRAY);
    }
    q = put_s(t, engine_guard_active() ? "GUARD ON  " : "GUARD ");   /* the overload guard: engaged, times */
    put_s(put_i(q, (int)eng_guard_count), "x");
    text_r(236, 55, &FONT_XS, t, engine_guard_active() ? C_AMB : C_DIM);
    cv_text(4, 55, &FONT_XS, "SEL: RUN PERF TEST", C_DIM);
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
        nm = caps_of(pref_name(r));
        static char t[24];
        if (track_page()) {                              /* a track's own: "BD  Tune" */
            put_s(put_s(put_s(t, engine_voice_name(ui.part == PART_909 ? T_909 : T_808, ui.sel[ui.part])), "  "), nm);
            nm = t;
        } else if (in_pattern(r)) {                      /* the pattern's: "P3  LENGTH" (it changes with it) */
            int pp = r.kind == R_DLEN || r.kind == R_DRATE ? r.a : r.kind >= R_BLEN && r.kind <= R_BTRANS ? NKIT + r.a
                     : r.kind == R_SWING ? PART_909 : part_view() ? ui.part : PART_909;
            put_s(put_s(put_i(put_s(t, "P"), seq.ppat[pp] + 1), "  "), nm);
            nm = t;
        }
    }
    pref_value(r, num, unit);
    {   /* one row: the name on the left, the value (+ unit) on the right, both on the box's middle */
        const int32_t bx = 8, by = 14, bw = 224, bh = 50;
        const felucca_font_t *vf = &FONT_L;           /* the big face has numbers only: a name in the next size */
        int32_t ly, uw = unit[0] ? tw(&FONT_S, unit) + 3 : 0;
        const char *q;
        for (q = num; *q; q++)
            if ((uint8_t)*q > '9')
                vf = &FONT_M;
        ly = vc(vf, by, bh);
        box(bx, by, bw, bh, C_BLACK);
        frame(bx, by, bw, bh, part_col());
        cv_text(bx + 12, vc(&FONT_B, by, bh), &FONT_B, nm, C_GRAY);
        w = tw(vf, num);
        x = bx + bw - 12 - uw - w;
        cv_text(x, ly, vf, num, C_WHITE);
        if (unit[0])
            cv_text(bx + bw - 12 - uw + 3, base_y(vf, ly, &FONT_S), &FONT_S, unit, C_GRAY);
    }
}

/* a list over the main area: six rows of 24 px */
static void draw_list(int band)
{
    int i;
    for (i = 0; i < 3; i++) {
        int row = ui.list_top + band * 3 + i, y = i * 23, sel = row == ui.list_sel;
        pref_t r;
        char num[16], unit[8];
        const x0x_param_t *d;
        if (row >= ui.list_n)
            break;
        r = list_rows[row];
        if (r.kind == R_HDR) {                         /* a section: whose values follow */
            char t[28];
            hdr_name(r.a, t);
            cv_text(8, vc(&FONT_XS, y + 4, 16), &FONT_XS, t, C_AMB);
            cv_rect(8, y + 19, 220, 1, C_LINE);
            continue;
        }
        d = pref_desc(r);
        if (sel)
            box(0, y + 1, 232, 21, dim(part_col(), 6));
        cv_text(8, vc(sel ? &FONT_B : &FONT_S, y + 1, 18), sel ? &FONT_B : &FONT_S, caps_of(pref_name(r)), sel ? C_WHITE : C_GRAY);
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
        int h = 2 * BAND_H * 6 / ui.list_n, y = 2 * BAND_H * ui.list_top / ui.list_n - band * BAND_H;
        rbox(235, 0, 3, BAND_H, 1, C_LINE);
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

static void draw_help(int band)
{
    const char *l[HELP_LINES];
    int n = help_lines(ui.help - 1, l), k;
    uint16_t c = part_col();
    if (band == 0) {
        box(6, 4, 228, 24, c);
        text_c(120, vc(&FONT_B, 4, 24), &FONT_B, BTN_LABEL[ui.help - 1], C_BLACK);
    }
    for (k = 0; k < n; k++) {                            /* evenly spaced; a line across the bands in both */
        int32_t y = 34 + k * 18 - band * BAND_H;
        if (y > -18 && y < BAND_H)
            text_c(120, y, &FONT_XS, l[k], C_WHITE);
    }
}

#ifdef X0X_HOST
/* every card, on every kind of screen, fits the screen: 1 = yes (prints the first that does not) */
static int help_fits(void)
{
    static const uint8_t views[] = {V_PART, V_HOME, V_SONG, V_GEN, V_FX, V_MIX};
    const char *l[HELP_LINES];
    ui_t keep = ui;
    uint8_t keep_any = any_button;
    int b, p, v, kb, n, k, ok = 1;
    for (v = 0; v < (int)sizeof views; v++)
        for (p = 0; p < NPARTS; p++)
            for (kb = 0; kb < 2; kb++)
                for (b = 0; b < NB; b++) {
                    ui.view = views[v];
                    ui.part = (uint8_t)p;
                    ui.rec = (uint8_t)kb;
                    {
                        const char *fa, *fb, *fc;
                        int32_t w;
                        ui.overlay = (uint8_t)(b % 3);           /* none, a list, a question */
                        ui.gen_stale[0] = (uint8_t)(b & 1);
                        ui.rec = (uint8_t)(b & 2);
                        any_button = (uint8_t)(b != 0);
                        footer_text(&fa, &fb, &fc);
                        w = 4 + tw(&FONT_T, fa) + (fb ? FOOT_GAP + tw(&FONT_T, fb) : 0) + (fc ? FOOT_GAP + tw(&FONT_T, fc) : 0);
                        if (w > 236) {
                            if (ok)
                                printf("footer too wide: \"%s / %s / %s\" (%d px)\n", fa, fb ? fb : "", fc ? fc : "", (int)w);
                            ok = 0;
                        }
                    }
                    n = help_lines(b, l);
                    if (n < 1 || n > HELP_LINES)
                        ok = 0;
                    for (k = 0; k < n; k++)
                        if (tw(&FONT_XS, l[k]) > 224) {
                            if (ok)
                                printf("help card too wide: %s: \"%s\" (%d px)\n", BTN_LABEL[b], l[k], (int)tw(&FONT_XS, l[k]));
                            ok = 0;
                        }
                }
    ui = keep;
    any_button = keep_any;
    return ok;
}
#endif

/* the footer: one line at the bottom of the main area, what the keys do on this screen (until the
 * first button, how to find out what any of them does) */
static uint16_t footer_text(const char **a, const char **b, const char **c)
{
    *a = *b = *c = 0;
    if (!any_button) {
        *a = "HOLD ANY BUTTON: WHAT IT DOES";
        return C_GRAY;
    }
    if (ui.overlay == O_ASK) {
        *a = "SEL: YES";
        *b = "HOME: NO";
    } else if (ui.overlay == O_LIST) {
        *a = "SELECT: MOVE";
        *b = "ALGO: CHANGE";
        *c = "HOME: BACK";
    } else if (ui.view == V_HOME) {
        *a = "WHITE: PATTERN";
        *b = "TWO: CHAIN";
        *c = "BLACK: MUTE";
    } else if (ui.view == V_SONG) {
        *a = "WHITE: THE BAR'S PATTERN";
        *b = "BLACK: MUTE";
    } else if (ui.view == V_MIX && cur_page() == 0) {
        *a = "BLACK 1-5: PART";
        *b = "AGAIN: NEXT PAGE";
    } else if (drum_mix_page() >= 0) {
        *a = "BLACK: TRACK";
        *b = "AGAIN: NEXT PAGE";
    } else if (ui.view == V_FX || ui.view == V_MIX) {
        *a = "AGAIN: NEXT PAGE";
        *b = "SEL: ALL AS A LIST";
    } else if (ui.view == V_GEN) {
        *a = ui.gen_stale[bidx()] ? "OCT+: NEW LINE *" : "OCT+: NEW LINE";
        *b = "OCT-: MUTATE";
        return ui.gen_stale[bidx()] ? C_WHITE : C_AMB;
    } else if (is_drum()) {
        *a = "BLACK: TRACK";
        *b = "WHITE: STEP";
        *c = "HOME+BLK: MUTE";
    } else if (is_303() && ui.rec) {
        *a = ui.rec ? "KEYS WRITE STEPS" : "KEYS PLAY";
        *b = ui.rec ? "ENV: REST" : "SEQ: STEPS";
        *c = ui.rec ? "LFO+OCT: TIE" : 0;
    } else if (is_303()) {
        *a = "PRESS: ON";
        *b = "TAP: OFF";
        *c = "HOLD + KNOBS: EDIT";
    } else {
        *a = "STEP + 1-8: PIN A SLICE";                /* (1-8 play the slices numbered above) */
        *b = "9 REV 10 HALF 11 STUT";
    }
    return C_DIM;
}
#define FOOT_Y 71                     /* in the knob row: its last line, in the tiny face */
static void draw_footer(void)
{
    const char *a, *b, *c;
    uint16_t col = footer_text(&a, &b, &c);
    segs(4, vc(&FONT_T, FOOT_Y, 9), &FONT_T, col, FOOT_GAP, a, b, c, 0);
}

static void draw_main(void)
{
    int band, readout = ui.touched >= 0 && (int32_t)(ui.touch_until - plat_ms()) > 0 && ui.overlay == O_NONE;
    if (!readout)
        ui.touched = -1;
    cv_begin(240, 2 * BAND_H, C_BLACK);           /* both bands, then one transfer: no tear between them */
    for (band = 0; band < 2; band++) {
        cv_band(band * BAND_H, BAND_H);
        if (ui.help)
            draw_help(band);
        else if (ui.overlay == O_ASK)
            draw_ask(band);
        else if (ui.overlay == O_LIST)
            draw_list(band);
        else if (ui.view == V_HOME)
            draw_home(band);
        else if (ui.view == V_FX)
            draw_fx(band);
        else if (ui.view == V_MIX)
            draw_mix(band);
        else if (ui.view == V_SONG)
            draw_song(band);
        else if (ui.view == V_PERF)
            draw_perf(band);
        else if (is_drum())
            draw_drum(band);
        else if (is_303())
            draw_303(band, ui.view == V_GEN);
        else
            draw_break(band);
        if (band == 1 && readout)
            draw_readout();
    }
    cv_band(0, 2 * BAND_H);
    cv_commit_rect(1, 0, MAIN_Y);
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
    MIXIN(seq.ppat[ui.part]);
    MIXIN(ui.song_sel);
    {
        const char *fa, *fb, *fc;
        MIXIN(footer_text(&fa, &fb, &fc));
        MIXIN((uintptr_t)fa ^ ((uintptr_t)fb << 3) ^ ((uintptr_t)fc << 7));
        MIXIN(pg.n);
    }
    for (i = 0; i < 4; i++) {
        pref_t r = pg.r[pgi][i];
        MIXIN(r.kind | r.a << 8 | r.b << 16 | (uint32_t)r.c << 24);
        MIXIN(pref_get(r));
        MIXIN(has_motion(r) | (motion_now(r) + 1) << 1);
    }
    if (ui.held_step >= 0) {
        const bstep_t *st = &cur_pat()->bass[bidx()].step[ui.held_step];
        MIXIN(st->note | st->flags << 8);
    }
#undef MIXIN
    return h;
}

static int strcmp_s(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a != *b;
}

/* the knob row's tag: whose knobs these are (BD, 909 KIT, P3 PATTERN, 909 MIX: BD, 303A STEP 5) */
static void knob_tag(char *t)
{
    int g = pg.group[cur_page()], dm = drum_mix_page();
    const char *ti = pg.title[cur_page()];
    if (ui.view == V_PART && is_303() && ui.held_step >= 0)
        put_i(put_s(put_s(t, PART_N[ui.part]), " STEP "), ui.held_step + 1);
    else if (dm >= 0)
        put_s(put_s(put_s(t, ti), ": "), engine_voice_name(dm ? T_808 : T_909, ui.sel[dm]));
    else if (ui.view == V_MIX && cur_page() == 0)
        put_s(put_s(t, "PARTS: "), PART_N[ui.mixsel]);
    else if (g == GR_PAT)
        put_s(put_s(put_i(put_s(t, "P"), seq.ppat[ui.part] + 1), " "), ti);
    else if (g == GR_KIT || g == GR_SOUND)
        put_s(put_s(put_s(t, PART_N[ui.part]), " "), ti);
    else
        put_s(t, ti);
    {   /* a section over several pages (same name): which of them, "BD (1/2)" */
        int p = cur_page(), a = p, b = p;
        while (a > 0 && !strcmp_s(pg.title[a - 1], ti))
            a--;
        while (b + 1 < pg.n && !strcmp_s(pg.title[b + 1], ti))
            b++;
        if (b > a) {
            char *q = t;
            while (*q)
                q++;
            put_s(put_i(put_s(put_i(put_s(q, " ("), p - a + 1), "/"), b - a + 1), ")");
        }
    }
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
    {   /* the rule; at its right end, where you are: the tag, then the page dots */
        char t[28];
        int dots = pg.n > 1 && ui.view != V_GLO;
        int32_t xd = 237 - (dots ? pg.n * 5 : 0), xt;
        knob_tag(t);
        xt = xd - (dots ? 5 : 0) - tw(&FONT_XS, t);
        cv_rect(0, 5, xt - 5, 1, C_LINE);
        cv_text(xt, vc(&FONT_XS, 0, 11), &FONT_XS, t, C_HI);
        if (dots) {
            int k;
            for (k = 0; k < pg.n; k++)
                cv_rect(xd + k * 5, 4, 3, 3, k == cur_page() ? C_HI : C_LINE);
        }
    }
    draw_footer();
    if (ui.view == V_PART && is_303() && ui.held_step >= 0) {     /* the held step's own knobs */
        static const char *const SN[4] = {"NOTE", "GATE", "ACCENT", "SLIDE"};
        static const char *const G[3] = {"REST", "NOTE", "TIE"};
        const bpart_t *bp = &cur_pat()->bass[bidx()];
        const bstep_t *st = &bp->step[ui.held_step];
        for (i = 0; i < 4; i++) {
            char v[12];
            int cx = i * 60 + 30;
            if (i)
                cv_rect(i * 60, 14, 1, 50, C_LINE);
            if (i == 0)
                put_note(v, held_note_of(bp, ui.held_step) + bp->transpose - 24);
            else if (i == 1)
                put_s(v, G[bstep_gate(st)]);
            else
                put_s(v, (st->flags & (i == 2 ? BS_ACCENT : BS_SLIDE)) ? "ON" : "OFF");
            text_c(cx, vc(&FONT_XS, 13, 10), &FONT_XS, SN[i], C_GRAY);
            text_c(cx, vc(&FONT_M, 30, 34), &FONT_M, v, C_WHITE);
        }
        cv_commit(3, 0, KNOB_Y);
        return;
    }
    for (i = 0; i < 4; i++) {
        pref_t r = pg.r[pgi][i];
        const x0x_param_t *d = pref_desc(r);
        char num[16], unit[8];
        int cx = i * 60 + 30, val, touched = ui.touched == i;
        uint16_t col = drum_mix_page() >= 0 ? PART_COL[drum_mix_page()] : part_col();   /* MIX: 909 in the 909's */
        if (i)
            cv_rect(i * 60, 14, 1, 50, C_LINE);
        if (!d)
            continue;
        if (touched)
            box(i * 60 + 2, 11, 56, 56, dim(col, 4));
        text_c(cx, vc(&FONT_XS, 13, 10), &FONT_XS, caps_of(pref_name(r)), touched ? C_WHITE : C_GRAY);
        val = pref_get(r);
        if (has_motion(r)) {                           /* recorded motion: a mark, and what it plays now */
            int mv = motion_now(r);
            dot(i * 60 + 52, 17, 2, mv >= 0 ? C_WHITE : C_DIM);
            if (mv >= 0)
                val = mv;
        }
        if (d->names && d->max < 12) {                 /* a switch: its positions as pips (more: a ring) */
            int n = d->max + 1, k, pw = n > 6 ? 3 : 6, gap = 2, w0 = n * (pw + gap) - gap;
            for (k = 0; k < n; k++)
                box(cx - w0 / 2 + k * (pw + gap), 33, pw, 9, k == val ? col : C_LINE);
        } else {
            arc(cx, 38, 12, d->max ? (float)val / (float)d->max : 0.0f, C_LINE, col,
                r.kind == R_BTRANS || is_pan(r) || (r.kind == R_ENG && r.a == T_MST && r.c == MST_DJF));   /* from the middle */
        }
        pref_value_of(r, val, num, unit);
        cell_value(cx, vc(&FONT_B, 53, 13), num, unit, touched ? C_WHITE : C_HI);
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
    if (ui.view == V_SONG || (seq.song_on && blink))
        b |= 1u << B_SEQ;
    if (ui.overlay != O_NONE)
        b |= 1u << B_SEL;
    if (is_303() && ui.rec)
        b |= 1u << B_SEQ;
    if (ui.view == V_HOME || ui.view == V_SONG ? ui.pbank : ui.spage)
        b |= 1u << B_OCTUP;
    if (ui.dirty && blink && ui.view == V_GLO)
        b |= 1u << B_SAVE;
    if (proj.set.keyled) {
        if (ui.view == V_HOME) {
            int pp = seq.ppat[PART_909] - ui.pbank * 16, cu = seq_cue_of(&seq, PART_909) - ui.pbank * 16;
            if (pp >= 0 && pp < 16)
                k |= 1u << WHITE_KEY[pp];
            if (seq_cue_of(&seq, PART_909) >= 0 && blink && cu >= 0 && cu < 16)
                k |= 1u << WHITE_KEY[cu];
            for (i = 0; i < NPARTS; i++)
                if (!part_muted((int)i))
                    k |= 1u << BLACK_KEY[i];
        } else if (ui.view == V_SONG) {               /* the selected bar: its 909 pattern, its unmuted parts */
            const song_t *sg = song();
            if (ui.song_sel < sg->len) {
                int sp = sg->bar[ui.song_sel].pat[PART_909] - ui.pbank * 16;
                if (sp >= 0 && sp < 16)
                    k |= 1u << WHITE_KEY[sp];
                for (i = 0; i < NPARTS; i++)
                    if (!(sg->bar[ui.song_sel].mute >> i & 1u))
                        k |= 1u << BLACK_KEY[i];
            }
        } else if (is_drum()) {
            const dpart_t *d = &cur_pat()->drum[ui.part];
            const uint32_t *bits = (ui.btn & (1u << B_ENV)) ? d->accent : d->hit[ui.sel[ui.part]];
            int ph = eng_step[TRK_DRUM + ui.part];
            for (i = 0; i < 16; i++) {
                int s = step_of((int)i), on = s < d->len && sm_get(bits, s);
                if (seq.playing && s == ph)
                    on = !on;
                if (on)
                    k |= 1u << WHITE_KEY[i];
            }
            k |= 1u << BLACK_KEY[ui.sel[ui.part]];
        } else if (is_303() && !ui.rec) {
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
    plat_glow(proj.set.keyled == 1);
    plat_play_red(ui.rec);                           /* PLAY green; red while recording */
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
    ui.brk_held = -1;
    ui.chain_first = -1;
    ui.touched = -1;
    ui.overlay = O_NONE;
    ui.outline_ok = 0;
    palette_set(proj.set.palette);
    for (i = 0; i < 4; i++)
        blit_hash[i] = 0;
    build_pages();
    undo_start();                                   /* the project as loaded is where undo stops */
}

/* AUTOSAVE: changes are saved by themselves while the pattern is stopped and nothing has been touched
 * for AUTOSAVE_QUIET ms, at most every AUTOSAVE_GAP ms. Stopped only: a flash erase silences the
 * audio. Only the objects that changed are written (project.c), usually one or two sectors. */
#define AUTOSAVE_QUIET 4000u
#define AUTOSAVE_GAP 20000u
static void autosave(void)
{
    uint32_t now = plat_ms();
    if (!ui.dirty || (proj.set.autosave_off && !ui.last_autosave) || seq.playing || seq.req_stop || ui.rec || perf_testing() ||
        ui.overlay == O_ASK || now - ui.act_t < AUTOSAVE_QUIET || (ui.saved_t && now - ui.saved_t < AUTOSAVE_GAP))
        return;
    ui.saved_t = now;
    ui.last_autosave = 0;
    switch (project_save()) {
    case 0:
        ui.dirty = 0;
        say("AUTOSAVED", 0);
        break;
    case -7:
        say("MEMORY FULL: NOT SAVED", 0);
        break;
    default:
        break;
    }
}

void ui_frame(void)
{
    uint32_t i;
    input();
    undo_tick();
    autosave();
    perf_test_tick();
    if (ui.view == V_PERF && !perf_testing())
        perf_window(0);
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
