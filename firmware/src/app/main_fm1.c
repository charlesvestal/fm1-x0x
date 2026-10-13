/* SPDX-License-Identifier: GPL-3.0-only
 * Boot, audio ISR and main loop adapted from Felucca's main.c / audio.c, Copyright (C) 2026
 * Leo Kuroshita (@kurogedelic), Hugelton Instruments. */
/* X0X on the FM-1: boot (WDT first, boot-loop guard, guards), LCD, input (TIMER5 10 kHz),
 * audio (ALNK0), USB; then the main loop: UI at ~60 frames/s, input polled in between. */
extern uint32_t _data_start[], _data_end[], _data_load[], _bss_start[], _bss_end[];
extern uint32_t _pool_start[], _pool_end[], _rt_start[], _rt_end[], _rt_load[], _c1_start[], _c1_end[], _c1_load[];

/* --------------------------------------------------------------- audio --- */
/* X0X: 512-frame halves (11.6 ms; Felucca 256). The render runs inside the DMA's deadline, and a
 * busy pattern averaging ~80 % ran over it about one half in 300 (a heavy step): a dropped half.
 * Twice the half averages a heavy step over twice the time, for one more half of latency. */
#define HALF_FRAMES 512u                    /* I2S half buffer: 11.6 ms at 44.1 kHz */
#define HALF_WORDS (HALF_FRAMES * 2u)
#define REND_FRAMES 256u                    /* engine_render's most per call */
#if FELUCCA_UAC
_Static_assert(REND_FRAMES == UA_HALF, "usb.c sizes the USB audio ring band for this block");
#endif
static int32_t abuf[2u * HALF_WORDS] __attribute__((aligned(4)));

#define DBG_MAGIC 0x44424731u                       /* "DBG1": read with `fm1t memr` */
struct x0x_dbg {
    uint32_t magic, halves, max_us, nested, in_audio, late, timer_irqs, ui_frames;
    uint32_t last_us, cpu_q8, boots, stage;
    uint32_t crash_seen;                            /* the crash count already reported at a boot */
} x0x_dbg __attribute__((section(".noinit")));
static uint8_t safe_mode;
#if X0X_DEBUG
/* development: the slowest block since boot, taken apart (read over USB: tools/fm1_debug.py) */
extern volatile uint32_t eng_dbg_wait, eng_dbg_nsub;
static volatile uint32_t dbg_tmr;                   /* 24 MHz ticks of TIMER5 nested in the render */
struct x0x_worst_s {
    uint32_t magic, us, wait, tmr, nsub, half_no, over90, over100, blocks, late_in;
} x0x_worst;
#endif                           /* two failed boots: no audio, USB on (safe_main) */

static void render_block(int32_t *o)                /* ALNK0: one half, measured */
{
    uint32_t t0 = fm1_ticks(), us, budget = HALF_FRAMES * 1000000u / FS;
    x0x_dbg.in_audio = 1;
#if FELUCCA_UAC
    uac_render_start();
#endif
    {
        uint32_t k;
        for (k = 0; k < HALF_FRAMES; k += REND_FRAMES) {
            engine_render(o + 2u * k, REND_FRAMES);
#if FELUCCA_UAC
            uac_tap(o + 2u * k, REND_FRAMES);       /* the USB audio input: the same master output */
#endif
            engine_volume(o + 2u * k, REND_FRAMES);     /* USB AUDIO FULL: the MASTER knob after the tap */
        }
    }
    us = (fm1_ticks() - t0) / FM1_TICKS_PER_US;
#if X0X_DEBUG
    x0x_worst.magic = 0x57525354u;                  /* "WRST" */
    x0x_worst.blocks++;
    if (us * 100u > budget * 90u)
        x0x_worst.over90++;
    if (us > budget)
        x0x_worst.over100++;
    if (us > x0x_worst.us) {
        x0x_worst.us = us;
        x0x_worst.wait = eng_dbg_wait;
        x0x_worst.tmr = dbg_tmr;
        x0x_worst.nsub = eng_dbg_nsub;
        x0x_worst.half_no = x0x_dbg.halves;
    }
    eng_dbg_wait = eng_dbg_nsub = 0;
    dbg_tmr = 0;
#endif
    x0x_dbg.cpu_q8 = (x0x_dbg.cpu_q8 * 15u + (us * 256u) / budget) / 16u;
    audio_cpu_pct = (x0x_dbg.cpu_q8 * 100u) >> 8;
    if (us * 100u / budget > audio_peak_pct)
        audio_peak_pct = us * 100u / budget;        /* the PERF screen's peak (read and reset) */
    engine_load(us * 100u / budget, HALF_FRAMES);
    x0x_dbg.halves++;
    x0x_dbg.last_us = us;
    if (us > x0x_dbg.max_us)
        x0x_dbg.max_us = us;
    x0x_dbg.in_audio = 0;
}

