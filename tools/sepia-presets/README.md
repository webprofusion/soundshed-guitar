# Sepia factory presets

The factory pack is generated, not hand-edited. Everything it is made from is in this folder.

| File | What it is |
|---|---|
| `presets/*.json` | One file per group. Each preset is a serial chain of effects. |
| `levels.json` | Each preset's end-of-chain level trim in dB. Written by `level.py`. |
| `build_pack.py` | Turns the two above into `core/ui/presets/factory/Sepia-Factory.soundshed.presets`. |
| `make_di.py` | Synthesises the three guitar DI clips the pack is measured with. |
| `level.py` | Updates `levels.json` from a measurement so all presets are equally loud. |
| `measurements.md` | The last measurement of the built pack. |

## Rules for a preset

- Built-in effects only. A preset must not need a `.nam` model, an IR, or a third-party plugin.
  `build_pack.py` refuses a chain that does.
- Original names that describe the tone. No artist, song, or product names.
- A noise gate first in every high-gain chain.
- Within a group, no two presets with the same amp voice, the same gain (within 0.1) and the
  same cabinet.

## Rebuilding

From the repository root, with `core` built with tests (`-DGUITARFX_CORE_BUILD_TESTS=ON`):

```bash
core/build/tests/SepiaEffectCatalog /tmp/sepia-catalog.json
```

```bash
python3 tools/sepia-presets/build_pack.py --catalog /tmp/sepia-catalog.json --out core/ui/presets/factory/Sepia-Factory.soundshed.presets
```

```bash
core/build/tests/SepiaFactoryPackTests --measure /tmp/sepia-measure.json
```

```bash
python3 tools/sepia-presets/level.py /tmp/sepia-measure.json
```

Then build the pack and run `SepiaFactoryPackTests` once more; it must pass with no arguments.

The DI clips in `core/tests/testdata/di/` only need regenerating if `make_di.py` changes:

```bash
python3 tools/sepia-presets/make_di.py core/tests/testdata/di
```

## What the measurement cannot tell you

Levels, clipping, gain structure and clicks on preset changes are measured. Whether a tone
sounds good is not: that takes a guitar and ears.
