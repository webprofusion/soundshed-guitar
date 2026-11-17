/**
 * GenericKnob — the app's rotary control: drag to change, double-click to
 * reset, double-click the value to type an exact one.
 *
 * Lifted out of controls.ts so anything that needs a knob can have one without
 * importing that 1,800-line module (which would put every such module in an
 * import cycle with it). controls.ts re-exports it, so existing importers keep
 * using `./controls.js`.
 */

import { appendLog } from "./logging.js";
import { setParameter } from "./bridge.js";
import { filmstripBackground, filmstripFrame, filmstripFrameCount, knobFaceAngle } from "./layoutKnobImages.js";
import { effectiveTaper, taperPositionToValue, valueToTaperPosition, type ParamTaper } from "./paramTaper.js";
import { clampValue, countStepDecimals, deriveRangeStep } from "./utils.js";
import { isWheelAdjustEnabled } from "./wheelAdjust.js";

/** A wheel notch on a tapered knob moves this share of its travel: 100 notches end to end. */
const TAPER_WHEEL_NOTCH = 0.01;

export interface KnobConfig {
  knobElement: HTMLElement;
  paramId: string;
  minValue: number;
  maxValue: number;
  defaultValue: number;
  displayFormat: (value: number) => string;
  valueDisplayId?: string;
  valueDisplay?: HTMLElement | null;
  labelElement?: HTMLElement | null;
  sensitivity?: number;
  stepValue?: number;
  /**
   * How the knob travels across min..max; absent means linear. On a log taper a drag or a
   * wheel notch moves an equal share of the travel, which is an equal ratio of the value,
   * and a full sweep takes the same drag as it would on a linear one.
   */
  taper?: ParamTaper;
  onValueChange?: (value: number) => void;
  onValueCommit?: (value: number) => void;
  sendParameter?: boolean;
}

export class GenericKnob {
  private knobElement: HTMLElement;
  private paramId: string;
  private minValue: number;
  private maxValue: number;
  private defaultValue: number;
  private currentValue: number;
  private displayFormat: (value: number) => string;
  private valueDisplay: HTMLElement | null;
  private labelElement: HTMLElement | null;
  private editableValueElement: HTMLElement | null;
  private sensitivity: number;
  private stepValue: number;
  private explicitStep: number | undefined;
  private declaredTaper: ParamTaper | undefined;
  /** The declared taper as it applies to the current range (see effectiveTaper). */
  private taper: ParamTaper;
  private onValueChange?: (value: number) => void;
  private onValueCommit?: (value: number) => void;
  private sendParameter: boolean;
  private isDragging = false;
  private startY = 0;
  private startValue = 0;
  private inlineEditor: HTMLInputElement | null = null;
  private activePointerId: number | null = null;

  constructor(config: KnobConfig) {
    this.knobElement = config.knobElement;
    this.paramId = config.paramId;
    this.minValue = config.minValue;
    this.maxValue = config.maxValue;
    this.defaultValue = config.defaultValue;
    this.currentValue = config.defaultValue;
    this.displayFormat = config.displayFormat;
    this.sensitivity = config.sensitivity ?? 0.5;
    this.stepValue = deriveRangeStep(this.minValue, this.maxValue, config.stepValue);
    this.explicitStep = Number.isFinite(config.stepValue) && (config.stepValue ?? 0) > 0 ? config.stepValue : undefined;
    this.declaredTaper = config.taper;
    this.taper = effectiveTaper(this.declaredTaper, this.minValue, this.maxValue);
    this.onValueChange = config.onValueChange;
    this.onValueCommit = config.onValueCommit;
    this.sendParameter = config.sendParameter ?? true;
    
    this.valueDisplay = config.valueDisplay
      ?? (config.valueDisplayId ? document.getElementById(config.valueDisplayId) : null);
    this.labelElement = config.labelElement
      ?? (this.knobElement.parentElement?.querySelector(
        ".knob-label, .node-param-label, .custom-control-label",
      ) as HTMLElement | null);
    this.editableValueElement = this.valueDisplay;
    if (this.editableValueElement && !this.editableValueElement.dataset.originalLabel) {
      this.editableValueElement.dataset.originalLabel = this.editableValueElement.textContent?.trim() ?? "";
    }

    this.initialize();
  }

