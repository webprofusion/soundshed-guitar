"""
Renders the Studio theme's images: the knob, the panel screws and the panel textures.

Run it with Blender (5.x), headless, from the repo root:

    blender -b --factory-startup -P tools/studio-theme-assets/generate.py -- [knob] [screws] [textures]

With no names after "--" it makes everything. Output goes to core/ui/images/themes/studio/.
The knob and screws are path traced (Cycles, top-down orthographic, one light rig shared by
both so their shadows agree); the textures are numpy noise, made to tile.

Everything is seeded, so a rerun reproduces the textures exactly and the renders to within
path-tracing noise.
"""

import math
import os
import sys

import bpy
import numpy as np

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = os.path.join(REPO, "core", "ui", "images", "themes", "studio")

# The light comes from above and a little to the left, as the stylesheet's shadows assume.
KEY_AZIMUTH = math.radians(112.0)  # measured anticlockwise from +X, so up and to the left
KEY_ELEVATION = math.radians(58.0)  # above the panel


# ---------------------------------------------------------------- scene helpers


def reset_scene(size, samples):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.samples = samples
    scene.cycles.use_adaptive_sampling = False
    scene.cycles.use_denoising = True
    scene.cycles.seed = 7
    scene.render.film_transparent = True
    scene.render.resolution_x = size
    scene.render.resolution_y = size
    scene.render.resolution_percentage = 100
    settings = scene.render.image_settings
    if hasattr(settings, "media_type"):
        settings.media_type = "IMAGE"
    settings.file_format = "PNG"
    settings.color_mode = "RGBA"
    settings.color_depth = "8"
    settings.compression = 100
    # Standard, not a filmic transform: the page composites these over flat CSS colours.
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"

    world = bpy.data.worlds.new("World")
    scene.world = world
    world.use_nodes = True
    background = world.node_tree.nodes["Background"]
    background.inputs["Color"].default_value = (0.9, 0.92, 1.0, 1.0)
    background.inputs["Strength"].default_value = 0.5
    return scene


def add_camera(scene, ortho_scale):
    data = bpy.data.cameras.new("Camera")
    data.type = "ORTHO"
    data.ortho_scale = ortho_scale
    camera = bpy.data.objects.new("Camera", data)
    camera.location = (0.0, 0.0, 10.0)
    scene.collection.objects.link(camera)
    scene.camera = camera


def add_area_light(scene, name, azimuth, elevation, distance, size, watts, colour=(1.0, 1.0, 1.0)):
    data = bpy.data.lights.new(name, "AREA")
    data.shape = "DISK"
    data.size = size
    data.energy = watts
    data.color = colour
    light = bpy.data.objects.new(name, data)
    direction = (
        math.cos(elevation) * math.cos(azimuth),
        math.cos(elevation) * math.sin(azimuth),
        math.sin(elevation),
    )
    light.location = tuple(distance * c for c in direction)
    # Aim the light's -Z at the origin.
    light.rotation_euler = (math.pi / 2 - elevation, 0.0, azimuth + math.pi / 2)
    scene.collection.objects.link(light)


def add_light_rig(scene, scale=1.0):
    add_area_light(scene, "Key", KEY_AZIMUTH, KEY_ELEVATION, 9.0, 5.0, 330.0 * scale, (1.0, 0.97, 0.92))
    # A low, cooler light from the right: the second, dimmer pair of streaks on spun metal.
    add_area_light(scene, "Side", math.radians(18.0), math.radians(40.0), 9.0, 4.0, 120.0 * scale, (0.9, 0.95, 1.0))
    add_area_light(scene, "Fill", math.radians(262.0), math.radians(50.0), 9.0, 7.0, 40.0 * scale)


def add_shadow_catcher(scene, half_size):
    mesh = bpy.data.meshes.new("Panel")
    mesh.from_pydata(
        [(-half_size, -half_size, 0.0), (half_size, -half_size, 0.0), (half_size, half_size, 0.0), (-half_size, half_size, 0.0)],
        [],
        [(0, 1, 2, 3)],
    )
    panel = bpy.data.objects.new("Panel", mesh)
    panel.is_shadow_catcher = True
    scene.collection.objects.link(panel)


def principled(name):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    return material, material.node_tree.nodes["Principled BSDF"]


def render_to(scene, filename, body_radius):
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, filename)
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    trim_shadow(path, body_radius)
    print("wrote", path)


