"""
Renders the Studio 1973 theme's images: the knob and the play page's backdrop (a 1973
recording-studio control room, softly out of focus).

Run it with Blender (5.x), headless, from the repo root:

    blender -b --factory-startup -P tools/studio-1973-theme-assets/studio_1973.py -- [knob] [room]

With no names after "--" it makes everything. "room preview <file>" makes a quick, small,
noisy backdrop to judge framing and light. Output goes to core/ui/images/themes/studio-1973/.

Studio 1973 is the wood-and-aluminium theme: walnut veneer, machined aluminium, the amber of
backlit meters and burnt orange. The knob is a solid turned aluminium hi-fi knob (spun face,
polished chamfer, fine fluting round its sides), rendered top-down with studio_2003.py's scene
helpers and light, framed like Studio's knob (1.3 diameters across) so the stylesheet can size
it the same way; the stylesheet draws the pointer. The backdrop is path traced with a shallow
depth of field, kept dim so the UI's text reads over it, with its light low and to the sides.
"""

import math
import os
import sys

import bpy

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "studio-theme-assets"))
import studio_2003 as base  # noqa: E402  (needs the path above)

OUT = os.path.join(base.REPO, "core", "ui", "images", "themes", "studio-1973")

TUNGSTEN = (1.0, 0.62, 0.30)
AMBER = (1.0, 0.55, 0.16)


def node(tree, kind, **inputs):
    created = tree.nodes.new(kind)
    for name, value in inputs.items():
        created.inputs[name].default_value = value
    return created


def aim(obj, target):
    from mathutils import Vector

    direction = Vector(target) - Vector(obj.location)
    obj.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()


# ------------------------------------------------------------------------- knob


def turned_aluminium(name, colour, roughness, anisotropic, ring_scale, ring_bump):
    """Turned on a lathe: concentric tool marks, so the light fans out in radial streaks."""
    material, bsdf = base.principled(name)
    tree = material.node_tree
    links = tree.links
    bsdf.inputs["Base Color"].default_value = (*colour, 1.0)
    bsdf.inputs["Metallic"].default_value = 1.0
    bsdf.inputs["Roughness"].default_value = roughness
    bsdf.inputs["Anisotropic"].default_value = anisotropic
    tangent = node(tree, "ShaderNodeTangent")
    tangent.direction_type = "RADIAL"
    tangent.axis = "Z"
    links.new(tangent.outputs["Tangent"], bsdf.inputs["Tangent"])
    coords = node(tree, "ShaderNodeTexCoord")
    if ring_bump > 0.0:
        rings = node(tree, "ShaderNodeTexWave", Scale=ring_scale, Distortion=0.5, Detail=2.0)
        rings.wave_type = "RINGS"
        rings.rings_direction = "Z"
        links.new(coords.outputs["Object"], rings.inputs["Vector"])
        bump = node(tree, "ShaderNodeBump", Strength=ring_bump)
        links.new(rings.outputs["Fac"], bump.inputs["Height"])
        links.new(bump.outputs["Normal"], bsdf.inputs["Normal"])
    # The finish is not quite even: the roughness wanders a little.
    smudge = node(tree, "ShaderNodeTexNoise", Scale=3.0, Detail=3.0)
    links.new(coords.outputs["Object"], smudge.inputs["Vector"])
    rough = node(tree, "ShaderNodeMapRange")
    rough.inputs["To Min"].default_value = roughness * 0.8
    rough.inputs["To Max"].default_value = roughness * 1.3
    links.new(smudge.outputs["Fac"], rough.inputs["Value"])
    links.new(rough.outputs["Result"], bsdf.inputs["Roughness"])
    return material


