# Brickwall Limiter

> A hard ceiling that prevents the output level from ever exceeding a set maximum — stops clipping dead.

## What it does

A brickwall limiter is a compressor with an instantaneous attack and an extremely high ratio — it simply prevents the signal from going above the Ceiling level, period. Unlike a compressor, you will not hear it working on normal playing; it only acts the instant a peak tries to exceed the ceiling. It is most useful at the end of the signal chain or on the master output to protect against unexpected loud spikes when combining multiple presets.

## Parameters

| Parameter | Range | Default | What it does |
|-----------|-------|---------|--------------|
| **Ceiling** | −24 to 0 dB | −0.1 dB | The maximum level the output can reach. −0.1 dB is the standard setting — just below digital zero to prevent clipping |
| **Release** | 1–500 ms | 50 ms | How quickly the limiter lets go after a peak subsides. Shorter release = more transparent on brief spikes; longer release = smoother behaviour on sustained loud passages |

## Tips

- **Leave the Ceiling at −0.1 dB** in almost all cases. This is the standard "true peak" protection setting used in professional audio.
- This effect node is separate from the **Multi-Preset Mixer** master output protection. Use this node when you want a limiter inside the signal chain itself.
- For overall mixer protection when blending multiple presets, use the mixer's **Output Limiter**. Its ceiling follows **Settings → Advanced DSP Level Targets** and defaults to `-1 dBFS`.
- A 50 ms release works well for most uses. If you hear the signal "pumping" (volume ducking briefly after loud peaks), try lengthening the release to 100–200 ms.
- If you are recording and the limiter is working frequently (the level is hitting the ceiling often), it is better to reduce the Output Trim or makeup gain earlier in the chain rather than relying on the limiter to tame a consistently too-loud signal.
