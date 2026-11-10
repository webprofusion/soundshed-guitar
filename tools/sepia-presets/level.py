#!/usr/bin/env python3
"""Update levels.json from a measurement of the built pack, so every preset is equally loud.

`SepiaFactoryPackTests --measure out.json` renders each preset over the three DI clips and
writes its loudness on each. This moves each preset's end-of-chain trim by the difference
between the pack's target and the mean of its three loudnesses. Rebuild the pack and measure
again afterwards: a trim after a compressor or a reverb lands where it was aimed, so one pass
is normally enough and a second confirms it.

Usage: python3 level.py measurements.json
"""

import json
import sys
from pathlib import Path


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)

    measured = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
    target = measured["targetLufs"]
    levels_path = Path(__file__).parent / "levels.json"
    levels = json.loads(levels_path.read_text(encoding="utf-8")) if levels_path.exists() else {}
    worst = 0.0

    for preset in measured["presets"]:
        loudness = [clip["lufs"] for clip in preset["clips"] if clip["lufs"] is not None]

        if not loudness:
            sys.exit(f"error: '{preset['name']}' rendered silence on every clip")

        error = target - sum(loudness) / len(loudness)
        worst = max(worst, abs(error))
        levels[preset["name"]] = round(levels.get(preset["name"], 0.0) + error, 2)

    names = {preset["name"] for preset in measured["presets"]}
    levels = {name: value for name, value in sorted(levels.items()) if name in names}
    levels_path.write_text(json.dumps(levels, indent=1) + "\n", encoding="utf-8")
    print(f"updated {levels_path.name}: {len(levels)} presets, largest correction {worst:.2f} dB")


if __name__ == "__main__":
    main()
