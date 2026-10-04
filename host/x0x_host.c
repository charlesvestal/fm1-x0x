/* SPDX-License-Identifier: GPL-3.0-only */
/* X0X on the host: the whole app — UI, project, engine, sequencer, every DSP part — run
 * against a simulated FM-1 driven by a script. Audio goes to a WAV, the screen to PNGs,
 * the key / button lights to a text dump, MIDI out to a log. The device-only parts
 * (HAL, USB, OTA, flash driver) are replaced by plat.h implemented here.
 *
 *   x0x_host SCRIPT [OUTDIR]
 *
 * Script, one command per line (# comments):
 *   wait MS                      run the device for MS milliseconds
 *   press BTN | release BTN | tap BTN     (BTN: FX SEL ENV LFO EDIT GLO HOME SAVE ARP SEQ PLAY REC OCT- OCT+)
 *   key K down|up | tapkey K     (K: 0..26 = F3..G5, or w0..w15 white keys, b0..b10 black keys)
 *   turn ENC N                   (ENC: SELECT ALGO PRESET K1 K2 K3 K4; N detents, signed, 80 ms apart)
 *   spin ENC N                   N detents at once (1 ms)
 *   master N                     MASTER pot 0..4096
 *   param T V I VALUE            a sound pot (engine.h target, voice, index), as its knob would
 *   slot K NAME A.wav [B.wav..]  user sample slot K (0..2): one loop (zone) per WAV, encoded here
 *   slotimg K F.hdr F.bin       slot K from tools/upload_breaks.py --dry-run's image (the device format)
 *   midi B0 B1 B2                incoming USB MIDI message (hex bytes)
 *   wav FILE | wavstop           start / stop recording the output
 *   peakreset                    restart the output peak (expect peak_db_min / peak_db_max)
 *   shot FILE.png                save the screen
 *   leds                         print the lit buttons and keys
 *   expect WHAT VALUE            check state (playing, pattern, cue, view-part, ...): exit 1 on mismatch;
 *                                VALUE "<N" / ">N" is a bound
 *   reboot                       save nothing; re-run boot from the simulated flash (persistence test)
 */
#define X0X_HOST 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#ifdef __APPLE__
#include <libproc.h>
#include <unistd.h>
#endif
#define __attribute__(x)

/* ------------------------------------------------------------ screen --- */
static uint16_t fb[240 * 240];          /* RGB565, byte-swapped as the panel takes it (gfx.c) */
static void lcd_sync(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    uint16_t sc = (uint16_t)(((c >> 8) & 0xFFu) | ((c & 0xFFu) << 8));
    for (j = y; j < y + h && j < 240u; j++)
        for (i = x; i < x + w && i < 240u; i++)
            fb[j * 240u + i] = sc;
}
static uint32_t blits;
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t i, j;
    blits++;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (x + i < 240u && y + j < 240u)
                fb[(y + j) * 240u + x + i] = px[j * w + i];
}
#include "../firmware/src/gfx.c"

/* ---------------------------------------------------------- platform --- */
#include "../firmware/src/app/plat.h"

static uint32_t now_ms, held_btn, held_keys, master = 2048;
static int32_t enc_acc[NE];
static uint32_t lit_btn, lit_keys;

uint32_t plat_ms(void) { return now_ms; }
uint32_t plat_buttons(void) { return held_btn; }
uint32_t plat_keys(void) { return held_keys; }
int32_t plat_enc(int role)
{
    int32_t v = enc_acc[role];
    enc_acc[role] = 0;
    return v;
}
uint32_t plat_master(void) { return master; }
void plat_leds(uint32_t b, uint32_t k)
{
    lit_btn = b;
    lit_keys = k;
}

#define MQ 256
static uint32_t min_q[MQ], mi_w, mi_r;
static FILE *midi_log;
int plat_midi_in(uint32_t *pkt)
{
    if (mi_r == mi_w)
        return 0;
    *pkt = min_q[mi_r++ % MQ];
    return 1;
}
static uint32_t midi_out_count, clock_out_count;
void plat_midi_out(uint32_t pkt)
{
    midi_out_count++;
    if (((pkt >> 8) & 0xFF) == 0xF8)
        clock_out_count++;
    else if (midi_log)
        fprintf(midi_log, "%u ms: %02X %02X %02X\n", now_ms, (pkt >> 8) & 0xFF, (pkt >> 16) & 0xFF, (pkt >> 24) & 0xFF);
}

