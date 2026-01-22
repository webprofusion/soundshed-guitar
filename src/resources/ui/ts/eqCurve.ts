export type EqBand = { freq: number; gainDb: number; q: number };

export function drawEqCurve(canvas: HTMLCanvasElement, bands: EqBand[]): void {
  const ctx = canvas.getContext("2d");
  if (!ctx) {
    return;
  }

  const width = canvas.width;
  const height = canvas.height;

  ctx.clearRect(0, 0, width, height);

  const padding = 8;
  const plotWidth = width - padding * 2;
  const plotHeight = height - padding * 2;
  const minDb = -18;
  const maxDb = 18;
  const minFreq = 20;
  const maxFreq = 20000;
  const sampleRate = 44100;

  ctx.strokeStyle = "rgba(255,255,255,0.08)";
  ctx.lineWidth = 1;
  const gridLines = 4;
  for (let i = 0; i <= gridLines; i += 1) {
    const y = padding + (plotHeight * i) / gridLines;
    ctx.beginPath();
    ctx.moveTo(padding, y);
    ctx.lineTo(width - padding, y);
    ctx.stroke();
  }

  const freqMarkers = [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000];
  freqMarkers.forEach((freq) => {
    const x = padding + plotWidth * (Math.log10(freq) - Math.log10(minFreq)) / (Math.log10(maxFreq) - Math.log10(minFreq));
    ctx.beginPath();
    ctx.moveTo(x, padding);
    ctx.lineTo(x, height - padding);
    ctx.stroke();
  });

  ctx.strokeStyle = "rgba(72, 168, 224, 0.9)";
  ctx.lineWidth = 2;
  ctx.beginPath();

  for (let i = 0; i <= plotWidth; i += 1) {
    const freq = minFreq * Math.pow(10, (i / plotWidth) * (Math.log10(maxFreq) - Math.log10(minFreq)));
    const magnitude = bands.reduce((acc, band) => acc * peakingMagnitude(freq, band, sampleRate), 1.0);
    const db = 20 * Math.log10(Math.max(1e-6, magnitude));
    const clampedDb = Math.max(minDb, Math.min(maxDb, db));
    const x = padding + i;
    const y = padding + (maxDb - clampedDb) / (maxDb - minDb) * plotHeight;
    if (i === 0) {
      ctx.moveTo(x, y);
    } else {
      ctx.lineTo(x, y);
    }
  }
  ctx.stroke();
}

function peakingMagnitude(freq: number, band: EqBand, sampleRate: number): number {
  if (!band || band.gainDb === 0 || band.freq <= 0) {
    return 1.0;
  }

  const w0 = 2 * Math.PI * band.freq / sampleRate;
  const cosw0 = Math.cos(w0);
  const sinw0 = Math.sin(w0);
  const q = Math.max(0.1, band.q || 1.0);
  const A = Math.pow(10, band.gainDb / 40);
  const alpha = sinw0 / (2 * q);

  const b0 = 1 + alpha * A;
  const b1 = -2 * cosw0;
  const b2 = 1 - alpha * A;
  const a0 = 1 + alpha / A;
  const a1 = -2 * cosw0;
  const a2 = 1 - alpha / A;

  const w = 2 * Math.PI * freq / sampleRate;
  const cosw = Math.cos(w);
  const sinw = Math.sin(w);
  const cos2w = Math.cos(2 * w);
  const sin2w = Math.sin(2 * w);

  const numRe = b0 + b1 * cosw + b2 * cos2w;
  const numIm = b1 * -sinw + b2 * -sin2w;
  const denRe = a0 + a1 * cosw + a2 * cos2w;
  const denIm = a1 * -sinw + a2 * -sin2w;

  const numMag = Math.sqrt(numRe * numRe + numIm * numIm);
  const denMag = Math.sqrt(denRe * denRe + denIm * denIm);
  if (denMag <= 0) {
    return 1.0;
  }

  return numMag / denMag;
}