def render_knob():
    scene = base.reset_scene(320, 384)
    world = scene.world.node_tree.nodes["Background"]
    # A darker, warmer room than the other knobs' studio: the metal's reflections go dark, so
    # its highlights stand out on a walnut panel.
    world.inputs["Color"].default_value = (1.0, 0.93, 0.84, 1.0)
    world.inputs["Strength"].default_value = 0.27
    base.add_camera(scene, 2.6)
    base.add_light_rig(scene)
    # Low lights that the chamfer catches as a crisp highlight (top left) and a dimmer one
    # (bottom right). They cast no shadow, so the soft shadow is still the rig's alone.
    for name, azimuth, elevation, watts in (("Glint", 118.0, 12.0, 520.0), ("Counter", 300.0, 14.0, 160.0)):
        base.add_area_light(scene, name, math.radians(azimuth), math.radians(elevation), 9.0, 3.0, watts, (1.0, 0.95, 0.88))
        bpy.data.objects[name].data.use_shadow = False
    base.add_shadow_catcher(scene, 4.0)
    # The panel shows in the knob's sides as it would for real: dark walnut, not white.
    panel, panel_bsdf = base.principled("Walnut")
    panel_bsdf.inputs["Base Color"].default_value = (0.035, 0.016, 0.008, 1.0)
    panel_bsdf.inputs["Roughness"].default_value = 0.4
    bpy.data.objects["Panel"].data.materials.append(panel)

    # (radius, height, material slot) from the centre of the face down to the panel. Slot 0 is
    # the spun face, 1 the polished chamfer, 2 the fluted sides, 3 the dark gap at the foot.
    # The face is dished a little; the sides taper out a touch so the flutes show from above.
    profile = [
        (0.02, 0.600, 0),
        (0.20, 0.603, 0),
        (0.40, 0.611, 0),
        (0.56, 0.622, 0),
        (0.66, 0.632, 0),
        (0.700, 0.636, 0),
        (0.706, 0.635, 1),
        (0.736, 0.614, 1),
        (0.766, 0.590, 1),
        (0.790, 0.570, 1),
        (0.800, 0.558, 2),
        (0.840, 0.470, 2),
        (0.895, 0.310, 2),
        (0.945, 0.150, 2),
        (0.962, 0.105, 2),
        (0.962, 0.090, 3),
        (0.985, 0.045, 3),
        (1.000, 0.000, 3),
    ]
    fluted_rows = range(10, 15)
    flutes = 72

    def radius_at(row, radius, angle):
        if row not in fluted_rows:
            return radius
        # Rounded ridges with narrow valleys, like a knurled grip cut on a lathe.
        ridge = abs(math.cos(0.5 * flutes * angle)) ** 0.5
        return radius * (1.0 - 0.024 * (1.0 - ridge))

    warm_silver = (0.86, 0.83, 0.78)
    face = turned_aluminium("Face", warm_silver, 0.16, 0.95, 70.0, 0.035)
    chamfer = turned_aluminium("Chamfer", (0.93, 0.9, 0.86), 0.05, 0.3, 0.0, 0.0)
    sides = turned_aluminium("Sides", (0.82, 0.79, 0.74), 0.38, 0.0, 0.0, 0.0)
    foot, foot_bsdf = base.principled("Foot")
    foot_bsdf.inputs["Base Color"].default_value = (0.02, 0.017, 0.014, 1.0)
    foot_bsdf.inputs["Roughness"].default_value = 0.55
    knob = base.add_spun_object(scene, "Knob", profile, 864, radius_at, [face, chamfer, sides, foot])
    knob.data.set_sharp_from_angle(angle=math.radians(30.0))
    base.render_to(scene, "knob.png", 1.0 / 1.3, out=OUT)


# ------------------------------------------------------------------------- room

ROOM_WIDTH = 2400
ROOM_HEIGHT = 1350
ROOM_SAMPLES = 96

# Linear colour ramps for the woods, darkest grain to lightest.
WALNUT = [(0.0, (0.022, 0.009, 0.004)), (0.42, (0.075, 0.032, 0.013)), (0.75, (0.13, 0.058, 0.024)), (1.0, (0.19, 0.088, 0.037))]
TEAK = [(0.0, (0.040, 0.016, 0.006)), (0.40, (0.12, 0.050, 0.017)), (0.78, (0.21, 0.095, 0.034)), (1.0, (0.28, 0.13, 0.048))]

# The console, in metres: the armrest's front edge is at y = 0, the faceplate rises from the
# armrest to the meter bridge at the slope below.
CONSOLE_HALF = 3.1
FACE_Y0, FACE_Z0 = 0.30, 0.82
FACE_Y1, FACE_Z1 = 1.10, 1.03
BRIDGE_Y1, BRIDGE_Z1 = 1.40, 1.36
WALL_Y = 6.0
SIDE_X = 3.7
CEILING_Z = 3.0


