"""
Renders the Stage theme's backdrop: a dark room with a slatted back wall and a varnished
plank floor, lit by one warm spotlight pooled in the middle of the floor.

Run it with Blender (5.x), headless, from the repo root:

    blender -b --factory-startup -P tools/stage-theme-assets/stage.py

Output goes to core/ui/images/themes/stage/stage.jpg. The stylesheet (css/themes/stage.css)
draws it behind the effect panel, anchored to the bottom so the lit floor sits under the
panel, and darkens its edges further, so keep the render itself dim: the UI's text has to
read over any part of it.
"""

import math
import os

import bpy

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = os.path.join(REPO, "core", "ui", "images", "themes", "stage")

WIDTH = 2400
HEIGHT = 1350
SAMPLES = 160


def reset_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.samples = SAMPLES
    scene.cycles.use_adaptive_sampling = False
    scene.cycles.use_denoising = True
    scene.cycles.seed = 11
    scene.render.resolution_x = WIDTH
    scene.render.resolution_y = HEIGHT
    scene.render.resolution_percentage = 100
    settings = scene.render.image_settings
    if hasattr(settings, "media_type"):
        settings.media_type = "IMAGE"
    settings.file_format = "JPEG"
    settings.color_mode = "RGB"
    settings.quality = 84
    # A photograph, not a sprite composited over CSS: a filmic curve keeps the pool of light
    # from clipping and the shadows from going flat.
    scene.view_settings.view_transform = "AgX"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = -0.55

    world = bpy.data.worlds.new("World")
    scene.world = world
    world.use_nodes = True
    background = world.node_tree.nodes["Background"]
    background.inputs["Color"].default_value = (0.5, 0.42, 0.34, 1.0)
    background.inputs["Strength"].default_value = 0.012
    return scene


def node(tree, kind, location, **inputs):
    created = tree.nodes.new(kind)
    created.location = location
    for name, value in inputs.items():
        created.inputs[name].default_value = value
    return created


def floor_material():
    """Varnished planks: long boards with a little colour between them, grain along each."""
    material = bpy.data.materials.new("Floor")
    material.use_nodes = True
    tree = material.node_tree
    bsdf = tree.nodes["Principled BSDF"]
    links = tree.links

    # The boards run away from the camera, so turn the texture space a quarter.
    coords = node(tree, "ShaderNodeTexCoord", (-1400, 0))
    mapping = node(tree, "ShaderNodeMapping", (-1200, 0))
    mapping.inputs["Rotation"].default_value = (0.0, 0.0, math.radians(90.0))
    links.new(coords.outputs["Object"], mapping.inputs["Vector"])

    # Boards: the brick texture, laid long along X, with a hairline gap.
    bricks = tree.nodes.new("ShaderNodeTexBrick")
    bricks.location = (-900, 200)
    bricks.offset = 0.37
    bricks.offset_frequency = 1
    bricks.inputs["Color1"].default_value = (0.22, 0.12, 0.06, 1.0)
    bricks.inputs["Color2"].default_value = (0.36, 0.21, 0.11, 1.0)
    bricks.inputs["Mortar"].default_value = (0.03, 0.015, 0.008, 1.0)
    bricks.inputs["Scale"].default_value = 1.0
    bricks.inputs["Mortar Size"].default_value = 0.004
    bricks.inputs["Brick Width"].default_value = 2.4
    bricks.inputs["Row Height"].default_value = 0.16
    bricks.inputs["Bias"].default_value = 0.0
    links.new(mapping.outputs["Vector"], bricks.inputs["Vector"])

    # Grain: stretched bands, wavy, running along the boards.
    grain_map = node(tree, "ShaderNodeMapping", (-1000, -250))
    grain_map.inputs["Scale"].default_value = (0.08, 1.0, 1.0)
    links.new(mapping.outputs["Vector"], grain_map.inputs["Vector"])
    grain = tree.nodes.new("ShaderNodeTexWave")
    grain.location = (-800, -250)
    grain.wave_type = "BANDS"
    grain.bands_direction = "Y"
    grain.inputs["Scale"].default_value = 9.0
    grain.inputs["Distortion"].default_value = 7.0
    grain.inputs["Detail"].default_value = 3.0
    grain.inputs["Detail Scale"].default_value = 1.6
    links.new(grain_map.outputs["Vector"], grain.inputs["Vector"])

    grain_ramp = tree.nodes.new("ShaderNodeValToRGB")
    grain_ramp.location = (-600, -250)
    grain_ramp.color_ramp.elements[0].position = 0.2
    grain_ramp.color_ramp.elements[0].color = (0.62, 0.62, 0.62, 1.0)
    grain_ramp.color_ramp.elements[1].position = 0.9
    grain_ramp.color_ramp.elements[1].color = (1.0, 1.0, 1.0, 1.0)
    links.new(grain.outputs["Fac"], grain_ramp.inputs["Fac"])

    tint = tree.nodes.new("ShaderNodeMix")
    tint.location = (-350, 100)
    tint.data_type = "RGBA"
    tint.blend_type = "MULTIPLY"
    tint.inputs["Factor"].default_value = 1.0
    links.new(bricks.outputs["Color"], tint.inputs["A"])
    links.new(grain_ramp.outputs["Color"], tint.inputs["B"])
    links.new(tint.outputs["Result"], bsdf.inputs["Base Color"])

    # Satin varnish, a touch rougher in the gaps; scuffs from a little noise.
    scuff = node(tree, "ShaderNodeTexNoise", (-800, -550), Scale=14.0, Detail=6.0)
    scuff_ramp = tree.nodes.new("ShaderNodeMapRange")
    scuff_ramp.location = (-600, -550)
    scuff_ramp.inputs["To Min"].default_value = 0.22
    scuff_ramp.inputs["To Max"].default_value = 0.42
    links.new(scuff.outputs["Fac"], scuff_ramp.inputs["Value"])
    links.new(scuff_ramp.outputs["Result"], bsdf.inputs["Roughness"])

    bump = node(tree, "ShaderNodeBump", (-350, -350), Strength=0.25, Distance=0.002)
    links.new(bricks.outputs["Fac"], bump.inputs["Height"])
    links.new(bump.outputs["Normal"], bsdf.inputs["Normal"])
    return material