/* flash: one buffer per object, kept across a simulated reboot */
static uint8_t store[OBJ_NOBJ][PLAT_STORE_MAX];
static int store_len[OBJ_NOBJ] = {-1, -1, -1, -1, -1, -1, -1};
static uint32_t store_writes;
int plat_store_load(uint32_t obj, void *dst, uint32_t max)
{
    int n = store_len[obj];
    if (obj >= OBJ_NOBJ || n < 0)
        return -1;
    if ((uint32_t)n > max)
        n = (int)max;
    memcpy(dst, store[obj], (size_t)n);
    return n;
}
int plat_store_save(uint32_t obj, const void *src, uint32_t len)
{
    if (obj >= OBJ_NOBJ || len > PLAT_STORE_MAX)
        return -1;
    memcpy(store[obj], src, len);
    store_len[obj] = (int)len;
    store_writes++;
    return 0;
}

/* sample slots, two ways in:
 *  - slotimg: a slot image exactly as tools/upload_breaks.py writes it, read by the firmware's own
 *    slots.c (the device path: what the upload tool writes is what the firmware parses);
 *  - slot: WAVs encoded here, one zone each (quick, for scenarios that do not test the format). */
static uint8_t slot_img[3][0x14000];
static int slot_is_img[3];
#define SMP_USER_XIP(k) ((const uint8_t *)slot_img[k])
static uint32_t st_crc32(const void *p, uint32_t n)
{
    const uint8_t *b = p;
    uint32_t c = 0xFFFFFFFFu, k;
    while (n--) {
        c ^= *b++;
        for (k = 0; k < 8; k++)
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return ~c;
}
#include "../firmware/src/app/slots.c"

/* WAV slots */
static uint8_t *slot_data[PLAT_NSLOTS][PLAT_SLOT_ZONES];
static uint32_t slot_n[PLAT_NSLOTS][PLAT_SLOT_ZONES], slot_rate[PLAT_NSLOTS][PLAT_SLOT_ZONES];
static int slot_nz[PLAT_NSLOTS];
static char slot_name[PLAT_NSLOTS][9];
int plat_slot_zones(int k)
{
    if (k < 0 || k >= PLAT_NSLOTS)
        return 0;
    return slot_is_img[k] ? usr_nz[k] : slot_nz[k];
}
const uint8_t *plat_slot(int k, int z, uint32_t *ns, uint32_t *rate, char name[9])
{
    if (k >= 0 && k < PLAT_NSLOTS && slot_is_img[k]) {    /* plat_fm1.c's plat_slot, verbatim */
        const smp_user_hdr_t *h;
        uint32_t i;
        if (z < 0 || z >= usr_nz[k])
            return 0;
        h = (const smp_user_hdr_t *)smp_user_xip((uint32_t)k);
        for (i = 0; i < 8u; i++)
            name[i] = h->name[i] >= 32 && h->name[i] < 127 ? h->name[i] : 0;
        name[8] = 0;
        *ns = h->zone[z].n;
        *rate = (h->zone[z].rate * 44100u + 32768u) >> 16;
        return smp_user_xip((uint32_t)k) + SMP_USER_DATA + h->zone[z].off;
    }
    if (k < 0 || k >= PLAT_NSLOTS || z < 0 || z >= slot_nz[k])
        return 0;
    *ns = slot_n[k][z];
    *rate = slot_rate[k][z];
    memcpy(name, slot_name[k], 9);
    return slot_data[k][z];
}

static uint32_t cpu_pct, cpu_peak;
uint32_t plat_cpu_pct(void) { return cpu_pct; }
uint32_t plat_xruns(void) { return 0; }
/* performance: no CPU counter here: the "cycles" are nanoseconds (plat_cycles_hz) */
static uint64_t host_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
void plat_perf_init(void) {}
uint32_t plat_cycles(void) { return (uint32_t)host_ns(); }
uint32_t plat_cycles_hz(void) { return 1000000000u; }
int plat_cycles_cpu(void) { return 0; }
uint32_t plat_ticks24(void) { return (uint32_t)(host_ns() * 24u / 1000u); }
uint32_t plat_cpu_peak_pct(void)
{
    uint32_t p = cpu_peak;
    cpu_peak = 0;
    return p;
}
int plat_stalls(uint32_t s[3]) { (void)s; return -1; }
void plat_stalls_enable(int on) { (void)on; }

/* -------------------------------------------------------------- app --- */
#include "../firmware/src/app/x0x.h"
#include "../firmware/src/app/project.c"
#include "../firmware/src/app/ui.c"

/* ---------------------------------------------------------- outputs --- */
static void png_write(const char *path)
{
    /* RGB PNG with stored (uncompressed) deflate blocks: no zlib needed */
    static uint8_t raw[240 * (1 + 240 * 3)];
    FILE *f = fopen(path, "wb");
    uint32_t crc_t[256], i, j, k, n = 0, a = 1, b = 0, pos, len = sizeof raw;
    uint8_t hdr[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        return;
    }
    for (i = 0; i < 256; i++) {
        uint32_t c = i;
        for (k = 0; k < 8; k++)
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_t[i] = c;
    }
    for (j = 0; j < 240; j++) {
        raw[n++] = 0;
        for (i = 0; i < 240; i++) {
            uint16_t p = fb[j * 240 + i];
            p = (uint16_t)((p >> 8) | (p << 8));
            raw[n++] = (uint8_t)(((p >> 11) & 31) * 255 / 31);
            raw[n++] = (uint8_t)(((p >> 5) & 63) * 255 / 63);
            raw[n++] = (uint8_t)((p & 31) * 255 / 31);
        }
    }
    for (i = 0; i < len; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
#define PUT32(v) do { uint8_t q_[4] = {(uint8_t)((v) >> 24), (uint8_t)((v) >> 16), (uint8_t)((v) >> 8), (uint8_t)(v)}; fwrite(q_, 1, 4, f); } while (0)
    fwrite(hdr, 1, 8, f);
    {
        uint8_t ih[17] = {'I', 'H', 'D', 'R', 0, 0, 0, 240, 0, 0, 0, 240, 8, 2, 0, 0, 0};
        uint32_t c = 0xFFFFFFFFu;
        PUT32(13u);
        fwrite(ih, 1, 17, f);
        for (i = 0; i < 17; i++)
            c = crc_t[(c ^ ih[i]) & 255] ^ (c >> 8);
        PUT32(~c);
    }
    {
        uint32_t nblk = (len + 65534) / 65535, zlen = 2 + len + nblk * 5 + 4, c = 0xFFFFFFFFu;
        uint8_t *z = malloc(zlen + 4), *q = z;
        memcpy(q, "IDAT", 4);
        q += 4;
        *q++ = 0x78;
        *q++ = 0x01;
        for (pos = 0; pos < len;) {
            uint32_t m = len - pos > 65535 ? 65535 : len - pos;
            *q++ = pos + m >= len ? 1 : 0;
            *q++ = (uint8_t)m;
            *q++ = (uint8_t)(m >> 8);
            *q++ = (uint8_t)~m;
            *q++ = (uint8_t)(~m >> 8);
            memcpy(q, raw + pos, m);
            q += m;
            pos += m;
        }
        *q++ = (uint8_t)(b >> 8);
        *q++ = (uint8_t)b;
        *q++ = (uint8_t)(a >> 8);
        *q++ = (uint8_t)a;
        PUT32(zlen);
        fwrite(z, 1, zlen + 4, f);
        for (i = 0; i < zlen + 4; i++)
            c = crc_t[(c ^ z[i]) & 255] ^ (c >> 8);
        PUT32(~c);
        free(z);
    }
    {
        uint8_t ie[4] = {'I', 'E', 'N', 'D'};
        uint32_t c = 0xFFFFFFFFu;
        PUT32(0u);
        fwrite(ie, 1, 4, f);
        for (i = 0; i < 4; i++)
            c = crc_t[(c ^ ie[i]) & 255] ^ (c >> 8);
        PUT32(~c);
    }
    fclose(f);
}

static FILE *wav;
static uint32_t wav_frames;
static float peak_out;
static void wav_open(const char *path)
{
    static const uint8_t h[44] = {0};
    wav = fopen(path, "wb");
    if (wav)
        fwrite(h, 1, 44, wav);
    wav_frames = 0;
}
static void wav_close(void)
{
    uint32_t bytes = wav_frames * 4, v;
    if (!wav)
        return;
    fseek(wav, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, wav);
    v = 36 + bytes;
    fwrite(&v, 4, 1, wav);
    fwrite("WAVEfmt ", 1, 8, wav);
    v = 16;
    fwrite(&v, 4, 1, wav);
    {
        uint16_t fmt[2] = {1, 2};
        uint32_t r[2] = {44100, 44100 * 4};
        uint16_t al[2] = {4, 16};
        fwrite(fmt, 2, 2, wav);
        fwrite(r, 4, 2, wav);
        fwrite(al, 2, 2, wav);
    }
    fwrite("data", 1, 4, wav);
    fwrite(&bytes, 4, 1, wav);
    fclose(wav);
    wav = 0;
}

/* --------------------------------------------------------- the device --- */
static double audio_due_ms;
static uint32_t ui_due;
static double render_ns_total, render_budget_ns_total;
/* instructions the kernel counted in engine_render (Felucca's tests/regress.c measure: the same
 * on every run, unlike wall time): per sample overall, and the worst block */
static uint64_t ins_total, ins_frames, ins_block_max;
/* the engine's stage timing (eng_prof, in plat_cycles() units: here nanoseconds), summed per block
 * while playing; printed with X0X_PROFILE=1 in the environment */
static uint64_t prof_tot[ENG_PROF_N];
static uint64_t instr_now(void)
{
#ifdef __APPLE__
    struct rusage_info_v4 ri;
    if (!proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t *)&ri))
        return ri.ri_instructions;
#endif
    return 0;
}

