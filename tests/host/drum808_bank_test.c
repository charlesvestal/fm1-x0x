/* SPDX-License-Identifier: GPL-3.0-only */
/* The 808's metal bank by blocks (bank_block, after Optimist) against the bank sample by sample (bank_tick, 8W8's
 * way), bit for bit: at the ratio's ends and between, blocks of 1..32 samples, retuned on the way, and the bank's
 * state after. After Optimist's test (github.com/w0ts/optimist 9f71feec, tests/x0x_drums_test.c, GPL-3.0-only). */
#define D8_BANK_CHECK 1
#include "drum808.c"
#include <stdio.h>
#include <string.h>

static drum808_t a, b;

int main(void)
{
    static const float R[7] = {0.25f, 0.5f, 0.97f, 1.0f, 1.83f, 3.1f, 4.0f};
    float bus[32], pair[32];
    uint32_t ri, s, diff = 0, len = 7u, n = 0;
    drum808_init(&a);
    drum808_init(&b);
    for (ri = 0; ri < 7u; ri++) {
        bank_ratio(&a.bank, R[ri]);
        bank_ratio(&b.bank, R[ri]);
        for (s = 0; s < 40000u; s += len) {
            uint32_t i;
            len = 1u + (s * 7u + ri) % 32u;
            bank_block(&b.bank, bus, pair, (int)len);
            for (i = 0; i < len; i++) {
                float t = bank_tick(&a.bank);
                float p = ((a.bank.ph[4] < BANK_DUTY ? 2.5f : -2.5f) + (a.bank.ph[5] < BANK_DUTY ? 2.5f : -2.5f)) * 0.5f;
                diff += memcmp(&t, &bus[i], 4) != 0 || memcmp(&p, &pair[i], 4) != 0;
            }
            n += len;
        }
    }
    diff += memcmp(&a.bank, &b.bank, sizeof a.bank) != 0;
    printf("  %s the metal bank by blocks = sample by sample (7 ratios, 1..32-sample blocks, %u samples)\n",
           diff ? "FAIL" : "ok  ", (unsigned)n);
    return diff != 0;
}
