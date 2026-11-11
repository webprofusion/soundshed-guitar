#!/usr/bin/env python3
"""Pull presets that measure alike apart, by measurement.

One amp model and a handful of cabinets make it easy to write sixty presets that land on much
the same sound. This finds the presets whose fingerprint (see similarity.py) sits too close to
another's and changes how they are voiced until it does not: a different cabinet, microphone
or microphone position, a different setting of the amp's tone controls, or an equaliser after
the cabinet. It never touches what a preset is: the amp's voice and gain, its pedals, its
effects and their order all stay, so a preset keeps its name's promise and its gain class.

Each round every too-close preset gets one proposal (two or three of those changes at once).
The pack is rebuilt and measured, and a proposal is kept only if it left the preset further
from its nearest neighbour than it was; otherwise it is undone. The choices come from a fixed
seed, so a run can be repeated.

Run level.py and SepiaFactoryPackTests afterwards: voicing changes move levels.

Usage, from core/ with the tests built:
    python3 ../tools/sepia-presets/diversify.py --catalog catalog.json [--rounds 8] [--threshold 2.5]
"""

import argparse
import copy
import json
import random
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).parent
sys.path.insert(0, str(HERE))
import similarity  # noqa: E402

GUITAR_CABS = [0, 1, 2, 3, 4]
EQ_PRESETS = ["mud-cut", "fizz-tamer", "mid-hump", "scoop", "presence-lift", "tight-low-end", "glassy-clean"]


def write_group(path, data):
    lines = ["{", ' "group": %s,' % json.dumps(data["group"]), ' "presets": [']
    presets = data["presets"]

    for i, preset in enumerate(presets):
        lines += ["  {", '   "name": %s, "tags": %s,' % (json.dumps(preset["name"]), json.dumps(preset["tags"])),
                  '   "chain": [']

        for j, node in enumerate(preset["chain"]):
            lines.append("    " + json.dumps(node) + ("," if j < len(preset["chain"]) - 1 else ""))

        lines += ["   ]", "  }" + ("," if i < len(presets) - 1 else "")]

    lines += [" ]", "}"]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def nudge(rng, params, key, low, high, default, reach):
    """Moves a 0..1 control by up to `reach` either way, at least a third of it, inside low..high."""
    current = params.get(key, default)
    step = rng.uniform(reach / 3.0, reach) * rng.choice((-1.0, 1.0))
    moved = min(high, max(low, current + step))

    # Against a wall, go the other way rather than nowhere.
    if abs(moved - current) < reach / 4.0:
        moved = min(high, max(low, current - step))

    params[key] = round(moved, 2)


def propose(rng, preset):
    """One change of voicing, in place. Returns a short description, or None if nothing applies."""
    chain = preset["chain"]
    cab = next((n for n in chain if n["type"] == "cab_simple"), None)
    amp = next((n for n in chain if n["type"] == "amp_builtin"), None)
    acoustic = next((n for n in chain if n["type"] == "acoustic_sim"), None)
    moves = []

    if cab is not None:
        params = cab.setdefault("params", {})

        # The lo-fi speakers are what a lo-fi preset is; only guitar cabinets are swapped.
        if params.get("cabinet", 0) in GUITAR_CABS:
            def swap_cab():
                params["cabinet"] = rng.choice([c for c in GUITAR_CABS if c != params.get("cabinet", 0)])
                return "cabinet"
            moves.append(swap_cab)

        def mic():
            params["micType"] = rng.choice([t for t in (0, 1, 2) if t != params.get("micType", 0)])
            nudge(rng, params, "micPosition", 0.1, 0.9, 0.5, 0.35)
            return "microphone"

        def box():
            nudge(rng, params, "size", 0.2, 0.85, 0.5, 0.3)
            nudge(rng, params, "mids", 0.2, 0.8, 0.5, 0.3)
            return "cabinet size and mids"

        def top():
            nudge(rng, params, "brightness", 0.2, 0.85, 0.5, 0.3)
            nudge(rng, params, "presence", 0.2, 0.85, 0.5, 0.3)
            return "cabinet top end"

        def room():
            params["micDistance"] = round(rng.uniform(0.25, 0.6), 2) if params.get("micDistance", 0.0) < 0.2 else 0.0
            return "microphone distance"

        moves += [mic, box, top, room]

    if amp is not None:
        params = amp.setdefault("params", {})

        def stack():
            nudge(rng, params, "bass", 0.2, 0.8, 0.5, 0.25)
            nudge(rng, params, "middle", 0.15, 0.85, 0.5, 0.3)
            nudge(rng, params, "treble", 0.2, 0.8, 0.5, 0.25)
            return "tone stack"

        def contour():
            nudge(rng, params, "contour", 0.0, 0.85, 0.2, 0.4)
            nudge(rng, params, "presence", 0.2, 0.8, 0.5, 0.25)
            return "contour and presence"

        moves += [stack, contour]

    if acoustic is not None:
        params = acoustic.setdefault("params", {})

        def body():
            params["size"] = rng.choice([s for s in (0, 1, 2) if s != params.get("size", 1)])
            nudge(rng, params, "body", 0.2, 0.9, 0.6, 0.3)
            nudge(rng, params, "top", 0.2, 0.9, 0.6, 0.3)
            return "acoustic body"

        moves.append(body)

    # An equaliser after the cabinet: the plainest way to give a preset a voice of its own.
    after = next((i for i, n in enumerate(chain) if n["type"] in ("cab_simple", "acoustic_sim")), None)

    if after is not None:
        existing = next((n for n in chain if n["type"] == "eq_parametric"), None)

        def equalise():
            if existing is not None:
                existing["preset"] = rng.choice([e for e in EQ_PRESETS if e != existing.get("preset")])
                existing.pop("params", None)
            else:
                chain.insert(after + 1, {"type": "eq_parametric", "preset": rng.choice(EQ_PRESETS)})
            return "equaliser"

        moves.append(equalise)

    if not moves:
        return None

    chosen = rng.sample(moves, k=min(len(moves), rng.choice((2, 2, 3))))
    return ", ".join(move() for move in chosen)


