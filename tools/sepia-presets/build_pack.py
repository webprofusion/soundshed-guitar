#!/usr/bin/env python3
"""Build the Sepia factory preset archive from the compact definitions in presets/.

Each file in presets/ is one group (a folder in the preset browser) and lists its presets as
serial chains:

    {
      "group": "Lo-Fi",
      "presets": [
        {
          "name": "Sunday Tape",
          "tags": ["clean", "tape"],
          "chain": [
            {"type": "amp_builtin", "preset": "clean-channel", "params": {"gain": 0.4}},
            {"type": "cab_simple", "params": {"cabinet": 8}},
            {"type": "tape_saturation", "preset": "cassette"}
          ]
        }
      ]
    }

Each chain gets a plain gain node at its end, set from levels.json, which is how the pack is
brought to one loudness (see level.py).

`type` is an effect's alias. A node starts from the effect's declared defaults, then its
factory `preset` if one is named, then `params`. Every parameter is written out, so a preset
never depends on what an effect's defaults happen to be in a later build.

The effect catalog (aliases, ids, parameters, factory presets) comes from the app itself:
`SepiaEffectCatalog` in core/tests dumps it as JSON. Nothing here is typed in twice.

Usage:
    python3 build_pack.py --catalog catalog.json --out ../../core/ui/presets/factory/Sepia-Factory.soundshed.presets

The archive is written deterministically (fixed timestamps, sorted keys), so rebuilding
without changing a definition gives the same bytes.
"""

import argparse
import json
import re
import sys
import zipfile
from pathlib import Path

PACK_NAME = "Sepia Factory"
STAMP = "2026-11-10T00:00:00.000Z"
ZIP_DATE = (2026, 11, 10, 0, 0, 0)
GROUP_ORDER = ["Clean & Indie", "Lo-Fi", "Shoegaze & Ambient", "Blues & Classic Rock", "Grunge & Punk", "Metal"]


# The preset browser filters by these (core/ui/ts/presetTags.ts). Every preset gets the ones
# that are true of it, so the filter bar finds factory presets as it finds a user's own.
LEAD_WORDS = ("lead", "solo", "shred", "legato", "slide")
RHYTHM_WORDS = ("rhythm", "chug", "riff", "gallop", "djent", "bounce", "bark", "crunch")


def browser_tags(name, group, own_tags, followed_db):
    """The standard tags for a preset: how much gain it measured, its role, and its mood."""
    tags = set()
    words = name.lower().split() + [tag.lower() for tag in own_tags]

    # followed_db is how much of a 12 dB drop in playing level reaches the output: about 12 for
    # a clean amp, about 0 for one that is saturated. None until the pack has been measured.
    # A preset that already says which it is keeps its word: a compressor in front of a clean
    # amp flattens the playing as a crunch does, and the measurement cannot tell them apart.
    if not {"clean", "crunch", "high-gain"} & {tag.lower() for tag in own_tags} and followed_db is not None:
        tags.add("clean" if followed_db >= 8.0 else "crunch" if followed_db >= 3.0 else "high-gain")

    if any(word in words for word in LEAD_WORDS):
        tags.add("lead")

    if any(word in words for word in RHYTHM_WORDS):
        tags.add("rhythm")

    if group == "Shoegaze & Ambient":
        tags.update(("ambient", "atmospheric"))
    elif group == "Lo-Fi":
        tags.add("atmospheric")

    return tags


class PackError(Exception):
    pass


def slug(text):
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")


def load_catalog(path):
    catalog = json.loads(Path(path).read_text(encoding="utf-8"))
    by_alias = {}

    for effect in catalog["effects"]:
        for alias in effect["aliases"]:
            by_alias[alias] = effect

    return by_alias


def node_params(effect, node, where):
    params = {p["id"]: p["default"] for p in effect["parameters"]}
    known = {p["id"]: p for p in effect["parameters"]}

    if "preset" in node:
        factory = {p["id"]: p for p in effect["presets"]}

        if node["preset"] not in factory:
            raise PackError(f"{where}: {effect['aliases'][0]} has no factory preset '{node['preset']}'")

        params.update(factory[node["preset"]]["parameters"])

    for key, value in node.get("params", {}).items():
        if key not in known:
            raise PackError(f"{where}: {effect['aliases'][0]} has no parameter '{key}'")

        low, high = known[key]["min"], known[key]["max"]

        if not low <= value <= high:
            raise PackError(f"{where}: {effect['aliases'][0]}.{key} = {value} is outside {low}..{high}")

        params[key] = value

    return params


