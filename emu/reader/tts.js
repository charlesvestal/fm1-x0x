// SPDX-License-Identifier: MIT
// The screen reader's voice: espeak-ng in a worker (tts_worker.js), played through Web Audio.
//   const tts = await createTTS();            // baseUrl: the folder holding tts_worker.js and espeak/
//   button.onclick = () => tts.unlock();      // browsers start audio only from a user gesture
//   tts.speak('Cutoff, 64');                  // interrupt (default) cuts off what is playing
//   tts.speak('Pattern 3 saved', { interrupt: false });   // queued after it
// speak() resolves when the utterance has played, or was cut off / dropped.

export async function createTTS(baseUrl = new URL('./', import.meta.url).href) {
    const base = new URL(baseUrl, location.href).href.replace(/\/?$/, '/');
    const worker = new Worker(new URL('tts_worker.js', base));
    const sampleRate = await new Promise((resolve, reject) => {
        worker.onmessage = (e) => e.data.type === 'ready' ? resolve(e.data.sampleRate) : reject(new Error(e.data.message));
        worker.onerror = (e) => reject(new Error(e.message || 'tts worker failed to load'));
        worker.postMessage({ type: 'init', base });
    });

    let ctx = null;
    let queue = [];             // {id, pcm, volume, done}: waiting for the worker or for the speaker
    let current = null;         // {item, src, gain}
    let nextId = 1;

    function unlock() {
        if (!ctx) {
            const AC = window.AudioContext || window.webkitAudioContext;
            ctx = new AC();
            // iOS Safari: a silent buffer started inside the gesture opens the audio path
            const s = ctx.createBufferSource();
            s.buffer = ctx.createBuffer(1, 1, ctx.sampleRate);
            s.connect(ctx.destination);
            s.start();
            ctx.onstatechange = pump;
        }
        if (ctx.state !== 'running') ctx.resume().then(pump, () => {});
    }

    function finish(item) { if (item.done) { item.done(); item.done = null; } }

    function pump() {
        if (current || !queue.length || !ctx || ctx.state !== 'running') return;
        const item = queue[0];
        if (!item.pcm) return;                      // still with the worker
        queue.shift();
        if (!item.pcm.length) { finish(item); pump(); return; }
        const buf = ctx.createBuffer(1, item.pcm.length, sampleRate);
        const f = buf.getChannelData(0);
        for (let i = 0; i < f.length; i++) f[i] = item.pcm[i] / 32768;
        const src = ctx.createBufferSource();
        const gain = ctx.createGain();
        src.buffer = buf;
        gain.gain.value = item.volume;
        src.connect(gain).connect(ctx.destination);
        src.onended = () => {
            if (current && current.src === src) { current = null; finish(item); pump(); }
        };
        current = { item, src, gain };
        src.start();
    }

    function stop() {
        for (const item of queue) finish(item);
        queue = [];
        if (current) {
            const { item, src, gain } = current;
            current = null;
            src.onended = null;
            const t = ctx.currentTime;              // a 5 ms fade instead of a click
            gain.gain.setTargetAtTime(0, t, 0.005);
            src.stop(t + 0.03);
            finish(item);
        }
    }

    worker.onmessage = (e) => {
        const m = e.data;
        if (m.type !== 'pcm') return;
        const item = queue.find((q) => q.id === m.id);
        if (!item) return;                          // dropped by an interrupt meanwhile
        item.pcm = m.pcm;
        pump();
    };

    function speak(text, { rate = 230, pitch = 50, interrupt = true, volume = 1 } = {}) {
        if (interrupt) stop();
        unlock();
        return new Promise((done) => {
            const id = nextId++;
            queue.push({ id, pcm: null, volume, done });
            worker.postMessage({ type: 'speak', id, text, rate, pitch });
        });
    }

    return {
        speak,
        stop,
        unlock,
        sampleRate,
        get speaking() { return !!current || queue.length > 0; },
    };
}