def wood(name, palette, scale, plank=None, roughness=0.35, coat=0.0, ring_scale=1.6):
    """
    Procedural wood: stretched spherical rings (so a flat cut shows flat-sawn arches), warped,
    with fine pores. plank = (axis, width, groove): V-groove boards side by side across that
    axis, each with its own figure and a little colour of its own. Coordinates are object
    space, in metres, so build meshes in metres rather than scaling them.
    """
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    tree = material.node_tree
    links = tree.links
    bsdf = tree.nodes["Principled BSDF"]
    coords = node(tree, "ShaderNodeTexCoord")
    vector = coords.outputs["Object"]
    shade = None
    groove = None
    if plank:
        axis, width, groove_width = plank
        sep = node(tree, "ShaderNodeSeparateXYZ")
        links.new(vector, sep.inputs["Vector"])
        across = tree.nodes.new("ShaderNodeMath")
        across.operation = "DIVIDE"
        links.new(sep.outputs[axis], across.inputs[0])
        across.inputs[1].default_value = width
        index = tree.nodes.new("ShaderNodeMath")
        index.operation = "FLOOR"
        links.new(across.outputs[0], index.inputs[0])
        frac = tree.nodes.new("ShaderNodeMath")
        frac.operation = "FRACT"
        links.new(across.outputs[0], frac.inputs[0])
        # Distance to the nearest groove, 0 at the groove and 0.5 mid-board.
        flip = tree.nodes.new("ShaderNodeMath")
        flip.operation = "SUBTRACT"
        flip.inputs[0].default_value = 1.0
        links.new(frac.outputs[0], flip.inputs[1])
        edge = tree.nodes.new("ShaderNodeMath")
        edge.operation = "MINIMUM"
        links.new(frac.outputs[0], edge.inputs[0])
        links.new(flip.outputs[0], edge.inputs[1])
        groove = node(tree, "ShaderNodeMapRange")
        groove.inputs["From Min"].default_value = 0.0
        groove.inputs["From Max"].default_value = groove_width / width
        links.new(edge.outputs[0], groove.inputs["Value"])
        # Each board its own random offset into the log, and its own shade.
        noise = tree.nodes.new("ShaderNodeTexWhiteNoise")
        noise.noise_dimensions = "1D"
        links.new(index.outputs[0], noise.inputs["W"])
        offset = tree.nodes.new("ShaderNodeVectorMath")
        offset.operation = "SCALE"
        links.new(noise.outputs["Color"], offset.inputs[0])
        offset.inputs["Scale"].default_value = 9.0
        moved = tree.nodes.new("ShaderNodeVectorMath")
        moved.operation = "ADD"
        links.new(vector, moved.inputs[0])
        links.new(offset.outputs[0], moved.inputs[1])
        vector = moved.outputs[0]
        shade = node(tree, "ShaderNodeMapRange")
        shade.inputs["To Min"].default_value = 0.75
        shade.inputs["To Max"].default_value = 1.2
        links.new(noise.outputs["Value"], shade.inputs["Value"])

    mapping = node(tree, "ShaderNodeMapping")
    mapping.inputs["Scale"].default_value = scale
    mapping.inputs["Location"].default_value = (3.1, 1.7, 2.3)
    links.new(vector, mapping.inputs["Vector"])
    rings = node(tree, "ShaderNodeTexWave", Scale=ring_scale, Distortion=5.0, Detail=4.0)
    rings.inputs["Detail Scale"].default_value = 0.7
    rings.inputs["Detail Roughness"].default_value = 0.6
    rings.wave_type = "RINGS"
    rings.rings_direction = "SPHERICAL"
    links.new(mapping.outputs["Vector"], rings.inputs["Vector"])
    pores = node(tree, "ShaderNodeTexNoise", Scale=40.0, Detail=2.0)
    links.new(mapping.outputs["Vector"], pores.inputs["Vector"])
    figure = tree.nodes.new("ShaderNodeMath")
    figure.operation = "MULTIPLY_ADD"
    links.new(pores.outputs["Fac"], figure.inputs[0])
    figure.inputs[1].default_value = 0.3
    links.new(rings.outputs["Fac"], figure.inputs[2])
    level = tree.nodes.new("ShaderNodeMath")
    level.operation = "SUBTRACT"
    links.new(figure.outputs[0], level.inputs[0])
    level.inputs[1].default_value = 0.15
    ramp = tree.nodes.new("ShaderNodeValToRGB")
    elements = ramp.color_ramp.elements
    elements[0].position, elements[0].color = palette[0][0], (*palette[0][1], 1.0)
    elements[1].position, elements[1].color = palette[-1][0], (*palette[-1][1], 1.0)
    for position, colour in palette[1:-1]:
        elements.new(position).color = (*colour, 1.0)
    links.new(level.outputs[0], ramp.inputs["Fac"])
    colour = ramp.outputs["Color"]

    if plank:
        # The groove is in shadow; the board's own shade on top.
        dark = node(tree, "ShaderNodeMapRange")
        dark.inputs["To Min"].default_value = 0.04
        links.new(groove.outputs["Result"], dark.inputs["Value"])
        factor = tree.nodes.new("ShaderNodeMath")
        factor.operation = "MULTIPLY"
        links.new(dark.outputs["Result"], factor.inputs[0])
        links.new(shade.outputs["Result"], factor.inputs[1])
        scaled = tree.nodes.new("ShaderNodeVectorMath")
        scaled.operation = "SCALE"
        links.new(colour, scaled.inputs[0])
        links.new(factor.outputs[0], scaled.inputs["Scale"])
        colour = scaled.outputs[0]
        bump = node(tree, "ShaderNodeBump", Strength=0.8, Distance=0.004)
        links.new(groove.outputs["Result"], bump.inputs["Height"])
        links.new(bump.outputs["Normal"], bsdf.inputs["Normal"])
    links.new(colour, bsdf.inputs["Base Color"])

    rough = node(tree, "ShaderNodeMapRange")
    rough.inputs["To Min"].default_value = roughness * 0.8
    rough.inputs["To Max"].default_value = roughness * 1.25
    links.new(pores.outputs["Fac"], rough.inputs["Value"])
    links.new(rough.outputs["Result"], bsdf.inputs["Roughness"])
    if coat > 0.0:
        bsdf.inputs["Coat Weight"].default_value = coat
        bsdf.inputs["Coat Roughness"].default_value = 0.08
    return material