void fm1_alnk0_irq(void)                            /* via isr_alnk0 (hal/fm1_isr.S) */
{
    uint8_t p = fm1_audio_pending();
    fm1_audio_ack_aux(p);
    if (p & FM1_AUDIO_HALF) {
        uint32_t half = fm1_audio_free_half();
        render_block(&abuf[half * HALF_WORDS]);
        fm1_audio_ack_half();
        if (fm1_audio_free_half() != half) {
#if X0X_DEBUG
            x0x_worst.late_in++;
#endif
            x0x_dbg.late++;                         /* the DMA moved on while we rendered */
            audio_xruns++;
        }
    }
}
extern void isr_alnk0(void);

static void audio_init(void)
{
    uint32_t i;
    for (i = 0; i < 2u * HALF_WORDS; i++)
        abuf[i] = 0;
    fm1_audio_init(abuf, HALF_WORDS, isr_alnk0, 3);
}

static void audio_silence(void)                     /* IRQs off (flash erase): the DMA would loop stale audio */
{
    uint32_t i;
    for (i = 0; i < sizeof abuf / sizeof abuf[0]; i++)
        abuf[i] = 0;
}

/* ------------------------------------------------------------ timer --- */
static uint8_t usb_due;
void fm1_timer5_irq(void)
{
    static uint32_t sub;
#if X0X_DEBUG
    uint32_t tt0 = fm1_ticks();
#endif
    fm1_timer5_ack();
    x0x_dbg.timer_irqs++;
    if (x0x_dbg.in_audio)
        x0x_dbg.nested++;
    fm1_input_tick();
    /* 2 kHz: all USB SIE traffic lives here. This ISR now nests into the audio render (so the key
     * matrix keeps its rhythm, below); USB shares the MIDI queues with the audio ISR, so a poll that
     * falls inside the render waits for the first tick outside it, as it did before the nesting. */
    if (sub % 5u == 0u) {
        usb_due = 1;
#if FELUCCA_UAC
        /* the USB audio stream cannot wait for the render (one packet per 1 ms frame, a render takes
         * up to ~11.6 ms): it runs nested too. It touches only EP4 (INDEX is set on every access) and
         * the consumer side of its ring, and usb_poll never runs nested, so the two never interleave */
        if (x0x_dbg.in_audio)
            uac_service();
#endif
    }
    if (usb_due && !x0x_dbg.in_audio) {
        usb_due = 0;
        usb_poll();
#if X0X_TRS
        uart_midi_poll();                           /* the TRS jack: same context, same queue */
#endif
    }
    if (++sub == 10u)
        sub = 0;
    {
        static uint32_t last, acc;
        uint32_t now = fm1_ticks();
        acc += now - last;
        last = now;
        while (acc >= 1000u * FM1_TICKS_PER_US) {
            acc -= 1000u * FM1_TICKS_PER_US;
            fm1_ms++;
        }
    }
#if X0X_DEBUG
    if (x0x_dbg.in_audio)
        dbg_tmr += fm1_ticks() - tt0;
#endif
}
extern void isr_timer5(void);