def trim_shadow(path, body_radius):
    """
    The catcher records the panel's ambient shading as well as the cast shadow, which leaves a
    faint square around the object. Take that floor off and fade the shadow out before the
    image's edge. body_radius is the object's radius as a fraction of half the image.
    """
    image = bpy.data.images.load(path)
    width, height = image.size
    pixels = np.empty(width * height * 4, dtype=np.float32)
    image.pixels.foreach_get(pixels)
    pixels = pixels.reshape(height, width, 4)
    alpha = pixels[..., 3]
    ys, xs = np.mgrid[0:height, 0:width]
    radius = np.hypot((xs + 0.5) / width * 2.0 - 1.0, (ys + 0.5) / height * 2.0 - 1.0)
    floor = float(np.percentile(alpha[radius > 0.98], 60))
    lifted = np.clip((alpha - floor) / (1.0 - floor), 0.0, 1.0)
    fade = np.clip((1.0 - radius) / (1.0 - (body_radius + 0.06)), 0.0, 1.0)
    fade = fade * fade * (3.0 - 2.0 * fade)
    pixels[..., 3] = np.where(radius <= body_radius, alpha, lifted * fade)
    image.pixels.foreach_set(pixels.ravel())
    image.filepath_raw = path
    image.file_format = "PNG"
    image.save()
    bpy.data.images.remove(image)


def spin_profile(profile, segments, radius_at):
    """Revolves (radius, height, material) rows about Z. radius_at(row, angle) can flute a row."""
    verts = []
    for row, (radius, height, _material) in enumerate(profile):
        for step in range(segments):
            angle = 2.0 * math.pi * step / segments
            r = radius_at(row, radius, angle)
            verts.append((r * math.cos(angle), r * math.sin(angle), height))
    faces = []
    materials = []
    for row in range(len(profile) - 1):
        for step in range(segments):
            nxt = (step + 1) % segments
            a = row * segments + step
            b = row * segments + nxt
            c = (row + 1) * segments + nxt
            d = (row + 1) * segments + step
            faces.append((a, d, c, b))
            materials.append(profile[row][2])
    # Close the top with a fan.
    top = len(verts)
    verts.append((0.0, 0.0, profile[0][1]))
    for step in range(segments):
        faces.append((top, step, (step + 1) % segments))
        materials.append(profile[0][2])
    return verts, faces, materials


def add_spun_object(scene, name, profile, segments, radius_at, slots):
    verts, faces, materials = spin_profile(profile, segments, radius_at)
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(verts, [], faces)
    for material in slots:
        mesh.materials.append(material)
    mesh.polygons.foreach_set("material_index", materials)
    mesh.shade_smooth()
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    scene.collection.objects.link(obj)
    return obj


# ------------------------------------------------------------------------- knob


def spun_aluminium():
    """A lathe-turned cap: concentric grooves, so highlights fan out as radial streaks."""
    material, bsdf = principled("SpunAluminium")
    tree = material.node_tree
    bsdf.inputs["Base Color"].default_value = (0.74, 0.74, 0.75, 1.0)
    bsdf.inputs["Metallic"].default_value = 1.0
    bsdf.inputs["Roughness"].default_value = 0.34
    bsdf.inputs["Anisotropic"].default_value = 0.86

    tangent = tree.nodes.new("ShaderNodeTangent")
    tangent.direction_type = "RADIAL"
    tangent.axis = "Z"
    tree.links.new(tangent.outputs["Tangent"], bsdf.inputs["Tangent"])

    # Fine turning marks, and years of fingers: the roughness wanders a little.
    coords = tree.nodes.new("ShaderNodeTexCoord")
    rings = tree.nodes.new("ShaderNodeTexWave")
    rings.wave_type = "RINGS"
    rings.rings_direction = "Z"
    rings.inputs["Scale"].default_value = 46.0
    rings.inputs["Distortion"].default_value = 0.6
    rings.inputs["Detail"].default_value = 2.0
    tree.links.new(coords.outputs["Object"], rings.inputs["Vector"])
    smudge = tree.nodes.new("ShaderNodeTexNoise")
    smudge.inputs["Scale"].default_value = 2.6
    smudge.inputs["Detail"].default_value = 3.0
    tree.links.new(coords.outputs["Object"], smudge.inputs["Vector"])

    rough = tree.nodes.new("ShaderNodeMapRange")
    rough.inputs["To Min"].default_value = 0.27
    rough.inputs["To Max"].default_value = 0.46
    tree.links.new(smudge.outputs["Fac"], rough.inputs["Value"])
    tree.links.new(rough.outputs["Result"], bsdf.inputs["Roughness"])

    bump = tree.nodes.new("ShaderNodeBump")
    bump.inputs["Strength"].default_value = 0.05
    tree.links.new(rings.outputs["Fac"], bump.inputs["Height"])
    tree.links.new(bump.outputs["Normal"], bsdf.inputs["Normal"])
    return material


