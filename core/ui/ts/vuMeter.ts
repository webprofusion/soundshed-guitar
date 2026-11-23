import type { SignalLevelMetrics } from "./types.js";

/**
 * The segmented peak meters in the control bar — one for the input, one for the output.
 *
 * Both are driven from the same 20 Hz signal diagnostics frame, so this is written to do
 * as little as possible per frame: DOM references are resolved once and cached, and every
 * write is guarded by a memo of what was last written. A meter that is not moving costs
 * one comparison a frame.
 *
 * Each meter owns its own peak-hold state, which is why this is a factory rather than a
 * pair of module-level functions — two meters sharing one hold would each reset the
 * other's tick.
 */

// Threshold (dBFS) for each segment, top → bottom in the DOM (rendered bottom-up via flex column-reverse).
// Matches the data-db attributes on .vu-seg elements in the ui-components markup.
const VU_SEGMENT_THRESHOLDS = [-3, -6, -9, -12, -18, -24, -36, -48] as const;

// Pixels per segment: 5px of .vu-seg plus the 2px flex gap, from css/vu-meter.css.
// Used to place the peak-hold tick at the top edge of the topmost lit segment.
const VU_SEGMENT_HEIGHT_PX = 7;

const VU_PEAK_HOLD_MS = 2000;

// Below this the readout reads -∞ rather than a number. The backend floors silence at
// -120 dBFS, which is a true value and a useless label; -60 is a little under the
// quietest segment, so the number still says something after the ladder has gone dark.
const VU_READOUT_FLOOR_DBFS = -60;

// The analog gauge a theme can show in place of the ladder (Studio 1973's needle meter):
// it reads RMS, as a VU meter does, with 0 VU at -18 dBFS (the usual digital alignment) and
// its end stop at +3 VU. The face is images/themes/studio-1973/vu-face.png, whose sweep is
// ±VU_GAUGE_SWEEP_DEG (tools/studio-1973-theme-assets/vu-face.mjs).
const VU_GAUGE_ZERO_DBFS = -18;
const VU_GAUGE_SWEEP_DEG = 42;
// Needle ballistics, per 20 Hz frame: a VU needle reaches 99% of a step in about 300 ms,
// a time constant near 65 ms, so each 50 ms frame closes 1 - e^(-50/65) of the gap.
const VU_GAUGE_BALLISTIC = 0.54;
// Smallest needle move worth a repaint: about a third of a degree.
const VU_GAUGE_MIN_STEP = 0.004;

/** Where a VU needle sits (0 rest, 1 end stop) for an RMS level in dBFS. */
export function vuGaugeDeflection(rmsDbfs: number): number {
  if (!isFinite(rmsDbfs)) return 0;
  return Math.min(1, Math.pow(10, (rmsDbfs - VU_GAUGE_ZERO_DBFS) / 20) / Math.SQRT2);
}

/**
 * The held peak as a label. Whole dB only — a tenth of a dB is past what anyone reads off
 * a meter at a glance, and "-12.4 dB" needs half again the width of "-12 dB". Overs carry
 * a "+" so a hot output is obvious even when every segment is already lit.
 *
 * The unit is written "dB" rather than "dBFS" to match the knob values alongside it in the
 * control bar; that these are full-scale is what the meter's own tooltip is for.
 */
function formatReadout(dbfs: number | null): string {
  if (dbfs === null || dbfs <= VU_READOUT_FLOOR_DBFS) {
    return "-∞ dB";
  }

  const whole = Math.round(dbfs);

  return `${whole > 0 ? "+" : ""}${whole} dB`;
}

export interface VuMeter {
  /** Applies one diagnostics frame. Pass null to clear the meter. */
  update(levels: SignalLevelMetrics | null): void;
}

export interface VuMeterOptions {
  /** Element id of the `.vu-segments` container holding the `.vu-seg` children. */
  segmentsId: string;
  /** Element id of the `.vu-peak-hold` tick inside that container. */
  peakHoldId: string;
  /** Element id of the `.vu-readout` label above the ladder. Optional. */
  readoutId?: string;
  /**
   * Element id of the `.vu-gauge` needle meter, which only some themes show. Optional. It is
   * driven by one custom property, --vu-needle (an angle), which the theme paints with: a
   * background, never a transform, so a moving needle repaints a 66 x 48 box and nothing else.
   */
  gaugeId?: string;
}