  private initialize(): void {
    // Set initial value from data attribute if present
    const dataValue = parseFloat(this.knobElement.dataset.value ?? "");
    if (!isNaN(dataValue)) {
      this.currentValue = dataValue;
    }

    this.knobElement.tabIndex = this.knobElement.tabIndex >= 0 ? this.knobElement.tabIndex : 0;
    this.knobElement.setAttribute("role", "slider");
    this.knobElement.setAttribute("aria-label", this.labelElement?.textContent?.trim() || this.paramId);
    this.knobElement.setAttribute("aria-valuemin", this.minValue.toString());
    this.knobElement.setAttribute("aria-valuemax", this.maxValue.toString());

    this.updateDisplay(this.currentValue);
    this.setupEventListeners();
  }

  private setupEventListeners(): void {
    // Use pointer events for unified support of mouse, touch, and pen interactions.
    // Attach move/up to the element; pointer capture ensures we receive events during drag
    // even if the pointer leaves the knob. This also avoids accumulating document listeners
    // when many effect knobs are created (e.g. node param panels).
    this.knobElement.addEventListener("pointerdown", (e) => this.onPointerDown(e));
    this.knobElement.addEventListener("pointermove", (e) => this.onPointerMove(e));
    this.knobElement.addEventListener("pointerup", (e) => this.onPointerUp(e));
    this.knobElement.addEventListener("pointercancel", (e) => this.onPointerUp(e));

    this.knobElement.addEventListener("dblclick", (e) => this.onDoubleClick(e as MouseEvent));
    this.knobElement.addEventListener("wheel", (e) => this.onWheel(e), { passive: false });
    this.editableValueElement?.addEventListener("dblclick", (e) => this.onValueDoubleClick(e as MouseEvent));

    // Legacy fallback only when PointerEvent API is unavailable (rare in modern browsers).
    if (typeof window !== "undefined" && !("PointerEvent" in window)) {
      this.knobElement.addEventListener("mousedown", (e) => this.onMouseDown(e));
      document.addEventListener("mousemove", (e) => this.onMouseMove(e));
      document.addEventListener("mouseup", () => this.onMouseUp());
    }

    // Ensure touch interactions don't trigger scrolling/zooming while manipulating the knob.
    if (this.knobElement.style && !this.knobElement.style.touchAction) {
      this.knobElement.style.touchAction = "none";
    }
  }

  private emitLiveValue(value: number): void {
    if (this.sendParameter) {
      setParameter(this.paramId, value);
    }

    if (this.onValueChange) {
      this.onValueChange(value);
    }
  }

  private commitCurrentValue(): void {
    if (this.sendParameter) {
      setParameter(this.paramId, this.currentValue);
      appendLog(`${this.paramId} → ${this.currentValue.toFixed(2)}`);
    }

    if (this.onValueCommit) {
      this.onValueCommit(this.currentValue);
    }
  }

  private applyValue(value: number, commit = false): void {
    this.setValue(value);
    this.emitLiveValue(this.currentValue);
    if (commit) {
      this.commitCurrentValue();
    }
  }

  private onDoubleClick(e: MouseEvent): void {
    e.preventDefault();
    e.stopPropagation();
    this.closeInlineEditor(true);
    this.setValue(this.defaultValue);
    if (this.sendParameter) {
      setParameter(this.paramId, this.defaultValue);
      appendLog(`${this.paramId} → ${this.defaultValue.toFixed(2)} (reset to default)`);
    }
    
    if (this.onValueChange) {
      this.onValueChange(this.defaultValue);
    }

    if (this.onValueCommit) {
      this.onValueCommit(this.defaultValue);
    }
  }

  private onPointerDown(e: PointerEvent): void {
    if (e.button !== undefined && e.button !== 0) {
      return; // Only primary button / touch
    }
    this.knobElement.focus();
    this.closeInlineEditor(true);
    this.isDragging = true;
    this.startY = e.clientY;
    this.startValue = this.currentValue;
    this.activePointerId = e.pointerId ?? null;

    if (typeof this.knobElement.setPointerCapture === "function") {
      try {
        this.knobElement.setPointerCapture(e.pointerId);
      } catch {
        // Capture may fail in some environments; fall back to document tracking (handled by existing mouse paths if needed)
      }
    }
    e.preventDefault();
  }

  private onPointerMove(e: PointerEvent): void {
    if (!this.isDragging) return;
    if (this.activePointerId != null && (e.pointerId ?? null) !== this.activePointerId) return;

    const newValue = this.dragValue(this.startY - e.clientY);

    this.currentValue = newValue;
    this.knobElement.dataset.value = newValue.toString();
    this.updateDisplay(newValue);

    this.emitLiveValue(this.currentValue);
    // No need to preventDefault on every move for pointer (capture handles delivery)
  }