def build_graph(preset, by_alias, where, level_db):
    nodes = [
        {"category": "utility", "id": "__input__", "label": "Input", "params": {"gainDb": 0}, "type": "input"},
        {"category": "utility", "id": "__output__", "label": "Output", "params": {"gainDb": 0}, "type": "output"},
    ]
    edges = []
    previous = "__input__"

    for index, node in enumerate(preset["chain"]):
        alias = node["type"]

        if alias not in by_alias:
            raise PackError(f"{where}: unknown effect type '{alias}'")

        effect = by_alias[alias]

        if effect.get("requiresResource"):
            raise PackError(f"{where}: {alias} needs a model or IR file; factory presets must not")

        node_id = f"{alias}_{index + 1}"
        nodes.append(
            {
                "category": effect["category"],
                "id": node_id,
                "label": node.get("label", effect["displayName"]),
                "params": node_params(effect, node, f"{where} node {index + 1}"),
                "type": effect["type"],
            }
        )
        edges.append({"from": previous, "to": node_id})
        previous = node_id

    # The pack is level-matched by a plain gain at the very end, where it changes nothing but
    # the level: a trim any earlier would push the stages after it harder or softer.
    nodes.append(
        {"category": "utility", "id": "level", "label": "Level", "params": {"gainDb": level_db}, "type": by_alias["gain"]["type"]}
    )
    edges.append({"from": previous, "to": "level"})
    edges.append({"from": "level", "to": "__output__"})
    return {"edges": edges, "nodes": nodes}


def build(definitions_dir, by_alias, levels, gain):
    groups = {}

    for path in sorted(Path(definitions_dir).glob("*.json")):
        data = json.loads(path.read_text(encoding="utf-8"))
        groups.setdefault(data["group"], []).extend((path.name, p) for p in data["presets"])

    unknown = sorted(set(groups) - set(GROUP_ORDER))

    if unknown:
        raise PackError(f"groups not in GROUP_ORDER: {unknown}")

    presets = []
    folders = []
    seen = {}

    for group in GROUP_ORDER:
        ids = []

        for source, preset in groups.get(group, []):
            name = preset["name"]
            where = f"{source}: '{name}'"
            preset_id = f"sepia-{slug(name)}"

            if preset_id in seen:
                raise PackError(f"{where}: same name as a preset in {seen[preset_id]}")

            seen[preset_id] = source
            level_db = round(float(levels.get(name, 0.0)), 2)

            if not -24.0 <= level_db <= 24.0:
                raise PackError(f"{where}: level trim {level_db} dB is outside -24..24")

            graph = build_graph(preset, by_alias, where, level_db)
            presets.append(
                {
                    "category": "Factory",
                    "createdAt": STAMP,
                    "graph": graph,
                    "id": preset_id,
                    "modifiedAt": STAMP,
                    "name": name,
                    "scenes": [{"graph": graph, "id": "scene-1", "title": "Scene 1"}],
                    "tags": sorted(
                        set(preset.get("tags", []))
                        | {slug(group)}
                        | browser_tags(name, group, preset.get("tags", []), gain.get(name))
                    ),
                    "version": 2,
                }
            )
            ids.append(preset_id)

        if ids:
            folders.append({"name": group, "presetIds": ids})

    return {
        "formatVersion": 1,
        "createdAt": STAMP,
        "presets": presets,
        "resources": [],
        "blends": [],
        "presetFolders": [{"name": PACK_NAME, "children": folders}],
    }


def write_archive(pack, out_path):
    out_path = Path(out_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    payload = json.dumps(pack, indent=1, sort_keys=True).encode("utf-8")

    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as archive:
        info = zipfile.ZipInfo("presets.json", ZIP_DATE)
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o644 << 16
        archive.writestr(info, payload)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--catalog", required=True, help="effect catalog JSON from SepiaEffectCatalog")
    parser.add_argument("--presets", default=str(Path(__file__).parent / "presets"), help="definitions directory")
    parser.add_argument("--levels", default=str(Path(__file__).parent / "levels.json"),
                        help="per-preset level trims in dB, by name (written by level.py)")
    parser.add_argument("--out", required=True, help="archive to write")
    parser.add_argument("--json", help="also write the uncompressed presets.json here")
    args = parser.parse_args()

    try:
        levels_path = Path(args.levels)
        levels = json.loads(levels_path.read_text(encoding="utf-8")) if levels_path.exists() else {}
        gain_path = levels_path.with_name("gain.json")
        gain = json.loads(gain_path.read_text(encoding="utf-8")) if gain_path.exists() else {}
        pack = build(args.presets, load_catalog(args.catalog), levels, gain)
    except PackError as error:
        sys.exit(f"error: {error}")

    write_archive(pack, args.out)

    if args.json:
        Path(args.json).write_text(json.dumps(pack, indent=1, sort_keys=True), encoding="utf-8")

    counts = ", ".join(f"{f['name']} {len(f['presetIds'])}" for f in pack["presetFolders"][0]["children"])
    print(f"wrote {args.out}: {len(pack['presets'])} presets ({counts})")


if __name__ == "__main__":
    main()