export function createVuMeter({ segmentsId, peakHoldId, readoutId, gaugeId }: VuMeterOptions): VuMeter {
  // Cached DOM references (populated on first update).
  let segments: HTMLElement[] | null = null;
  let peakHoldEl: HTMLElement | null = null;
  let readoutEl: HTMLElement | null = null;
  let lastReadoutText: string | null = null;

  let peakHoldDbfs = -Infinity;
  let peakHoldTimer: ReturnType<typeof setTimeout> | null = null;
  let lastPeakDbfs: number | null = null;
  let lastActiveStates: boolean[] | null = null;
  let lastPeakHoldVisible = false;
  let lastPeakHoldTop: string | null = null;
  let gaugeEl: HTMLElement | null = null;
  let needle = 0;
  let lastNeedle = -1;

  // The needle eases toward the RMS level each frame and drops straight to rest when the
  // frames stop (null), since no later frame would carry it down.
  function moveNeedle(levels: SignalLevelMetrics | null): void {
    if (!gaugeEl) return;
    const target = levels ? vuGaugeDeflection(levels.rmsDbfs) : 0;
    needle = levels ? needle + (target - needle) * VU_GAUGE_BALLISTIC : 0;
    if (needle < VU_GAUGE_MIN_STEP && target === 0) needle = 0;
    if (Math.abs(needle - lastNeedle) < VU_GAUGE_MIN_STEP && !(needle === 0 && lastNeedle !== 0)) return;
    lastNeedle = needle;
    const deg = -VU_GAUGE_SWEEP_DEG + 2 * VU_GAUGE_SWEEP_DEG * needle;
    gaugeEl.style.setProperty("--vu-needle", `${deg.toFixed(2)}deg`);
  }

  function update(levels: SignalLevelMetrics | null): void {
    if (!segments) {
      const container = document.getElementById(segmentsId);
      segments = container
        ? Array.from(container.querySelectorAll<HTMLElement>(".vu-seg"))
        : [];
      peakHoldEl = document.getElementById(peakHoldId);
      readoutEl = readoutId ? document.getElementById(readoutId) : null;
      gaugeEl = gaugeId ? document.getElementById(gaugeId) : null;
      lastActiveStates = new Array(segments.length).fill(false);
    }

    moveNeedle(levels);

    const writeReadout = (dbfs: number | null): void => {
      if (!readoutEl) return;
      const text = formatReadout(dbfs);
      if (text === lastReadoutText) return;
      readoutEl.textContent = text;
      lastReadoutText = text;
    };

    if (!segments.length) return;

    const dbfs = levels && isFinite(levels.peakDbfs) ? levels.peakDbfs : null;

    // Check if the value actually changed before updating
    if (dbfs === lastPeakDbfs && dbfs === null) {
      return; // Already null, no update needed
    }

    if (dbfs === null) {
      // Fade out: update only if we had active segments
      if (lastActiveStates?.some((active) => active)) {
        segments.forEach((s) => s.classList.remove("active"));
        lastActiveStates.fill(false);
      }
      if (lastPeakHoldVisible && peakHoldEl) {
        peakHoldEl.classList.remove("visible");
        lastPeakHoldVisible = false;
      }
      writeReadout(null);
      lastPeakDbfs = null;
      return;
    }

    // Update segment indicators only if peak value changed significantly (rounded to 0.5 dB to reduce updates)
    const roundedDbfs = Math.round(dbfs * 2) / 2;
    const lastRoundedDbfs = lastPeakDbfs !== null ? Math.round(lastPeakDbfs * 2) / 2 : null;

    if (roundedDbfs !== lastRoundedDbfs) {
      // Compute new active states
      const newActiveStates = VU_SEGMENT_THRESHOLDS.map((threshold) => dbfs >= threshold);

      // Update only segments that changed state
      segments.forEach((seg, i) => {
        const isNowActive = newActiveStates[i];
        const wasActive = lastActiveStates?.[i] ?? false;
        if (isNowActive !== wasActive) {
          seg.classList.toggle("active", isNowActive);
        }
      });

      lastActiveStates = newActiveStates;
      lastPeakDbfs = dbfs;
    }

    // Peak-hold tick: update if new peak is higher. The readout is the same held value in
    // words, so the number and the tick can never disagree — and it is written before the
    // "is anything lit" guard below, so a signal under the quietest segment still reads out.
    if (dbfs >= peakHoldDbfs) {
      peakHoldDbfs = dbfs;
      writeReadout(dbfs);

      if (peakHoldTimer !== null) clearTimeout(peakHoldTimer);
      peakHoldTimer = setTimeout(() => {
        peakHoldDbfs = -Infinity;
        if (peakHoldEl) peakHoldEl.classList.remove("visible");
        lastPeakHoldVisible = false;
      }, VU_PEAK_HOLD_MS);

      // Position peak-hold tick at the top of the topmost lit segment.
      // With top-to-bottom DOM order (red at top), firstLitIdx is the loudest lit segment.
      const firstLitIdx = segments.findIndex((s) => s.classList.contains("active"));
      if (firstLitIdx >= 0 && peakHoldEl) {
        const newTop = `${firstLitIdx * VU_SEGMENT_HEIGHT_PX}px`;
        if (newTop !== lastPeakHoldTop) {
          peakHoldEl.style.top = newTop;
          lastPeakHoldTop = newTop;
        }
        if (!lastPeakHoldVisible) {
          peakHoldEl.classList.add("visible");
          lastPeakHoldVisible = true;
        }
      }
    }
  }

  return { update };
}