def measure(catalog, build_dir, work):
    pack = HERE / ".." / ".." / "core" / "ui" / "presets" / "factory" / "Sepia-Factory.soundshed.presets"
    subprocess.run([sys.executable, "-I", str(HERE / "build_pack.py"), "--catalog", catalog, "--out", str(pack)],
                   check=True, stdout=subprocess.DEVNULL)
    out = Path(work) / "measure.json"
    subprocess.run([str(Path(build_dir) / "tests" / "SepiaFactoryPackTests"), "--measure", str(out)],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    measured = json.loads(out.read_text(encoding="utf-8"))["presets"]
    return {p["name"]: similarity.fingerprint(p) for p in measured}


def nearest(prints):
    names = list(prints)
    best = {name: (1.0e9, None) for name in names}

    for i, a in enumerate(names):
        for b in names[i + 1:]:
            d = similarity.distance(prints[a], prints[b])

            if d < best[a][0]:
                best[a] = (d, b)

            if d < best[b][0]:
                best[b] = (d, a)

    return best


def summary(best, threshold):
    distances = sorted(d for d, _ in best.values())
    close = sum(1 for d in distances if d < threshold)
    return f"nearest-neighbour distance: smallest {distances[0]:.2f}, median {distances[len(distances) // 2]:.2f}; " \
           f"{close} of {len(distances)} presets under {threshold}"


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--catalog", required=True)
    parser.add_argument("--build", default="build", help="the core build directory")
    parser.add_argument("--rounds", type=int, default=8)
    parser.add_argument("--threshold", type=float, default=2.5)
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()

    rng = random.Random(args.seed)
    files = sorted((HERE / "presets").glob("*.json"))
    groups = {path: json.loads(path.read_text(encoding="utf-8")) for path in files}
    by_name = {preset["name"]: preset for data in groups.values() for preset in data["presets"]}

    def save():
        for path, data in groups.items():
            write_group(path, data)

    with tempfile.TemporaryDirectory() as work:
        prints = measure(args.catalog, args.build, work)
        best = nearest(prints)
        print("start:  " + summary(best, args.threshold), flush=True)

        for round_number in range(1, args.rounds + 1):
            # Of two presets that are too alike only one moves, or they would chase each other.
            movers = []
            claimed = set()

            for name, (d, other) in sorted(best.items(), key=lambda item: item[1][0]):
                if d < args.threshold and name not in claimed and other not in movers:
                    movers.append(name)
                    claimed.update((name, other))

            if not movers:
                break

            before = {name: copy.deepcopy(by_name[name]["chain"]) for name in movers}
            changed = {name: propose(rng, by_name[name]) for name in movers}
            save()
            trial = measure(args.catalog, args.build, work)
            trial_best = nearest(trial)
            kept = 0

            for name in movers:
                if changed[name] is not None and trial_best[name][0] > best[name][0] + 0.15:
                    kept += 1
                else:
                    by_name[name]["chain"] = before[name]

            save()
            # A preset's fingerprint is its own: the kept ones are as just measured, and the
            # ones put back are as they were, so nothing needs measuring again.
            for name in movers:
                if by_name[name]["chain"] is not before[name]:
                    prints[name] = trial[name]

            best = nearest(prints)
            print(f"round {round_number}: {len(movers)} proposals, {kept} kept;  " + summary(best, args.threshold),
                  flush=True)

    print("done. Now run level.py and SepiaFactoryPackTests.")


if __name__ == "__main__":
    main()
