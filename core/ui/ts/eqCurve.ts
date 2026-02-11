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

// ===== Interactive EQ Curve with Draggable Handles =====

const BAND_COLORS = [
  "rgba(232, 168, 56, 0.9)",   // Low - warm amber
  "rgba(126, 207, 74, 0.9)",   // Low Mid - green
  "rgba(74, 200, 207, 0.9)",   // High Mid - cyan
  "rgba(207, 106, 232, 0.9)",  // High - purple
];

export interface EqBandConfig {
  freq: number;
  gainDb: number;
  q: number;
  freqMin: number;
  freqMax: number;
  gainMin: number;
  gainMax: number;
  hasQ: boolean;
  qMin: number;
  qMax: number;
  label: string;
}

export type EqBandChangeHandler = (bandIndex: number, freq: number, gainDb: number, q: number) => void;

interface HandleInfo {
  bandIndex: number;
  type: "main" | "q-left" | "q-right";
  x: number;
  y: number;
  radius: number;
}

/**
 * Interactive EQ curve with draggable band handles.
 *
 * Each band is represented by a main handle (drag horizontally for frequency,
 * vertically for gain). Bands with adjustable Q also show wing handles that
 * can be dragged horizontally to widen/narrow the bandwidth.
 */
export class EqCurveInteraction {
  private canvas: HTMLCanvasElement;
  private bands: EqBandConfig[];
  private onChange: EqBandChangeHandler;
  private onCommit: EqBandChangeHandler;
  private handles: HandleInfo[] = [];
  private hoveredHandle: HandleInfo | null = null;
  private dragHandle: HandleInfo | null = null;
  private isDragging = false;
  private destroyed = false;

  private readonly HANDLE_RADIUS = 7;
  private readonly Q_HANDLE_RADIUS = 5;
  private readonly PADDING = 8;
  private readonly MIN_FREQ = 20;
  private readonly MAX_FREQ = 20000;
  private readonly MIN_DB = -18;
  private readonly MAX_DB = 18;
  private readonly SAMPLE_RATE = 44100;
  private readonly HIT_SLOP = 4;

  private boundPointerDown: (e: PointerEvent) => void;
  private boundPointerMove: (e: PointerEvent) => void;
  private boundPointerUp: (e: PointerEvent) => void;
  private boundPointerLeave: (e: PointerEvent) => void;

  constructor(
    canvas: HTMLCanvasElement,
    bands: EqBandConfig[],
    onChange: EqBandChangeHandler,
    onCommit: EqBandChangeHandler
  ) {
    this.canvas = canvas;
    this.bands = bands.map(b => ({ ...b }));
    this.onChange = onChange;
    this.onCommit = onCommit;

    this.boundPointerDown = this.handlePointerDown.bind(this);
    this.boundPointerMove = this.handlePointerMove.bind(this);
    this.boundPointerUp = this.handlePointerUp.bind(this);
    this.boundPointerLeave = this.handlePointerLeave.bind(this);

    this.canvas.addEventListener("pointerdown", this.boundPointerDown);
    this.canvas.addEventListener("pointermove", this.boundPointerMove);
    this.canvas.addEventListener("pointerup", this.boundPointerUp);
    this.canvas.addEventListener("pointerleave", this.boundPointerLeave);
    this.canvas.style.touchAction = "none";

    this.draw();
  }

  updateBands(bands: EqBandConfig[]): void {
    this.bands = bands.map(b => ({ ...b }));
    if (!this.isDragging) {
      this.draw();
    }
  }

  draw(): void {
    if (this.destroyed) return;
    const ctx = this.canvas.getContext("2d");
    if (!ctx) return;

    const width = Math.max(1, this.canvas.clientWidth);
    const height = Math.max(1, this.canvas.clientHeight);
    if (this.canvas.width !== width || this.canvas.height !== height) {
      this.canvas.width = width;
      this.canvas.height = height;
    }

    // Draw base curve (clears canvas, draws grid + combined response)
    const eqBands: EqBand[] = this.bands.map(b => ({ freq: b.freq, gainDb: b.gainDb, q: b.q }));
    drawEqCurve(this.canvas, eqBands);

    // Draw per-band shaded fills and individual response curves
    this.drawBandOverlays(ctx);

    // Compute handle positions and render them
    this.handles = [];
    this.computeHandles();
    this.drawHandles(ctx);
  }

  // --- Coordinate mapping ---

  private get plotWidth(): number {
    return this.canvas.width - this.PADDING * 2;
  }