def matte(name, colour, roughness, metallic=0.0):
    material, bsdf = base.principled(name)
    bsdf.inputs["Base Color"].default_value = (*colour, 1.0)
    bsdf.inputs["Roughness"].default_value = roughness
    bsdf.inputs["Metallic"].default_value = metallic
    return material


def glow(name, colour, strength):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    tree = material.node_tree
    tree.nodes.remove(tree.nodes["Principled BSDF"])
    emit = node(tree, "ShaderNodeEmission", Strength=strength)
    emit.inputs["Color"].default_value = (*colour, 1.0)
    tree.links.new(emit.outputs["Emission"], tree.nodes["Material Output"].inputs["Surface"])
    return material


def meter_glow(name, strength):
    """
    A backlit meter face: cream and hottest low in the middle, where the lamps sit behind the
    scale, deepening to amber at its edges.
    """
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    tree = material.node_tree
    links = tree.links
    tree.nodes.remove(tree.nodes["Principled BSDF"])
    coords = node(tree, "ShaderNodeTexCoord")
    # Generated coordinates run 0..1 across the face (x) and up it (z); centre the hot spot
    # a quarter of the way up.
    mapping = node(tree, "ShaderNodeMapping")
    mapping.inputs["Scale"].default_value = (1.5, 0.0, 1.2)
    mapping.inputs["Location"].default_value = (-0.75, 0.0, -0.36)
    links.new(coords.outputs["Generated"], mapping.inputs["Vector"])
    spot = tree.nodes.new("ShaderNodeTexGradient")
    spot.gradient_type = "QUADRATIC_SPHERE"
    links.new(mapping.outputs["Vector"], spot.inputs["Vector"])
    ramp = tree.nodes.new("ShaderNodeValToRGB")
    elements = ramp.color_ramp.elements
    elements[0].position, elements[0].color = 0.0, (1.0, 0.30, 0.04, 1.0)
    elements[1].position, elements[1].color = 0.85, (1.0, 0.74, 0.42, 1.0)
    elements.new(0.4).color = (1.0, 0.52, 0.16, 1.0)
    links.new(spot.outputs["Fac"], ramp.inputs["Fac"])
    level = node(tree, "ShaderNodeMapRange")
    level.inputs["To Min"].default_value = 0.25 * strength
    level.inputs["To Max"].default_value = 1.3 * strength
    links.new(spot.outputs["Fac"], level.inputs["Value"])
    emit = tree.nodes.new("ShaderNodeEmission")
    links.new(ramp.outputs["Color"], emit.inputs["Color"])
    links.new(level.outputs["Result"], emit.inputs["Strength"])
    links.new(emit.outputs["Emission"], tree.nodes["Material Output"].inputs["Surface"])
    return material


def add_mesh(scene, name, verts, faces, material, bevel=0.0):
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(verts, [], faces)
    mesh.materials.append(material)
    obj = bpy.data.objects.new(name, mesh)
    scene.collection.objects.link(obj)
    if bevel > 0.0:
        modifier = obj.modifiers.new("Bevel", "BEVEL")
        modifier.width = bevel
        modifier.segments = 3
    return obj


def add_box(scene, name, x0, x1, y0, y1, z0, z1, material, bevel=0.0):
    verts = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0), (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]
    faces = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    return add_mesh(scene, name, verts, faces, material, bevel)


def add_prism_x(scene, name, x0, x1, outline, material, bevel=0.0):
    """Extrudes an (y, z) outline, anticlockwise seen from +X, from x0 to x1."""
    n = len(outline)
    verts = [(x0, y, z) for y, z in outline] + [(x1, y, z) for y, z in outline]
    faces = [tuple(reversed(range(n))), tuple(range(n, 2 * n))]
    for i in range(n):
        j = (i + 1) % n
        faces.append((i, j, n + j, n + i))
    return add_mesh(scene, name, verts, faces, material, bevel)


def add_quad(scene, name, corners, material):
    return add_mesh(scene, name, corners, [(0, 1, 2, 3)], material)


def face_point(x, v, lift=0.0):
    """A point on the sloping faceplate: v metres up the slope from its front edge."""
    slope = math.atan2(FACE_Z1 - FACE_Z0, FACE_Y1 - FACE_Y0)
    return (x, FACE_Y0 + v * math.cos(slope) - lift * math.sin(slope), FACE_Z0 + v * math.sin(slope) + lift * math.cos(slope))


