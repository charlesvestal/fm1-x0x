/* SPDX-License-Identifier: GPL-3.0-only */
/* The engines' output bit for bit: bass303, drum909 and drum808 rendered over scenarios (notes, accents, slides,
 * idle gaps, swept pots, every drive / shape, lite, odd block sizes, stereo pans, sends), each hashed (FNV-1a over
 * the float bits, signed zeros included) and compared with tests/host/dsp_hash.golden. A speed-up that is meant to
 * change no sample must leave every hash as it was.
 *   dsp_hash_test GOLDEN            compare
 *   dsp_hash_test GOLDEN --write    write the hashes (only when the sound is meant to change)
 * With DSP_HASH_BENCH=1 in the environment (macOS) it runs every scenario five times and prints the fewest host
 * instructions each took per sample (the count still varies by a few %: a measure of a speed-up on this machine,
 * not the device's count). */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <libproc.h>
#include <unistd.h>
#endif
#include "bass303.h"
#include "drum909.h"
#include "drum808.h"

#define NB 256
static uint32_t rng = 0x12345678u;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static uint64_t fnv(uint64_t h, const float *x, int n)
{
    int i, k;
    for (i = 0; i < n; i++) {
        uint32_t u;
        memcpy(&u, &x[i], 4);
        for (k = 0; k < 4; k++) {
            h ^= (u >> (8 * k)) & 0xFFu;
            h *= 0x100000001B3ull;
        }
    }
    return h;
}

#define NSC 64
static char names[NSC][40];
static uint64_t hashes[NSC];
static int nsc;
static uint64_t ins0;
static uint64_t instr_now(void)
{
#ifdef __APPLE__
    struct rusage_info_v4 ri;
    if (!proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t *)&ri))
        return ri.ri_instructions;
#endif
    return 0;
}
static double best[NSC];
static void result(const char *name, uint64_t h, int samples)
{
    double ips = (double)(instr_now() - ins0) / (double)samples;
    if (best[nsc] == 0.0 || ips < best[nsc])
        best[nsc] = ips;
    snprintf(names[nsc], sizeof names[nsc], "%s", name);
    hashes[nsc++] = h;
}

/* block sizes: fixed, or random 1..256 when blk is 0 */
static int blksz(int blk) { return blk ? blk : 1 + (int)(rnd() % NB); }

/* ---- bass303 ---- */
static bass303_t b3[2];
/* wave, drive type, lite (0 / 1 / 2 = switching), block, pots swept */
static void s303(const char *name, int wave, int drv, int lite, int blk, int sweep, int seconds)
{
    static float out[2][NB];
    uint64_t h = 0xCBF29CE484222325ull;
    int t = 0, total = 44100 * seconds, v, step = 0, next = 0, held = 0;
    ins0 = instr_now();
    rng = 0x9E3779B9u ^ (uint32_t)(wave * 7 + drv * 31 + lite * 131 + blk * 7919 + sweep);
    for (v = 0; v < 2; v++) {
        bass303_init(&b3[v]);
        bass303_set(&b3[v], BASS303_WAVE, wave);
        bass303_set(&b3[v], BASS303_DRVTYPE, drv);
        bass303_set(&b3[v], BASS303_DRIVE, drv ? 90 : 0);
        bass303_set_lite(&b3[v], lite == 1);
    }
    while (t < total) {
        int n = blksz(blk);
        if (n > total - t)
            n = total - t;
        if (t >= next) {                          /* a 16th at ~125 BPM; every 16 steps a 1.5 s rest */
            int note = 30 + (int)(rnd() % 30), acc = (rnd() % 3) == 0, sl = (rnd() % 4) == 0;
            if ((step & 15) == 15) {
                for (v = 0; v < 2; v++)
                    bass303_note_off(&b3[v]);
                held = 0;
                next = t + 66150;
            } else {
                for (v = 0; v < 2; v++) {
                    if ((rnd() % 5) == 0 && held) {
                        bass303_note_off(&b3[v]);
                    } else
                        bass303_note_on(&b3[v], note + 12 * v, acc, sl && held);
                }
                held = 1;
                next = t + 5292;
            }
            step++;
            if (sweep)
                for (v = 0; v < 2; v++) {
                    int p = (int)(rnd() % BASS303_NPARAMS), max = bass303_param(p)->max;
                    if (p == BASS303_WAVE || p == BASS303_DRVTYPE)
                        continue;
                    bass303_set(&b3[v], p, (int)(rnd() % (unsigned)(max + 1)));
                }
            if (lite == 2 && (step % 5) == 0)
                for (v = 0; v < 2; v++)
                    bass303_set_lite(&b3[v], (step / 5) & 1);
        }
        for (v = 0; v < 2; v++) {
            bass303_render(&b3[v], out[v], n);
            h = fnv(h, out[v], n);
        }
        t += n;
    }
    result(name, h, total);
}

