"""
Renders the High Gain theme's images: the knob, the diamond plate and tolex textures, and
the play page's backdrop (a black grille with valves glowing behind it).

Run it with Blender (5.x), headless, from the repo root:

    blender -b --factory-startup -P tools/high-gain-theme-assets/high_gain.py -- [knob] [textures] [grille]

With no names after "--" it makes everything. "grille preview <file>" makes a quick, small,
noisy backdrop to judge framing and light. Output goes to core/ui/images/themes/high-gain/.

High Gain is the dark theme drawn as a modern high-gain amp head: black tolex, black diamond
plate, a perforated black grille with the valves' red-orange glow showing through it, and
chunky black knobs. The knob and backdrop are path traced; the textures are numpy height
fields, shaded here and made to tile. The knob shares studio_2003.py's scene helpers and its
light (above and to the left), plus a faint red-orange light from below, the valves' glow.
"""

import math
import os
import sys
import tempfile

import bpy
import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "studio-theme-assets"))
import studio_2003 as base  # noqa: E402  (needs the path above)

OUT = os.path.join(base.REPO, "core", "ui", "images", "themes", "high-gain")

VALVE_GLOW = (1.0, 0.16, 0.025)


# ------------------------------------------------------------------------- knob


def gunmetal_cap():
    """The cap insert: dark spun metal, so the light draws rings of streaks across it."""
    material, bsdf = base.principled("Gunmetal")
    tree = material.node_tree
    bsdf.inputs["Base Color"].default_value = (0.20, 0.205, 0.215, 1.0)
    bsdf.inputs["Metallic"].default_value = 1.0
    bsdf.inputs["Roughness"].default_value = 0.3
    bsdf.inputs["Anisotropic"].default_value = 0.85
    tangent = tree.nodes.new("ShaderNodeTangent")
    tangent.direction_type = "RADIAL"
    tangent.axis = "Z"
    tree.links.new(tangent.outputs["Tangent"], bsdf.inputs["Tangent"])
    coords = tree.nodes.new("ShaderNodeTexCoord")
    rings = tree.nodes.new("ShaderNodeTexWave")
    rings.wave_type = "RINGS"
    rings.rings_direction = "Z"
    rings.inputs["Scale"].default_value = 60.0
    rings.inputs["Distortion"].default_value = 0.4
    tree.links.new(coords.outputs["Object"], rings.inputs["Vector"])
    bump = tree.nodes.new("ShaderNodeBump")
    bump.inputs["Strength"].default_value = 0.04
    tree.links.new(rings.outputs["Fac"], bump.inputs["Height"])
    tree.links.new(bump.outputs["Normal"], bsdf.inputs["Normal"])
    return material


def black_bakelite():
    """The body: black, semi-gloss, with a little wander in its sheen."""
    material, bsdf = base.principled("Bakelite")
    tree = material.node_tree
    bsdf.inputs["Base Color"].default_value = (0.018, 0.018, 0.019, 1.0)
    bsdf.inputs["Specular IOR Level"].default_value = 0.6
    coords = tree.nodes.new("ShaderNodeTexCoord")
    noise = tree.nodes.new("ShaderNodeTexNoise")
    noise.inputs["Scale"].default_value = 6.0
    noise.inputs["Detail"].default_value = 5.0
    tree.links.new(coords.outputs["Object"], noise.inputs["Vector"])
    rough = tree.nodes.new("ShaderNodeMapRange")
    rough.inputs["To Min"].default_value = 0.22
    rough.inputs["To Max"].default_value = 0.38
    tree.links.new(noise.outputs["Fac"], rough.inputs["Value"])
    tree.links.new(rough.outputs["Result"], bsdf.inputs["Roughness"])
    return material


