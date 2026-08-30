/**
 * Shared range-selection model for the waveform editors.
 *
 * The Practice Tool (loop regions over a backing track) and the Riff Capture
 * editor (crop markers over a captured take) both let the user drag a pair of
 * handles across a canvas. They had grown separate copies of this arithmetic,
 * and the copies disagreed: dragging one handle past the other collapsed the
 * Practice Tool's region into a sliver that slid along with the pointer, and
 * silently destroyed the Riff editor's trailing marker. One implementation,
 * one behaviour.
 *
 * Deliberately headless — no DOM beyond the pointer helper, no bridge, no
 * state import — so it stays unit-testable and cycle-free.
 */

export type RatioRange = { startRatio: number; endRatio: number };

/** Which of the two bounds a gesture is currently moving. */
export type RangeHandle = "start" | "end";

/**
 * A clamped range, plus whether the bounds had to be reordered to get there.
 *
 * `swapped` is the important half. A caller that tracks which handle it is
 * moving MUST flip that tracking when this is set: after a crossing, the
 * handle under the pointer is the *other* bound. Keep writing the original one
 * and every subsequent event re-crosses, so the region never anchors — it
 * shrinks to the width of one pointer step and slides along instead of growing
 * away from the stationary handle.
 */
export type ClampedRange = RatioRange & { swapped: boolean };

export function otherHandle(handle: RangeHandle): RangeHandle {
  return handle === "start" ? "end" : "start";
}

/**
 * Clamps a pair of 0..1 ratios into an ordered range no narrower than
 * `minSpanRatio`, reporting whether the two arrived inverted.
 *
 * `minSpanRatio` is supplied by the caller rather than fixed here because the
 * two editors mean different things by "too short": the Practice Tool wants a
 * floor in *seconds* (converted against track duration), the Riff editor a
 * flat ratio of the take. Unifying that is a separate decision from fixing the
 * crossing behaviour, so it stays with the callers for now.
 */
export function clampRatioRange(
  startRatio: number,
  endRatio: number,
  minSpanRatio: number,
  moving?: RangeHandle,
): ClampedRange {
  let start = Math.max(0, Math.min(1, startRatio));
  let end = Math.max(0, Math.min(1, endRatio));
  let swapped = false;

  if (start > end) {
    [start, end] = [end, start];
    swapped = true;
  }

  if (end - start < minSpanRatio) {
    // Squeezing the range below its floor has to push *something*. Push the
    // bound the user is holding, never the one they left alone — otherwise the
    // stationary handle gets shoved out of the way, which reads as the
    // selection sliding when the user believes they are only resizing it. A
    // swap flips which bound the pointer holds, so account for that first.
    const held = moving && swapped ? otherHandle(moving) : moving;
    if (held === "start") {
      start = end - minSpanRatio;
      if (start < 0) {
        start = 0;
        end = Math.min(1, minSpanRatio);
      }
    } else if (start + minSpanRatio <= 1) {
      end = start + minSpanRatio;
    } else {
      start = Math.max(0, 1 - minSpanRatio);
      end = 1;
    }
  }

  return { startRatio: start, endRatio: end, swapped };
}

/** Horizontal pointer position over `canvas`, as a 0..1 ratio of its width. */
export function ratioFromPointer(event: MouseEvent, canvas: HTMLCanvasElement): number {
  const rect = canvas.getBoundingClientRect();
  if (rect.width <= 0) {
    return 0;
  }
  return Math.max(0, Math.min(1, (event.clientX - rect.left) / rect.width));
}