/* ---- drums ---- */
static drum909_t d9;
static drum808_t d8;
static void rand_pots(int is909, int voice)
{
    int np = is909 ? drum909_nparams(voice) : drum808_nparams(voice), i;
    for (i = 0; i < np; i++) {
        const x0x_param_t *p = is909 ? drum909_param(voice, i) : drum808_param(voice, i);
        int val = (int)(rnd() % (unsigned)(p->max + 1));
        if ((rnd() % 3) == 0)
            val = p->def;
        if (is909)
            drum909_set(&d9, voice, i, val);
        else
            drum808_set(&d8, voice, i, val);
    }
}

/* mode: 0 the defaults, 1 random pots, 2 random pots and each voice let ring to its end (sparse hits);
 * stereo: render_st */
static void sdrum(const char *name, int is909, int mode, int stereo, int blk, int seconds)
{
    static float dl[NB], dr[NB], rv[NB], dy[NB];
    uint64_t h = 0xCBF29CE484222325ull;
    int t = 0, total = 44100 * seconds, next = 0, nv = is909 ? DR_NUM : D8_NUM, v;
    ins0 = instr_now();
    rng = 0x2545F491u ^ (uint32_t)(is909 * 17 + mode * 101 + stereo * 1009 + blk * 7919);
    if (is909)
        drum909_init(&d9);
    else
        drum808_init(&d8);
    if (mode)
        for (v = 0; v <= nv; v++)                 /* (nv: the kit) */
            rand_pots(is909, v);
    while (t < total) {
        int n = blksz(blk);
        if (n > total - t)
            n = total - t;
        if (t >= next) {
            int hits = mode == 2 ? 1 : 1 + (int)(rnd() % 4), k;
            for (k = 0; k < hits; k++) {
                float vel = (rnd() % 4) == 0 ? 1.0f : (float)(20 + rnd() % 100) / 127.0f;
                v = (int)(rnd() % (unsigned)nv);
                if (mode && (rnd() % 4) == 0)
                    rand_pots(is909, v);
                if (is909)
                    drum909_trigger(&d9, v, vel);
                else
                    drum808_trigger(&d8, v, vel);
            }
            next = t + (mode == 2 ? 44100 * 3 + (int)(rnd() % 44100) : 2646 + (int)(rnd() % 8000));
        }
        memset(dl, 0, sizeof dl);
        memset(dr, 0, sizeof dr);
        memset(rv, 0, sizeof rv);
        memset(dy, 0, sizeof dy);
        if (is909) {
            if (stereo)
                drum909_render_st(&d9, dl, dr, rv, dy, n);
            else
                drum909_render(&d9, dl, rv, dy, n);
        } else {
            if (stereo)
                drum808_render_st(&d8, dl, dr, rv, dy, n);
            else
                drum808_render(&d8, dl, rv, dy, n);
        }
        h = fnv(h, dl, n);
        if (stereo)
            h = fnv(h, dr, n);
        h = fnv(h, rv, n);
        h = fnv(h, dy, n);
        h ^= (uint64_t)(is909 ? drum909_active(&d9) : drum808_active(&d8));
        h *= 0x100000001B3ull;
        t += n;
    }
    result(name, h, total);
}