def render_knob():
    scene = base.reset_scene(320, 384)
    scene.world.node_tree.nodes["Background"].inputs["Strength"].default_value = 0.25
    # 1.15 skirt diameters across, so the knob fills the value arc it sits in.
    base.add_camera(scene, 2.3)
    base.add_light_rig(scene, 1.15)
    # The valves' glow, low from the front: a warm rim on the skirt's lower edge.
    base.add_area_light(scene, "Glow", math.radians(270.0), math.radians(16.0), 9.0, 6.0, 90.0, VALVE_GLOW)
    base.add_shadow_catcher(scene, 4.0)

    # (radius, height, material slot) from the centre of the cap down to the panel. Slot 0 is
    # the metal cap, slot 1 the black body: a lip round the cap, straight knurled sides, then
    # a step out onto a skirt that rounds down to the panel.
    profile = [
        (0.02, 0.650, 0),
        (0.40, 0.656, 0),
        (0.57, 0.660, 0),
        (0.590, 0.652, 1),
        (0.640, 0.668, 1),
        (0.730, 0.664, 1),
        (0.775, 0.648, 1),
        (0.800, 0.620, 1),
        (0.800, 0.210, 1),
        (0.812, 0.180, 1),
        (0.940, 0.160, 1),
        (0.985, 0.138, 1),
        (1.000, 0.100, 1),
        (1.000, 0.020, 1),
        (0.990, 0.000, 1),
    ]
    knurled_rows = (7, 8)
    flutes = 44

    def radius_at(row, radius, angle):
        if row not in knurled_rows:
            return radius
        ridge = abs(math.cos(0.5 * flutes * angle)) ** 0.6
        return radius * (1.0 - 0.045 * (1.0 - ridge))

    knob = base.add_spun_object(scene, "Knob", profile, 528, radius_at, [gunmetal_cap(), black_bakelite()])
    knob.data.set_sharp_from_angle(angle=math.radians(50.0))
    base.render_to(scene, "knob.png", 1.0 / 1.15, out=OUT)


# --------------------------------------------------------------------- textures


def normals_from_height(height, strength):
    """Surface normals of a tiling height field (rows down, columns right), by wrapped differences."""
    dy = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * 0.5 * strength
    dx = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * 0.5 * strength
    n = np.stack([-dx, dy, np.ones_like(height)], axis=-1)
    return n / np.linalg.norm(n, axis=-1, keepdims=True)


def shade(normals, colour, light, ambient, gloss, shininess):
    """
    Lambert plus a Blinn highlight, seen from straight in front. light is (x right, y up, z out
    of the screen); the stylesheet's light is above the panel, so y is positive.
    """
    light = np.asarray(light, dtype=np.float64)
    light /= np.linalg.norm(light)
    half = light + np.array([0.0, 0.0, 1.0])
    half /= np.linalg.norm(half)
    diffuse = np.clip(normals @ light, 0.0, 1.0)
    spec = np.clip(normals @ half, 0.0, 1.0) ** shininess
    return colour[None, None, :] * (ambient + (1.0 - ambient) * diffuse)[..., None] + gloss * spec[..., None]


