/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal */
/* firmware/src/ota.c's handshake (cmd 0x11) on the host, its hooks a fake wire: the plain query (no body,
 * M-UPGRADE's) gets the identity cut to its name, '_' and three digits (M-UPGRADE 3.11 refuses more), the query with
 * the body 01 the whole identity, any other body nothing; the same during a session (ota_read answers a handshake
 * between reads).
 * The update loader (-DFELUCCA_LOADER) answers the plain query with its whole identity and not the one with 01.
 * The scheme is PERF-1's (deamonSLINKY/PERF-1 775106b). From felucca-bsp's tests/ota_test.c (41a6e86). Not the
 * session: tests/ota_test.c serves it a real package.
 *
 *   cc -std=c11 -O1 -Wall -Wextra -Werror -Wno-unused-function [-DTEST_ID='"FM-1_600"'] [-DFELUCCA_LOADER=1]
 *      tests/ota_hs_test.c -o ota_hs_test && ./ota_hs_test */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef TEST_ID
#ifdef FELUCCA_LOADER
#define TEST_ID "ota-FM-1_900"
#else
#define TEST_ID "FM-1_9010200"   /* a release's: FM-1_9 and XXYYZZ (X0X 1.2.0) */
#endif
#endif
#define FELUCCA_ID TEST_ID
#define FELUCCA_OTA_DRYRUN 1

static uint8_t rx[64];                  /* one pending host frame (7-bit bytes) */
static uint32_t rx_len, rx_full;
static uint8_t tx[8][64];               /* what the device sent, decoded */
static uint32_t tx_len[8], tx_n;
static uint32_t now_ms;

static void host_send(const uint8_t *body, uint32_t n)      /* the host's 0x11 with this body (n bytes) */
{
    uint8_t m[16];
    uint32_t i, s = 0, acc = 0, nb = 0;
    m[0] = 0, m[1] = 0x59, m[2] = 0x11, m[3] = (uint8_t)n, m[4] = 0, m[5] = 0;
    for (i = 0; i < n; i++)
        s += m[6 + i] = body[i];
    m[6 + n] = (uint8_t)~s;
    rx_len = 0;
    for (i = 0; i < 7u + n; i++) {                          /* pack7, LSB first */
        acc |= (uint32_t)m[i] << nb;
        for (nb += 8u; nb >= 7u; nb -= 7u, acc >>= 7)
            rx[rx_len++] = (uint8_t)(acc & 0x7Fu);
    }
    if (nb)
        rx[rx_len++] = (uint8_t)(acc & 0x7Fu);
    rx_full = 1;
}

static int ota_wire_send(const uint8_t *p, uint32_t n)
{
    uint32_t acc = 0, nb = 0, o = 0, i;
    if (tx_n < 8u) {
        for (i = 1; i + 1u < n; i++) {                      /* unpack7 between F0 and F7 */
            acc |= (uint32_t)(p[i] & 0x7Fu) << nb;
            nb += 7u;
            if (nb >= 8u) {
                if (o < sizeof tx[0])
                    tx[tx_n][o] = (uint8_t)acc;
                o++;
                acc >>= 8;
                nb -= 8u;
            }
        }
        tx_len[tx_n++] = o;
    }
    return 0;
}
static int ota_frame_get(const uint8_t **p, uint32_t *n)
{
    if (!rx_full)
        return 0;
    *p = rx;
    *n = rx_len;
    return 1;
}
static void ota_frame_done(void) { rx_full = 0; }
static uint32_t ota_now_ms(void) { return now_ms += 100u; }
static void ota_idle(void) {}
static int ota_erase(uint32_t off) { (void)off; return 0; }
static int ota_prog(uint32_t off, const void *p, uint32_t n) { (void)off, (void)p, (void)n; return 0; }
static int ota_fread(uint32_t off, void *p, uint32_t n) { (void)off; memset(p, 0xFF, n); return 0; }
static void ota_show(uint32_t step, int32_t code) { (void)step, (void)code; }
static void ota_commit(const uint8_t *parm) { (void)parm; }

#include "../firmware/src/ota.c"

static int fails;
static void want(const char *what, int ok)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    fails += !ok;
}

/* the k-th reply an identity frame (34 bytes: 00 59 11, length 27, the text, zeros, checksum) holding text */
static int ident_is(uint32_t k, const char *text)
{
    const uint8_t *m = tx[k];
    uint32_t i, s = 0, n = (uint32_t)strlen(text);
    if (k >= tx_n || tx_len[k] != 34u || m[0] != 0 || m[1] != 0x59 || m[2] != 0x11 || m[3] != 27u)
        return 0;
    for (i = 0; i < 27u; i++)
        s += m[6 + i];
    return (uint8_t)~s == m[33] && !memcmp(m + 6, text, n) && !m[6 + n] && m[6 + 26] == 0;
}

int main(void)
{
    static const uint8_t one[1] = {1}, two[1] = {2}, onetwo[2] = {1, 2};
    char cut[16];
#ifdef FELUCCA_LOADER
    snprintf(cut, sizeof cut, "%s", TEST_ID);
#else
    snprintf(cut, sizeof cut, "%.*s", (int)(strchr(TEST_ID, '_') - TEST_ID) + 4, TEST_ID);
#endif
    printf("identity %s, to the plain query %s\n", TEST_ID, cut);

    tx_n = 0;
    host_send(NULL, 0);
    want("ota_service: the plain query answered with the cut identity", ota_service() == 7u && tx_n == 1u &&
         ident_is(0, cut));
    want("the cut identity is NAME_ and 1-3 digits (M-UPGRADE 3.11)",
         strlen(strchr(cut, '_') + 1) >= 1u && strlen(strchr(cut, '_') + 1) <= 3u);
    tx_n = 0;
    host_send(one, 1);
#ifdef FELUCCA_LOADER
    want("ota_service: the loader leaves the query with 01 alone", ota_service() == 8u && tx_n == 0u);
#else
    want("ota_service: the query with 01 answered with the whole identity", ota_service() == 8u && tx_n == 1u &&
         ident_is(0, TEST_ID));
#endif
    tx_n = 0;
    host_send(two, 1);
    want("ota_service: body 02 not answered", ota_service() == 8u && tx_n == 0u);
    host_send(onetwo, 2);
    want("ota_service: body 01 02 not answered", ota_service() == 9u && tx_n == 0u);

    /* in a session: ota_read sends its read request, then a handshake comes in before the host's data */
    {
        uint8_t b[8];
        tx_n = 0;
        now_ms = 0;
        host_send(one, 1);
        want("ota_read gives up (no data), having answered the handshake as ota_service does",
             ota_read(0, b, 8) == -1 &&
#ifdef FELUCCA_LOADER
             tx_n == 3u && tx[0][2] == 0x30 && tx[1][2] == 0x30 && tx[2][2] == 0x30
#else
             tx_n == 4u && tx[0][2] == 0x30 && ident_is(1, TEST_ID) && tx[2][2] == 0x30
#endif
        );
        tx_n = 0;
        now_ms = 0;
        host_send(NULL, 0);
        want("ota_read: the plain query answered with the cut identity",
             ota_read(0, b, 8) == -1 && tx_n == 4u && ident_is(1, cut));
    }
    if (fails)
        printf("%d FAILED\n", fails);
    else
        printf("ota tests passed\n");
    return fails != 0;
}