def add_row(scene, name, material, radius, height, v, pitch, count, x0, segments=20):
    """A row of knob caps (or lamps) standing on the faceplate, by array modifier."""
    slope = math.atan2(FACE_Z1 - FACE_Z0, FACE_Y1 - FACE_Y0)
    bpy.ops.mesh.primitive_cylinder_add(vertices=segments, radius=radius, depth=height, location=face_point(x0, v, height / 2))
    obj = bpy.context.active_object
    obj.name = name
    obj.rotation_euler = (-slope, 0.0, 0.0)
    obj.data.materials.append(material)
    bpy.ops.object.shade_smooth()
    array = obj.modifiers.new("Row", "ARRAY")
    array.count = count
    array.use_relative_offset = False
    array.use_constant_offset = True
    array.constant_offset_displace = (pitch, 0.0, 0.0)
    return obj


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


def build_console(scene):
    walnut_gloss = wood("ConsoleWalnut", WALNUT, (0.35, 7.0, 7.0), roughness=0.3, coat=0.6)
    walnut_front = wood("ConsoleFront", WALNUT, (5.0, 5.0, 0.35), ("X", 0.16, 0.004), roughness=0.4)
    faceplate = matte("Faceplate", (0.022, 0.014, 0.009), 0.4, 0.25)
    h = CONSOLE_HALF

    # The padded armrest's wooden rail, the front of the console under it, the faceplate and
    # the meter bridge, between two wooden cheeks.
    add_box(scene, "Armrest", -h, h, 0.0, FACE_Y0, 0.74, FACE_Z0 + 0.02, walnut_gloss, bevel=0.025)
    add_box(scene, "Front", -h, h, 0.04, 0.30, 0.0, 0.74, walnut_front)
    add_quad(
        scene,
        "Faceplate",
        [(-h, FACE_Y0, FACE_Z0), (h, FACE_Y0, FACE_Z0), (h, FACE_Y1, FACE_Z1), (-h, FACE_Y1, FACE_Z1)],
        faceplate,
    )
    add_box(scene, "Bridge", -h, h, FACE_Y1, BRIDGE_Y1, FACE_Z1 - 0.02, BRIDGE_Z1, walnut_gloss, bevel=0.012)
    add_box(scene, "BridgeCap", -h - 0.02, h + 0.02, FACE_Y1 - 0.03, BRIDGE_Y1 + 0.02, BRIDGE_Z1, BRIDGE_Z1 + 0.035, walnut_gloss, bevel=0.012)
    add_box(scene, "Back", -h, h, BRIDGE_Y1 - 0.02, BRIDGE_Y1 + 0.02, 0.0, BRIDGE_Z1, walnut_front)
    outline = [(0.0, 0.0), (BRIDGE_Y1 + 0.02, 0.0), (BRIDGE_Y1 + 0.02, BRIDGE_Z1 + 0.06), (FACE_Y1 - 0.06, BRIDGE_Z1 + 0.06), (FACE_Y1 - 0.06, FACE_Z1 + 0.04), (0.0, FACE_Z0 + 0.04)]
    for side in (-1, 1):
        x = side * (h + 0.02)
        add_prism_x(scene, "Cheek", min(x, x + side * 0.07), max(x, x + side * 0.07), outline, walnut_gloss, bevel=0.015)

    # The meters: a dark strip along the bridge with backlit faces in it, each with a needle.
    meter_face = meter_glow("MeterFace", 0.8)
    needle = matte("Needle", (0.02, 0.012, 0.008), 0.5)
    strip_y = FACE_Y1 - 0.002
    add_quad(scene, "MeterStrip", [(-h + 0.05, strip_y, 1.07), (h - 0.05, strip_y, 1.07), (h - 0.05, strip_y, 1.30), (-h + 0.05, strip_y, 1.30)], faceplate)
    count = 16
    for i in range(count):
        cx = -h + 0.3 + i * (2 * h - 0.6) / (count - 1)
        y = strip_y - 0.003
        add_quad(scene, "Meter", [(cx - 0.1, y, 1.12), (cx + 0.1, y, 1.12), (cx + 0.1, y, 1.25), (cx - 0.1, y, 1.25)], meter_face)
        tilt = math.radians(-35.0 + 70.0 * ((i * 0.618) % 1.0) * 0.6 + (i % 3) * 6.0)
        px, pz = cx + 0.085 * math.sin(tilt), 1.115 + 0.085 * math.cos(tilt)
        dx, dz = 0.0015 * math.cos(tilt), -0.0015 * math.sin(tilt)
        ny = y - 0.004
        add_quad(scene, "Needle", [(cx - dx, ny, 1.115 - dz), (cx + dx, ny, 1.115 + dz), (px + dx, ny, pz + dz), (px - dx, ny, pz - dz)], needle)

    # Knob caps, rows of them, a few in burnt orange and cream; fader caps at the front; and a
    # scatter of small lamps.
    aluminium = matte("CapAluminium", (0.8, 0.77, 0.72), 0.25, 1.0)
    orange = matte("CapOrange", (0.55, 0.13, 0.025), 0.3)
    cream = matte("CapCream", (0.62, 0.52, 0.38), 0.35)
    black = matte("CapBlack", (0.015, 0.013, 0.012), 0.3)
    channels = 44
    pitch = (2 * h - 0.3) / (channels - 1)
    x0 = -h + 0.15
    for row, (v, material, radius) in enumerate(
        [(0.30, black, 0.016), (0.40, aluminium, 0.014), (0.49, orange, 0.015), (0.58, aluminium, 0.014), (0.67, cream, 0.015), (0.76, aluminium, 0.014)]
    ):
        add_row(scene, f"Caps{row}", material, radius, 0.026, v, pitch, channels, x0)
    add_row(scene, "Faders", cream, 0.012, 0.03, 0.12, pitch, channels, x0, 4)
    lamps = [glow("LampAmber", AMBER, 40.0), glow("LampRed", (1.0, 0.2, 0.04), 30.0)]
    for i in range(channels):
        if (i * 7) % 5 < 2:
            bpy.ops.mesh.primitive_uv_sphere_add(radius=0.007, location=face_point(x0 + i * pitch, 0.22, 0.004), segments=12, ring_count=6)
            bpy.context.active_object.data.materials.append(lamps[(i * 3) % 2])