#if BOARD_SMK37
/* ------------------------------------------------------------ SMK-37 Pro --- */
/* What the SMK-37 Pro has beyond the FM-1's panel (smk_input.h), after the UI (ui.c) in this unity build.
 * The pads: the 16 white keys' lights (plat_fm1.c plat_leds) in the colour of the part on screen, lit half
 * bright, dim an eighth, else dark; sent between LCD transfers (lcd.c padled_flush). */
static void smk_pads(void)
{
    uint16_t c = PART_COL[ui.part % NPARTS];
    uint32_t r = (uint32_t)(c >> 11) * 255u / 31u, g = (uint32_t)((c >> 5) & 63u) * 255u / 63u,
             b = (uint32_t)(c & 31u) * 255u / 31u, w;
    for (w = 0; w < SMK_NPAD; w++) {
        uint32_t k = SMK_WHITE_NOTE[w], pad = SMK_PAD_OF_WHITE(w), sh;
        sh = (smk_keys_lit >> k) & 1u ? 1u : (smk_keys_dim >> k) & 1u ? 3u : 0u;
        if (sh)
            padled_set(pad, (uint8_t)(r >> sh), (uint8_t)(g >> sh), (uint8_t)(b >> sh));
        else
            padled_set(pad, 0, 0, 0);
    }
    padled_flush();
}

/* The four faders, by FADER BANK (its LED lit on bank B): A = the levels of 909, 808, 303 A, 303 B; B = the
 * BREAK level, the reverb and delay returns, the master FILTER (middle off, low-pass left, high-pass right).
 * They set the same values as the knobs (saved with the project; a knob move while REC runs records motion).
 * A fader takes over only once it moves (also after a bank change), so nothing jumps. */
static void smk_faders(void)
{
    static const char *const NAME[2][4] = {{"909 LEVEL", "808 LEVEL", "303 A LEVEL", "303 B LEVEL"},
                                           {"BREAK LEVEL", "REV RETURN", "DLY RETURN", "FILTER"}};
    static uint8_t last[4] = {0xFF, 0xFF, 0xFF, 0xFF}, held;
    uint32_t i, down = (fm1_in.buttons >> SMK_BTN_FADER_BANK) & 1u;
    if (down && !held) {                                /* FADER BANK: the other bank, each fader waits to move */
        smk_fbank ^= 1u;
        for (i = 0; i < 4u; i++)
            last[i] = 0xFF;
        say(smk_fbank ? "FADERS: BRK REV DLY FILTER" : "FADERS: 909 808 303A 303B", 0);   /* (say: 27 chars) */
    }
    held = (uint8_t)down;
    for (i = 0; i < 4u; i++) {
        uint32_t v = fm1_in.fader[i];
        pref_t r;
        const x0x_param_t *d;
        char t[28], num[16], unit[8], *q;
        uint32_t n = 0;
        if (last[i] == 0xFF || fm1_in.frames < 200u) {   /* (the first readings settle) */
            last[i] = (uint8_t)v;
            continue;
        }
        if (v == last[i])
            continue;
        last[i] = (uint8_t)v;
        if (!smk_fbank)
            r = PR(R_ENG, T_MIX, i, MX_LEVEL);
        else if (i == 0)
            r = PR(R_ENG, T_MIX, PART_BRK, MX_LEVEL);
        else if (i == 1)
            r = PR(R_ENG, T_FX, 0, FX_RV_LEVEL);
        else if (i == 2)
            r = PR(R_ENG, T_FX, 0, FX_DL_LEVEL);
        else
            r = PR(R_ENG, T_MST, 0, MST_DJF);
        d = pref_desc(r);
        if (!d)
            continue;
        pref_set(r, (int)((v * d->max + 63u) / 127u));
        motion_touch(r);
        ui.act_t = plat_ms();                            /* (the autosave waits for quiet) */
        pref_value(r, num, unit);
        q = put_s(put_s(t, NAME[smk_fbank][i]), " ");
        while (n < 16u && num[n])
            n++;
        if (n < 8u)                                      /* NAME (<= 11) + " " + num (< 8) + unit (< 8): fits say() */
            put_s(put_s(q, num), unit);
        say(t, 0);
    }
}