#include <time.h>
static double now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

static void boot(void)
{
    engine_init(proj.pat, &proj.arr.song, proj.arr.lane, &proj.sound);
    if (project_load() != 0)
        project_defaults();
    engine_apply_sound(&proj.sound);
    seq.bpm = (float)proj.set.bpm_x10 / 10.0f;
    seq.accent_q7 = proj.set.accent_q7;
    seq.send_clock = proj.set.clk_out;
    seq.send_notes = proj.set.notes_out;
    engine_brk_loops();
    lcd_fill(0, 0, 240, 240, C_BLACK);
    ui_init();
}

static uint8_t last_pat = 0xFF;
static void run_ms(uint32_t ms)
{
    static int32_t blk[512];
    uint32_t i;
    for (i = 0; i < ms; i++) {
        now_ms++;
        while (audio_due_ms <= (double)now_ms) {      /* 256-frame half buffers, as the I2S DMA */
            double t0 = now_ns(), dt;
            uint32_t k;
            uint64_t i0 = instr_now(), di;
            uint32_t pr0[ENG_PROF_N];
            for (k = 0; k < ENG_PROF_N; k++)
                pr0[k] = eng_prof[k];
            engine_render(blk, 256);
#ifdef X0X_WEB
            web_audio(blk, 256);                         /* the browser build: to the AudioWorklet */
#endif
            di = instr_now() - i0;
            dt = now_ns() - t0;
            if (seq.playing) {                           /* the cost that matters: while it plays */
                for (k = 0; k < ENG_PROF_N; k++)
                    prof_tot[k] += (uint32_t)(eng_prof[k] - pr0[k]);
                ins_total += di;
                ins_frames += 256;
                if (di > ins_block_max)
                    ins_block_max = di;
            }
            render_ns_total += dt;
            render_budget_ns_total += 256.0 * 1e9 / 44100.0;
            cpu_pct = (uint32_t)(100.0 * dt / (256.0 * 1e9 / 44100.0));
            if (cpu_pct > cpu_peak)
                cpu_peak = cpu_pct;
            for (k = 0; k < 256; k++) {
                float l = (float)blk[2 * k] / 8388608.0f;
                int16_t s[2];
                if (fabsf(l) > peak_out)
                    peak_out = fabsf(l);
                s[0] = s[1] = (int16_t)(l * 32767.0f);
                if (wav) {
                    fwrite(s, 2, 2, wav);
                    wav_frames++;
                }
            }
            audio_due_ms += 256.0 * 1000.0 / 44100.0;
        }
        if (seq.ppat[TRK_BRK] != last_pat) {
            last_pat = seq.ppat[TRK_BRK];
            engine_brk_loops();
        }
        if (now_ms >= ui_due) {
            ui_due = now_ms + 16;
            ui_frame();
        } else {
            ui_input_only();
        }
    }
}

