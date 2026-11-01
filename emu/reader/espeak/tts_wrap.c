// SPDX-License-Identifier: GPL-3.0-or-later
// The screen reader's speech: espeak-ng (English only) behind four calls for JavaScript.
//   tts_init() once, then tts_synth(text, wpm, pitch) -> sample count; the samples
//   (16-bit mono, tts_rate() Hz) are at tts_samples() until the next tts_synth.
#include <stdlib.h>
#include <string.h>
#include <emscripten.h>
#include <espeak-ng/speak_lib.h>

static short *buf;
static int len, cap, rate;

static int on_audio(short *wav, int n, espeak_EVENT *ev)
{
    (void)ev;
    if (!wav || n <= 0) return 0;
    if (len + n > cap) {
        int c = cap ? cap : 65536;
        while (c < len + n) c *= 2;
        short *b = realloc(buf, (size_t)c * sizeof *b);
        if (!b) return 1;                       // out of memory: stop this utterance
        buf = b; cap = c;
    }
    memcpy(buf + len, wav, (size_t)n * sizeof *wav);
    len += n;
    return 0;
}

EMSCRIPTEN_KEEPALIVE int tts_init(void)
{
    if (rate > 0) return rate;
    rate = espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS, 0, "/espeak-ng-data", espeakINITIALIZE_DONT_EXIT);
    if (rate <= 0) return -1;
    espeak_SetSynthCallback(on_audio);
    if (espeak_SetVoiceByName("en-us") != EE_OK && espeak_SetVoiceByName("en") != EE_OK) return -2;
    return rate;
}

EMSCRIPTEN_KEEPALIVE int tts_synth(const char *text, int wpm, int pitch)
{
    if (tts_init() <= 0) return -1;
    len = 0;
    espeak_SetParameter(espeakRATE, wpm, 0);
    espeak_SetParameter(espeakPITCH, pitch, 0);
    if (espeak_Synth(text, strlen(text) + 1, 0, POS_CHARACTER, 0, espeakCHARS_UTF8, NULL, NULL) != EE_OK)
        return -1;
    return len;
}

EMSCRIPTEN_KEEPALIVE short *tts_samples(void) { return buf; }
EMSCRIPTEN_KEEPALIVE int tts_length(void) { return len; }
EMSCRIPTEN_KEEPALIVE int tts_rate(void) { return rate; }