/* The wheels (mux C address 4 = pitch, 5 = mod), self-calibrating as in the reference port: the pitch wheel's
 * rest is its centre (taken at power-on, followed slowly while it rests), each wheel's range grows to what it
 * has reached. On a 303 (the part on screen) they play its sound without changing what is saved: pitch = its
 * TUNE (about +-1.5 semitones), mod = its CUTOFF opened from the saved value up to the top. Released (or
 * another part chosen), the saved values sound again. Other parts: the wheels do nothing. */
static void smk_wheels(void)
{
    enum { DEAD = 12, SPAN0 = 80, MOD_LO = 485, MOD_HI = 615 };
    static int32_t pc, plo, phi, mlo, mhi, rest;
    static int8_t part = -1;
    static int16_t tune_now = -1, cut_now = -1;
    int32_t p = fm1_in.wheel[0], m = fm1_in.wheel[1], d, bend, mw, want;
    if (fm1_in.frames < 300u)                         /* (the readings settle) */
        return;
    if (!pc) {
        pc = p;
        plo = p - SPAN0;
        phi = p + SPAN0;
        mlo = m < MOD_LO ? m : MOD_LO;
        mhi = m > MOD_HI ? m : MOD_HI;
    }
    if (p < plo)
        plo = p;
    if (p > phi)
        phi = p;
    if (m < mlo)
        mlo = m;
    if (m > mhi)
        mhi = m;
    d = p - pc;
    if (d > -DEAD && d < DEAD) {
        bend = 0;
        if (++rest > 200) {                            /* resting: the centre follows a slow drift */
            pc += d > 0 ? 1 : d < 0 ? -1 : 0;
            rest = 0;
        }
    } else {
        rest = 0;
        bend = d > 0 ? (d - DEAD) * 63 / (phi - pc - DEAD > 1 ? phi - pc - DEAD : 1)
                     : (d + DEAD) * 63 / (pc - plo - DEAD > 1 ? pc - plo - DEAD : 1);
        bend = clamp(bend, -63, 63);
    }
    mw = mhi - m <= DEAD ? 0 : (mhi - m - DEAD) * 127 / (mhi - mlo - DEAD > 1 ? mhi - mlo - DEAD : 1);
    mw = clamp(mw, 0, 127);
    if (part != (is_303() ? bidx() : -1)) {          /* another part: the last 303 sounds as saved again */
        if (part >= 0 && tune_now >= 0)
            engine_set(T_303, part, BASS303_TUNE, proj.sound.v[T_303][part][BASS303_TUNE]);
        if (part >= 0 && cut_now >= 0)
            engine_set(T_303, part, BASS303_CUTOFF, proj.sound.v[T_303][part][BASS303_CUTOFF]);
        part = (int8_t)(is_303() ? bidx() : -1);
        tune_now = cut_now = -1;
    }
    if (part < 0)
        return;
    want = bend ? clamp(proj.sound.v[T_303][part][BASS303_TUNE] + bend, 0, 127) : -1;
    if (want != tune_now) {
        engine_set(T_303, part, BASS303_TUNE, want >= 0 ? want : proj.sound.v[T_303][part][BASS303_TUNE]);
        tune_now = (int16_t)want;
    }
    {
        int32_t base = proj.sound.v[T_303][part][BASS303_CUTOFF];
        want = mw ? base + (127 - base) * mw / 127 : -1;
    }
    if (want != cut_now) {
        engine_set(T_303, part, BASS303_CUTOFF, want >= 0 ? want : proj.sound.v[T_303][part][BASS303_CUTOFF]);
        cut_now = (int16_t)want;
    }
}

/* < and >: one detent of SELECT back / forward (the page, a list's row: what SELECT does there; not with HOME held,
 * where SELECT is the tempo), as on the other SMK-37 Pro ports. Not if HARDWARE CALIBRATION gave them an FM-1 label. */
