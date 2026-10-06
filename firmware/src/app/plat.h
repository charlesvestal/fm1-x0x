/* SPDX-License-Identifier: GPL-3.0-only */
/* X0X platform interface: everything the app needs from the hardware.
 *
 * The device implementation (plat_fm1.c) wraps Felucca's HAL and panel map; the
 * host implementation (host/plat_host.c) is a simulator fed by a script, which is
 * what lets the whole app — UI, sequencer, engines — run and be tested on a Mac.
 * Drawing goes through Felucca's lcd_fill / lcd_blit (lcd.c on the device, a
 * framebuffer on the host) and gfx.c on top. */
#pragma once
#include <stdint.h>

/* big zero-initialised buffers go in the .pool RAM region on the device */
#ifdef X0X_HOST
#define X0X_POOL
#else
#define X0X_POOL __attribute__((section(".pool")))
#endif

/* panel controls, by printed label (Felucca's panel.c map turns these into matrix ids; Felucca
 * calls the second button SCL, the panel prints SEL) */
enum { B_FX, B_SEL, B_ENV, B_LFO, B_EDIT, B_GLO, B_HOME, B_SAVE, B_ARP, B_SEQ, B_PLAY, B_REC,
       B_OCTDN, B_OCTUP, NB };
enum { EN_SELECT, EN_ALGO, EN_PRESET, EN_K1, EN_K2, EN_K3, EN_K4, NE };

#define NKEYS 27                       /* F3 .. G5 */

uint32_t plat_ms(void);
uint32_t plat_buttons(void);           /* held: bit B_* */
uint32_t plat_keys(void);              /* held: bit k = key k (0 = F3) */
int32_t plat_enc(int role);            /* detents since the last call, + = clockwise; EN_* */
uint32_t plat_master(void);            /* MASTER pot, 0..4096 gain Q12 */
void plat_leds(uint32_t buttons, uint32_t keys);   /* lit: bit B_*, bit key */

/* USB MIDI, 4-byte USB-MIDI event packets (cable 0) */
int plat_midi_in(uint32_t *pkt);       /* 1 = got one (called from the audio ISR) */
void plat_midi_out(uint32_t pkt);      /* queue one (audio ISR or main loop) */

/* persistent storage (Felucca storage.c objects: A/B sectors, CRC, torn-write safe) */
int plat_store_load(uint32_t obj, void *dst, uint32_t max);   /* bytes loaded, < 0 = none / bad */
int plat_store_save(uint32_t obj, const void *src, uint32_t len);   /* 0 = ok */
#define PLAT_STORE_MAX 3840u           /* bytes per object */
enum { OBJ_SET, OBJ_SOUND, OBJ_PAT0, OBJ_PAT1, OBJ_PANEL, OBJ_SONG, OBJ_MOTION, OBJ_NOBJ };
/* flash: OBJ_SET 0xFC000; SOUND / PAT0 / PAT1 / SONG 0x97000.. (2 sectors each); OBJ_PANEL 0xDC000,
 * OBJ_MOTION 0xDE000 (all inside Felucca's data area, 0x97000..0xDFFFF) */

/* user sample slots (break loops), read in place; 0 = empty slot */
#define PLAT_NSLOTS 3
/* every zone of a slot is one loop (tools/upload_breaks.py packs several bars per slot) */
#define PLAT_SLOT_ZONES 16
int plat_slot_zones(int k);            /* loops in slot k, 0 = empty */
/* slot k, zone z: IMA ADPCM from predictor 0 / index 0, *nsamples, *rate in Hz; name = the slot's */
const uint8_t *plat_slot(int k, int z, uint32_t *nsamples, uint32_t *rate, char name[9]);

/* audio load in % of the render budget, and xruns since boot (device: from the ISR) */
uint32_t plat_cpu_pct(void);
uint32_t plat_xruns(void);

/* performance (the PERF screen; firmware/hal/fm1_perf.h): a counter for timing stages, the CPU's
 * clock cycles when its counter runs, else the 24 MHz timer; which one it is; the 24 MHz timer;
 * the highest half-buffer load since the last call, %; the stall counters (fetch, read, write:
 * 0 = read, -1 = off), and switching them on (experimental: see fm1_perf.h) */
void plat_perf_init(void);
uint32_t plat_cycles(void);
int plat_cycles_cpu(void);
uint32_t plat_cycles_hz(void);           /* the counter's rate when it is not the CPU's */
uint32_t plat_ticks24(void);
uint32_t plat_cpu_peak_pct(void);
int plat_stalls(uint32_t s[3]);
void plat_stalls_enable(int on);
