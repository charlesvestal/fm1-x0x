/* SPDX-License-Identifier: GPL-3.0-only */
/* X0X in the browser: the host simulator (host/x0x_host.c: the whole app, UI, sequencer, engines,
 * against a simulated FM-1) compiled to WebAssembly and run in an AudioWorklet. The worklet asks
 * for audio; producing it advances the device's clock one millisecond at a time, which runs the
 * engine every 256 samples (the I2S half buffer) and the UI every 16 ms, as on the FM-1. The page
 * sends input, and reads the screen, the lights and the saved objects back through these exports.
 *
 *   web/emu/build.sh   ->  build/emu/x0x.wasm (+ the page) */
#include <stdint.h>

static void web_audio(const int32_t *blk, uint32_t n);
#include "../../host/x0x_host.c"

#undef __attribute__

/* output ring, stereo float, filled by run_ms, drained by web_render */
#define RING 4096u
static float ring[RING * 2];
static uint32_t ring_w, ring_r;
static void web_audio(const int32_t *blk, uint32_t n)
{
    uint32_t k;
    for (k = 0; k < n; k++) {
        uint32_t i = (ring_w++ % RING) * 2u;
        ring[i] = (float)blk[2 * k] / 8388608.0f;
        ring[i + 1] = (float)blk[2 * k + 1] / 8388608.0f;
    }
}

static float out_l[1024], out_r[1024];
__attribute__((used, visibility("default"))) float *web_out_l(void) { return out_l; }
__attribute__((used, visibility("default"))) float *web_out_r(void) { return out_r; }

/* n frames (<= 1024) into out_l / out_r: the device runs until they exist */
__attribute__((used, visibility("default"))) void web_render(uint32_t n)
{
    uint32_t k;
    if (n > 1024u)
        n = 1024u;
    while (ring_w - ring_r < n)
        run_ms(1);
    for (k = 0; k < n; k++) {
        uint32_t i = (ring_r++ % RING) * 2u;
        out_l[k] = ring[i];
        out_r[k] = ring[i + 1];
    }
}

__attribute__((used, visibility("default"))) void web_boot(void) { boot(); }
__attribute__((used, visibility("default"))) void web_buttons(uint32_t m) { held_btn = m; }
__attribute__((used, visibility("default"))) void web_keys(uint32_t m) { held_keys = m; }
__attribute__((used, visibility("default"))) void web_enc(int role, int32_t n)
{
    if (role >= 0 && role < NE)
        enc_acc[role] += n;
}
__attribute__((used, visibility("default"))) void web_master(uint32_t v) { master = v > 4096u ? 4096u : v; }
__attribute__((used, visibility("default"))) uint32_t web_lit_buttons(void) { return lit_btn; }
__attribute__((used, visibility("default"))) uint32_t web_lit_keys(void) { return lit_keys; }
__attribute__((used, visibility("default"))) uint16_t *web_fb(void) { return fb; }
__attribute__((used, visibility("default"))) uint32_t web_blits(void) { return blits; }
__attribute__((used, visibility("default"))) uint32_t web_now(void) { return now_ms; }
__attribute__((used, visibility("default"))) uint32_t web_cpu(void) { return cpu_pct; }

/* saved objects: the page keeps them (localStorage) and gives them back before boot */
__attribute__((used, visibility("default"))) uint8_t *web_store(uint32_t obj) { return obj < OBJ_NOBJ ? store[obj] : 0; }
__attribute__((used, visibility("default"))) int web_store_len(uint32_t obj) { return obj < OBJ_NOBJ ? store_len[obj] : -1; }
__attribute__((used, visibility("default"))) void web_store_set_len(uint32_t obj, int n)
{
    if (obj < OBJ_NOBJ)
        store_len[obj] = n < 0 ? -1 : n > (int)PLAT_STORE_MAX ? (int)PLAT_STORE_MAX : n;
}
__attribute__((used, visibility("default"))) uint32_t web_store_writes(void) { return store_writes; }
__attribute__((used, visibility("default"))) uint32_t web_nobj(void) { return OBJ_NOBJ; }
__attribute__((used, visibility("default"))) uint32_t web_store_max(void) { return PLAT_STORE_MAX; }

/* user break slots (the BREAK part's own loops, as tools/upload_breaks.py puts them on the FM-1):
 * the page decodes a file to mono 22050 Hz 16-bit into web_slot_pcm and hands it over one loop
 * (zone) at a time; it is IMA-encoded here exactly as the simulator's "slot" command does. A slot
 * holds what 80 KiB of flash holds on the device, all its loops together. */
#define WEB_SLOT_MAX (2u * (0x14000u - 512u))
static int16_t web_pcm[WEB_SLOT_MAX];
static char web_name[9];
__attribute__((used, visibility("default"))) int16_t *web_slot_pcm(void) { return web_pcm; }
__attribute__((used, visibility("default"))) uint32_t web_slot_max(void) { return WEB_SLOT_MAX; }
__attribute__((used, visibility("default"))) char *web_slot_namebuf(void) { return web_name; }
__attribute__((used, visibility("default"))) void web_slot_clear(int k)
{
    int z;
    if (k < 0 || k >= PLAT_NSLOTS)
        return;
    for (z = 0; z < PLAT_SLOT_ZONES; z++) {
        free(slot_data[k][z]);
        slot_data[k][z] = 0;
        slot_n[k][z] = 0;
    }
    slot_nz[k] = 0;
    slot_is_img[k] = 0;
    web_name[8] = 0;
    memcpy(slot_name[k], web_name, 9);                /* the name the page put in web_slot_namebuf */
}
/* loop z of slot k from the first n samples of web_slot_pcm: 0 = kept */
__attribute__((used, visibility("default"))) int web_slot_zone(int k, int z, uint32_t n, uint32_t rate)
{
    if (k < 0 || k >= PLAT_NSLOTS || z < 0 || z >= PLAT_SLOT_ZONES || !n)
        return -1;
    if (n > WEB_SLOT_MAX)
        n = WEB_SLOT_MAX;
    free(slot_data[k][z]);
    if (!(slot_data[k][z] = calloc(1, (n + 1) / 2 + 1)))
        return -1;
    ima_encode(web_pcm, n, slot_data[k][z]);
    slot_n[k][z] = n;
    slot_rate[k][z] = rate;
    if (z + 1 > slot_nz[k])
        slot_nz[k] = z + 1;
    return 0;
}
/* the slots changed: the break part reloads its loops (and the LOOPS page's names) */
__attribute__((used, visibility("default"))) void web_slots_changed(void) { engine_brk_loops(); }