static void smk_arrows(void)
{
    static uint32_t prev;
    uint32_t now = fm1_in.buttons & (1u << SMK_BTN_LEFT | 1u << SMK_BTN_RIGHT), id, i;
    uint32_t pressed = now & ~prev;
    prev = now;
    if (!pressed || (plat_buttons() & (1u << B_HOME)))
        return;
    for (id = SMK_BTN_LEFT; id <= SMK_BTN_RIGHT; id++) {
        if (!((pressed >> id) & 1u))
            continue;
        for (i = 0; i < NB && panel.btn[i] != id; i++)
            ;
        if (i == NB) {
            uint32_t l = fm1__lock();
            fm1_in.enc_steps[panel.enc[EN_SELECT]] += (int16_t)((id == SMK_BTN_RIGHT ? 1 : -1) * panel.dir[EN_SELECT]);
            fm1__unlock(l);
        }
    }
}

/* every frame, after ui_frame; the wheels also between frames. The 10 outer keys (C3..E3, G#5..C6) have no
 * X0X role: their events are dropped. */
/* pads as a step grid (steps, patterns: 1-8 top row) unless they play notes (a 303's REC keyboard) */
static void smk_grid(void)
{
    smk_pad_grid = (uint8_t)(ui.view == V_HOME || ui.view == V_SONG || (ui.btn & (1u << B_HOME | 1u << B_SAVE)) ||
                             !(is_303() && ui.rec));
}

static void smk_frame(void)
{
    while (smk_extkey_take() >= 0)
        ;
    smk_arrows();
    smk_faders();
    smk_wheels();
    smk_grid();
    smk_pads();
}
#endif

/* ------------------------------------------------------------- boot --- */
#define BOOTGUARD_MAGIC 0x42475244u
struct bootguard_s bootguard __attribute__((section(".noinit")));   /* declared in x0x.c (ota_commit) */

static void hexs(char *b, uint32_t v)
{
    uint32_t i;
    for (i = 0; i < 8u; i++)
        b[i] = "0123456789ABCDEF"[(v >> (28u - 4u * i)) & 15u];
    b[8] = 0;
}

static void fm1_fault(const fm1_crash_t *c)
{
    char b[12];
    uint32_t t0;
    fm1_audio_stop();
    lcd_fill(0, 0, 240, 240, RGB(160, 0, 0));
    draw_text_box(0, 8, 240, &FONT_S, "X0X CRASH", C_WHITE, 1);
    hexs(b, c->vec);
    draw_text_box(10, 40, 220, &FONT_S, b, C_WHITE, 0);
    hexs(b, c->pc);
    draw_text_box(10, 60, 220, &FONT_S, b, C_WHITE, 0);
    hexs(b, c->emu);
    draw_text_box(10, 84, 220, &FONT_S, b, C_WHITE, 0);
    hexs(b, c->dbg);
    draw_text_box(10, 102, 220, &FONT_S, b, C_WHITE, 0);
    hexs(b, c->rets);
    draw_text_box(10, 120, 220, &FONT_S, b, C_WHITE, 0);
    t0 = fm1_ticks();
    while ((uint32_t)(fm1_ticks() - t0) < 4000u * 1000u * FM1_TICKS_PER_US)
        ;
    fm1_reboot();
}

static void enter_uboot(const char *why)
{
    fm1_audio_stop();
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 110, 240, &FONT_S, why, C_WHITE, 1);
    fm1_delay_ms(20);
    usb_detach();
    fm1_delay_ms(30);
    bootguard.pending = 0;                          /* intentional reset: not a failed boot */
    fm1_enter_uboot();
}

/* SAFE MODE: X0X crashed (or hung) twice while starting. Nothing that makes sound runs: no audio,
 * no engine, no UI; USB is on, so the web installer (X0X, or the stock firmware) and M-UPGRADE can
 * reach it. PLAY tries X0X again. The chip's own update mode stays the last resort (fm1_cstart). */
