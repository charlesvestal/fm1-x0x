// SPDX-License-Identifier: GPL-3.0-only
// (A copy of fm1-reader's web/words.js, github.com/charlesvestal/fm1-reader, for the emulator's screen reader:
// web/emu/build.sh says when it no longer matches ../fm1-reader's.)
// The screen's text in words: the firmware's own list first (its words file, fetched from the link it gives:
// PROTOCOL.md), then the common way to say numbers and signs. One copy, used by the page, the audit and the
// tests.
//   spoken('BD (1/2): TUNE 34', words) -> 'bass drum, page 1 of 2: tune 34'
//   words: [[screen, said], ...] from a words file ({ format: 1, words: [...] }); see tools/gen_words.py
//
// A list entry matches whole words, capitals or not. "#" in both takes a number (digits, a sign, a decimal
// point), up to three, in order; "*" in the screen's text takes a word and drops it. The longest match wins;
// an earlier entry when they match as much (the firmware's own come before the common ones).

const al = (c) => /[A-Za-z]/.test(c || '');
const dg = (c) => /[0-9]/.test(c || '');
const an = (c) => al(c) || dg(c);
const NOTE = { 2: 'half', 4: 'quarter', 8: 'eighth', 16: 'sixteenth', 32: 'thirty-second' };
// after a capital A, these make it the note, not the article
const NOTE_WORD = /^(MINOR|MAJOR|MIN|MAJ|SHARP|FLAT|NATURAL|DORIAN|PHRYGIAN|LYDIAN|MIXOLYDIAN|LOCRIAN|AEOLIAN|IONIAN|PENTATONIC|BLUES|HARMONIC|MELODIC|CHROMATIC|SEVENTH|SIXTH|NINTH|ELEVENTH|THIRTEENTH|DIMINISHED|AUGMENTED|SUSPENDED|ADD|DOMINANT|POWER|minor|major|sharp|flat|seventh|diminished|augmented|suspended)\b/;

function match(s, i, k) {                         // length matched at s[i], and the numbers
    const caps = [];
    let p = i;
    for (let j = 0; j < k.length; j++) {
        const c = k[j];
        if (c === '#') {
            const b = p;
            if (s[p] === '-' || s[p] === '+') p++;
            while (dg(s[p]) || (s[p] === '.' && dg(s[p + 1]))) p++;
            if (p === b || !dg(s[p - 1])) return null;
            caps.push(s.slice(b, p));
        } else if (c === '*') {
            const b = p;
            while (an(s[p])) p++;
            if (p === b) return null;
        } else {
            if ((s[p] || '').toLowerCase() !== c.toLowerCase()) return null;
            p++;
        }
    }
    if (an(s[p]) && an(s[p - 1])) return null;    // whole: not the start of a longer word
    return { len: p - i, caps };
}