def wall_material():
    """Dark stained slats, vertical, with deep gaps between them."""
    material = bpy.data.materials.new("Wall")
    material.use_nodes = True
    tree = material.node_tree
    bsdf = tree.nodes["Principled BSDF"]
    links = tree.links

    coords = node(tree, "ShaderNodeTexCoord", (-1000, 0))
    slats = tree.nodes.new("ShaderNodeTexWave")
    slats.location = (-800, 0)
    slats.wave_type = "BANDS"
    slats.bands_direction = "X"
    slats.wave_profile = "SAW"
    slats.inputs["Scale"].default_value = 3.2
    slats.inputs["Distortion"].default_value = 0.0
    links.new(coords.outputs["Object"], slats.inputs["Vector"])

    ramp = tree.nodes.new("ShaderNodeValToRGB")
    ramp.location = (-550, 0)
    elements = ramp.color_ramp.elements
    elements[0].position = 0.0
    elements[0].color = (0.004, 0.002, 0.001, 1.0)
    elements[1].position = 0.12
    elements[1].color = (0.12, 0.065, 0.03, 1.0)
    extra = elements.new(0.9)
    extra.color = (0.16, 0.085, 0.04, 1.0)
    links.new(slats.outputs["Fac"], ramp.inputs["Fac"])
    links.new(ramp.outputs["Color"], bsdf.inputs["Base Color"])
    bsdf.inputs["Roughness"].default_value = 0.55
    return material


def add_quad(scene, name, corners, material):
    """A quad with its corners in metres, so texture space is in metres too."""
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(corners, [], [(0, 1, 2, 3)])
    mesh.materials.append(material)
    quad = bpy.data.objects.new(name, mesh)
    scene.collection.objects.link(quad)
    return quad


def add_spot(scene, name, location, target, watts, size_deg, blend, colour, radius):
    data = bpy.data.lights.new(name, "SPOT")
    data.energy = watts
    data.spot_size = math.radians(size_deg)
    data.spot_blend = blend
    data.color = colour
    data.shadow_soft_size = radius
    light = bpy.data.objects.new(name, data)
    light.location = location
    scene.collection.objects.link(light)
    aim(light, target)
    return light


def aim(obj, target):
    from mathutils import Vector

    direction = Vector(target) - Vector(obj.location)
    obj.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()


def build():
    scene = reset_scene()

    add_quad(scene, "Floor", [(-20.0, -6.0, 0.0), (20.0, -6.0, 0.0), (20.0, 7.0, 0.0), (-20.0, 7.0, 0.0)], floor_material())
    add_quad(scene, "Wall", [(-20.0, 7.0, 0.0), (20.0, 7.0, 0.0), (20.0, 7.0, 12.0), (-20.0, 7.0, 12.0)], wall_material())

    # The pool of light the effect stands in, a little in front of the wall.
    add_spot(scene, "Key", (0.0, 1.8, 7.5), (0.0, 3.6, 0.0), 3400.0, 40.0, 0.7, (1.0, 0.8, 0.58), 0.5)
    # A faint wash up the wall, so the slats read behind the pool.
    add_spot(scene, "Wash", (0.0, 3.0, 0.3), (0.0, 7.0, 3.6), 520.0, 80.0, 1.0, (1.0, 0.72, 0.45), 0.4)

    camera_data = bpy.data.cameras.new("Camera")
    camera_data.lens = 30.0
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, -2.2, 1.55)
    scene.collection.objects.link(camera)
    aim(camera, (0.0, 4.5, 0.95))
    scene.camera = camera
    return scene


if __name__ == "__main__":
    import sys

    # "-- preview <path>": a quick, small, noisy render to look at the framing and light.
    args = sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else []
    scene = build()
    if args[:1] == ["preview"]:
        scene.render.resolution_percentage = 40
        scene.cycles.samples = 48
        path = args[1]
    else:
        os.makedirs(OUT, exist_ok=True)
        path = os.path.join(OUT, "stage.jpg")
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    print("wrote", path)
