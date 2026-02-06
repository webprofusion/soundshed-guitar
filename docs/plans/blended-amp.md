# Blended Amp Feature (High-Level)

The blended amp effect lets multiple NAM models behave as a single amp node in the signal chain. A blend definition groups models and defines a set of mapped parameters (e.g., Gain, Tone, Presence) that describe where each model should appear in the parameter space. The player adjusts one set of knobs, and the blend resolves to the closest available model (snap mode) or interpolates between nearby models (interpolate mode).

## Why It Exists
- Combine multiple amp models into a single, consistent control surface.
- Preserve user workflow: tweak familiar amp knobs without swapping models manually.
- Enable curated “model collections” that behave like a single virtual amp.

## Blend Definition (Conceptual)
A blend definition contains:
- **Name + category** shown in the UI.
- **Model list** (resource IDs).
- **Active parameters** (e.g., Gain, Treble).
- **Model mappings** (per-model parameter values).
- **Blend mode** (`snap` or `interpolate`).

Mappings are stored in normalized values ($0..1$ for most parameters). Each mapping represents a point in the parameter space that corresponds to a specific model.

## Blend Editor (High-Level)
The blend editor is used to create and refine definitions:
- **Mapped parameters**: Select which knobs participate in the blend.
- **Model mapping table**: Assign values for each parameter per model.
- **Test tab**: Try the blend using knob controls and preview the resolved model.
- **Blend mode**: Choose between `snap` and `interpolate`.

## Snap Mode Behavior
- All parameter knobs snap to the nearest mapped points.
- The knob being dragged takes precedence, and other parameters update to the matching mapping.
- UI highlights which mapped points are selectable without changing other parameters.

## Interpolate Mode Behavior
- The system resolves the nearest two mappings and blends between them.
- Knobs do not snap, but the preview still shows the nearest models and weights.

## UI Surfaces
Blended amp controls appear in two places:
- **Normal effect parameter panel** when a blended amp node is selected.
- **Blend editor test tab** for authoring and verification.

Both surfaces share the same mapping logic and visual indicators for available points.
