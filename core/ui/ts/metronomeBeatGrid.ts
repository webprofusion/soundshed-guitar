/**
 * The metronome's beat strip: one dot per beat of the bar.
 *
 * It is both the display and the editor. Clicking a beat cycles it through
 * accent → medium → normal → off, which is the whole accent pattern UI; the
 * engine's `metronomeBeat` messages light the dot that is currently sounding.
 * Ticks under each dot show how the beat is subdivided, and a gap opens where
 * an odd meter's grouping starts a new cluster.
 */

import type { BeatLevel } from "./metronomeMeters.js";
import { beatLevels, groupHeads, nextBeatLevel } from "./metronomeMeters.js";

export interface BeatGridState {
  num: number;
  den: number;
  grouping: string;
  pattern: string;
  ticksPerBeat: number;
  editable: boolean;
}

const LEVEL_CLASS: Record<BeatLevel, string> = {
  accent: "is-accent",
  medium: "is-medium",
  normal: "is-normal",
  off: "is-off",
};

const LEVEL_NAME: Record<BeatLevel, string> = {
  accent: "accent",
  medium: "medium",
  normal: "normal",
  off: "silent",
};

/** How long a beat stays lit. Short enough not to blur at 300 bpm. */
const PULSE_MS = 120;

export class MetronomeBeatGrid {
  private readonly container: HTMLElement;

  private readonly onChange: (pattern: BeatLevel[]) => void;

  private levels: BeatLevel[] = [];

  private pulseTimer: number | null = null;

  private lastPulsed: HTMLElement | null = null;

  constructor(container: HTMLElement, onChange: (levels: BeatLevel[]) => void) {
    this.container = container;
    this.onChange = onChange;
  }

  render(state: BeatGridState): void {
    this.levels = beatLevels(state.pattern, state.num, state.den, state.grouping);
    const heads = groupHeads(state.num, state.den, state.grouping);
    const ticks = Math.max(1, Math.min(8, Math.round(state.ticksPerBeat)));

    this.clearPulse();
    this.container.innerHTML = "";
    this.container.classList.toggle("is-disabled", !state.editable);

    this.levels.forEach((level, index) => {
      const beat = document.createElement("button");
      beat.type = "button";
      beat.className = `metro-beat ${LEVEL_CLASS[level]}`;
      beat.dataset.beat = String(index);
      beat.disabled = !state.editable;
      beat.classList.toggle("is-group-head", heads.has(index));
      beat.setAttribute("aria-label", `Beat ${index + 1}: ${LEVEL_NAME[level]}`);
      beat.title = `Beat ${index + 1} — ${LEVEL_NAME[level]}. Click to change.`;

      const dot = document.createElement("span");
      dot.className = "metro-beat-dot";
      beat.appendChild(dot);

      if (ticks > 1) {
        const row = document.createElement("span");
        row.className = "metro-beat-ticks";
        row.setAttribute("aria-hidden", "true");
        // The beat itself is the first tick; only the ones between beats are drawn.
        for (let i = 1; i < ticks; i += 1) {
          row.appendChild(document.createElement("i"));
        }
        beat.appendChild(row);
      }

      beat.addEventListener("click", () => {
        if (!state.editable) return;
        this.cycle(index, beat);
      });

      this.container.appendChild(beat);
    });
  }

  /** Lights the beat the engine just played. */
  pulse(beatIndex: number): void {
    const beat = this.container.querySelector<HTMLElement>(`.metro-beat[data-beat="${beatIndex}"]`);
    if (!beat) return;

    if (this.lastPulsed && this.lastPulsed !== beat) {
      this.lastPulsed.classList.remove("is-playing");
    }

    // Restart the animation even when the same beat repeats (a one-beat bar).
    beat.classList.remove("is-playing");
    void beat.offsetWidth;
    beat.classList.add("is-playing");
    this.lastPulsed = beat;

    if (this.pulseTimer !== null) window.clearTimeout(this.pulseTimer);
    this.pulseTimer = window.setTimeout(() => {
      beat.classList.remove("is-playing");
      this.pulseTimer = null;
    }, PULSE_MS);
  }

  clearPulse(): void {
    if (this.pulseTimer !== null) {
      window.clearTimeout(this.pulseTimer);
      this.pulseTimer = null;
    }
    this.lastPulsed?.classList.remove("is-playing");
    this.lastPulsed = null;
  }

  private cycle(index: number, beat: HTMLElement): void {
    const level = nextBeatLevel(this.levels[index] ?? "normal");
    this.levels[index] = level;

    for (const className of Object.values(LEVEL_CLASS)) beat.classList.remove(className);
    beat.classList.add(LEVEL_CLASS[level]);
    beat.setAttribute("aria-label", `Beat ${index + 1}: ${LEVEL_NAME[level]}`);
    beat.title = `Beat ${index + 1} — ${LEVEL_NAME[level]}. Click to change.`;

    this.onChange([...this.levels]);
  }
}