  /** The value `deltaY` pixels of upward drag from where the drag started. */
  private dragValue(deltaY: number): number {
    if (this.taper === "linear") {
      return clampValue(this.startValue + deltaY * this.sensitivity, this.minValue, this.maxValue);
    }

    // Through the taper, at the rate that sweeps a linear knob end to end, so the whole
    // travel is the same drag either way. effectiveTaper guarantees a positive span here.
    const span = this.maxValue - this.minValue;
    const startPosition = valueToTaperPosition(this.startValue, this.minValue, this.maxValue, this.taper);
    const position = startPosition + (deltaY * this.sensitivity) / span;
    return taperPositionToValue(position, this.minValue, this.maxValue, this.taper);
  }

  /** The value one wheel notch up (1) or down (-1) from the current one. */
  private wheelValue(direction: 1 | -1): number {
    if (this.taper === "linear") {
      return this.currentValue + direction * this.stepValue;
    }

    const position = valueToTaperPosition(this.currentValue, this.minValue, this.maxValue, this.taper);
    const next = taperPositionToValue(position + direction * TAPER_WHEEL_NOTCH, this.minValue, this.maxValue, this.taper);
    // Low on a log taper a notch can be smaller than a declared step, which the caller's
    // snap would then undo; move at least one step.
    if (this.explicitStep !== undefined && Math.abs(next - this.currentValue) < this.explicitStep) {
      return this.currentValue + direction * this.explicitStep;
    }
    return next;
  }

  private onPointerUp(e: PointerEvent): void {
    if (!this.isDragging) return;
    if (this.activePointerId != null && (e.pointerId ?? null) !== this.activePointerId) return;

    if (typeof this.knobElement.releasePointerCapture === "function") {
      try {
        this.knobElement.releasePointerCapture(e.pointerId);
      } catch {}
    }

    this.isDragging = false;
    this.activePointerId = null;
    this.commitCurrentValue();
  }

  private onMouseDown(e: MouseEvent): void {
    this.closeInlineEditor(true);
    this.isDragging = true;
    this.startY = e.clientY;
    this.startValue = this.currentValue;
    e.preventDefault();
  }

  private onMouseMove(e: MouseEvent): void {
    if (!this.isDragging) return;

    const newValue = this.dragValue(this.startY - e.clientY);

    this.currentValue = newValue;
    this.knobElement.dataset.value = newValue.toString();
    this.updateDisplay(newValue);

    this.emitLiveValue(this.currentValue);
  }

  private onMouseUp(): void {
    if (!this.isDragging) return;

    this.isDragging = false;
    this.commitCurrentValue();
  }

  private onWheel(e: WheelEvent): void {
    if (this.isDragging || !isWheelAdjustEnabled()) {
      return;
    }

    e.preventDefault();
    this.knobElement.focus();
    this.applyValue(this.wheelValue(e.deltaY < 0 ? 1 : -1), true);
  }

  private onValueDoubleClick(e: MouseEvent): void {
    e.preventDefault();
    e.stopPropagation();
    this.openInlineEditor();
  }

  private openInlineEditor(): void {
    if (!this.editableValueElement || this.inlineEditor) {
      return;
    }

    const input = document.createElement("input");
    input.type = "number";
    input.className = "knob-inline-editor";
    input.min = this.minValue.toString();
    input.max = this.maxValue.toString();
    // A derived step on a log taper spans a whole low decade (19.99 on 1-2000 Hz), so leave
    // the typed value free rather than flag every exact entry as off-step.
    input.step = this.taper === "log" && this.explicitStep === undefined ? "any" : this.stepValue.toString();
    input.value = this.currentValue.toFixed(countStepDecimals(this.stepValue));

    this.inlineEditor = input;
    this.editableValueElement.classList.add("is-editing");
    this.editableValueElement.textContent = "";
    this.editableValueElement.appendChild(input);

    const savedValue = this.currentValue;
    let finishing = false;

    const finish = (revertToLabel = false) => {
      if (finishing) return;
      finishing = true;
      if (!revertToLabel) {
        // Commit whatever is in the input right now
        const parsedValue = Number.parseFloat(input.value);
        if (Number.isFinite(parsedValue)) {
          this.applyValue(parsedValue, true);
        }
      } else {
        // Revert to the value that was set before the editor opened
        this.applyValue(savedValue, false);
      }
      this.closeInlineEditor(revertToLabel);
    };

    // Only preview while typing — do NOT rewrite input.value so the user can type freely
    input.addEventListener("input", () => {
      const parsedValue = Number.parseFloat(input.value);
      if (!Number.isFinite(parsedValue)) {
        return;
      }
      // Live-preview the value without committing or reformatting the field
      this.emitLiveValue(clampValue(parsedValue, this.minValue, this.maxValue));
    });

    input.addEventListener("blur", () => {
      finish(false);
    });

    input.addEventListener("keydown", (event) => {
      if (event.key === "Enter") {
        event.preventDefault();
        finish(false);
      } else if (event.key === "Escape") {
        event.preventDefault();
        finish(true);
      }
    });

    input.focus();
    input.select();
  }