static void safe_main(void)
{
    char b[24];
    uint32_t t0, play;
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 18, 240, &FONT_B, "X0X SAFE MODE", C_HI, 1);
    draw_text_box(0, 52, 240, &FONT_S, "IT CRASHED TWICE", C_WHITE, 1);
    draw_text_box(0, 72, 240, &FONT_S, "WHILE STARTING.", C_WHITE, 1);
    draw_text_box(0, 102, 240, &FONT_S, "NO SOUND. USB IS ON:", C_WHITE, 1);
    draw_text_box(0, 122, 240, &FONT_S, "REINSTALL FROM THE", C_WHITE, 1);
    draw_text_box(0, 142, 240, &FONT_S, "WEB INSTALLER.", C_WHITE, 1);
    draw_text_box(0, 172, 240, &FONT_S, "PLAY: TRY AGAIN", C_HI, 1);
    if (fm1_crash.magic == FM1_CRASH_MAGIC) {
        b[0] = 'P';
        b[1] = 'C';
        b[2] = ' ';
        hexs(b + 3, fm1_crash.pc);
        draw_text_box(0, 208, 240, &FONT_S, b, C_GRAY, 1);
    }
    fm1_input_init();
    panel_init();
    usb_start();
    fm1_timer5_start(isr_timer5, 1);
    fm1_guard_lock_top();
    fm1_irq_enable_all();
    play = 1u << panel.btn[B_PLAY];
    t0 = fm1_ms;
    for (;;) {
        fm1_wdt_feed();
        usb_retry(fm1_ms);
        if (bootguard.pending && fm1_ms - t0 > 10000u)
            bootguard.pending = 0;                  /* safe mode itself is up: not another failed boot */
        if ((fm1_in.buttons & play) && fm1_ms - t0 > 500u) {
            bootguard.failed = 0;                   /* try X0X again, from scratch */
            bootguard.pending = 0;
            draw_text_box(0, 172, 240, &FONT_S, "STARTING X0X...", C_HI, 1);
            usb_detach();
            fm1_delay_ms(30);
            fm1_reboot();
        }
        ota_service();
        if (usb.ota_req) {
            usb.ota_req = 0;
            if (flash_ok)
                ota_session();                      /* an install commits and resets; else back here */
        }
        if (usb.uboot_req)
            enter_uboot("UBOOT (USB)");
    }
}