  private get plotHeight(): number {
    return this.canvas.height - this.PADDING * 2;
  }

  private freqToX(freq: number): number {
    const logMin = Math.log10(this.MIN_FREQ);
    const logMax = Math.log10(this.MAX_FREQ);
    return this.PADDING + this.plotWidth * (Math.log10(freq) - logMin) / (logMax - logMin);
  }

  private xToFreq(x: number): number {
    const logMin = Math.log10(this.MIN_FREQ);
    const logMax = Math.log10(this.MAX_FREQ);
    const ratio = (x - this.PADDING) / this.plotWidth;
    return this.MIN_FREQ * Math.pow(10, ratio * (logMax - logMin));
  }

  private dbToY(db: number): number {
    const clamped = Math.max(this.MIN_DB, Math.min(this.MAX_DB, db));
    return this.PADDING + (this.MAX_DB - clamped) / (this.MAX_DB - this.MIN_DB) * this.plotHeight;
  }

  private yToDb(y: number): number {
    return this.MAX_DB - (y - this.PADDING) / this.plotHeight * (this.MAX_DB - this.MIN_DB);
  }

  // --- Drawing helpers ---

  private drawBandOverlays(ctx: CanvasRenderingContext2D): void {
    const zeroY = this.dbToY(0);

    for (let i = 0; i < this.bands.length; i++) {
      const band = this.bands[i];
      if (Math.abs(band.gainDb) < 0.05) continue;

      const color = BAND_COLORS[i % BAND_COLORS.length];

      // Subtle filled area showing band contribution
      ctx.fillStyle = color.replace(/[\d.]+\)$/, "0.06)");
      ctx.beginPath();
      ctx.moveTo(this.PADDING, zeroY);

      for (let px = 0; px <= this.plotWidth; px++) {
        const freq = this.xToFreq(this.PADDING + px);
        const mag = peakingMagnitude(freq, band, this.SAMPLE_RATE);
        const db = 20 * Math.log10(Math.max(1e-6, mag));
        ctx.lineTo(this.PADDING + px, this.dbToY(db));
      }

      ctx.lineTo(this.PADDING + this.plotWidth, zeroY);
      ctx.closePath();
      ctx.fill();

      // Individual band response curve
      ctx.strokeStyle = color.replace(/[\d.]+\)$/, "0.2)");
      ctx.lineWidth = 1;
      ctx.beginPath();

      for (let px = 0; px <= this.plotWidth; px++) {
        const freq = this.xToFreq(this.PADDING + px);
        const mag = peakingMagnitude(freq, band, this.SAMPLE_RATE);
        const db = 20 * Math.log10(Math.max(1e-6, mag));
        const y = this.dbToY(db);
        if (px === 0) ctx.moveTo(this.PADDING, y);
        else ctx.lineTo(this.PADDING + px, y);
      }