export function spoken(s, words = []) {
    let o = '';
    let i = 0;
    const last = () => o[o.length - 1];
    while (i < s.length) {
        const c = s[i];
        const start = i === 0 || !an(s[i - 1]);
        if (start && an(c)) {                      // a word: the list
            let best = null, said = '';
            for (const [k, v] of words) {
                const m = match(s, i, k);
                if (m && (!best || m.len > best.len)) { best = m; said = v; }
            }
            if (best) {
                let n = 0;
                o += said.replace(/#/g, () => best.caps[n++] ?? '');
                i += best.len;
                continue;
            }
        }
        if (start && c === '1' && s[i + 1] === '/') {        // 1/16, 1/8T (a triplet), 1/8. (dotted: the dot ends it)
            let q = i + 2, d = '';
            while (dg(s[q])) d += s[q++];
            if (NOTE[d]) {
                const trip = s[q] === 'T', dot = s[q] === '.' && (q + 1 >= s.length || s[q + 1] === ',' || s[q + 1] === '.');
                if ((trip || dot) ? !an(s[q + 1]) : !an(s[q])) {
                    o += (dot ? 'dotted ' : '') + NOTE[d] + (trip ? ' triplet' : '');
                    i = q + (trip || dot ? 1 : 0);
                    continue;
                }
            }
        }
        if (start && dg(c)) {                                  // 4:1 a ratio, 1-16 a range, 1/3 a share
            let q = i;
            while (dg(s[q]) || (s[q] === '.' && dg(s[q + 1]))) q++;
            if ((s[q] === ':' || s[q] === '-' || s[q] === '/') && dg(s[q + 1])) {
                let e = q + 1;
                while (dg(s[e])) e++;
                if (!an(s[e])) {
                    o += s.slice(i, q) + (s[q] === '/' ? ' of ' : ' to ');
                    i = q + 1;
                    continue;
                }
            }
        }
        if (start && c >= 'A' && c <= 'G' && s[i + 1] === '#') {   // C#3: C sharp 3
            o += c + ' sharp' + (dg(s[i + 2]) || s[i + 2] === '-' ? ' ' : '');
            i += 2;
            continue;
        }
        if (start && c === 'x' && dg(s[i + 1])) {                  // x2: 2 times
            let q = i + 1;
            while (dg(s[q])) q++;
            if (!an(s[q])) {
                const n = s.slice(i + 1, q);
                o += n + (n === '1' ? ' time' : ' times');
                i = q;
                continue;
            }
        }
        if (start && c === 'A' && s[i + 1] === ' ' && al(s[i + 2]) && al(s[i + 3]) &&
            !NOTE_WORD.test(s.slice(i + 2))) {             // A NEW LINE: the article (A MINOR, A FLAT: the note)
            o += 'a';
            i++;
            continue;
        }
        if (start && al(c)) {                                      // a word in capitals: a word, not letters
            let q = i, caps = 0, lower = 0;
            while (al(s[q]) || s[q] === "'") {
                if (/[A-Z]/.test(s[q])) caps++;
                if (/[a-z]/.test(s[q])) lower++;
                q++;
            }
            const w = s.slice(i, q);
            o += caps >= 2 && !lower && !dg(s[q]) ? w.toLowerCase() : w;
            i = q;
            continue;
        }
        if (c === '(' && dg(s[i + 1])) {                           // (1/2): page 1 of 2
            const m = /^\((\d+)\/(\d+)\)/.exec(s.slice(i));
            if (m) {
                o = o.replace(/ +$/, '') + `, page ${m[1]} of ${m[2]}`;
                i += m[0].length;
                continue;
            }
        }
        if (c === '%') { o += (o && last() !== ' ' ? ' ' : '') + 'percent'; i++; continue; }
        if (c === '+' && (i === 0 || s[i - 1] === ' ' || s[i - 1] === '(') && s[i + 1] === ' ') { o += 'plus'; i++; continue; }
        if (c === '/' && i > 0 && s[i - 1] === ' ' && s[i + 1] === ' ') { o += 'or'; i++; continue; }
        if (c === ' ') {                                           // one space; none before a mark or at the end
            while (s[i + 1] === ' ') i++;
            if (!o || last() === ' ' || ',.:'.includes(s[i + 1] || '.') ) { i++; continue; }
        }
        if (c === '.' && o && ',:?!.'.includes(last())) {           // "DELAY,." / "PATTERN?.": one mark
            if (last() === ',' || last() === ':') o = o.slice(0, -1) + '.';
            i++;
            continue;
        }
        o += c;
        i++;
    }
    return o;
}

// a words file, checked: { format: 1, words: [[screen, said], ...] }
export function wordsOf(json) {
    if (!json || json.format !== 1 || !Array.isArray(json.words)) throw new Error('not a words file (format 1)');
    return json.words.filter((w) => Array.isArray(w) && typeof w[0] === 'string' && typeof w[1] === 'string' && w[0]);
}
