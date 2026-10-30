/**
 * Which features a layout uses that an older app cannot draw, and the schema version that
 * follows from them.
 *
 * A layout's `version` is the oldest schema that can render it. One that uses nothing newer
 * than the original layout format stays version 1 with no `requires` list, so it is saved
 * exactly as before; one that uses a newer feature is version 2 and lists each such feature,
 * so a later app can say what it cannot show rather than guessing. Older apps ignore both
 * fields and draw what they understand (docs/plans/layout-designer-hires.md, rule 7).
 */

import { usableKnobImage, usableToggleImage } from "./layoutControlImages.js";
import { usableControlsArea } from "./layoutControlsArea.js";
import { usableStrip } from "./layoutKnobImages.js";
import { usableControlShadow } from "./layoutShadows.js";
import { LEGACY_DIMENSION_LIMITS, type EffectLayout } from "./layoutTypes.js";

/** The features, in the order they are listed. */
export const LAYOUT_FEATURES = [
  "large-canvas",
  "scaling",
  "text-style",
  "control-size",
  "knob-image",
  "image-toggle",
  "image-slider",
  "image-overlay",
  "control-shadow",
  "default-controls-style",
  "switch-image",
] as const;
export type LayoutFeature = (typeof LAYOUT_FEATURES)[number];

function isSet(value: unknown): boolean {
  return value !== undefined && value !== null && value !== "";
}

/** The newer features `layout` uses, in LAYOUT_FEATURES order. */
export function layoutRequirements(layout: EffectLayout): LayoutFeature[] {
  const used = new Set<LayoutFeature>();

  if (layout.dimensions.width > LEGACY_DIMENSION_LIMITS.maxWidth || layout.dimensions.height > LEGACY_DIMENSION_LIMITS.maxHeight) {
    used.add("large-canvas");
  }

  const scaling = layout.scaling;
  if (scaling && (
    scaling.mode === "fit" || scaling.mode === "fixed"
    || (typeof scaling.maxScale === "number" && scaling.maxScale > 1)
    || (typeof scaling.minScale === "number" && scaling.minScale > 0)
    || scaling.align === "start"
  )) {
    used.add("scaling");
  }

  if (isSet(layout.controlLabelFontSize) || isSet(layout.controlValueFontSize)
    || isSet(layout.controlFontFamily) || isSet(layout.controlLabelMaxWidth)
    || layout.controls.some((control) => isSet(control.style?.labelFontSize) || isSet(control.style?.valueFontSize))) {
    used.add("text-style");
  }

  if (usableKnobImage(layout.knobImage)) used.add("knob-image");
  for (const control of layout.controls) {
    const style = control.style;
    if (isSet(style?.knobDiameter) || isSet(style?.sliderLength) || isSet(style?.toggleWidth) || isSet(style?.toggleHeight)) {
      used.add("control-size");
    }
    if (usableKnobImage(style?.knobImage)) used.add("knob-image");
    if ((style?.toggleImage?.onImageId && style.toggleImage.offImageId) || usableStrip(style?.toggleImage?.strip)) used.add("image-toggle");
    if (style?.sliderImage?.trackImageId || style?.sliderImage?.thumbImageId || usableStrip(style?.sliderImage?.strip)) used.add("image-slider");
  }
  if ((layout.overlays ?? []).some((overlay) => overlay.style?.imageId)) {
    used.add("image-overlay");
  }
  if (usableControlShadow(layout.controlShadow)) used.add("control-shadow");
  if (usableToggleImage(layout.switchImage)) used.add("switch-image");
  if (layout.useDefaultControls && (layout.defaultControlsMainOnly === true || layout.defaultControlsPanels === false
    || usableControlsArea(layout.defaultControlsArea))) {
    used.add("default-controls-style");
  }

  return LAYOUT_FEATURES.filter((feature) => used.has(feature));
}

/**
 * What an older app draws for an image knob, written beside it: a turning face is also a
 * custom-image knob (knobStyle "custom" + knobImageId), which older apps turn the same way; a
 * filmstrip, which they would squash into a circle, falls back to a standard knob.
 */
function writeKnobImageFallbacks(layout: EffectLayout): void {
  for (const control of layout.controls) {
    const knobImage = usableKnobImage(control.style?.knobImage);
    if (!knobImage || !control.style) continue;
    if (knobImage.mode === "rotate") {
      control.style.knobStyle = "custom";
      control.style.knobImageId = knobImage.imageId;
    } else {
      if (control.style.knobStyle === "custom") control.style.knobStyle = "default";
      delete control.style.knobImageId;
    }
  }
}

/** Sets `version` and `requires` from what `layout` uses, and the older apps' fallbacks, as saving does. */
export function stampLayoutRequirements(layout: EffectLayout): EffectLayout {
  writeKnobImageFallbacks(layout);
  const requires = layoutRequirements(layout);
  if (requires.length) {
    layout.version = 2;
    layout.requires = requires;
  } else {
    layout.version = 1;
    delete layout.requires;
  }
  return layout;
}
