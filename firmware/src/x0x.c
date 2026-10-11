/* SPDX-License-Identifier: GPL-3.0-only */
/* X0X firmware for the M-VAVE FM-1: one compilation unit for the platform and the app
 * (Felucca's layout: the HAL is header-only, so everything that touches it is here).
 * The DSP (dsp/), the sequencer (seq/) and the audio engine (app/engine.c) are separate
 * units compiled at -O2 by tools/build.py; this one is -Os. Order matters. */
#include <stdint.h>
#include "fm1_time.h"
#include "fm1_sys.h"
#include "fm1_irq.h"
#include "fm1_guard.h"
#if BOARD_SMK37
#define SMK_BIG __attribute__((section(".pool")))   /* SMK buffers in the pool (zeroed at boot): .data+.bss is full */
#include "smk_input.h"               /* SMK-37 Pro: keybed, pads, buttons, encoders, faders, wheels (fm1_input.h API) */
#else
#include "fm1_input.h"
#endif
#include "fm1_timer.h"
#include "fm1_audio.h"
#if !BOARD_SMK37
#include "fm1_adc.h"                 /* (SMK: smk_input.h stands in for it) */
#endif
#include "fm1_lcd_hw.h"
#include "fm1_cpu1.h"
#include "fm1_flash.h"
#include "fm1_xip.h"
#include "fm1_perf.h"

#include "libc.c"
#include "lcd.c"
#include "gfx.c"

static volatile uint32_t fm1_ms;  /* milliseconds since boot (TIMER4-based, TIMER5 ISR) */
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
static inline int32_t clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

#include "app/plat.h"

#define FELUCCA_OTA 1             /* M-UPGRADE update entry: keeps the web installer working */
#ifndef X0X_CDC
#define X0X_CDC 0                 /* USB serial console off: one plain MIDI interface */
#endif
#define FELUCCA_CDC X0X_CDC
#ifndef X0X_UAC
#define X0X_UAC 1                 /* USB audio input: the master output, recordable over the cable */
#endif
#define FELUCCA_UAC X0X_UAC
#ifndef FELUCCA_ID
#if BOARD_SMK37
#define FELUCCA_ID "SMK-37 Pro_900"   /* package identity (tools/build.py) */
#else
#define FELUCCA_ID "FM-1_900"     /* package identity (tools/build.py) */
#endif
#endif
#include "usb.c"
#ifndef X0X_TRS
#if BOARD_SMK37
#define X0X_TRS 0                 /* SMK-37 Pro: no MIDI IN (its jack is MIDI OUT, UART1 TX on PC7); PH8 is a
                                   * key-matrix row there and must never become UART RX */
#else
#define X0X_TRS 1                 /* MIDI IN on the TRS jack */
#endif
#endif
#if BOARD_SMK37 && X0X_TRS
#error "BOARD_SMK37: midi_uart.c is the FM-1 MIDI IN driver (RX on PH8, a key-matrix row on the SMK)"
#endif
#if X0X_TRS
#include "midi_uart.c"
#endif

static uint8_t flash_ok;          /* JEDEC id matched at boot */
static void audio_silence(void);
static int st_read(uint32_t off, void *dst, uint32_t n)   /* 256-byte IRQ-off windows: audio keeps up */
{
    uint8_t *d = dst;
    while (n) {
        uint32_t k = n > 256u ? 256u : n, f = irq_save();
        int rc = FL_FAR(fl_read_ram)(off, d, k);
        irq_restore(f);
        if (rc)
            return rc;
        off += k;
        d += k;
        n -= k;
    }
    return 0;
}
static int st_erase(uint32_t off)
{
    uint32_t took;
    if (!FL_STORE_OK(off, 0x1000u))
        return -8;
    audio_silence();
    return fl_erase4k(off, &took);
}
static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    if (!FL_STORE_OK(off, n))
        return -8;
    return fl_write(off, src, n);
}
#define ST_IDLE() fm1_wdt_feed()   /* (storage.c, SMK store owner check: between sector erases) */
#include "storage.c"

struct bootguard_s { uint32_t magic, failed, pending; };
extern struct bootguard_s bootguard;
#include "ota.c"
static uint32_t ota_now_ms(void) { return fm1_ms; }
static void ota_idle(void) { fm1_wdt_feed(); }
static int ota_in_area(uint32_t off, uint32_t n) { return FL_IN(off, n, OTA_AREA, OTA_AREA + OTA_AREA_LEN); }
static int ota_erase(uint32_t off)
{
    uint32_t took;
    if (!ota_in_area(off, 0x1000u) || (off & 0xFFFu))
        return -8;
    audio_silence();
    return fl_erase4k(off, &took);
}
static int ota_prog(uint32_t off, const void *p, uint32_t n)
{
    if (!ota_in_area(off, n))
        return -8;
    return fl_write(off, p, n);
}
static int ota_fread(uint32_t off, void *p, uint32_t n) { return st_read(off, p, n); }
static void ota_show(uint32_t step, int32_t code)
{
    static const char *const STEP[] = {"", "PACKAGE", "CHECK HEAD", "LOADER", "CONFIRM", "RESTART"};
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 92, 240, &FONT_S, "UPDATE", C_WHITE, 1);
    if (step < 9u) {
        draw_text_box(0, 124, 240, &FONT_S, STEP[step < 6u ? step : 0], C_HI, 1);
        return;
    }
    draw_text_box(0, 124, 240, &FONT_S, code == 1 ? "DRY RUN OK" : "FAILED", C_HI, 1);
    fm1_delay_ms(1500);
}
static void ota_commit(const uint8_t *parm)
{
    bootguard.pending = 0;                              /* intentional reset */
    bootguard.failed = 0;                               /* a new install starts with a clean slate */
    usb_detach();
    fm1_delay_ms(30);
    fm1_enter_update(parm);
}

#include "app/slots.c"
#include "app/x0x.h"
#include "app/panel.c"
#include "app/plat_fm1.c"
#include "app/project.c"
#include "app/ui.c"
#include "app/editor.c"
#include "app/main_fm1.c"