  private closeInlineEditor(revertToLabel = false): void {
    if (!this.editableValueElement || !this.inlineEditor) {
      return;
    }

    const input = this.inlineEditor;
    this.inlineEditor = null;
    this.editableValueElement.classList.remove("is-editing");
    input.remove();

    if (revertToLabel) {
      this.editableValueElement.textContent = this.editableValueElement.dataset.originalLabel ?? "";
      return;
    }

    this.updateDisplay(this.currentValue);
  }

  private updateDisplay(value: number): void {
    if (this.valueDisplay) {
      const formattedValue = this.displayFormat(value);
      if (!this.inlineEditor) {
        this.valueDisplay.textContent = formattedValue;
        this.valueDisplay.dataset.originalLabel = formattedValue;
      }
    }

    this.knobElement.setAttribute("aria-valuenow", value.toString());
    this.knobElement.setAttribute("aria-valuetext", this.displayFormat(value));

    const span = this.maxValue - this.minValue;
    const pct = this.taper === "linear"
      ? (span > 0 ? (value - this.minValue) / span : 0)
      : valueToTaperPosition(value, this.minValue, this.maxValue, this.taper);
    const rotation = pct * 270 - 135;
    this.knobElement.style.setProperty("--knob-pct", pct.toString());
    const indicator = this.knobElement.querySelector(".knob-indicator") as HTMLElement | null;
    if (indicator) {
      indicator.style.transform = `translateX(-50%) rotate(${rotation}deg)`;
    }
    const customFace = this.knobElement.querySelector(".custom-knob-face") as HTMLElement | null;
    if (customFace) {
      // A filmstrip shows the frame for the position; a face turns over its own range, or the
      // -135..135 every custom face always used (layoutKnobImages.ts).
      const { knobImage, frames, columns, minAngle, maxAngle } = this.knobElement.dataset;
      const frameCount = knobImage === "filmstrip" ? filmstripFrameCount(Number(frames)) : 0;
      if (frameCount) {
        const { size, position } = filmstripBackground(filmstripFrame(pct, frameCount), frameCount, Number(columns) || 1);
        customFace.style.backgroundSize = size;
        customFace.style.backgroundPosition = position;
      } else {
        const angle = minAngle !== undefined && maxAngle !== undefined
          ? knobFaceAngle(pct, Number(minAngle), Number(maxAngle))
          : rotation;
        customFace.style.transform = `rotate(${angle}deg)`;
      }
    }
  }

  public setValue(value: number): void {
    this.currentValue = clampValue(value, this.minValue, this.maxValue);
    this.knobElement.dataset.value = this.currentValue.toString();
    this.updateDisplay(this.currentValue);
  }

  public getValue(): number {
    return this.currentValue;
  }

  /** True while a pointer or touch drag holds the knob. */
  public isBeingDragged(): boolean {
    return this.isDragging;
  }

  /**
   * Moves the knob's ends, for a range that follows another setting. A full
   * sweep keeps the same drag distance, and the value is clamped into the new range.
   */
  public setRange(minValue: number, maxValue: number, stepValue?: number): void {
    const oldSpan = this.maxValue - this.minValue;
    const newSpan = maxValue - minValue;
    if (oldSpan > 0 && newSpan > 0) {
      this.sensitivity *= newSpan / oldSpan;
    }

    this.minValue = minValue;
    this.maxValue = maxValue;
    this.stepValue = deriveRangeStep(minValue, maxValue, stepValue);
    this.explicitStep = Number.isFinite(stepValue) && (stepValue ?? 0) > 0 ? stepValue : undefined;
    this.taper = effectiveTaper(this.declaredTaper, minValue, maxValue);
    this.knobElement.dataset.min = minValue.toString();
    this.knobElement.dataset.max = maxValue.toString();
    if (stepValue !== undefined) {
      this.knobElement.dataset.step = stepValue.toString();
    } else {
      delete this.knobElement.dataset.step;
    }
    this.knobElement.setAttribute("aria-valuemin", minValue.toString());
    this.knobElement.setAttribute("aria-valuemax", maxValue.toString());
    this.setValue(this.currentValue);
  }
}
