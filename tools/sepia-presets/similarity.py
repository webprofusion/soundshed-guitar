#!/usr/bin/env python3
"""List the factory presets that measure most alike, so they can be told apart or dropped.

Reads the JSON that `SepiaFactoryPackTests --measure` writes. Each preset is reduced to a
fingerprint: its spectrum in eight octave bands on each of the three DI clips, how saturated
it is, how wide, how much its level moves, and how long it rings on. The distance between two
presets is a weighted difference of those, in rough "decibels of difference": under about 2
two presets are hard to tell apart, over 5 they are plainly different.

A fingerprint is not an ear. It cannot hear that one chorus is prettier than another. It can
say that two presets put the same energy in the same places at the same gain, which is what
"they all sound the same" means.

Usage: python3 similarity.py measurements.json [--closest N] [--threshold D]
"""

import json
import math
import sys

CLOSE = 2.5


def fingerprint(preset):
    values = []

    for clip in preset["clips"]:
        # Spectrum: the shape matters, not bands that are silent anyway.
        values += [max(v, -40.0) * 0.45 for v in clip["octavesDb"]]
        values.append(max(clip["widthDb"], -40.0) * 0.12)
        values.append(min(clip["movementDb"], 12.0) * 0.5)
        values.append(max(clip["tailDb"], -50.0) * 0.12)

    # How saturated it is counts for a lot: it is the first thing heard.
    values.append(preset["followedDb"] * 0.9)
    return values


def distance(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)) / 3.0)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)

    args = sys.argv[2:]
    closest = int(args[args.index("--closest") + 1]) if "--closest" in args else 25
    threshold = float(args[args.index("--threshold") + 1]) if "--threshold" in args else CLOSE
    presets = json.load(open(sys.argv[1], encoding="utf-8"))["presets"]

    if "octavesDb" not in presets[0]["clips"][0]:
        sys.exit("error: this measurement has no fingerprints; rebuild SepiaFactoryPackTests")

    prints = [(p["name"], p["group"], fingerprint(p)) for p in presets]
    pairs = []
    nearest = {}

    for i, (name_a, group_a, print_a) in enumerate(prints):
        for name_b, group_b, print_b in prints[i + 1:]:
            d = distance(print_a, print_b)
            pairs.append((d, name_a, name_b, group_a == group_b))

            for one, other in ((name_a, name_b), (name_b, name_a)):
                if one not in nearest or d < nearest[one][0]:
                    nearest[one] = (d, other)

    pairs.sort()
    too_close = [pair for pair in pairs if pair[0] < threshold]
    distances = sorted(d for d, _ in nearest.values())
    print(f"{len(presets)} presets. Distance to each preset's nearest neighbour: "
          f"smallest {distances[0]:.1f}, median {distances[len(distances) // 2]:.1f}, largest {distances[-1]:.1f}")
    print(f"{len(too_close)} pair(s) closer than {threshold}:")

    for d, a, b, same_group in pairs[:max(closest, 0)]:
        mark = "" if d >= threshold else "  <- too alike"
        print(f"  {d:5.1f}  {a}  ~  {b}{'' if same_group else '  (different groups)'}{mark}")


if __name__ == "__main__":
    main()
