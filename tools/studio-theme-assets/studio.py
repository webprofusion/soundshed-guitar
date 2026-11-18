"""
Renders the Studio theme's images: the knob and the moulding grain.

Run it with Blender (5.x), headless, from the repo root:

    blender -b --factory-startup -P tools/studio-theme-assets/studio.py -- [knob] [textures]

With no names after "--" it makes everything. Output goes to core/ui/images/themes/studio/.

Studio is the clean, current take on studio hardware (Studio 2003, studio_2003.py, is the
used one), and it is all moulded plastic: a light knob cap standing on a darker plastic
skirt, lit by one large soft box above and a little to the left, so its shadow is wide and
soft rather than crisp. The scene helpers are shared with studio_2003.py.
"""

import math
import os
import sys

import bpy
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import studio_2003 as base  # noqa: E402  (needs the path above)

OUT = os.path.join(base.REPO, "core", "ui", "images", "themes", "studio")

# The stylesheet's shadows fall straight down and a touch to the right.
KEY_AZIMUTH = math.radians(100.0)
KEY_ELEVATION = math.radians(66.0)


def add_soft_box_rig(scene):
    # A soft box big enough to wrap the knob: soft shadows, broad highlights.
    base.add_area_light(scene, "Key", KEY_AZIMUTH, KEY_ELEVATION, 9.0, 9.0, 300.0, (1.0, 0.985, 0.96))
    # Bounce from the bench in front, cooler, so the knob's lower side is not black.
    base.add_area_light(scene, "Fill", math.radians(270.0), math.radians(35.0), 9.0, 10.0, 70.0, (0.94, 0.97, 1.0))
    # A thin rim from the right, for the skirt's edge.
    base.add_area_light(scene, "Rim", math.radians(10.0), math.radians(28.0), 9.0, 3.0, 60.0)


# ------------------------------------------------------------------------- knob


def moulded_plastic(name, colour, roughness):
    """Moulded ABS: matte, with the fine spark-eroded texture the tool leaves."""
    material, bsdf = base.principled(name)
    tree = material.node_tree
    bsdf.inputs["Base Color"].default_value = (*colour, 1.0)
    bsdf.inputs["Roughness"].default_value = roughness
    bsdf.inputs["Specular IOR Level"].default_value = 0.45
    bsdf.inputs["Subsurface Weight"].default_value = 0.06
    bsdf.inputs["Subsurface Radius"].default_value = (0.02, 0.02, 0.02)
    coords = tree.nodes.new("ShaderNodeTexCoord")
    grain = tree.nodes.new("ShaderNodeTexNoise")
    grain.inputs["Scale"].default_value = 260.0
    grain.inputs["Detail"].default_value = 2.0
    tree.links.new(coords.outputs["Object"], grain.inputs["Vector"])
    bump = tree.nodes.new("ShaderNodeBump")
    bump.inputs["Strength"].default_value = 0.035
    tree.links.new(grain.outputs["Fac"], bump.inputs["Height"])
    tree.links.new(bump.outputs["Normal"], bsdf.inputs["Normal"])
    return material


def render_knob():
    scene = base.reset_scene(320, 384)
    scene.world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.95, 0.96, 1.0, 1.0)
    scene.world.node_tree.nodes["Background"].inputs["Strength"].default_value = 0.55
    base.add_camera(scene, 2.6)
    add_soft_box_rig(scene)
    base.add_shadow_catcher(scene, 4.0)

    # (radius, height, material slot) from the centre of the cap down to the panel. Slot 0 is
    # the light cap, slot 1 the darker skirt it is moulded onto. A wide top, dished a little
    # for a fingertip (which also gives the soft box a gradient to draw), a generous radius
    # into straight sides, then a step out onto the skirt, which rounds down to the panel.
    profile = [
        (0.02, 0.624, 0),
        (0.30, 0.627, 0),
        (0.52, 0.635, 0),
        (0.62, 0.640, 0),
        (0.70, 0.637, 0),
        (0.755, 0.627, 0),
        (0.795, 0.610, 0),
        (0.822, 0.586, 0),
        (0.837, 0.556, 0),
        (0.842, 0.520, 0),
        (0.842, 0.240, 0),
        (0.838, 0.212, 0),
        (0.830, 0.200, 1),
        (0.900, 0.196, 1),
        (0.950, 0.184, 1),
        (0.982, 0.160, 1),
        (0.998, 0.124, 1),
        (1.000, 0.030, 1),
        (0.990, 0.000, 1),
    ]

    def radius_at(_row, radius, _angle):
        return radius

    cap = moulded_plastic("Cap", (0.83, 0.83, 0.81), 0.48)
    skirt = moulded_plastic("Skirt", (0.10, 0.102, 0.108), 0.42)
    knob = base.add_spun_object(scene, "Knob", profile, 256, radius_at, [cap, skirt])
    knob.data.set_sharp_from_angle(angle=math.radians(60.0))
    base.render_to(scene, "knob.png", 1.0 / 1.3, out=OUT)


# --------------------------------------------------------------------- textures


def make_grain(rng):
    """
    The moulding's fine texture as a transparent overlay, so one image serves every plastic
    colour: pale flecks on the upper slopes of the grain and dark ones on the lower, at a few
    per cent opacity.
    """
    size = 256
    tooth = base.unit(base.blur_wrap(rng.standard_normal((size, size)), 0.7, 0.7))
    relief = base.unit(np.roll(tooth, 1, axis=0) - np.roll(tooth, -1, axis=0))
    field = 0.6 * relief + 0.4 * tooth
    alpha = np.clip(np.abs(field) * 0.012, 0.0, 0.045)
    light = np.array([1.0, 1.0, 1.0])
    dark = np.array([0.10, 0.11, 0.12])
    rgb = np.where((field > 0)[..., None], light[None, None, :], dark[None, None, :])
    base.save_image("grain.png", np.concatenate([rgb, alpha[..., None]], axis=2), out=OUT)


def make_textures():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    rng = np.random.default_rng(2026)
    make_grain(rng)


# ------------------------------------------------------------------------- main

JOBS = {"knob": render_knob, "textures": make_textures}

if __name__ == "__main__":
    names = sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else []
    unknown = [name for name in names if name not in JOBS]
    if unknown:
        raise SystemExit(f"unknown job(s): {', '.join(unknown)}; expected {', '.join(JOBS)}")
    for name in names or JOBS:
        JOBS[name]()
