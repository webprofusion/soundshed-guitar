# Multi-Preset Mixer

The Multi-Preset Mixer lets you run two or more complete presets at the same time, each processing your guitar independently, and blend their outputs together. This is ideal for combining two very different amp characters — for example, a tight high-gain tone on one side and an airy clean tone on the other.

---

## Opening the mixer

Click the **Mixer** tab in the navigation bar.

---

## Adding presets to the mixer

1. Click **Add Preset** in the mixer panel.
2. The Preset Browser opens. Select the preset you want to add.
3. The preset appears as a new channel strip in the mixer.

Repeat to add more presets — there is no hard limit, though CPU usage increases with each active preset.

---

## Mixer controls

Each channel strip has its own set of controls:

| Control | What it does |
|---------|--------------|
| **Fader** | Sets the output volume for this preset (0–100%) |
| **Pan** | Positions this preset in the stereo field (left to right) |
| **Mute** | Silences this channel while keeping it active |
| **Solo** | Solos this channel — all others are muted |
| **Preset name** | Click to swap the preset loaded into this channel |

---

## Master controls

At the right side (or bottom) of the mixer:

| Control | What it does |
|---------|--------------|
| **Master Gain** | Scales the final combined output level |
| **Output Limiter** | Toggles the mixer's final master-output protection clamp to prevent clipping when mixing multiple loud presets |

---

## Fading between presets

You can automate smooth fades between preset levels during a performance by assigning MIDI controls to the faders (see Settings for MIDI mapping). The faders respond smoothly to MIDI values so there are no clicks or pops when riding the level.

---

## Tips

- Use **Mute** to quickly audition the contribution of a single amp — mute everything else rather than soloing, so you can flip back to the full blend with one click.
- Pan two similar-character amps slightly left and right to widen the stereo image without changing the tone character.
- Keep the master output limiter on when blending multiple presets so the combined level never clips the output. The ceiling it uses is set in **Settings → Advanced DSP Level Targets**.
