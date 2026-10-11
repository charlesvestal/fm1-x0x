/* SPDX-License-Identifier: GPL-3.0-only
 * Adapted from Felucca's panel.c, Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hugelton Instruments. */
/* Physical panel: which matrix button / encoder carries which printed label (plat.h's
 * B_* / EN_*). HARDWARE CALIBRATION (hold OCT- and OCT+ while powering on) asks for each
 * label in turn; the table is kept in flash (OBJ_PANEL) and in .noinit. */
static const char *const B_NAME[NB] = {"FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE",
                                        "ARP", "SEQ", "PLAY", "REC", "OCT-", "OCT+"};
static const char *const E_NAME[NE] = {"SELECT", "ALGORITHM", "PRESETS", "KNOB 1", "KNOB 2",
                                        "KNOB 3", "KNOB 4"};
#if BOARD_SMK37
#define PANEL_MAGIC 0x50584E53u          /* "SNXP" (SMK-37 Pro): an FM-1 table never loads here */
#define PANEL_NBTN FM1_NBTN              /* matrix buttons (smk_input.h ids 0..17) */
#define PANEL_NENC FM1_NENC              /* encoders K1..K8 (0..7) */
#else
#define PANEL_MAGIC 0x50414E35u          /* "PAN5": Felucca's layout, so its calibration stays valid */
#define PANEL_NBTN 14u
#define PANEL_NENC 7u
#endif

typedef struct {
    uint32_t magic;
    uint8_t btn[NB];             /* matrix button id (0..13) per label */
    uint8_t enc[NE];             /* matrix encoder (0..6) per role */
    int8_t dir[NE];              /* +1 / -1 so that clockwise is + */
} panel_t;
panel_t panel __attribute__((section(".noinit")));

#if BOARD_SMK37
/* SMK-37 Pro (smk_input.h button ids; the printed SMK label -> the FM-1 label): FX = FX, SCALE = SEL,
 * CHORD = ENV, NOTE REPEAT = LFO, PARA = EDIT, GLOBE = GLO, PATCH = HOME, BT = SAVE, ARP, SEQ as printed,
 * SEQ PLAY = PLAY, SEQ REC = REC, OCTAVE- / + = OCT- / OCT+. FADER BANK switches the faders' bank (main_fm1.c);
 * KNOB BANK, < and > stay free. Knobs as the FM-1's top row: K1 = MASTER (smk_input.h, a virtual pot),
 * K2 = SELECT, K3 = ALGORITHM, K4 = PRESETS; K5..K8 = KNOB 1..4. HARDWARE CALIBRATION (OCTAVE- + OCTAVE+
 * held at power-on) re-teaches all of it but K1. */
static const panel_t PANEL_DEFAULT = {
    PANEL_MAGIC,
    {14, 8, 9, 7, 13, 10, 12, 11, 6, 15, 16, 17, 4, 5},
    {1, 2, 3, 4, 5, 6, 7},
    {1, 1, 1, 1, 1, 1, 1},
};
#else
static const panel_t PANEL_DEFAULT = {
    PANEL_MAGIC,
    {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 0, 1},
    {0, 1, 6, 2, 3, 4, 5},
    {1, 1, 1, 1, 1, 1, 1},
};
#endif

static int panel_valid(const panel_t *p)
{
    uint32_t i, ok = p->magic == PANEL_MAGIC;
    for (i = 0; ok && i < NB; i++)
        ok = p->btn[i] < PANEL_NBTN;
    for (i = 0; ok && i < NE; i++)
        ok = p->enc[i] < PANEL_NENC && (p->dir[i] == 1 || p->dir[i] == -1);
    return (int)ok;
}

static void panel_init(void)
{
    panel_t p;
    if (plat_store_load(OBJ_PANEL, &p, sizeof p) == (int)sizeof p && panel_valid(&p))
        panel = p;
    if (!panel_valid(&panel))
        panel = PANEL_DEFAULT;
}

#define SETUP_IDLE_MS 30000u
static void panel_setup(void)
{
    uint32_t i, used = 0, t0 = fm1_ms;
    const panel_t old = panel;
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 10, 240, &FONT_S, "HARDWARE CALIBRATION", C_WHITE, 1);
    draw_text_box(0, 30, 240, &FONT_S, "TEACH EACH BUTTON AND KNOB", C_GRAY, 1);
    while (fm1_in.buttons) {
        fm1_wdt_feed();
        if (fm1_ms - t0 > SETUP_IDLE_MS)
            goto timeout;
    }
    fm1_input_edges(0);
    for (i = 0; i < NB; i++) {
        uint32_t p = 0, id;
        draw_text_box(0, 80, 240, &FONT_S, "PRESS", C_GRAY, 1);
        draw_text_box(0, 100, 240, &FONT_M, B_NAME[i], C_WHITE, 1);
        t0 = fm1_ms;
        while (!(p & ~used)) {
            fm1_wdt_feed();
            p |= fm1_input_edges(0);
            if (fm1_ms - t0 > SETUP_IDLE_MS)
                goto timeout;
        }
        for (id = 0; id < PANEL_NBTN; id++)
            if (((p & ~used) >> id) & 1u)
                break;
        panel.btn[i] = (uint8_t)id;
        used |= 1u << id;
    }
#if BOARD_SMK37
    used = 1u << SMK_ENC_MASTER;                    /* K1 is the MASTER pot: never taught a role */
#else
    used = 0;
#endif
    for (i = 0; i < NE; i++) {
        uint32_t e;
        int32_t st = 0;
        draw_text_box(0, 80, 240, &FONT_S, "TURN RIGHT", C_GRAY, 1);
        draw_text_box(0, 100, 240, &FONT_M, E_NAME[i], C_WHITE, 1);
        for (e = 0; e < PANEL_NENC; e++)
            fm1_enc_take(e);
        t0 = fm1_ms;
        for (;;) {
            fm1_wdt_feed();
            if (fm1_ms - t0 > SETUP_IDLE_MS)
                goto timeout;
            for (e = 0; e < PANEL_NENC; e++)
                if (!((used >> e) & 1u) && (st = fm1_enc_take(e)) != 0)
                    break;
            if (e < PANEL_NENC)
                break;
        }
        panel.enc[i] = (uint8_t)e;
        panel.dir[i] = (int8_t)(st > 0 ? 1 : -1);
        used |= 1u << e;
        fm1_delay_ms(300);
        fm1_enc_take(e);
    }
    panel.magic = PANEL_MAGIC;
    plat_store_save(OBJ_PANEL, &panel, sizeof panel);
    lcd_fill(0, 0, 240, 240, C_BLACK);
    return;
timeout:
    panel = old;
    lcd_fill(0, 0, 240, 240, C_BLACK);
}
