// SPDX-License-Identifier: GPL-3.0-or-later
// GPL, not MIT like the rest of fm1-reader: it runs eSpeak NG (GPL-3.0-or-later) in its own scope. tts.js only
// talks to it by messages.
// The screen reader's speech worker (a classic worker): espeak-ng (espeak/espeak.js) instantiated
// once, then each message {id, text, rate, pitch} is answered with {id, pcm: Int16Array, sampleRate}.
'use strict';
let M = null, sampleRate = 0;

self.onmessage = async (e) => {
    const m = e.data;
    if (m.type === 'init') {
        try {
            const dir = new URL('espeak/', m.base).href;
            importScripts(dir + 'espeak.js');
            M = await self.createEspeak({ locateFile: (p) => dir + p });
            sampleRate = M._tts_init();
            if (sampleRate <= 0) throw new Error('espeak-ng init failed (' + sampleRate + ')');
            self.postMessage({ type: 'ready', sampleRate });
        } catch (err) {
            self.postMessage({ type: 'error', message: String(err && err.message || err) });
        }
        return;
    }
    if (m.type === 'speak') {
        const p = M.stringToNewUTF8(String(m.text));
        const n = M._tts_synth(p, m.rate | 0, m.pitch | 0);
        M._free(p);
        const pcm = n > 0 ? M.HEAP16.slice(M._tts_samples() >> 1, (M._tts_samples() >> 1) + n) : new Int16Array(0);
        self.postMessage({ type: 'pcm', id: m.id, pcm, sampleRate }, [pcm.buffer]);
    }
};
