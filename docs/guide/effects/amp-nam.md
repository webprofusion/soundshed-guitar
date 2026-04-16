# Neural Amp Model (NAM)

> Loads a neural capture of a real guitar amplifier and runs your signal through it.

## What it does

A Neural Amp Model is a digital "photograph" of a real amplifier. It captures not just the amp's frequency response, but the way it responds dynamically to picking attack, chord voicings, and volume swells — so it feels like the real thing as you play. Any NAM-format model file can be loaded, including captures from the Tone3000 community library or your own captures.

## Parameters

| Parameter | Range | Default | What it does |
|-----------|-------|---------|--------------|
| **Input Gain** | −24 to +24 dB | 0 dB | Scales the signal level going into the model. Raise it to push the amp into more saturation; reduce it to clean up the tone |
| **Output Gain** | −24 to +24 dB | 0 dB | Scales the output level coming out of the model. Use this to match loudness between different amp models |

## Selecting a model

Click the **folder icon** on the NAM node to open the resource browser. You can choose from:

- **Library models** — downloaded via Tone3000 or added to your resource folders
- **Local files** — browse your computer for a `.nam` model file

## Tips

- **Input Gain is like a boost pedal into the amp.** +3 to +6 dB can push a model that feels slightly clean into a more saturated character, just as a clean boost into a real amp does.
- **Match output levels** when A/B comparing two models — a louder model will always sound more impressive, so level-match to make a fair comparison.
- **User Input Calibration** (in Settings) is the main way to match your guitar and interface to the app. It applies one fixed gain before the chain, not a hidden per-model input correction.
- **NAM output normalization** may still trim the model's output automatically. Soundshed first uses product-owned `normalizationGainDb` metadata when present, then falls back to model loudness against the shared nominal target.
- Always follow the NAM node with a [Cabinet IR](cab-ir.md) or [Simple Cabinet](cab-simple.md) — the model is the amp head only, without a speaker cabinet.