static void fm1_main(void)
{
    int32_t knob = 512 * 16;
    uint32_t last_frame = 0, k;
    uint8_t last_pat = 0xFF;
    {
        uint32_t f = irq_save();
        flash_ok = FL_JEDEC_OK(FL_FAR(fl_jedec_ram)());   /* the expected 1 MiB part, else RAM only */
        irq_restore(f);
        if (flash_ok) {
            fl_plain_window_init();                 /* user sample slots read as plaintext through XIP */
#if BOARD_SMK37
            st_owner_check();                       /* SMK: never parse another ported firmware's store (storage.c) */
#endif
            for (k = 0; k < SMP_USER_SLOTS; k++)
                smp_user_scan(k);
            ota_boot_cleanup();
        }
    }
    lcd_init();
    palette_set(1);
    draw_text_box(0, 96, 240, &FONT_M, "X0X", C_HI, 1);
    draw_text_box(0, 134, 240, &FONT_S, "909 808 303 303 BREAK", C_GRAY, 1);
    if (x0x_dbg.magic != DBG_MAGIC) {
        memset(&x0x_dbg, 0, sizeof x0x_dbg);
        x0x_dbg.magic = DBG_MAGIC;
    }
    if (safe_mode)
        safe_main();                                /* never returns */
    if (fm1_crash.magic == FM1_CRASH_MAGIC && fm1_crash.count != x0x_dbg.crash_seen) {
        char b[16];                                 /* a crash since the last boot: say so, a moment */
        x0x_dbg.crash_seen = fm1_crash.count;
        b[0] = 'P';
        b[1] = 'C';
        b[2] = ' ';
        hexs(b + 3, fm1_crash.pc);
        draw_text_box(0, 176, 240, &FONT_S, fm1_crash.core == 1 ? "THE SECOND CORE CRASHED"
                      : "RESTARTED AFTER A CRASH", RGB(255, 80, 60), 1);
        draw_text_box(0, 196, 240, &FONT_S, b, C_GRAY, 1);
        fm1_wdt_feed();
        fm1_delay_ms(2500);
        fm1_wdt_feed();
    }
    x0x_dbg.boots++;
    x0x_dbg.max_us = 0;
    fm1_input_init();
    fm1_adc_init();
#if BOARD_SMK37
    if (flash_ok)                                   /* the factory key / pad velocity curves (USRTRIM, read only) */
        smk_trim_load(fm1_xip_ptr(SMK_TRIM_FLASH));
#endif
    panel_init();
    led_pos_init();
    plat_perf_init();
    engine_init(proj.pat, &proj.arr.song, proj.arr.lane, &proj.sound);
    if (project_load() != 0)
        project_defaults();                         /* nothing saved yet (or another format) */
    engine_apply_sound(&proj.sound);
    seq.bpm = (float)proj.set.bpm_x10 / 10.0f;
    seq.accent_q7 = proj.set.accent_q7;
    project_apply_io();                             /* clock and notes out, the MIDI channels, the output */
    engine_brk_loops();
    plat_cpu2_set(1);                               /* the second core: at power-on, before the audio and the
                                                     * timers, as Melodee starts it (started later, it does not
                                                     * reach its entry); if it does not answer, one core */
    audio_init();
    usb_start();
#if X0X_TRS
    uart_midi_init();                               /* before TIMER5: it read-modify-writes port H too */
#endif
    /* above ALNK0 (3): the key / encoder / LED matrix is scanned one column per tick and must keep
     * its rhythm. Below the audio (as Felucca has it), a render that takes most of its 5.8 ms half
     * buffer stopped the scan for milliseconds: fast encoder turns lost their steps, and the column
     * lit when the render began stayed lit through it, so the LEDs flickered with the audio load. */
    fm1_timer5_start(isr_timer5, 4);
    fm1_guard_lock_top();
    fm1_irq_enable_all();
    fm1_delay_ms(30);
#if BOARD_SMK37
    if ((fm1_in.buttons & FM1_OCT_BOTH) == FM1_OCT_BOTH)
        panel_setup();                              /* OCTAVE- + OCTAVE+ held at power-on */
#else
    if ((fm1_in.buttons & 3u) == 3u)
        panel_setup();                              /* OCT- + OCT+ held at power-on */
#endif
    fm1_delay_ms(300);
    lcd_fill(0, 0, 240, 240, C_BLACK);
    ui_init();
#if BOARD_SMK37
    if (st_owner_cleared)
        ui_say("OLD DATA CLEARED", 0);          /* another firmware's store (storage.c st_owner_check); say() has 27 chars */
#endif

    for (;;) {
        fm1_wdt_feed();
        usb_retry(fm1_ms);
        if (fm1_ms > 30000u && bootguard.pending) {     /* a crash or hang in the first 30 s counts */
            bootguard.pending = 0;
            bootguard.failed = 0;
        }
        {
            int32_t b = fm1_adc_read(FM1_ADC_BATT);     /* the battery: a slow average (plat_battery) */
            if (b > 0)
                batt_raw = batt_raw ? batt_raw + (b - batt_raw) / 32 : b;
        }
        {
            int32_t a = fm1_adc_read(FM1_ADC_MASTER);
            if (a >= 0) {
                uint32_t k10;
                knob += (a * 16 - knob) / 8;
                k10 = (uint32_t)(knob / 16);
                master_q12 = (k10 * k10) >> 8;           /* 0 .. ~4096 */
            }
        }
        {   /* OCT- + OCT+ held 5 s: update mode (Felucca's, unchanged) */
            static uint32_t t0, shown;
            uint32_t both = (1u << panel.btn[B_OCTDN]) | (1u << panel.btn[B_OCTUP]);
            if ((fm1_in.buttons & both) != both) {
                if (shown)
                    ui_say("UPDATE MODE CANCELLED", 0);
                shown = 0;
                t0 = fm1_ms;
            } else if (fm1_ms - t0 > 2000u && fm1_ms - t0 <= 5000u) {
                uint32_t left = (5000u - (fm1_ms - t0) + 999u) / 1000u;
                if (left != shown) {
                    char d[4] = {(char)('0' + left), '.', '.', 0};
                    ui_say("UPDATE MODE IN ", d);
                    shown = left;
                }
            } else if (fm1_ms - t0 > 5000u) {
                enter_uboot("UBOOT");
            }
        }
        ed_service();                               /* sample upload SysEx */
        ota_service();                              /* M-UPGRADE handshake */
        if (usb.ota_req) {
            usb.ota_req = 0;
            seq_stop(&seq);
            if (flash_ok)
                ota_session();                      /* returns only if nothing was committed */
            lcd_fill(0, 0, 240, 240, C_BLACK);
            ui_init();
        }
        if (usb.uboot_req)
            enter_uboot("UBOOT (USB)");
#if X0X_CDC
        cdc_task();
#endif
        if (seq.ppat[TRK_BRK] != last_pat) {        /* the break's new pattern may name other loops */
            last_pat = seq.ppat[TRK_BRK];
            engine_brk_loops();
        }
        if (fm1_ms - last_frame >= 16u) {           /* ~60 frames/s at most; input in between */
            last_frame = fm1_ms;
            x0x_dbg.ui_frames++;
            ui_frame();
#if BOARD_SMK37
            smk_frame();                            /* faders, wheels, pads */
#endif
        } else {
            ui_input_only();
#if BOARD_SMK37
            smk_wheels();                           /* (the wheels follow the hand, not the screen) */
            smk_grid();
#endif
        }
    }
}