def build_tape_machine(scene, x0, x1, y0, y1):
    cabinet = wood("TapeCabinet", TEAK, (6.0, 0.35, 6.0), roughness=0.35, coat=0.4)
    panel = matte("TapePanel", (0.05, 0.045, 0.04), 0.35, 0.6)
    reel = matte("Reel", (0.75, 0.72, 0.68), 0.22, 1.0)
    hub = matte("Hub", (0.02, 0.02, 0.02), 0.4)
    add_box(scene, "TapeCabinet", x0, x1, y0, y1, 0.0, 1.25, cabinet, bevel=0.01)
    add_box(scene, "TapeDeck", x0 + 0.02, x1 - 0.02, y0 + 0.04, y1 - 0.02, 1.25, 2.0, panel, bevel=0.01)
    face_y = y0 + 0.035
    for i, x in enumerate((x0 + 0.21, x1 - 0.21)):
        bpy.ops.mesh.primitive_cylinder_add(vertices=64, radius=0.17, depth=0.012, location=(x, face_y - 0.01, 1.76))
        disc = bpy.context.active_object
        disc.rotation_euler = (math.radians(90.0), 0.0, 0.0)
        disc.data.materials.append(reel)
        bpy.ops.mesh.primitive_cylinder_add(vertices=32, radius=0.07 if i else 0.11, depth=0.02, location=(x, face_y - 0.017, 1.76))
        tape = bpy.context.active_object
        tape.rotation_euler = (math.radians(90.0), 0.0, 0.0)
        tape.data.materials.append(hub)
    meter = meter_glow("TapeMeter", 0.4)
    for x in (x0 + 0.24, x1 - 0.24):
        add_quad(scene, "TapeMeter", [(x - 0.07, face_y - 0.002, 1.40), (x + 0.07, face_y - 0.002, 1.40), (x + 0.07, face_y - 0.002, 1.49), (x - 0.07, face_y - 0.002, 1.49)], meter)


def build_rack(scene, x0, x1, y0, y1):
    cabinet = wood("RackCabinet", TEAK, (6.0, 6.0, 0.35), roughness=0.35, coat=0.4)
    panel = matte("RackPanel", (0.045, 0.04, 0.036), 0.35, 0.6)
    ears = matte("RackEars", (0.75, 0.72, 0.68), 0.3, 1.0)
    add_box(scene, "Rack", x0, x1, y0, y1, 0.0, 1.95, cabinet, bevel=0.01)
    lamp = glow("RackLamp", AMBER, 25.0)
    meter = meter_glow("RackMeter", 0.35)
    z = 0.55
    for unit in range(8):
        height = 0.13 if unit % 3 else 0.2
        y = y0 - 0.005
        add_quad(scene, "RackUnit", [(x0 + 0.04, y, z), (x1 - 0.04, y, z), (x1 - 0.04, y, z + height - 0.01), (x0 + 0.04, y, z + height - 0.01)], panel)
        for edge in (x0 + 0.04, x1 - 0.07):
            add_quad(scene, "RackEar", [(edge, y - 0.002, z), (edge + 0.03, y - 0.002, z), (edge + 0.03, y - 0.002, z + height - 0.01), (edge, y - 0.002, z + height - 0.01)], ears)
        if unit % 3 == 0:
            add_quad(scene, "RackMeter", [(x0 + 0.12, y - 0.003, z + 0.05), (x0 + 0.26, y - 0.003, z + 0.05), (x0 + 0.26, y - 0.003, z + 0.15), (x0 + 0.12, y - 0.003, z + 0.15)], meter)
        else:
            bpy.ops.mesh.primitive_uv_sphere_add(radius=0.008, location=(x1 - 0.14, y - 0.006, z + height / 2), segments=12, ring_count=6)
            bpy.context.active_object.data.materials.append(lamp)
        z += height