/* ---------------------------------------------------------- script --- */
static const char *const BTN_N[NB] = {"FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ",
                                      "PLAY", "REC", "OCT-", "OCT+"};
static const char *const ENC_N[NE] = {"SELECT", "ALGO", "PRESET", "K1", "K2", "K3", "K4"};
static const int WHITE_K[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};
static const int BLACK_K[11] = {1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25};

static int find_name(const char *const *t, int n, const char *s)
{
    int i;
    for (i = 0; i < n; i++)
        if (!strcmp(t[i], s))
            return i;
    return -1;
}

static int key_of(const char *s)
{
    if (s[0] == 'w')
        return WHITE_K[atoi(s + 1) & 15];
    if (s[0] == 'b')
        return BLACK_K[atoi(s + 1) % 11];
    return atoi(s);
}

/* IMA ADPCM encoder (tools/sampleio.py's) */
static const int16_t IMA_STEP_T[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767};
static const int8_t IMA_IDX_T[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
static void ima_encode(const int16_t *x, uint32_t n, uint8_t *out)
{
    int pred = 0, idx = 0;
    uint32_t i;
    for (i = 0; i < n; i++) {
        int step = IMA_STEP_T[idx], diff = x[i] - pred, code = 0, vd = step >> 3;
        if (diff < 0) {
            code = 8;
            diff = -diff;
        }
        if (diff >= step) {
            code |= 4;
            diff -= step;
            vd += step;
        }
        if (diff >= step >> 1) {
            code |= 2;
            diff -= step >> 1;
            vd += step >> 1;
        }
        if (diff >= step >> 2) {
            code |= 1;
            vd += step >> 2;
        }
        pred = code & 8 ? pred - vd : pred + vd;
        pred = pred > 32767 ? 32767 : pred < -32768 ? -32768 : pred;
        idx += IMA_IDX_T[code & 7];
        idx = idx < 0 ? 0 : idx > 88 ? 88 : idx;
        if (i & 1)
            out[i >> 1] |= (uint8_t)(code << 4);
        else
            out[i >> 1] = (uint8_t)code;
    }
}

static int load_slot(int k, int z, const char *path)
{
    FILE *f = fopen(path, "rb");
    uint8_t h[12];
    uint32_t rate = 44100, n = 0;
    uint16_t ch = 1, bits = 16;
    int16_t *x = 0;
    if (!f)
        return -1;
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        fclose(f);
        return -1;
    }
    for (;;) {
        uint8_t c[8];
        uint32_t sz;
        if (fread(c, 1, 8, f) != 8)
            break;
        sz = (uint32_t)c[4] | (uint32_t)c[5] << 8 | (uint32_t)c[6] << 16 | (uint32_t)c[7] << 24;
        if (!memcmp(c, "fmt ", 4)) {
            uint8_t fm[16];
            if (fread(fm, 1, 16, f) != 16)
                break;
            ch = (uint16_t)(fm[2] | fm[3] << 8);
            rate = (uint32_t)fm[4] | (uint32_t)fm[5] << 8 | (uint32_t)fm[6] << 16 | (uint32_t)fm[7] << 24;
            bits = (uint16_t)(fm[14] | fm[15] << 8);
            fseek(f, (long)(sz - 16 + (sz & 1)), SEEK_CUR);
        } else if (!memcmp(c, "data", 4)) {
            uint32_t frames = sz / (ch * (bits / 8u));
            uint8_t *raw = malloc(sz);
            if (fread(raw, 1, sz, f) != sz) {
                free(raw);
                break;
            }
            /* to mono 22050 (the slots' rate): average channels, decimate by 2 when 44.1k */
            {
                uint32_t step = rate >= 44100 ? 2 : 1, j;
                x = malloc(sizeof(int16_t) * (frames / step + 1));
                for (j = 0; j + step <= frames; j += step) {
                    int32_t acc = 0;
                    uint32_t s, cc;
                    for (s = 0; s < step; s++)
                        for (cc = 0; cc < ch; cc++) {
                            const uint8_t *p = raw + ((j + s) * ch + cc) * (bits / 8u);
                            int32_t v = bits == 16 ? (int16_t)(p[0] | p[1] << 8) : (int32_t)((p[0] | p[1] << 8 | p[2] << 16) << 8) >> 16;
                            acc += v;
                        }
                    x[n++] = (int16_t)(acc / (int32_t)(step * ch));
                }
                rate /= step;
            }
            free(raw);
            break;
        } else {
            fseek(f, (long)(sz + (sz & 1)), SEEK_CUR);
        }
    }
    fclose(f);
    if (!x || !n)
        return -1;
    if (n > 2u * (0x14000u - 512u))
        n = 2u * (0x14000u - 512u);                  /* an 80 KiB slot */
    free(slot_data[k][z]);
    slot_data[k][z] = calloc(1, (n + 1) / 2 + 1);
    ima_encode(x, n, slot_data[k][z]);
    slot_n[k][z] = n;
    slot_rate[k][z] = rate;
    if (z + 1 > slot_nz[k])
        slot_nz[k] = z + 1;
    free(x);
    return 0;
}

