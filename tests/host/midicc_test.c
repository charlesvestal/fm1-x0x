/* SPDX-License-Identifier: GPL-3.0-only */
/* X0X's MIDI CC map (firmware/src/app/engine.c): each drum voice's CCs reach its Level, Tune and Decay by name;
 * every CC that is not an alias is the one its pot sends back, on its own role; a value sent and taken back in
 * is the value it was, for every pot's range; the aliases are never sent. */
#include <stdio.h>
#include <string.h>
#include "../../firmware/src/app/engine.h"
#include "../../firmware/src/app/plat.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* the platform engine.c needs (nothing plays here) */
int plat_cpu2_run(void (*fn)(uint32_t), uint32_t a) { (void)fn; (void)a; return 0; }
int plat_cpu2_wait(void) { return 0; }
uint32_t plat_cycles(void) { return 0; }
int plat_midi_in(uint32_t *p) { (void)p; return 0; }
uint32_t plat_master(void) { return 4096; }
int plat_slot_zones(int k) { (void)k; return 0; }
const uint8_t *plat_slot(int k, int z, uint32_t *n, uint32_t *r, char nm[9]) { (void)k; (void)z; (void)n; (void)r; (void)nm; return 0; }
static uint32_t sent[4], nsent;
void plat_midi_out(uint32_t p) { if (nsent < 4) sent[nsent] = p; nsent++; }
void plat_midi_out_main(uint32_t p) { plat_midi_out(p); }
int plat_midi_out_room(void) { return 64; }

static int named(int t, int v, int i, const char *n)
{
    const x0x_param_t *d = engine_param(t, v, i);
    return d && !strcmp(d->name, n);
}

int main(void)
{
    static const uint8_t DEF_CH[NPARTS] = {9, 10, 1, 2, 3};
    int role, cc, t, v, i, kit, k, x, r2, mapped = 0;
    for (k = 0; k < NPARTS; k++)
        seq.ch[k] = DEF_CH[k];
    engine_cc_config(CC_OUT_KNOBS, 0);
    /* the drum voices: Level 102-112, Tune 20-30, Decay 70-80 (the 909 clap's is its Tail; its rimshot has none) */
    for (kit = 0; kit < NKIT; kit++)
        for (v = 0; v < NDRUM; v++) {
            int tk = kit ? T_808 : T_909, nr = !kit && v == 5;
            CHECK(engine_cc_param(kit, 102 + v, &t, &k, &i) && t == tk && k == v && named(t, v, i, "Level"),
                  "kit %d voice %d: CC %d is not its Level", kit, v, 102 + v);
            CHECK(engine_cc_param(kit, 20 + v, &t, &k, &i) && t == tk && k == v && named(t, v, i, "Tune"),
                  "kit %d voice %d: CC %d is not its Tune", kit, v, 20 + v);
            if (nr)
                CHECK(!engine_cc_param(kit, 70 + v, &t, &k, &i), "the 909 rimshot has no Decay");
            else
                CHECK(engine_cc_param(kit, 70 + v, &t, &k, &i) && t == tk && k == v &&
                      (named(t, v, i, "Decay") || (!kit && v == 6 && named(t, v, i, "Tail"))),
                      "kit %d voice %d: CC %d is not its Decay", kit, v, 70 + v);
        }
    CHECK(engine_cc_param(PART_909, 12, &t, &v, &i) && named(t, v, i, "Accent"), "the 909 kit's Accent");
    CHECK(engine_cc_param(PART_808, 12, &t, &v, &i) && named(t, v, i, "Accent"), "the 808 kit's Accent");
    /* every CC there is: an alias or the pot's own; what the pot sends comes back as it was */
    for (role = 0; role <= CC_ROLE_MIX; role++)
        for (cc = 0; cc < 128; cc++) {
            const x0x_param_t *d;
            int own, alias;
            if (!engine_cc_param(role, cc, &t, &v, &i))
                continue;
            d = engine_param(t, v, i);
            CHECK(d != 0, "role %d CC %d: no pot", role, cc);
            if (!d)
                continue;
            own = engine_cc_of(t, v, i, &r2);
            alias = (role >= PART_303A && role <= PART_BRK && (cc == 74 || cc == 71 || cc == 75)) ||
                    (role == CC_ROLE_MIX && (cc == 74 || (cc >= 102 && cc < 107)));
            if (alias) {
                CHECK(own != cc, "role %d CC %d is an alias and is sent", role, cc);
                continue;
            }
            mapped++;
            CHECK(own == cc && r2 == role, "role %d CC %d: its pot sends CC %d on role %d", role, cc, own, r2);
            for (x = 0; x <= d->max; x++) {
                int y, back;
                nsent = 0;
                engine_cc_send(t, v, i, x);
                CHECK(nsent >= 1, "role %d CC %d: nothing sent", role, cc);
                y = (int)(sent[0] >> 24 & 0x7Fu);
                CHECK((sent[0] >> 16 & 0x7Fu) == (uint32_t)cc && (sent[0] >> 8 & 0xFFu) == (0xB0u | (uint32_t)engine_cc_chan(role)),
                      "role %d CC %d: sent as %08X", role, cc, sent[0]);
                back = (y * d->max + 63) / 127;                     /* (midicc.c cc_in) */
                CHECK(back == x, "role %d CC %d (max %d): %d went out as %d, came back %d", role, cc, d->max, x, y, back);
                if (back != x)
                    break;
            }
        }
    CHECK(mapped == 125, "%d CCs mapped (125: 909 37, 808 38, two 303s 32, the break 7, the mix 11)", mapped);
    /* a part's level also goes out on MIX MIDI CH's 102-106 */
    nsent = 0;
    engine_cc_send(T_MIX, PART_303B, MX_LEVEL, 100);
    CHECK(nsent == 2 && (sent[1] >> 16 & 0x7Fu) == 105 && (sent[1] >> 8 & 0xFFu) == 0xB0u, "303 B level on 105");
    /* OFF sends nothing; a part whose channel is OFF neither (but its level still on MIX MIDI CH) */
    engine_cc_config(CC_OUT_OFF, 0);
    nsent = 0;
    engine_cc_send(T_303, 0, 0, 64);
    CHECK(nsent == 0, "CC OUT OFF sent %u", nsent);
    engine_cc_config(CC_OUT_KNOBS, 0);
    seq.ch[PART_303A] = SEQ_CH_OFF;
    nsent = 0;
    engine_cc_send(T_303, 0, 0, 64);
    CHECK(nsent == 0, "303 A's channel OFF sent %u", nsent);
    nsent = 0;
    engine_cc_send(T_MIX, PART_303A, MX_LEVEL, 64);
    CHECK(nsent == 1 && (sent[0] >> 16 & 0x7Fu) == 104, "303 A's level on MIX MIDI CH only");
    printf(fails ? "midicc: %d FAILED\n" : "midicc: all ok\n", fails);
    return fails != 0;
}