def moulded_plastic():
    """The black body: semi-gloss, handled, with a little dust in its sheen."""
    material, bsdf = principled("MouldedPlastic")
    tree = material.node_tree
    bsdf.inputs["Base Color"].default_value = (0.03, 0.029, 0.03, 1.0)
    bsdf.inputs["Specular IOR Level"].default_value = 0.6
    coords = tree.nodes.new("ShaderNodeTexCoord")
    noise = tree.nodes.new("ShaderNodeTexNoise")
    noise.inputs["Scale"].default_value = 5.0
    noise.inputs["Detail"].default_value = 6.0
    tree.links.new(coords.outputs["Object"], noise.inputs["Vector"])
    rough = tree.nodes.new("ShaderNodeMapRange")
    rough.inputs["To Min"].default_value = 0.24
    rough.inputs["To Max"].default_value = 0.42
    tree.links.new(noise.outputs["Fac"], rough.inputs["Value"])
    tree.links.new(rough.outputs["Result"], bsdf.inputs["Roughness"])
    return material


def render_knob():
    scene = reset_scene(320, 384)
    add_camera(scene, 2.6)
    add_light_rig(scene)
    add_shadow_catcher(scene, 4.0)

    flutes = 34
    # (radius, height, material slot), from the cap's rim down to the panel. Slot 0 is the
    # aluminium cap, slot 1 the plastic body.
    profile = [
        (0.58, 0.560, 0),
        (0.62, 0.548, 0),
        (0.635, 0.520, 0),
        (0.64, 0.500, 1),
        (0.70, 0.492, 1),
        (0.86, 0.430, 1),
        (0.915, 0.385, 1),
        (0.945, 0.320, 1),
        (0.975, 0.160, 1),
        (1.000, 0.000, 1),
    ]
    first_fluted_row = 6

    def radius_at(row, radius, angle):
        if row < first_fluted_row:
            return radius
        depth = 0.03 * min(1.0, (row - first_fluted_row + 1) / 2.0)
        # Rounded grips with narrow valleys between them.
        ridge = abs(math.cos(0.5 * flutes * angle)) ** 0.7
        return radius * (1.0 - depth * (1.0 - ridge))

    add_spun_object(scene, "Knob", profile, 360, radius_at, [spun_aluminium(), moulded_plastic()])
    render_to(scene, "knob.png", 1.0 / 1.3)


# ----------------------------------------------------------------------- screws


def plated_steel():
    material, bsdf = principled("PlatedSteel")
    tree = material.node_tree
    bsdf.inputs["Metallic"].default_value = 1.0
    coords = tree.nodes.new("ShaderNodeTexCoord")
    noise = tree.nodes.new("ShaderNodeTexNoise")
    noise.inputs["Scale"].default_value = 3.0
    noise.inputs["Detail"].default_value = 8.0
    tree.links.new(coords.outputs["Object"], noise.inputs["Vector"])
    # Bright zinc, dulled and slightly brown where it has tarnished.
    ramp = tree.nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].position = 0.25
    ramp.color_ramp.elements[0].color = (0.50, 0.47, 0.42, 1.0)
    ramp.color_ramp.elements[1].position = 0.7
    ramp.color_ramp.elements[1].color = (0.72, 0.72, 0.72, 1.0)
    tree.links.new(noise.outputs["Fac"], ramp.inputs["Fac"])
    tree.links.new(ramp.outputs["Color"], bsdf.inputs["Base Color"])
    rough = tree.nodes.new("ShaderNodeMapRange")
    rough.inputs["To Min"].default_value = 0.42
    rough.inputs["To Max"].default_value = 0.24
    tree.links.new(noise.outputs["Fac"], rough.inputs["Value"])
    tree.links.new(rough.outputs["Result"], bsdf.inputs["Roughness"])
    return material


def render_screw(filename, socket_angle):
    scene = reset_scene(96, 384)
    add_camera(scene, 2.9)
    add_light_rig(scene)
    add_shadow_catcher(scene, 4.0)

    # A button head with a hex socket, from the socket's floor up its walls and out over the dome.
    profile = [
        (0.40, 0.10, 1),
        (0.40, 0.335, 1),
        (0.44, 0.35, 0),
        (0.62, 0.325, 0),
        (0.84, 0.25, 0),
        (0.96, 0.14, 0),
        (1.0, 0.0, 0),
    ]
    hex_rows = 3

    def radius_at(row, radius, angle):
        if row >= hex_rows:
            return radius
        # A regular hexagon whose flats sit at `radius`.
        sector = (angle - socket_angle) % (math.pi / 3) - math.pi / 6
        return radius / math.cos(sector)

    socket, socket_bsdf = principled("Socket")
    socket_bsdf.inputs["Base Color"].default_value = (0.05, 0.045, 0.04, 1.0)
    socket_bsdf.inputs["Metallic"].default_value = 1.0
    socket_bsdf.inputs["Roughness"].default_value = 0.6
    head = add_spun_object(scene, "Head", profile, 144, radius_at, [plated_steel(), socket])
    head.data.set_sharp_from_angle(angle=math.radians(50.0))
    render_to(scene, filename, 1.0 / 1.45)