static void scenarios(void)
{
    nsc = 0;
    s303("303_saw", 0, BASS303_DRV_OFF, 0, 0, 0, 8);
    s303("303_sqr", 1, BASS303_DRV_OFF, 0, 0, 0, 8);
    s303("303_saw_sweep", 0, BASS303_DRV_OFF, 0, 0, 1, 8);
    s303("303_sqr_sweep", 1, BASS303_DRV_OFF, 0, 0, 1, 8);
    s303("303_soft_saw", 0, BASS303_DRV_SOFT, 0, 0, 1, 6);
    s303("303_soft_sqr", 1, BASS303_DRV_SOFT, 0, 0, 1, 6);
    s303("303_rat_saw", 0, BASS303_DRV_RAT, 0, 0, 1, 6);
    s303("303_rat_sqr", 1, BASS303_DRV_RAT, 0, 0, 1, 6);
    s303("303_blk1", 0, BASS303_DRV_SOFT, 0, 1, 1, 3);
    s303("303_blk37", 1, BASS303_DRV_RAT, 0, 37, 1, 6);
    s303("303_blk256", 0, BASS303_DRV_OFF, 0, 256, 1, 6);
    s303("303_lite", 0, BASS303_DRV_OFF, 1, 0, 1, 6);
    s303("303_lite_sqr_rat", 1, BASS303_DRV_RAT, 1, 0, 1, 6);
    s303("303_lite_switch", 0, BASS303_DRV_SOFT, 2, 0, 1, 8);
    sdrum("909_default", 1, 0, 0, 0, 10);
    sdrum("909_pots", 1, 1, 0, 0, 20);
    sdrum("909_pots_st", 1, 1, 1, 0, 20);
    sdrum("909_ring_st", 1, 2, 1, 0, 60);
    sdrum("909_blk1", 1, 1, 1, 1, 4);
    sdrum("909_blk256", 1, 1, 1, 256, 20);
    sdrum("808_default", 0, 0, 0, 0, 10);
    sdrum("808_pots", 0, 1, 0, 0, 20);
    sdrum("808_pots_st", 0, 1, 1, 0, 20);
    sdrum("808_ring_st", 0, 2, 1, 0, 60);
    sdrum("808_blk1", 0, 1, 1, 1, 4);
    sdrum("808_blk7", 0, 1, 0, 7, 10);
    sdrum("808_blk256", 0, 1, 1, 256, 20);
}

int main(int argc, char **argv)
{
    FILE *f;
    int i, fails = 0, write = argc > 2 && !strcmp(argv[2], "--write");
    if (argc < 2) {
        fprintf(stderr, "usage: %s GOLDEN [--write]\n", argv[0]);
        return 2;
    }
    scenarios();
    if (getenv("DSP_HASH_BENCH")) {
        for (i = 1; i < 5; i++)
            scenarios();
        for (i = 0; i < nsc; i++)
            printf("  bench %-18s %8.1f host instructions / sample\n", names[i], best[i]);
    }
    if (write) {
        f = fopen(argv[1], "w");
        if (!f)
            return 2;
        for (i = 0; i < nsc; i++)
            fprintf(f, "%s %016llx\n", names[i], (unsigned long long)hashes[i]);
        fclose(f);
        printf("wrote %d hashes to %s\n", nsc, argv[1]);
        return 0;
    }
    f = fopen(argv[1], "r");
    if (!f) {
        printf("no golden file %s\n", argv[1]);
        return 1;
    }
    for (i = 0; i < nsc; i++) {
        char nm[64];
        unsigned long long g;
        if (fscanf(f, "%63s %llx", nm, &g) != 2 || strcmp(nm, names[i])) {
            printf("  FAIL golden file out of step at %s\n", names[i]);
            fails++;
            break;
        }
        printf("  %s %-18s %016llx%s\n", g == hashes[i] ? "ok  " : "FAIL", names[i], (unsigned long long)hashes[i],
               g == hashes[i] ? "" : " (golden differs)");
        fails += g != hashes[i];
    }
    fclose(f);
    printf(fails ? "dsp_hash: FAIL\n" : "dsp_hash: every engine bit for bit as the golden renders\n");
    return fails != 0;
}
