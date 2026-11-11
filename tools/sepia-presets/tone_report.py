#!/usr/bin/env python3
"""Print where each preset's energy sits and how fast it renders, and flag the odd ones out.

Reads the JSON that `SepiaFactoryPackTests --measure` writes. For each preset it shows the share
of energy in four bands on the palm-mute clip (low under 250 Hz, low-mid to 1 kHz, high-mid to
4 kHz, high above), then lists presets whose balance is far from the rest of their group: more
than 8 dB off the group's median in any band. That is not a verdict on a tone, only a list of
the ones worth a listen first: a lo-fi telephone preset is supposed to be an outlier.

Usage: python3 tone_report.py measurements.json [--markdown]
"""

import json
import statistics
import sys

BANDS = ["low", "low-mid", "high-mid", "high"]
CLIP = "palm-mute"
OUTLIER_DB = 8.0


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)

    measured = json.load(open(sys.argv[1], encoding="utf-8"))
    groups = {}

    for preset in measured["presets"]:
        clip = next(c for c in preset["clips"] if c["clip"] == CLIP)

        if "bandsDb" not in clip:
            sys.exit("error: this measurement has no band levels; rebuild SepiaFactoryPackTests")

        groups.setdefault(preset["group"], []).append((preset["name"], clip["bandsDb"], preset.get("realtimeFactor")))

    flagged = []

    for group, presets in groups.items():
        medians = [statistics.median(p[1][band] for p in presets) for band in range(4)]
        print(f"\n{group}  (median: " + ", ".join(f"{BANDS[b]} {medians[b]:+.1f}" for b in range(4)) + " dB)")
        print(f"  {'preset':<28}" + "".join(f"{name:>10}" for name in BANDS) + f"{'speed':>8}")

        for name, bands, speed in presets:
            marks = ""

            for band in range(4):
                if abs(bands[band] - medians[band]) > OUTLIER_DB:
                    flagged.append((group, name, BANDS[band], bands[band] - medians[band]))
                    marks = "  <-"

            speed_text = f"{speed:7.0f}x" if speed else "       -"
            print(f"  {name:<28}" + "".join(f"{value:>10.1f}" for value in bands) + speed_text + marks)

    print(f"\n{len(flagged)} band(s) more than {OUTLIER_DB:.0f} dB from their group's median:")

    for group, name, band, delta in flagged:
        print(f"  {name} ({group}): {band} {delta:+.1f} dB")


if __name__ == "__main__":
    main()
