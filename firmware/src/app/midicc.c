/* SPDX-License-Identifier: GPL-3.0-only */
/* X0X's MIDI CCs, the main loop's side (the map and the ISR's side: engine.c). A CC that comes in sets its
 * pot as a turn of its knob would: pref_set (the macros, saving), motion_touch (recorded while recording, the
 * knob over its lane for a pass), one undo step for a sweep. With CC OUT on, what changes by hand goes out
 * (cc_hand: a knob, a list row, ENV + knob); never what came in. A load, an undo or a redo sends every
 * mapped value (the snapshot), so a controller with rings catches up. Part of ui.c, after speak(). */

static void cc_quiet(void)                     /* what a CC changed is not said: the reader is told it was */
{
    pref_t r;
    if (!ann_on())
        return;
    sp_readout();
    put_s(sp.knob, sp_buf);
    if (ui.overlay == O_LIST && ui.list_sel < ui.list_n) {
        r = list_rows[ui.list_sel];
        if (r.kind != R_ACT && r.kind != R_HDR)
            sp.list_v = (int16_t)pref_get(r);
    }
}

static void cc_in(uint32_t pkt)
{
    int ch = (int)(pkt >> 8 & 15u), cc = (int)(pkt >> 16 & 0x7Fu), val = (int)(pkt >> 24 & 0x7Fu);
    int role, t, v, i, k, nv;
    if ((pkt >> 8 & 0xF0u) != 0xB0u || engine_cc_echo(pkt) || perf_testing())
        return;                                 /* (the PERF test owns the sound) */
    for (role = 0; role <= CC_ROLE_MIX; role++) {
        pref_t r;
        const x0x_param_t *d;
        if (engine_cc_chan(role) != ch || !engine_cc_param(role, cc, &t, &v, &i))
            continue;
        r = t == T_FX && v == 1 && i == 0 ? PR(R_TAPE, 0, 0, 0) : PR(R_ENG, t, v, i);
        d = pref_desc(r);
        if (!d)
            continue;
        nv = (val * d->max + 63) / 127;
        ui.act_t = plat_ms();                   /* (a sweep is one undo step, as a knob's) */
        if (nv == pref_get(r))
            continue;
        pref_set(r, nv);
        motion_touch(r);
        for (k = 0; k < 4; k++) {               /* on the page showing: its knob lights */
            pref_t q = pg.r[cur_page()][k];
            if (q.kind == r.kind && q.a == r.a && q.b == r.b && q.c == r.c) {
                ui.touched = (int8_t)k;
                ui.touch_until = plat_ms() + 900u;
            }
        }
    }
}

static void cc_in_drain(void)
{
    uint32_t pkt;
    int any = 0;
    while (engine_cc_take(&pkt)) {
        cc_in(pkt);
        any = 1;
    }
    if (any)
        cc_quiet();
}

static void cc_out_pref(pref_t r)              /* pref_set: a pot changed (sent when by hand) */
{
    if (!cc_hand)
        return;
    if (r.kind == R_TAPE)
        engine_cc_send(T_FX, 1, 0, proj.sound.v[T_FX][1][0]);
    else if (r.kind == R_ENG)
        engine_cc_send(r.a, r.b, r.c, proj.sound.v[r.a][r.b][r.c]);
}

/* the snapshot: every role's mapped CCs in turn, CC_SNAP_FRAME a frame, while the out queue has room for them
 * and the notes. cc_snap: 0 idle, else the next role * 128 + CC, + 1 */
#define CC_SNAP_FRAME 8
#define CC_SNAP_END ((CC_ROLE_MIX + 1) * 128)
static uint16_t cc_snap X0X_POOL;
static void cc_snapshot(void)
{
    if (proj.set.cc_out != CC_OUT_OFF)
        cc_snap = 1;
}
static void cc_snap_tick(void)
{
    int sent = 0;
    while (cc_snap && sent < CC_SNAP_FRAME) {
        int k = cc_snap - 1, role = k / 128, cc = k % 128, t, v, i, r2;
        if (k >= CC_SNAP_END) {
            cc_snap = 0;
            break;
        }
        if (engine_cc_param(role, cc, &t, &v, &i) && engine_cc_of(t, v, i, &r2) == cc && r2 == role) {
            if (plat_midi_out_room() < 24)
                break;                          /* the queue is busy: the rest at the next frame */
            sent += engine_cc_send(t, v, i, proj.sound.v[t][v][i]);
        }
        cc_snap++;
    }
}