def downsample(rgb, factor):
    h, w, c = rgb.shape
    return rgb.reshape(h // factor, factor, w // factor, factor, c).mean(axis=(1, 3))


def make_plate(rng):
    """
    Black powder-coated diamond plate: raised lozenges at 45 degrees, each cell turned a quarter
    from its neighbours. Made at 2x what it is drawn (512 px, drawn at 256), and shaded at 2x
    that again and averaged down, so the lozenges' edges stay smooth.
    """
    ss = 2
    size = 512 * ss
    cells = 16  # even, so the alternating cells tile
    pitch = size / cells
    y, x = np.mgrid[0:size, 0:size].astype(np.float64) + 0.5
    row = np.floor(y / pitch)
    col = np.floor(x / pitch)
    u = (x - (col + 0.5) * pitch) / pitch
    v = (y - (row + 0.5) * pitch) / pitch
    turn = np.where((row + col) % 2 == 0, 1.0, -1.0)
    along = (u + turn * v) / math.sqrt(2.0)
    across = (u - turn * v) / math.sqrt(2.0)
    # A lens: long and narrow, rounded on top, with pointed ends.
    lens = 1.0 - (np.abs(along) / 0.38) ** 1.6 - (np.abs(across) / 0.12) ** 2
    height = np.sqrt(np.clip(lens, 0.0, None))
    # The sheet under it is not perfectly flat, and the powder coat has a fine orange-peel tooth.
    wave = base.unit(base.blur_wrap(rng.standard_normal((size, size)), 40.0, 40.0))
    tooth = base.unit(base.blur_wrap(rng.standard_normal((size, size)), 1.2 * ss, 1.2 * ss))
    surface = 5.0 * height + 0.25 * wave + 0.05 * tooth
    normals = normals_from_height(surface, 1.0 / ss)
    colour = np.array([0.085, 0.088, 0.094])
    rgb = shade(normals, colour, (-0.25, 0.75, 0.6), 0.4, 0.2, 14.0)
    # Scuffs: the lozenges' tops are rubbed a touch lighter than the sheet.
    rgb += (0.035 * np.clip(height - 0.5, 0.0, None))[..., None]
    rgb = downsample(rgb, ss)
    base.save_image("plate.jpg", base.opaque(rgb), jpeg_quality=90, out=OUT)


def make_tolex(rng):
    """
    Black pebble-grain vinyl: rounded bumps a few pixels across with creases between them. Made
    at 2x what it is drawn (512 px, drawn at 256).
    """
    size = 512
    pebbles = base.unit(base.blur_wrap(rng.standard_normal((size, size)), 2.2, 2.2))
    big = base.unit(base.blur_wrap(rng.standard_normal((size, size)), 9.0, 9.0))
    # Pebbles are flat-topped: soften the highs, keep the creases between them sharp.
    surface = np.tanh(1.2 * pebbles) + 0.3 * big
    normals = normals_from_height(surface, 0.45)
    colour = np.array([0.062, 0.062, 0.066])
    rgb = shade(normals, colour, (-0.2, 0.8, 0.55), 0.5, 0.05, 10.0)
    base.save_image("tolex.jpg", base.opaque(rgb), jpeg_quality=90, out=OUT)


def make_textures():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    rng = np.random.default_rng(666)
    make_plate(rng)
    make_tolex(rng)


# ----------------------------------------------------------------------- grille

GRILLE_WIDTH = 2400
GRILLE_HEIGHT = 1350
GRILLE_SAMPLES = 192
# The chassis deck the valves stand on: low, so their glow rises behind the lower half of
# the page, under the effect panel, and the top of the page stays dark.
DECK = -1.0


def hex_mask_path():
    """
    One period of a hex-perforated sheet as an image: white where the metal is, black in the
    holes, with the metal's edges softened for the bump. Pointy-topped holes on a triangular
    lattice (neighbours side by side along a row); the period is one hole-pitch wide and two
    rows tall.
    """
    width = 256
    height = int(round(width * math.sqrt(3.0)))  # two rows of the lattice
    y, x = np.mgrid[0:height, 0:width].astype(np.float64) + 0.5
    pitch = float(width)
    radius = 0.40 * pitch  # centre to flat side of a hole
    best = np.full(x.shape, np.inf)
    row_step = height / 2.0
    for cy, cx in [(0, 0), (0, pitch), (height, 0), (height, pitch), (row_step, pitch / 2)]:
        dx = np.abs(x - cx)
        dy = np.abs(y - cy)
        # Distance to a pointy-topped hexagon's edge, measured like its apothem.
        hexd = np.maximum(dx, (dx * 0.5 + dy * math.sqrt(3.0) / 2.0))
        best = np.minimum(best, hexd)
    edge = np.clip((best - radius) / (0.06 * pitch), 0.0, 1.0)
    metal = np.sqrt(edge)
    image = bpy.data.images.new("HexMask", width, height, alpha=False)
    image.colorspace_settings.name = "Non-Color"
    rgba = np.ones((height, width, 4))
    rgba[..., 0] = rgba[..., 1] = rgba[..., 2] = metal
    image.pixels.foreach_set(np.flipud(rgba).astype(np.float32).ravel())
    path = os.path.join(tempfile.gettempdir(), "high-gain-hex-mask.png")
    image.filepath_raw = path
    image.file_format = "PNG"
    image.save()
    bpy.data.images.remove(image)
    return path, height / width


def grille_material(mask_path, aspect, holes_across, frame_width):
    material = bpy.data.materials.new("Grille")
    material.use_nodes = True
    tree = material.node_tree
    links = tree.links
    bsdf = tree.nodes["Principled BSDF"]
    output = tree.nodes["Material Output"]
    bsdf.inputs["Base Color"].default_value = (0.012, 0.012, 0.013, 1.0)
    bsdf.inputs["Metallic"].default_value = 0.6
    bsdf.inputs["Roughness"].default_value = 0.42

    coords = tree.nodes.new("ShaderNodeTexCoord")
    mapping = tree.nodes.new("ShaderNodeMapping")
    scale = holes_across / frame_width
    # The sheet stands in XZ: turn Z into the image's Y. Y and Z get the same scale, so the
    # result is the same whichever the node applies first.
    mapping.inputs["Scale"].default_value = (scale, scale / aspect, scale / aspect)
    mapping.inputs["Rotation"].default_value = (math.radians(-90.0), 0.0, 0.0)
    links.new(coords.outputs["Object"], mapping.inputs["Vector"])
    texture = tree.nodes.new("ShaderNodeTexImage")
    texture.image = bpy.data.images.load(mask_path)
    texture.image.colorspace_settings.name = "Non-Color"
    texture.extension = "REPEAT"
    texture.interpolation = "Cubic"
    links.new(mapping.outputs["Vector"], texture.inputs["Vector"])

    # Holes are where the mask is dark; the soft ramp at their edges becomes the bump.
    cut = tree.nodes.new("ShaderNodeMath")
    cut.operation = "GREATER_THAN"
    cut.inputs[1].default_value = 0.02
    links.new(texture.outputs["Color"], cut.inputs[0])
    bump = tree.nodes.new("ShaderNodeBump")
    bump.inputs["Strength"].default_value = 0.6
    bump.inputs["Distance"].default_value = 0.004
    links.new(texture.outputs["Color"], bump.inputs["Height"])
    links.new(bump.outputs["Normal"], bsdf.inputs["Normal"])

    transparent = tree.nodes.new("ShaderNodeBsdfTransparent")
    mix = tree.nodes.new("ShaderNodeMixShader")
    links.new(cut.outputs["Value"], mix.inputs["Fac"])
    links.new(transparent.outputs["BSDF"], mix.inputs[1])
    links.new(bsdf.outputs["BSDF"], mix.inputs[2])
    links.new(mix.outputs["Shader"], output.inputs["Surface"])
    return material


def emission(name, colour, strength):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    tree = material.node_tree
    tree.nodes.remove(tree.nodes["Principled BSDF"])
    node = tree.nodes.new("ShaderNodeEmission")
    node.inputs["Color"].default_value = (*colour, 1.0)
    node.inputs["Strength"].default_value = strength
    tree.links.new(node.outputs["Emission"], tree.nodes["Material Output"].inputs["Surface"])
    return material


def glass():
    material, bsdf = base.principled("Glass")
    bsdf.inputs["Base Color"].default_value = (0.9, 0.9, 0.92, 1.0)
    bsdf.inputs["Roughness"].default_value = 0.04
    bsdf.inputs["Transmission Weight"].default_value = 1.0
    bsdf.inputs["IOR"].default_value = 1.47
    return material


def matte(name, colour, roughness):
    material, bsdf = base.principled(name)
    bsdf.inputs["Base Color"].default_value = (*colour, 1.0)
    bsdf.inputs["Roughness"].default_value = roughness
    return material


def add_cylinder(scene, name, radius, depth, location, material, segments=48):
    bpy.ops.mesh.primitive_cylinder_add(vertices=segments, radius=radius, depth=depth, location=location)
    obj = bpy.context.active_object
    obj.name = name
    obj.data.materials.append(material)
    bpy.ops.object.shade_smooth()
    return obj


def add_valve(scene, x, y, z_base, scale, materials):
    """A power valve standing on the chassis: a glass bottle with a glowing plate inside."""
    glass_mat, plate_mat, heater_mat, base_mat = materials
    add_cylinder(scene, "ValveBase", 0.11 * scale, 0.10 * scale, (x, y, z_base + 0.05 * scale), base_mat)
    add_cylinder(scene, "ValveGlass", 0.10 * scale, 0.62 * scale, (x, y, z_base + 0.41 * scale), glass_mat)
    bpy.ops.mesh.primitive_uv_sphere_add(radius=0.10 * scale, location=(x, y, z_base + 0.72 * scale))
    top = bpy.context.active_object
    top.data.materials.append(glass_mat)
    bpy.ops.object.shade_smooth()
    add_cylinder(scene, "ValvePlate", 0.055 * scale, 0.36 * scale, (x, y, z_base + 0.38 * scale), plate_mat, 24)
    add_cylinder(scene, "ValveHeater", 0.012 * scale, 0.42 * scale, (x, y - 0.06 * scale, z_base + 0.38 * scale), heater_mat, 12)


def build_grille():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.samples = GRILLE_SAMPLES
    scene.cycles.use_adaptive_sampling = False
    scene.cycles.use_denoising = True
    scene.cycles.seed = 13
    scene.cycles.transparent_max_bounces = 16
    scene.render.resolution_x = GRILLE_WIDTH
    scene.render.resolution_y = GRILLE_HEIGHT
    scene.render.resolution_percentage = 100
    settings = scene.render.image_settings
    if hasattr(settings, "media_type"):
        settings.media_type = "IMAGE"
    settings.file_format = "JPEG"
    settings.color_mode = "RGB"
    settings.quality = 86
    scene.view_settings.view_transform = "AgX"
    # Punchy keeps the glow red rather than letting the filmic curve wash it out to peach.
    scene.view_settings.look = "AgX - Punchy"
    scene.view_settings.exposure = -0.2

    world = bpy.data.worlds.new("World")
    scene.world = world
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.5, 0.5, 0.55, 1.0)
    world.node_tree.nodes["Background"].inputs["Strength"].default_value = 0.004

    # The grille stands in the XZ plane at y = 0, the camera looks at it from -Y.
    frame_width = 4.0
    frame_height = frame_width * GRILLE_HEIGHT / GRILLE_WIDTH
    mask_path, aspect = hex_mask_path()
    mesh = bpy.data.meshes.new("Grille")
    w = frame_width * 0.75
    h = frame_height * 0.75
    mesh.from_pydata([(-w, 0.0, -h), (w, 0.0, -h), (w, 0.0, h), (-w, 0.0, h)], [], [(0, 1, 2, 3)])
    mesh.materials.append(grille_material(mask_path, aspect, 92.0, frame_width))
    grille = bpy.data.objects.new("Grille", mesh)
    scene.collection.objects.link(grille)

    # Behind it: the chassis, a black back wall, and a row of power valves along the lower half.
    chassis = matte("Chassis", (0.02, 0.02, 0.021), 0.5)
    mesh = bpy.data.meshes.new("Back")
    mesh.from_pydata([(-6, 1.8, -4), (6, 1.8, -4), (6, 1.8, 4), (-6, 1.8, 4)], [], [(0, 1, 2, 3)])
    mesh.materials.append(chassis)
    back = bpy.data.objects.new("Back", mesh)
    scene.collection.objects.link(back)
    mesh = bpy.data.meshes.new("Deck")
    mesh.from_pydata([(-6, 0.05, DECK), (6, 0.05, DECK), (6, 1.8, DECK), (-6, 1.8, DECK)], [], [(0, 1, 2, 3)])
    mesh.materials.append(matte("Deck", (0.02, 0.02, 0.021), 0.5))
    deck = bpy.data.objects.new("Deck", mesh)
    scene.collection.objects.link(deck)
    # The chassis front under the deck, so nothing lit shows below the valves.
    mesh = bpy.data.meshes.new("Front")
    mesh.from_pydata([(-6, 0.05, -4), (6, 0.05, -4), (6, 0.05, DECK), (-6, 0.05, DECK)], [], [(0, 1, 2, 3)])
    mesh.materials.append(chassis)
    front = bpy.data.objects.new("Front", mesh)
    scene.collection.objects.link(front)

    materials = (
        glass(),
        emission("Plate", VALVE_GLOW, 0.4),
        emission("Heater", (1.0, 0.42, 0.12), 5.0),
        matte("ValveBase", (0.03, 0.03, 0.03), 0.4),
    )
    for i, x in enumerate(np.linspace(-1.75, 1.75, 8)):
        add_valve(scene, float(x), 1.0 + 0.08 * (i % 2), DECK, 1.2, materials)

    # The glow fills the chassis: a broad warm light behind the valves, thrown on the back
    # wall so they stand dark against it, and kept off the deck in front of them.
    data = bpy.data.lights.new("ValveWash", "AREA")
    data.shape = "RECTANGLE"
    data.size = 4.2
    data.size_y = 0.4
    data.energy = 110.0
    data.color = VALVE_GLOW
    wash = bpy.data.objects.new("ValveWash", data)
    wash.location = (0.0, 1.3, DECK + 0.5)
    wash.rotation_euler = (math.radians(90.0), 0.0, 0.0)
    wash.visible_camera = False  # its glow, not the light itself, should show through the holes
    scene.collection.objects.link(wash)

    # A dim cool light from above the front, so the grille's metal has an edge to it.
    data = bpy.data.lights.new("Top", "AREA")
    data.shape = "RECTANGLE"
    data.size = 6.0
    data.size_y = 1.0
    data.energy = 140.0
    data.color = (0.85, 0.9, 1.0)
    top = bpy.data.objects.new("Top", data)
    top.location = (0.0, -1.2, 2.6)
    base_aim(top, (0.0, 0.0, 0.2))
    scene.collection.objects.link(top)

    camera_data = bpy.data.cameras.new("Camera")
    camera_data.lens = 50.0
    camera_data.sensor_width = 36.0
    camera_data.dof.use_dof = True
    camera_data.dof.focus_distance = 3.6
    camera_data.dof.aperture_fstop = 0.35
    camera = bpy.data.objects.new("Camera", camera_data)
    # Distance that frames frame_width across with a 50 mm lens on a 36 mm sensor.
    distance = frame_width * 50.0 / 36.0
    camera.location = (0.0, -distance, 0.0)
    camera.rotation_euler = (math.radians(90.0), 0.0, 0.0)
    scene.collection.objects.link(camera)
    scene.camera = camera
    camera_data.dof.focus_distance = distance
    return scene


def base_aim(obj, target):
    from mathutils import Vector

    direction = Vector(target) - Vector(obj.location)
    obj.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()


def render_grille(preview_path=None):
    scene = build_grille()
    if preview_path:
        scene.render.resolution_percentage = 40
        scene.cycles.samples = 48
        path = preview_path
    else:
        os.makedirs(OUT, exist_ok=True)
        path = os.path.join(OUT, "grille.jpg")
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    print("wrote", path)


# ------------------------------------------------------------------------- main

JOBS = {"knob": render_knob, "textures": make_textures, "grille": render_grille}

if __name__ == "__main__":
    names = sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else []
    if names[:2] == ["grille", "preview"]:
        render_grille(names[2])
        raise SystemExit(0)
    unknown = [name for name in names if name not in JOBS]
    if unknown:
        raise SystemExit(f"unknown job(s): {', '.join(unknown)}; expected {', '.join(JOBS)}")
    for name in names or JOBS:
        JOBS[name]()
