#!/usr/bin/env node
/**
 * Renders the bundled metronome click kits to WAV.
 *
 *   node scripts/generate-metronome-kits.js
 *
 * Each kit is three one-shots — High (accent), Low (normal beat) and Sub
 * (subdivision tick) — written as 16-bit mono PCM at 48 kHz into
 * `metronome/<id>/`, plus the `metronome/kits.json` manifest the engine reads
 * to build the sound picker.
 *
 * All three one-shots of a kit are peak-normalised to the same level: an
 * accent reads through timbre, and how loud each beat level actually plays is
 * the engine's business (see the level gains in MetronomeSupport.h).
 *
 * The renders are deterministic: the noise source is a seeded PRNG, so running
 * this again produces byte-identical files and no diff churn. Replacing any of
 * these folders with recorded samples is supported and needs no code change —
 * keep the file names, and drop the kit's entry from the manifest only if you
 * are removing the sound entirely.
 */

const fs = require('fs');
const path = require('path');

const SAMPLE_RATE = 48000;
const ROOT = path.resolve(__dirname, '..');
const OUT_DIR = path.join(ROOT, 'metronome');

// ── Signal helpers ────────────────────────────────────────────────────

/** Deterministic PRNG so regenerated kits are byte-identical. */
function mulberry32(seed) {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

function noise(length, seed) {
  const rand = mulberry32(seed);
  const out = new Float64Array(length);
  for (let i = 0; i < length; i += 1) out[i] = rand() * 2 - 1;
  return out;
}

/** RBJ biquad. `type` is one of lowpass | highpass | bandpass. */
function biquad(input, type, freq, q) {
  const w0 = (2 * Math.PI * freq) / SAMPLE_RATE;
  const cos = Math.cos(w0);
  const alpha = Math.sin(w0) / (2 * q);
  let b0;
  let b1;
  let b2;
  if (type === 'lowpass') {
    b0 = (1 - cos) / 2;
    b1 = 1 - cos;
    b2 = (1 - cos) / 2;
  } else if (type === 'highpass') {
    b0 = (1 + cos) / 2;
    b1 = -(1 + cos);
    b2 = (1 + cos) / 2;
  } else {
    b0 = alpha;
    b1 = 0;
    b2 = -alpha;
  }
  const a0 = 1 + alpha;
  const a1 = -2 * cos;
  const a2 = 1 - alpha;

  const out = new Float64Array(input.length);
  let x1 = 0;
  let x2 = 0;
  let y1 = 0;
  let y2 = 0;
  for (let i = 0; i < input.length; i += 1) {
    const x0 = input[i];
    const y0 = (b0 / a0) * x0 + (b1 / a0) * x1 + (b2 / a0) * x2 - (a1 / a0) * y1 - (a2 / a0) * y2;
    out[i] = y0;
    x2 = x1;
    x1 = x0;
    y2 = y1;
    y1 = y0;
  }
  return out;
}

/** Exponential decay with a short attack ramp, in seconds. */
function envelope(length, attackSec, decaySec) {
  const attack = Math.max(1, Math.round(attackSec * SAMPLE_RATE));
  const tau = Math.max(1e-4, decaySec) / 4;
  const out = new Float64Array(length);
  for (let i = 0; i < length; i += 1) {
    const t = i / SAMPLE_RATE;
    const rise = i < attack ? i / attack : 1;
    out[i] = rise * Math.exp(-t / tau);
  }
  return out;
}

function sine(length, freq, phase = 0) {
  const out = new Float64Array(length);
  for (let i = 0; i < length; i += 1) {
    out[i] = Math.sin((2 * Math.PI * freq * i) / SAMPLE_RATE + phase);
  }
  return out;
}

function mix(length, ...parts) {
  const out = new Float64Array(length);
  for (const [signal, gain] of parts) {
    for (let i = 0; i < length; i += 1) out[i] += signal[i] * gain;
  }
  return out;
}

function applyEnvelope(signal, env) {
  const out = new Float64Array(signal.length);
  for (let i = 0; i < signal.length; i += 1) out[i] = signal[i] * env[i];
  return out;
}

/** Peak-normalises, then applies `peak`. Also fades the last 2 ms to zero so
 *  the one-shot cannot click when the engine stops reading it. */
function finish(signal, peak) {
  let max = 0;
  for (let i = 0; i < signal.length; i += 1) max = Math.max(max, Math.abs(signal[i]));
  const scale = max > 0 ? peak / max : 0;
  const fade = Math.min(signal.length, Math.round(0.002 * SAMPLE_RATE));
  const out = new Float64Array(signal.length);
  for (let i = 0; i < signal.length; i += 1) {
    const tail = i >= signal.length - fade ? (signal.length - i) / fade : 1;
    out[i] = signal[i] * scale * tail;
  }
  return out;
}

function seconds(value) {
  return Math.round(value * SAMPLE_RATE);
}

function writeWav(filePath, samples) {
  const dataBytes = samples.length * 2;
  const buffer = Buffer.alloc(44 + dataBytes);
  buffer.write('RIFF', 0, 'ascii');
  buffer.writeUInt32LE(36 + dataBytes, 4);
  buffer.write('WAVE', 8, 'ascii');
  buffer.write('fmt ', 12, 'ascii');
  buffer.writeUInt32LE(16, 16); // PCM chunk size
  buffer.writeUInt16LE(1, 20); // PCM
  buffer.writeUInt16LE(1, 22); // mono
  buffer.writeUInt32LE(SAMPLE_RATE, 24);
  buffer.writeUInt32LE(SAMPLE_RATE * 2, 28); // byte rate
  buffer.writeUInt16LE(2, 32); // block align
  buffer.writeUInt16LE(16, 34); // bits per sample
  buffer.write('data', 36, 'ascii');
  buffer.writeUInt32LE(dataBytes, 40);
  for (let i = 0; i < samples.length; i += 1) {
    const clamped = Math.max(-1, Math.min(1, samples[i]));
    buffer.writeInt16LE(Math.round(clamped * 32767), 44 + i * 2);
  }
  fs.mkdirSync(path.dirname(filePath), { recursive: true });
  fs.writeFileSync(filePath, buffer);
}

// ── Voices ────────────────────────────────────────────────────────────
//
// Each builder takes {freq, decay, peak, seed} and returns samples. The three
// voices of a kit are the same builder at different pitches and lengths, which
// is what makes an accent read as the same instrument hit harder.

const VOICES = {
  beep: ({ freq, decay, peak }) => {
    const length = seconds(decay);
    return finish(applyEnvelope(sine(length, freq), envelope(length, 0.0005, decay)), peak);
  },

  click: ({ freq, decay, peak, seed }) => {
    const length = seconds(decay);
    const body = biquad(noise(length, seed), 'bandpass', freq, 1.2);
    const ping = sine(length, freq * 2, 0);
    const env = envelope(length, 0.0002, decay * 0.5);
    return finish(applyEnvelope(mix(length, [body, 0.7], [ping, 0.5]), env), peak);
  },

  woodblock: ({ freq, decay, peak, seed }) => {
    const length = seconds(decay);
    const attack = biquad(noise(length, seed), 'highpass', 3000, 0.7);
    const tone = mix(length, [sine(length, freq), 1], [sine(length, freq * 2.7), 0.35]);
    const body = applyEnvelope(tone, envelope(length, 0.0004, decay * 0.7));
    const transient = applyEnvelope(attack, envelope(length, 0.0001, 0.004));
    return finish(mix(length, [body, 1], [transient, 0.5]), peak);
  },

  rim: ({ freq, decay, peak, seed }) => {
    const length = seconds(decay);
    const crack = biquad(noise(length, seed), 'bandpass', 2400, 0.8);
    const body = sine(length, freq);
    return finish(
      mix(
        length,
        [applyEnvelope(crack, envelope(length, 0.0001, decay * 0.4)), 1],
        [applyEnvelope(body, envelope(length, 0.0003, decay * 0.25)), 0.55],
      ),
      peak,
    );
  },

  cowbell: ({ freq, decay, peak }) => {
    const length = seconds(decay);
    const tone = mix(length, [sine(length, freq), 1], [sine(length, freq * 1.55), 0.8]);
    const shaped = biquad(tone, 'bandpass', freq * 1.3, 0.6);
    return finish(applyEnvelope(shaped, envelope(length, 0.0006, decay * 0.6)), peak);
  },

  hihat: ({ freq, decay, peak, seed }) => {
    const length = seconds(decay);
    const metal = biquad(biquad(noise(length, seed), 'highpass', freq, 0.8), 'bandpass', freq * 1.6, 0.9);
    return finish(applyEnvelope(metal, envelope(length, 0.0002, decay * 0.45)), peak);
  },

  shaker: ({ freq, decay, peak, seed }) => {
    const length = seconds(decay);
    const grains = biquad(noise(length, seed), 'bandpass', freq, 0.7);
    return finish(applyEnvelope(grains, envelope(length, 0.006, decay * 0.5)), peak);
  },

  soft: ({ freq, decay, peak }) => {
    const length = seconds(decay);
    const tone = mix(length, [sine(length, freq), 1], [sine(length, freq * 2), 0.18]);
    const warmed = biquad(tone, 'lowpass', freq * 3, 0.7);
    return finish(applyEnvelope(warmed, envelope(length, 0.003, decay * 0.6)), peak);
  },
};

// ── Kits ──────────────────────────────────────────────────────────────
//
// `order` is the order the picker shows them in. kit1 is the sampled drum kit
// that has always shipped and is listed here without being rendered.

const KITS = [
  {
    id: 'click',
    label: 'Click',
    voice: 'click',
    high: { freq: 2200, decay: 0.05, peak: 0.95, seed: 11 },
    low: { freq: 1500, decay: 0.05, peak: 0.95, seed: 12 },
    sub: { freq: 1500, decay: 0.03, peak: 0.95, seed: 13 },
  },
  {
    id: 'beep',
    label: 'Beep',
    voice: 'beep',
    high: { freq: 1800, decay: 0.045, peak: 0.95 },
    low: { freq: 1200, decay: 0.045, peak: 0.95 },
    sub: { freq: 1200, decay: 0.025, peak: 0.95 },
  },
  {
    id: 'woodblock',
    label: 'Wood Block',
    voice: 'woodblock',
    high: { freq: 1250, decay: 0.09, peak: 0.95, seed: 21 },
    low: { freq: 900, decay: 0.09, peak: 0.95, seed: 22 },
    sub: { freq: 900, decay: 0.05, peak: 0.95, seed: 23 },
  },
  {
    id: 'rim',
    label: 'Rim',
    voice: 'rim',
    high: { freq: 480, decay: 0.07, peak: 0.95, seed: 31 },
    low: { freq: 360, decay: 0.07, peak: 0.95, seed: 32 },
    sub: { freq: 360, decay: 0.04, peak: 0.95, seed: 33 },
  },
  {
    id: 'cowbell',
    label: 'Cowbell',
    voice: 'cowbell',
    high: { freq: 835, decay: 0.16, peak: 0.95 },
    low: { freq: 620, decay: 0.14, peak: 0.95 },
    sub: { freq: 620, decay: 0.07, peak: 0.95 },
  },
  {
    id: 'hihat',
    label: 'Hi-Hat',
    voice: 'hihat',
    high: { freq: 6500, decay: 0.075, peak: 0.95, seed: 41 },
    low: { freq: 5200, decay: 0.06, peak: 0.95, seed: 42 },
    sub: { freq: 5200, decay: 0.035, peak: 0.95, seed: 43 },
  },
  {
    id: 'shaker',
    label: 'Shaker',
    voice: 'shaker',
    high: { freq: 7000, decay: 0.11, peak: 0.95, seed: 51 },
    low: { freq: 5800, decay: 0.09, peak: 0.95, seed: 52 },
    sub: { freq: 5800, decay: 0.055, peak: 0.95, seed: 53 },
  },
  {
    id: 'soft',
    label: 'Soft Kit',
    voice: 'soft',
    high: { freq: 1050, decay: 0.1, peak: 0.95, seed: 61 },
    low: { freq: 700, decay: 0.1, peak: 0.95, seed: 62 },
    sub: { freq: 700, decay: 0.06, peak: 0.95, seed: 63 },
  },
];

/** Kits already on disk as recorded samples, listed but never rendered. */
const SAMPLED_KITS = [{ id: 'kit1', label: 'Drum Kit', hasSub: false }];

function main() {
  const manifest = [];

  for (const kit of SAMPLED_KITS) {
    const entry = {
      id: kit.id,
      label: kit.label,
      highPath: `metronome/${kit.id}/High.wav`,
      lowPath: `metronome/${kit.id}/Low.wav`,
    };
    if (kit.hasSub) entry.subPath = `metronome/${kit.id}/Sub.wav`;
    manifest.push(entry);
  }

  for (const kit of KITS) {
    const render = VOICES[kit.voice];
    if (!render) throw new Error(`unknown voice ${kit.voice} for kit ${kit.id}`);

    for (const [slot, params] of [
      ['High', kit.high],
      ['Low', kit.low],
      ['Sub', kit.sub],
    ]) {
      writeWav(path.join(OUT_DIR, kit.id, `${slot}.wav`), render(params));
    }

    manifest.push({
      id: kit.id,
      label: kit.label,
      highPath: `metronome/${kit.id}/High.wav`,
      lowPath: `metronome/${kit.id}/Low.wav`,
      subPath: `metronome/${kit.id}/Sub.wav`,
    });
    process.stdout.write(`[metronome-kits] rendered ${kit.id}\n`);
  }

  const manifestPath = path.join(OUT_DIR, 'kits.json');
  fs.writeFileSync(manifestPath, `${JSON.stringify({ kits: manifest }, null, 2)}\n`);
  process.stdout.write(`[metronome-kits] wrote ${path.relative(ROOT, manifestPath)} (${manifest.length} kits)\n`);
}

main();