def add_sconce(scene, name, x, y, z, facing):
    """A wall light: a short brass-rimmed drum, glowing, throwing light up and down the wall."""
    shade = glow("SconceShade", (1.0, 0.36, 0.08), 0.5)
    rim = matte("Brass", (0.6, 0.4, 0.18), 0.3, 1.0)
    off = 0.09 * facing
    bpy.ops.mesh.primitive_cylinder_add(vertices=32, radius=0.075, depth=0.2, location=(x, y + off, z))
    bpy.context.active_object.data.materials.append(shade)
    bpy.ops.mesh.primitive_cylinder_add(vertices=32, radius=0.08, depth=0.02, location=(x, y + off, z + 0.1))
    bpy.context.active_object.data.materials.append(rim)
    bpy.ops.mesh.primitive_cylinder_add(vertices=32, radius=0.08, depth=0.02, location=(x, y + off, z - 0.1))
    bpy.context.active_object.data.materials.append(rim)
    wall = y + 0.02 * facing
    add_spot(scene, name + "Up", (x, y + 1.5 * off, z + 0.12), (x, wall, z + 2.0), 70.0, 100.0, 1.0, TUNGSTEN, 0.05)
    add_spot(scene, name + "Down", (x, y + 1.5 * off, z - 0.12), (x, wall, z - 2.0), 170.0, 110.0, 1.0, TUNGSTEN, 0.05)


