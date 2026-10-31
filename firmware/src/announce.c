/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal */
/* Announce: what the screen says, for a screen reader on the computer (fm1-reader: a page that speaks it,
 * or hands it to the computer's own screen reader). Shared by the FM-1 firmwares, one copy: fm1-base's
 * device/announce.c (tests/announce_test.c); fm1-reader's tools/sync_announce.sh copies it into the firmwares not on
 * fm1-base. Change it here, not in a copy. The app says what changed and when; this keeps the protocol.
 *
 * The firmware is in charge of the words, without spending flash on them: it sends the screen's own text,
 * and its HELLO gives the link to its words file (its words.tsv, built by gen_words.py into ANN_WORDS_URL),
 * which the reader fetches to say "BD TUNE 37" as "bass drum tune 37". The link names the exact words this
 * build was made with, so an old device and a new one each get their own.
 *
 * SysEx over USB-MIDI, F0 7D 53 52 cmd args.. F7 ("SR"; fm1-reader/PROTOCOL.md):
 *   reader -> device   1 HELLO ver      listen (again every ~2 s: a reader not heard for 5 s is gone)
 *                      2 BYE            stop
 *                      3 WHERE          say everything on the screen now
 *   device -> reader   1 HELLO ver, name, 0, words link   (the answer to each HELLO)
 *                      16 SAY flags, text  (7-bit ASCII to the F7; flags 1: cut off what is being said,
 *                                           2: a warning or an error: not cut off by what follows)
 * Nothing is built or sent while no reader listens: a DAW on the same port never sees a byte.
 *
 * The includer supplies:
 *   ann_wire_send(p, n)   one complete F0..F7 message to the host; 0 = sent, else not sent at all (no room)
 * and calls ann_frame() with each received SysEx (the bytes between F0 and F7) and ann_tick() from its main
 * loop. The app calls ann_on(), ann_say(), ann_later() and ann_where().
 * Options before the #include: ANN_NAME (the HELLO answer), ANN_WORDS_URL (gen_words.py's ann_words.h),
 * ANN_MAX (longest SAY; the SysEx send queue must hold ANN_MAX + 9 bytes, (ANN_MAX + 9) / 3 USB-MIDI
 * packets), ANN_DATA (a section for its RAM). */
#ifndef ANN_NAME
#define ANN_NAME "FM-1"
#endif
#ifndef ANN_WORDS_URL
#define ANN_WORDS_URL ""                      /* no words file: the reader says the screen's text as it is */
#endif
#ifndef ANN_DATA
#define ANN_DATA                              /* where its RAM goes (zeroed at start), e.g. a pool section */
#endif
#ifndef ANN_MAX
#define ANN_MAX 240u                          /* text in one SAY; longer is cut at a word */
#endif
#define ANN_VER 1u
#define ANN_H0 0x7Du
#define ANN_H1 0x53u
#define ANN_H2 0x52u
#define ANN_NOW 1u                            /* cut off what the reader is saying (a knob, a page) */
#define ANN_KEEP 2u                           /* a warning or an error: heard whole, not cut off by what follows */
#define ANN_GONE_MS 5000u
enum { ANN_HELLO = 1, ANN_BYE = 2, ANN_WHERE = 3, ANN_SAY = 16 };

static int ann_wire_send(const uint8_t *p, uint32_t n);

static struct {
    uint8_t on, where;
    uint32_t heard;                           /* the last HELLO */
    char later[ANN_MAX + 1];                  /* the newest of a run (a knob turning), said when it settles */
    uint8_t later_flags, later_set;
    uint32_t later_t, later_first, later_quiet;
} ann ANN_DATA;

static int ann_on(void) { return ann.on; }

/* a + b (b may be 0; a 0 byte between them for HELLO's name and link) */
static void ann_send(uint32_t cmd, uint32_t flags, const char *a, const char *b)
{
    char w[ANN_MAX + 2];
    uint8_t f[8 + ANN_MAX];
    uint32_t n = 0, k = 0, i, two = cmd == ANN_HELLO;
    for (; a; a = b, b = 0) {
        while (*a && k < ANN_MAX)
            w[k++] = *a++;
        if (*a) {                             /* too long: back to the last space */
            while (k && w[k - 1] != ' ')
                k--;
            while (k && w[k - 1] == ' ')
                k--;
            break;
        }
        if (two && b)
            w[k++] = 1;                       /* (the 0 between: put in as the frame is written) */
    }
    w[k] = 0;
    f[n++] = 0xF0;
    f[n++] = ANN_H0;
    f[n++] = ANN_H1;
    f[n++] = ANN_H2;
    f[n++] = (uint8_t)cmd;
    f[n++] = (uint8_t)(flags & 0x7Fu);
    for (i = 0; w[i]; i++)
        f[n++] = w[i] == 1 && two ? 0 : (uint8_t)w[i] & 0x7Fu;
    f[n++] = 0xF7;
    ann_wire_send(f, n);
}

/* say a + b (b may be 0) now, after anything still waiting in ann_later */
static void ann_say(const char *a, const char *b, uint32_t flags)
{
    if (!ann.on || !a || !*a)
        return;
    if (ann.later_set && !(flags & ANN_NOW))  /* in order: what was waiting first */
        ann_send(ANN_SAY, ann.later_flags, ann.later, 0);
    ann.later_set = 0;
    ann_send(ANN_SAY, flags, a, b);
}

/* say s once it has stayed the newest for quiet_ms (each call replaces what waits; a run that keeps
 * changing is said at least every 4 x quiet_ms, so a long turn is heard as it goes) */
static void ann_later(const char *s, uint32_t flags, uint32_t quiet_ms, uint32_t now)
{
    uint32_t i;
    if (!ann.on)
        return;
    for (i = 0; s[i] && i < ANN_MAX; i++)
        ann.later[i] = s[i];
    ann.later[i] = 0;
    if (!ann.later_set)
        ann.later_first = now;
    ann.later_set = 1;
    ann.later_flags = (uint8_t)flags;
    ann.later_t = now;
    ann.later_quiet = quiet_ms;
}

/* the reader asked for the whole screen: 1 once */
static int ann_where(void)
{
    int w = ann.where;
    ann.where = 0;
    return w && ann.on;
}

static void ann_tick(uint32_t now)
{
    if (ann.on && now - ann.heard > ANN_GONE_MS) {
        ann.on = 0;
        ann.later_set = 0;
    }
    if (ann.later_set && (now - ann.later_t >= ann.later_quiet || now - ann.later_first >= 4u * ann.later_quiet)) {
        ann.later_set = 0;
        ann_send(ANN_SAY, ann.later_flags, ann.later, 0);
    }
}

/* a received SysEx (between F0 and F7): 1 if it was the reader's */
static int ann_frame(const uint8_t *p, uint32_t n, uint32_t now)
{
    if (n < 4u || p[0] != ANN_H0 || p[1] != ANN_H1 || p[2] != ANN_H2)
        return 0;
    switch (p[3]) {
    case ANN_HELLO:
        if (!ann.on)
            ann.where = 1;                    /* a new reader: what is on the screen first */
        ann.on = 1;
        ann.heard = now;
        ann_send(ANN_HELLO, ANN_VER, ANN_NAME, ANN_WORDS_URL);
        break;
    case ANN_BYE:
        ann.on = 0;
        ann.later_set = 0;
        break;
    case ANN_WHERE:
        ann.where = 1;
        break;
    default:
        break;
    }
    return 1;
}