def render_screws():
    # Two socket angles, so a panel's four screws were plainly not all driven home alike.
    render_screw("screw-a.png", math.radians(11.0))
    render_screw("screw-b.png", math.radians(38.0))


# --------------------------------------------------------------------- textures


def blur_wrap(field, sigma_y, sigma_x):
    """Gaussian blur by FFT, so the result tiles."""
    fy = np.fft.fftfreq(field.shape[0])[:, None]
    fx = np.fft.fftfreq(field.shape[1])[None, :]
    kernel = np.exp(-2.0 * (math.pi**2) * ((sigma_y * fy) ** 2 + (sigma_x * fx) ** 2))
    return np.real(np.fft.ifft2(np.fft.fft2(field) * kernel))


def unit(field):
    return (field - field.mean()) / (field.std() + 1e-9)


def save_image(filename, rgba, jpeg_quality=None):
    """rgba: float array (height, width, 4), sRGB values in 0..1, row 0 at the top."""
    height, width, _ = rgba.shape
    image = bpy.data.images.new(filename, width, height, alpha=True)
    image.colorspace_settings.name = "sRGB"
    # Blender stores rows bottom-up. An 8-bit image keeps display (sRGB) values as they are.
    pixels = np.flipud(np.clip(rgba, 0.0, 1.0)).astype(np.float32)
    image.pixels.foreach_set(pixels.ravel())

    scene = bpy.context.scene
    scene.view_settings.view_transform = "Standard"
    settings = scene.render.image_settings
    if hasattr(settings, "media_type"):
        settings.media_type = "IMAGE"
    if jpeg_quality is None:
        settings.file_format = "PNG"
        settings.color_mode = "RGBA"
        settings.compression = 100
    else:
        settings.file_format = "JPEG"
        settings.color_mode = "RGB"
        settings.quality = jpeg_quality
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, filename)
    image.save_render(path, scene=scene)
    bpy.data.images.remove(image)
    print("wrote", path)


def opaque(rgb):
    return np.concatenate([rgb, np.ones(rgb.shape[:2] + (1,))], axis=2)


def make_panel(rng):
    """Powder-coated steel in a warm putty: orange-peel grain lit from above, faint mottling."""
    size = 384
    grain = unit(blur_wrap(rng.standard_normal((size, size)), 0.9, 0.9))
    # Light from the top: a bump's upper slope is brighter than its lower one.
    relief = unit(np.roll(grain, 1, axis=0) - np.roll(grain, -1, axis=0))
    mottle = unit(blur_wrap(rng.standard_normal((size, size)), 26.0, 26.0))
    speck = unit(blur_wrap(rng.standard_normal((size, size)), 0.6, 0.6))
    shade = 1.0 + 0.02 * relief + 0.004 * mottle + 0.007 * speck
    base = np.array([0.835, 0.812, 0.752])
    rgb = base[None, None, :] * shade[..., None]
    # Age: the mottling leans slightly yellow where it is darker.
    rgb[..., 2] *= 1.0 - 0.003 * np.clip(-mottle, 0.0, None)
    save_image("panel.jpg", opaque(rgb), jpeg_quality=90)


def make_brushed(rng):
    """Brushed aluminium, grain running left to right."""
    height, width = 192, 512
    noise = rng.standard_normal((height, width))
    fine = unit(blur_wrap(noise, 0.35, 42.0))
    coarse = unit(blur_wrap(rng.standard_normal((height, width)), 1.4, 150.0))
    cloud = unit(blur_wrap(rng.standard_normal((height, width)), 30.0, 90.0))
    shade = 1.0 + 0.034 * fine + 0.02 * coarse + 0.012 * cloud
    base = np.array([0.795, 0.800, 0.805])
    save_image("brushed.jpg", opaque(base[None, None, :] * shade[..., None]), jpeg_quality=88)