void fm1_cstart(void)
{
    uint32_t *s, *d, p3, src, wdt;
    fm1_cpu1_halt();                                /* a second core left running by the reset would read
                                                     * the RAM cleared below (Jangada, fm1-x0x#10) */
    fm1_time_init();
    fm1_reset_reason();
    p3 = fm1_boot.p3_rst;
    src = fm1_boot.rst_src;
    wdt = fm1_boot.wdt_con;
    fm1_wdt_arm(0x0D);
    if (bootguard.magic != BOOTGUARD_MAGIC) {
        bootguard.magic = BOOTGUARD_MAGIC;
        bootguard.failed = 0;
        bootguard.pending = 0;
    }
    if (bootguard.pending)
        bootguard.failed++;
    bootguard.pending = 1;
    if (bootguard.failed >= 4u) {                   /* safe mode failed too: the chip's own update mode */
        bootguard.failed = 0;
        bootguard.pending = 0;
        fm1_enter_uboot();
    }
    p3 |= (bootguard.failed >= 2u) << 8;            /* two failed boots: safe mode (kept past the .bss clear) */
    fm1_irq_init();
    irq_stack_mark();                               /* before any interrupt: PERF's IRQ STACK */
    for (d = _bss_start; d < _bss_end; d++)
        *d = 0;
    for (d = _pool_start; d < _pool_end; d++)
        *d = 0;
    for (s = _data_load, d = _data_start; d < _data_end; s++, d++)
        *d = *s;
    for (s = _rt_load, d = _rt_start; d < _rt_end; s++, d++)
        *d = *s;
    for (s = _c1_load, d = _c1_start; d < _c1_end; s++, d++)
        *d = *s;                                    /* the second core's loop (fm1_cpu1.h) */
    fm1_mailbox_clear();
    fm1_guard_enable(FM1_GUARD_STACK | FM1_GUARD_WRITE | FM1_GUARD_BUS | FM1_GUARD_PC);
    safe_mode = (uint8_t)(p3 >> 8);
    fm1_boot.p3_rst = (uint8_t)p3;
    fm1_boot.rst_src = src;
    fm1_boot.wdt_con = (uint8_t)wdt;
    fm1_main();
    for (;;)
        ;
}