def build_room():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.samples = ROOM_SAMPLES
    scene.cycles.use_adaptive_sampling = False
    scene.cycles.use_denoising = True
    scene.cycles.seed = 1973
    scene.cycles.max_bounces = 4
    scene.render.resolution_x = ROOM_WIDTH
    scene.render.resolution_y = ROOM_HEIGHT
    scene.render.resolution_percentage = 100
    settings = scene.render.image_settings
    if hasattr(settings, "media_type"):
        settings.media_type = "IMAGE"
    settings.file_format = "JPEG"
    settings.color_mode = "RGB"
    settings.quality = 85
    scene.view_settings.view_transform = "AgX"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0

    world = bpy.data.worlds.new("World")
    scene.world = world
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.5, 0.36, 0.24, 1.0)
    world.node_tree.nodes["Background"].inputs["Strength"].default_value = 0.01

    # The room: V-groove panelling on the back and side walls, a dark slatted ceiling and a
    # burnt-orange carpet.
    back = wood("BackPanelling", TEAK, (5.0, 5.0, 0.3), ("X", 0.18, 0.022), roughness=0.42, coat=0.15, ring_scale=4.0)
    side = wood("SidePanelling", TEAK, (5.0, 5.0, 0.3), ("Y", 0.18, 0.022), roughness=0.42, coat=0.15, ring_scale=4.0)
    add_quad(scene, "BackWall", [(-SIDE_X, WALL_Y, 0.0), (SIDE_X, WALL_Y, 0.0), (SIDE_X, WALL_Y, CEILING_Z), (-SIDE_X, WALL_Y, CEILING_Z)], back)
    add_quad(scene, "LeftWall", [(-SIDE_X, -4.0, 0.0), (-SIDE_X, WALL_Y, 0.0), (-SIDE_X, WALL_Y, CEILING_Z), (-SIDE_X, -4.0, CEILING_Z)], side)
    add_quad(scene, "RightWall", [(SIDE_X, WALL_Y, 0.0), (SIDE_X, -4.0, 0.0), (SIDE_X, -4.0, CEILING_Z), (SIDE_X, WALL_Y, CEILING_Z)], side)
    add_quad(scene, "Ceiling", [(-SIDE_X, -4.0, CEILING_Z), (-SIDE_X, WALL_Y, CEILING_Z), (SIDE_X, WALL_Y, CEILING_Z), (SIDE_X, -4.0, CEILING_Z)], matte("Ceiling", (0.008, 0.006, 0.005), 0.9))
    add_quad(scene, "Floor", [(-SIDE_X, -4.0, 0.0), (SIDE_X, -4.0, 0.0), (SIDE_X, WALL_Y, 0.0), (-SIDE_X, WALL_Y, 0.0)], matte("Carpet", (0.12, 0.035, 0.012), 0.95))

    # A walnut rail along the top of the panelling under a dark soffit, and two framed
    # burnt-orange fabric panels (acoustic absorbers) between the wall lights.
    trim = wood("Trim", WALNUT, (0.35, 7.0, 7.0), roughness=0.3, coat=0.5)
    add_box(scene, "Rail", -SIDE_X, SIDE_X, WALL_Y - 0.04, WALL_Y, 2.68, 2.76, trim, bevel=0.01)
    add_box(scene, "Soffit", -SIDE_X, SIDE_X, WALL_Y - 0.45, WALL_Y, 2.76, CEILING_Z, matte("Soffit", (0.012, 0.008, 0.006), 0.8))
    fabric, fabric_bsdf = base.principled("Fabric")
    fabric_bsdf.inputs["Base Color"].default_value = (0.085, 0.02, 0.006, 1.0)
    fabric_bsdf.inputs["Roughness"].default_value = 1.0
    fabric_bsdf.inputs["Sheen Weight"].default_value = 0.4
    for x0, x1 in ((-1.75, -0.2), (0.2, 1.75)):
        add_box(scene, "PanelFrame", x0, x1, WALL_Y - 0.06, WALL_Y, 1.2, 2.45, trim, bevel=0.012)
        add_box(scene, "Panel", x0 + 0.05, x1 - 0.05, WALL_Y - 0.075, WALL_Y - 0.03, 1.25, 2.4, fabric, bevel=0.01)

    build_console(scene)
    build_tape_machine(scene, 2.15, 2.95, 2.7, 3.25)
    build_rack(scene, -2.95, -2.25, 2.7, 3.25)

    # Light: a wall light at each side of the back wall grazing the boards, and two warm
    # ceiling spots on the console's front.
    add_sconce(scene, "SconceL", -3.0, WALL_Y, 1.95, -1)
    add_sconce(scene, "SconceR", 3.0, WALL_Y, 1.95, -1)
    add_spot(scene, "ConsoleL", (-1.9, -0.2, CEILING_Z - 0.05), (-1.9, 0.3, 0.8), 100.0, 50.0, 0.6, TUNGSTEN, 0.1)
    add_spot(scene, "ConsoleR", (1.9, -0.2, CEILING_Z - 0.05), (1.9, 0.3, 0.8), 100.0, 50.0, 0.6, TUNGSTEN, 0.1)

    # Small spots from the ceiling on the tape machine and the rack, and a dim, broad warm
    # light over the console so the room is not black between the pools.
    add_spot(scene, "TapeSpot", (2.55, 1.6, CEILING_Z - 0.05), (2.55, 2.7, 1.5), 140.0, 45.0, 0.8, TUNGSTEN, 0.1)
    add_spot(scene, "RackSpot", (-2.6, 1.6, CEILING_Z - 0.05), (-2.6, 2.7, 1.4), 120.0, 45.0, 0.8, TUNGSTEN, 0.1)
    data = bpy.data.lights.new("Wash", "AREA")
    data.shape = "RECTANGLE"
    data.size = 6.0
    data.size_y = 3.0
    data.energy = 120.0
    data.color = TUNGSTEN
    wash = bpy.data.objects.new("Wash", data)
    wash.location = (0.0, 1.5, CEILING_Z - 0.1)
    wash.visible_camera = False
    scene.collection.objects.link(wash)

    # A strip light on the floor along the back wall, grazing the boards from below, so the
    # panelling reads low down and fades into the dark above.
    data = bpy.data.lights.new("Graze", "AREA")
    data.shape = "RECTANGLE"
    data.size = 2 * SIDE_X - 0.4
    data.size_y = 0.08
    data.energy = 60.0
    data.spread = math.radians(50.0)
    data.color = TUNGSTEN
    graze = bpy.data.objects.new("Graze", data)
    graze.location = (0.0, WALL_Y - 0.35, 0.12)
    scene.collection.objects.link(graze)
    aim(graze, (0.0, WALL_Y, 0.9))
    graze.visible_camera = False

    camera_data = bpy.data.cameras.new("Camera")
    camera_data.lens = 35.0
    camera_data.sensor_width = 36.0
    camera_data.dof.use_dof = True
    camera_data.dof.focus_distance = 1.8
    camera_data.dof.aperture_fstop = 1.8
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = (0.0, -3.0, 1.3)
    camera.rotation_euler = (math.radians(95.0), 0.0, 0.0)
    scene.collection.objects.link(camera)
    scene.camera = camera
    return scene


def render_room(preview_path=None):
    scene = build_room()
    if preview_path:
        scene.render.resolution_percentage = 40
        scene.cycles.samples = 48
        path = preview_path
    else:
        os.makedirs(OUT, exist_ok=True)
        path = os.path.join(OUT, "room.jpg")
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    print("wrote", path)


# ------------------------------------------------------------------------- main

JOBS = {"knob": render_knob, "room": render_room}

if __name__ == "__main__":
    names = sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else []
    if names[:2] == ["room", "preview"]:
        render_room(names[2])
        raise SystemExit(0)
    unknown = [name for name in names if name not in JOBS]
    if unknown:
        raise SystemExit(f"unknown job(s): {', '.join(unknown)}; expected {', '.join(JOBS)}")
    for name in names or JOBS:
        JOBS[name]()