static int expect(const char *what, const char *val)
{
    int got;
    if (!strcmp(what, "playing"))
        got = seq.playing;
    else if (!strcmp(what, "pattern"))
        got = seq.ppat[PART_909] + 1;
    else if (!strcmp(what, "cue"))
        got = seq_cue_of(&seq, PART_909) + 1;
    else if (!strncmp(what, "ppat", 4))            /* ppatP: part P's pattern, 1-based */
        got = seq.ppat[atoi(what + 4) % NPARTS] + 1;
    else if (!strcmp(what, "perftest"))            /* -1 idle, 0..2 running, 3 done */
        got = perf.test;
    else if (!strncmp(what, "perfres", 7))         /* perfresK: scenario K's share of the CPU x 10 */
        got = perf.res_load[atoi(what + 7) % 3];
    else if (!strcmp(what, "patptr"))              /* 1: the sequencer plays the project's patterns */
        got = seq.pat == proj.pat;
    else if (!strcmp(what, "songon"))
        got = seq.song_on;
    else if (!strcmp(what, "songlen"))
        got = proj.arr.song.len;
    else if (!strcmp(what, "songpos"))
        got = seq.song_pos + 1;
    else if (!strncmp(what, "songbar", 7)) {       /* songbarK.P: bar K (1-based) part P's pattern, 1-based; P 5 = mutes */
        int k, q;
        sscanf(what, "songbar%d.%d", &k, &q);
        got = q < NPARTS ? proj.arr.song.bar[k - 1].pat[q] + 1 : proj.arr.song.bar[k - 1].mute;
    } else if (!strcmp(what, "lanes"))
        got = motion_count(proj.arr.lane);
    else if (!strncmp(what, "motion", 6)) {        /* motionT.V.I: what that knob's lane plays now (-1 none) */
        int t, v, i, part;
        sscanf(what, "motion%d.%d.%d", &t, &v, &i);
        part = engine_motion_part(t, v);
        got = engine_motion_value(motion_find(proj.arr.lane, seq.ppat[part], part, t, v, i));
    } else if (!strncmp(what, "laneval", 7)) {     /* lanevalT.V.I.S: the lane's value on step S (255 none, -1 no lane) */
        int t, v, i, st, part, k;
        sscanf(what, "laneval%d.%d.%d.%d", &t, &v, &i, &st);
        part = engine_motion_part(t, v);
        k = motion_find(proj.arr.lane, seq.ppat[part], part, t, v, i);
        got = k < 0 ? -1 : proj.arr.lane[k].val[st];
        if (k >= 0 && getenv("X0X_LANEDUMP")) {
            int j;
            for (j = 0; j < 16; j++)
                printf(" %d", proj.arr.lane[k].val[j]);
            printf("  (lane %d)\n", k);
        }
    }
    else if (!strcmp(what, "part"))
        got = ui.part;
    else if (!strcmp(what, "sel"))                  /* the selected track of the part on screen */
        got = ui.sel[ui.part];
    else if (!strcmp(what, "view"))
        got = ui.view;
    else if (!strcmp(what, "tempo"))
        got = (int)(seq.bpm + 0.5f);
    else if (!strcmp(what, "clocks_out"))
        got = (int)clock_out_count;
    else if (!strcmp(what, "store_writes"))
        got = (int)store_writes;
    else if (!strcmp(what, "peak_db_max")) {
        double db = 20.0 * log10(peak_out > 1e-9f ? peak_out : 1e-9f);
        if (db > atof(val)) {
            printf("FAIL expect peak <= %s dBFS, got %.1f\n", val, db);
            return 1;
        }
        printf("  ok peak %.1f dBFS\n", db);
        return 0;
    } else if (!strcmp(what, "peak_db_min")) {
        double db = 20.0 * log10(peak_out > 1e-9f ? peak_out : 1e-9f);
        if (db < atof(val)) {
            printf("FAIL expect peak >= %s dBFS, got %.1f\n", val, db);
            return 1;
        }
        printf("  ok peak %.1f dBFS\n", db);
        return 0;
    } else if (!strncmp(what, "hit", 3)) {         /* hitK.V.S: kit K voice V step S set */
        int k, v, s;
        sscanf(what, "hit%d.%d.%d", &k, &v, &s);
        got = (int)((proj.pat[seq.ppat[k]].drum[k].hit[v] >> s) & 1u);
    } else if (!strncmp(what, "gate", 4)) {        /* gateB.S: 303 B step S gate */
        int b, s;
        sscanf(what, "gate%d.%d", &b, &s);
        got = bstep_gate(&proj.pat[seq.ppat[PART_303A + b]].bass[b].step[s]);
    } else if (!strncmp(what, "note", 4)) {
        int b, s;
        sscanf(what, "note%d.%d", &b, &s);
        got = proj.pat[seq.ppat[PART_303A + b]].bass[b].step[s].note;
    } else if (!strncmp(what, "brk", 3)) {
        int s;
        sscanf(what, "brk%d", &s);
        got = (int)((proj.pat[seq.ppat[PART_BRK]].brk.steps >> s) & 1u);
    } else if (!strncmp(what, "sound", 5)) {       /* soundT.V.I */
        int t, v, i;
        sscanf(what, "sound%d.%d.%d", &t, &v, &i);
        got = proj.sound.v[t][v][i];
    } else if (!strcmp(what, "mute"))
        got = (int)seq.mute;
    else if (!strcmp(what, "loops"))
        got = engine_brk_nslots();
    else if (!strcmp(what, "loop_a"))
        got = proj.pat[seq.ppat[PART_BRK]].brk.slot_a;
    else {
        printf("FAIL unknown expect %s\n", what);
        return 1;
    }
    if (val[0] == '<' || val[0] == '>') {          /* a bound: "<30", ">0" */
        int lim = (int)strtol(val + 1, 0, 0);
        if (val[0] == '<' ? got >= lim : got <= lim) {
            printf("FAIL expect %s %s, got %d\n", what, val, got);
            return 1;
        }
        printf("  ok %s == %d (%s)\n", what, got, val);
        return 0;
    }
    if (got != (int)strtol(val, 0, 0)) {
        printf("FAIL expect %s == %s, got %d\n", what, val, got);
        return 1;
    }
    printf("  ok %s == %d\n", what, got);
    return 0;
}