      ctx.stroke();
    }
  }

  private computeHandles(): void {
    for (let i = 0; i < this.bands.length; i++) {
      const band = this.bands[i];
      const x = this.freqToX(band.freq);
      const y = this.dbToY(band.gainDb);

      // Main frequency/gain handle
      this.handles.push({ bandIndex: i, type: "main", x, y, radius: this.HANDLE_RADIUS });

      // Q wing handles for bands that support Q adjustment
      if (band.hasQ && band.q > 0) {
        const bw = 2 * Math.asinh(1 / (2 * band.q)) / Math.LN2;
        const fLow = band.freq / Math.pow(2, bw / 2);
        const fHigh = band.freq * Math.pow(2, bw / 2);

        this.handles.push({
          bandIndex: i, type: "q-left",
          x: this.freqToX(Math.max(this.MIN_FREQ, fLow)), y,
          radius: this.Q_HANDLE_RADIUS,
        });
        this.handles.push({
          bandIndex: i, type: "q-right",
          x: this.freqToX(Math.min(this.MAX_FREQ, fHigh)), y,
          radius: this.Q_HANDLE_RADIUS,
        });
      }
    }
  }

  private drawHandles(ctx: CanvasRenderingContext2D): void {
    // Draw Q connecting lines first (behind handles)
    for (const handle of this.handles) {
      if (handle.type !== "q-left" && handle.type !== "q-right") continue;
      const mainHandle = this.handles.find(h => h.bandIndex === handle.bandIndex && h.type === "main");
      if (!mainHandle) continue;

      const color = BAND_COLORS[handle.bandIndex % BAND_COLORS.length];
      ctx.strokeStyle = color.replace(/[\d.]+\)$/, "0.3)");
      ctx.lineWidth = 1.5;
      ctx.setLineDash([3, 3]);
      ctx.beginPath();
      ctx.moveTo(mainHandle.x, mainHandle.y);
      ctx.lineTo(handle.x, handle.y);
      ctx.stroke();
      ctx.setLineDash([]);
    }

    // Draw all handle circles
    for (const handle of this.handles) {
      const isHovered = this.hoveredHandle !== null &&
        this.hoveredHandle.bandIndex === handle.bandIndex &&
        this.hoveredHandle.type === handle.type;
      const isDragged = this.dragHandle !== null &&
        this.dragHandle.bandIndex === handle.bandIndex &&
        this.dragHandle.type === handle.type;

      const color = BAND_COLORS[handle.bandIndex % BAND_COLORS.length];

      ctx.save();
      if (isDragged) {
        ctx.shadowColor = color;
        ctx.shadowBlur = 12;
      } else if (isHovered) {
        ctx.shadowColor = color;
        ctx.shadowBlur = 6;
      }

      const radius = isHovered || isDragged ? handle.radius + 2 : handle.radius;

      if (handle.type === "main") {
        // Circle for main handles
        ctx.beginPath();
        ctx.arc(handle.x, handle.y, radius, 0, Math.PI * 2);
        ctx.fillStyle = color;
        ctx.fill();
      } else {
        // Semi-translucent square for Q wing handles
        const size = radius * 2;
        ctx.fillStyle = color.replace(/[\d.]+\)$/, "0.35)");
        ctx.fillRect(handle.x - size / 2, handle.y - size / 2, size, size);
      }

      ctx.strokeStyle = "rgba(255, 255, 255, 0.9)";
      ctx.lineWidth = isDragged ? 2 : 1.5;
      if (handle.type === "main") {
        ctx.stroke();
      } else {
        const size = (isHovered || isDragged ? handle.radius + 2 : handle.radius) * 2;
        ctx.strokeRect(handle.x - size / 2, handle.y - size / 2, size, size);
      }
      ctx.restore();
    }

    // Draw tooltip for hovered or actively-dragged handle
    const tooltipHandle = this.dragHandle ?? this.hoveredHandle;
    if (tooltipHandle) {
      const band = this.bands[tooltipHandle.bandIndex];
      const handle = this.handles.find(
        h => h.bandIndex === tooltipHandle.bandIndex && h.type === tooltipHandle.type
      );
      if (!handle) return;

      let label: string;
      if (tooltipHandle.type === "main") {
        label = `${band.label}: ${Math.round(band.freq)} Hz  ${band.gainDb >= 0 ? "+" : ""}${band.gainDb.toFixed(1)} dB`;
      } else {
        label = `Q: ${band.q.toFixed(1)}`;
      }

      ctx.save();
      ctx.font = "11px -apple-system, BlinkMacSystemFont, sans-serif";
      ctx.textAlign = "center";

      const labelY = Math.max(20, handle.y - handle.radius - 10);
      const metrics = ctx.measureText(label);
      const pad = 5;
      const bgX = handle.x - metrics.width / 2 - pad;
      const bgW = metrics.width + pad * 2;
      const bgH = 18;
      const bgY = labelY - 12;

      // Tooltip background
      const r = 4;
      ctx.fillStyle = "rgba(0, 0, 0, 0.8)";
      ctx.beginPath();
      ctx.moveTo(bgX + r, bgY);
      ctx.lineTo(bgX + bgW - r, bgY);
      ctx.arcTo(bgX + bgW, bgY, bgX + bgW, bgY + r, r);
      ctx.lineTo(bgX + bgW, bgY + bgH - r);
      ctx.arcTo(bgX + bgW, bgY + bgH, bgX + bgW - r, bgY + bgH, r);
      ctx.lineTo(bgX + r, bgY + bgH);
      ctx.arcTo(bgX, bgY + bgH, bgX, bgY + bgH - r, r);
      ctx.lineTo(bgX, bgY + r);
      ctx.arcTo(bgX, bgY, bgX + r, bgY, r);
      ctx.closePath();
      ctx.fill();

      ctx.fillStyle = "#ffffff";
      ctx.fillText(label, handle.x, labelY);
      ctx.restore();
    }
  }

  // --- Pointer event handlers ---

  private getCanvasCoords(e: PointerEvent): { x: number; y: number } {
    const rect = this.canvas.getBoundingClientRect();
    const scaleX = this.canvas.width / rect.width;
    const scaleY = this.canvas.height / rect.height;
    return {
      x: (e.clientX - rect.left) * scaleX,
      y: (e.clientY - rect.top) * scaleY,
    };
  }

  private hitTestHandle(x: number, y: number): HandleInfo | null {
    let bestDist = Infinity;
    let bestHandle: HandleInfo | null = null;

    for (const h of this.handles) {
      const dx = x - h.x;
      const dy = y - h.y;
      const dist = Math.sqrt(dx * dx + dy * dy);
      if (dist <= h.radius + this.HIT_SLOP && dist < bestDist) {
        bestDist = dist;
        bestHandle = h;
      }
    }
    return bestHandle;
  }

  private handlePointerDown(e: PointerEvent): void {
    const coords = this.getCanvasCoords(e);
    const hit = this.hitTestHandle(coords.x, coords.y);
    if (hit) {
      this.isDragging = true;
      this.dragHandle = hit;
      this.canvas.setPointerCapture(e.pointerId);
      this.canvas.style.cursor = "grabbing";
      e.preventDefault();
      e.stopPropagation();
    }
  }

  private handlePointerMove(e: PointerEvent): void {
    const coords = this.getCanvasCoords(e);

    if (this.isDragging && this.dragHandle) {
      const band = this.bands[this.dragHandle.bandIndex];

      if (this.dragHandle.type === "main") {
        // Horizontal → frequency, vertical → gain
        let newFreq = this.xToFreq(coords.x);
        newFreq = Math.max(band.freqMin, Math.min(band.freqMax, newFreq));
        newFreq = Math.round(newFreq);

        let newGain = this.yToDb(coords.y);
        newGain = Math.max(band.gainMin, Math.min(band.gainMax, newGain));
        // Snap to 0 dB when close
        if (Math.abs(newGain) < 0.3) newGain = 0;
        newGain = Math.round(newGain * 10) / 10;

        band.freq = newFreq;
        band.gainDb = newGain;
        this.onChange(this.dragHandle.bandIndex, band.freq, band.gainDb, band.q);
      } else {
        // Q handle drag: distance from center frequency controls Q
        const dragFreq = this.xToFreq(coords.x);
        const centerFreq = band.freq;
        let ratio: number;
        if (this.dragHandle.type === "q-right") {
          ratio = Math.max(1.05, dragFreq / centerFreq);
        } else {
          ratio = Math.max(1.05, centerFreq / Math.max(1, dragFreq));
        }
        const bw = 2 * Math.log2(ratio);
        let newQ = bw > 0.001 ? 1 / (2 * Math.sinh(bw * Math.LN2 / 2)) : band.qMax;
        newQ = Math.max(band.qMin, Math.min(band.qMax, newQ));
        newQ = Math.round(newQ * 10) / 10;

        band.q = newQ;
        this.onChange(this.dragHandle.bandIndex, band.freq, band.gainDb, band.q);
      }

      this.draw();
      return;
    }

    // Hover detection (not dragging)
    const hit = this.hitTestHandle(coords.x, coords.y);
    if (hit !== this.hoveredHandle) {
      this.hoveredHandle = hit;
      this.canvas.style.cursor = hit ? "grab" : "";
      this.draw();
    }
  }

  private handlePointerUp(e: PointerEvent): void {
    if (this.isDragging && this.dragHandle) {
      const band = this.bands[this.dragHandle.bandIndex];
      this.onCommit(this.dragHandle.bandIndex, band.freq, band.gainDb, band.q);
      this.isDragging = false;
      this.dragHandle = null;
      this.canvas.releasePointerCapture(e.pointerId);

      const coords = this.getCanvasCoords(e);
      const hit = this.hitTestHandle(coords.x, coords.y);
      this.hoveredHandle = hit;
      this.canvas.style.cursor = hit ? "grab" : "";
      this.draw();
    }
  }

  private handlePointerLeave(_e: PointerEvent): void {
    if (!this.isDragging) {
      this.hoveredHandle = null;
      this.canvas.style.cursor = "";
      this.draw();
    }
  }

  destroy(): void {
    this.destroyed = true;
    this.canvas.removeEventListener("pointerdown", this.boundPointerDown);
    this.canvas.removeEventListener("pointermove", this.boundPointerMove);
    this.canvas.removeEventListener("pointerup", this.boundPointerUp);
    this.canvas.removeEventListener("pointerleave", this.boundPointerLeave);
  }
}