def splat(layer, y, x, weight):
    """Adds weight at fractional positions, bilinear, wrapping at the edges."""
    height, width = layer.shape
    y0 = np.floor(y).astype(int)
    x0 = np.floor(x).astype(int)
    fy = y - y0
    fx = x - x0
    for dy, wy in ((0, 1.0 - fy), (1, fy)):
        for dx, wx in ((0, 1.0 - fx), (1, fx)):
            np.add.at(layer, ((y0 + dy) % height, (x0 + dx) % width), weight * wy * wx)


def make_wear(rng):
    """Years of use, as a transparent overlay: hairline scratches, scuffs, dust and grime."""
    size = 640
    light = np.zeros((size, size))  # bare metal showing through, bright
    dark = np.zeros((size, size))  # dirt and dull rub marks

    def stroke(layer, length, weight, angle, curve):
        steps = max(2, int(length * 2))
        t = np.linspace(0.0, 1.0, steps)
        x0, y0 = rng.uniform(0, size, 2)
        heading = angle + curve * (t - 0.5)
        x = x0 + np.cumsum(np.cos(heading)) * 0.5
        y = y0 + np.cumsum(np.sin(heading)) * 0.5
        # Scratches start and stop softly, and skip.
        envelope = np.sin(math.pi * t) ** 0.6 * (0.55 + 0.45 * rng.uniform(0.0, 1.0, steps).cumsum() % 1.0)
        splat(layer, y, x, weight * envelope * 0.5)

    # Long hairlines, mostly along the direction things get slid across a desk.
    for _ in range(26):
        stroke(light, rng.uniform(30, 190), rng.uniform(0.2, 0.8), rng.normal(0.0, 0.3), rng.normal(0.0, 0.12))
    for _ in range(16):
        stroke(dark, rng.uniform(20, 120), rng.uniform(0.25, 0.8), rng.uniform(0.0, math.pi), rng.normal(0.0, 0.2))
    # Scuffs: tight bundles of short parallel strokes.
    for _ in range(9):
        cx, cy = rng.uniform(0, size, 2)
        angle = rng.uniform(0.0, math.pi)
        layer = light if rng.uniform() < 0.6 else dark
        for _ in range(int(rng.uniform(8, 22))):
            steps = int(rng.uniform(16, 60))
            t = np.linspace(0.0, 1.0, steps)
            ox, oy = rng.normal(0.0, 7.0, 2)
            x = cx + ox + np.cos(angle) * (t - 0.5) * steps * 0.5
            y = cy + oy + np.sin(angle) * (t - 0.5) * steps * 0.5
            splat(layer, y, x, rng.uniform(0.15, 0.5) * np.sin(math.pi * t) * 0.5)
    # Nicks and dust.
    count = 380
    splat(light, rng.uniform(0, size, count), rng.uniform(0, size, count), rng.uniform(0.2, 1.0, count) * (rng.uniform(size=count) < 0.35))
    splat(dark, rng.uniform(0, size, count), rng.uniform(0, size, count), rng.uniform(0.2, 0.9, count))

    light = blur_wrap(light, 0.45, 0.45)
    dark = blur_wrap(dark, 0.6, 0.6)
    # Grime: broad, faint clouds of dirt.
    grime = np.clip(unit(blur_wrap(rng.standard_normal((size, size)), 34.0, 34.0)) - 0.55, 0.0, None)
    grime = grime / (grime.max() + 1e-9)

    light_alpha = np.clip(light * 0.36, 0.0, 0.5)
    dark_alpha = np.clip(dark * 0.17 + grime * 0.05, 0.0, 0.3)
    alpha = np.clip(light_alpha + dark_alpha, 0.0, 1.0)
    # Two flat colours rather than a blend: at these alphas it looks the same and the PNG is
    # a third of the size.
    alpha = np.where(alpha < 0.01, 0.0, alpha)
    bright = np.array([0.97, 0.96, 0.93])
    dirt = np.array([0.20, 0.17, 0.13])
    rgb = np.where((light_alpha > dark_alpha)[..., None], bright[None, None, :], dirt[None, None, :])
    save_image("wear.png", np.concatenate([rgb, alpha[..., None]], axis=2))


def make_textures():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    rng = np.random.default_rng(1987)
    make_panel(rng)
    make_brushed(rng)
    make_wear(rng)


# ------------------------------------------------------------------------- main

JOBS = {"knob": render_knob, "screws": render_screws, "textures": make_textures}

if __name__ == "__main__":
    names = sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else []
    unknown = [name for name in names if name not in JOBS]
    if unknown:
        raise SystemExit(f"unknown job(s): {', '.join(unknown)}; expected {', '.join(JOBS)}")
    for name in names or JOBS:
        JOBS[name]()