#ifndef X0X_WEB
int main(int argc, char **argv)
{
    FILE *sc;
    char line[512], out[400];
    const char *dir = argc > 2 ? argv[2] : ".";
    int lineno = 0, fails = 0;
    if (argc < 2) {
        fprintf(stderr, "usage: x0x_host SCRIPT [OUTDIR]\n");
        return 2;
    }
    sc = fopen(argv[1], "r");
    if (!sc) {
        perror(argv[1]);
        return 2;
    }
    snprintf(out, sizeof out, "%s/midi_out.txt", dir);
    midi_log = fopen(out, "w");
    boot();
    while (fgets(line, sizeof line, sc)) {
        char cmd[32] = {0}, a[256] = {0}, b[64] = {0}, c[64] = {0}, line_d[64] = {0};
        lineno++;
        if (line[0] == '#' || sscanf(line, "%31s %255s %63s %63s %63s", cmd, a, b, c, line_d) < 1)
            continue;
        if (!strcmp(cmd, "wait"))
            run_ms((uint32_t)atoi(a));
        else if (!strcmp(cmd, "press") || !strcmp(cmd, "release") || !strcmp(cmd, "tap")) {
            int i = find_name(BTN_N, NB, a);
            if (i < 0) {
                printf("line %d: no button %s\n", lineno, a);
                return 2;
            }
            if (cmd[0] != 'r')
                held_btn |= 1u << i;
            if (cmd[0] == 't')
                run_ms(40);
            if (cmd[0] != 'p')
                held_btn &= ~(1u << i);
            run_ms(cmd[0] == 't' ? 40 : 2);
        } else if (!strcmp(cmd, "key") || !strcmp(cmd, "tapkey")) {
            int k = key_of(a);
            if (cmd[0] == 't') {
                held_keys |= 1u << k;
                run_ms(60);
                held_keys &= ~(1u << k);
                run_ms(30);
            } else {
                if (!strcmp(b, "down"))
                    held_keys |= 1u << k;
                else
                    held_keys &= ~(1u << k);
                run_ms(2);
            }
        } else if (!strcmp(cmd, "turn")) {
            int e = find_name(ENC_N, NE, a), n = atoi(b), s = n > 0 ? 1 : -1;
            if (e < 0) {
                printf("line %d: no encoder %s\n", lineno, a);
                return 2;
            }
            for (; n; n -= s) {                       /* one detent every 80 ms: no acceleration */
                enc_acc[e] += s;
                run_ms(80);
            }
        } else if (!strcmp(cmd, "spin")) {              /* spin ENC N: all N detents at once (a fast hand) */
            int e = find_name(ENC_N, NE, a);
            if (e < 0) {
                printf("line %d: no encoder %s\n", lineno, a);
                return 2;
            }
            enc_acc[e] += atoi(b);
            run_ms(1);
        } else if (!strcmp(cmd, "param")) {             /* param T V I VALUE: a sound pot, as a knob would set it */
            int t = atoi(a), v = atoi(b), i = atoi(c), val = atoi(line_d);
            if (t < 0 || t >= NTARGETS || v < 0 || v >= NVOICES_MAX || i < 0 || i >= NPARAMS_MAX) {
                printf("line %d: no param %d %d %d\n", lineno, t, v, i);
                return 2;
            }
            proj.sound.v[t][v][i] = (uint8_t)val;
            engine_set(t, v, i, val);
            run_ms(1);
        } else if (!strcmp(cmd, "master"))
            master = (uint32_t)atoi(a);
        else if (!strcmp(cmd, "slot")) {                /* slot K NAME a.wav [b.wav ...]: one zone each */
            char *tok, *rest = line + 4;
            int k, z = 0;
            strtok(rest, " \t\n");                          /* K */
            k = atoi(a);
            tok = strtok(0, " \t\n");                      /* NAME */
            snprintf(slot_name[k], sizeof slot_name[k], "%s", tok ? tok : "");
            slot_nz[k] = 0;
            while ((tok = strtok(0, " \t\n")) && z < PLAT_SLOT_ZONES) {
                if (load_slot(k, z++, tok)) {
                    printf("line %d: cannot load %s\n", lineno, tok);
                    return 2;
                }
            }
            engine_brk_loops();
        } else if (!strcmp(cmd, "slotimg")) {          /* slotimg K FILE.hdr FILE.bin: as uploaded */
            int k = atoi(a);
            FILE *fh = fopen(b, "rb"), *fd = fopen(c, "rb");
            size_t nh, nd;
            if (!fh || !fd || k < 0 || k > 2) {
                printf("line %d: cannot read the slot image\n", lineno);
                return 2;
            }
            memset(slot_img[k], 0xFF, sizeof slot_img[k]);       /* erased flash */
            nh = fread(slot_img[k], 1, sizeof(smp_user_hdr_t), fh);   /* SMP_END writes the header, */
            nd = fread(slot_img[k] + SMP_USER_DATA, 1, sizeof slot_img[k] - SMP_USER_DATA, fd);   /* SMP_WRITE the data */
            fclose(fh);
            fclose(fd);
            (void)nh;
            {
                const smp_user_hdr_t *h = (const smp_user_hdr_t *)slot_img[k];
                if (st_crc32(slot_img[k] + SMP_USER_DATA, h->data_len) != h->crc || nd < h->data_len) {
                    printf("line %d: slot image CRC mismatch (the device would refuse it)\n", lineno);
                    return 2;
                }
            }
            slot_is_img[k] = 1;
            smp_user_scan((uint32_t)k);
            printf("  slot %d: %d zone(s) from the image\n", k, usr_nz[k]);
            engine_brk_loops();
        } else if (!strcmp(cmd, "midi")) {
            uint32_t s = (uint32_t)strtoul(a, 0, 16), d1 = (uint32_t)strtoul(b, 0, 16), d2 = (uint32_t)strtoul(c, 0, 16);
            uint32_t cin = s >= 0xF0 ? 0x0F : s >> 4;
            min_q[mi_w++ % MQ] = cin | s << 8 | d1 << 16 | d2 << 24;
        } else if (!strcmp(cmd, "wav")) {
            snprintf(out, sizeof out, "%s/%s", dir, a);
            wav_open(out);
        } else if (!strcmp(cmd, "peakreset"))
            peak_out = 0.0f;
        else if (!strcmp(cmd, "wavstop"))
            wav_close();
        else if (!strcmp(cmd, "shot")) {
            snprintf(out, sizeof out, "%s/%s", dir, a);
            png_write(out);
        } else if (!strcmp(cmd, "leds")) {
            int i;
            printf("  leds: buttons");
            for (i = 0; i < NB; i++)
                if (lit_btn >> i & 1u)
                    printf(" %s", BTN_N[i]);
            printf(" | white");
            for (i = 0; i < 16; i++)
                printf("%c", (lit_keys >> WHITE_K[i] & 1u) ? '#' : '.');
            printf(" black");
            for (i = 0; i < 11; i++)
                printf("%c", (lit_keys >> BLACK_K[i] & 1u) ? '#' : '.');
            printf("\n");
        } else if (!strcmp(cmd, "expect"))
            fails += expect(a, b);
        else if (!strcmp(cmd, "reboot")) {
            memset(&proj, 0, sizeof proj);
            boot();
        } else if (!strcmp(cmd, "echo"))
            printf("%s", line + 5);
        else {
            printf("line %d: unknown command %s\n", lineno, cmd);
            return 2;
        }
    }
    wav_close();
    if (midi_log)
        fclose(midi_log);
    printf("host: %u ms simulated, %u blits, render %.1f%% of real time (host CPU)%s\n", now_ms, blits,
           render_budget_ns_total > 0 ? 100.0 * render_ns_total / render_budget_ns_total : 0.0,
           fails ? ", EXPECTATIONS FAILED" : "");
    if (getenv("X0X_PROFILE")) {
        static const char *const N[ENG_PROF_N] = {"909", "808", "303A", "303B", "BREAK", "FX", "MASTER", "SEQ"};
        uint64_t tot = 0;
        int k;
        for (k = 0; k < ENG_PROF_N; k++)
            tot += prof_tot[k];
        printf("profile (share of the render, ns per sample):");
        for (k = 0; k < ENG_PROF_N; k++)
            printf(" %s %.1f%% (%.1f)", N[k], tot ? 100.0 * (double)prof_tot[k] / (double)tot : 0.0,
                   ins_frames ? (double)prof_tot[k] / (double)ins_frames : 0.0);
        printf("\n");
    }
    if (ins_frames && ins_total)
        printf("host: playing, %.0f instructions / sample (mean), %.0f in the worst block\n",
               (double)ins_total / (double)ins_frames, (double)ins_block_max / 256.0);
    return fails ? 1 : 0;
}
#endif /* X0X_WEB */
