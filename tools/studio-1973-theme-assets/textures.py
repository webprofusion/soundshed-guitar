"""
Makes the Studio 1973 theme's tiling textures: walnut veneer (flat-sawn with flowing figure
for the console's big panels, and quarter-sawn with tight straight grain for the units, each
with a copy whose grain runs down), teak veneer, padded saddle
leather and the leather's stitching.

Run it with Blender (5.x), headless, from the repo root:

    blender -b --factory-startup -P tools/studio-1973-theme-assets/textures.py -- [walnut] [walnut-fine] [teak] [leather]

With no names after "--" it makes everything. Output goes to core/ui/images/themes/studio-1973/.
Run with plain Python (numpy only) and a folder after "--preview" instead, it writes PNG
previews there, which is quicker for judging the look.

Every texture is a numpy field built from noise filtered in the frequency domain, so it tiles
in both directions with no seam. Each is made at twice the size it is drawn, so its finest
grain stays under a pixel and reads as figure rather than as lines. The light is above the
panel (shadows fall down), as in the theme's stylesheet.
"""

import math
import os
import struct
import sys
import zlib

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(REPO, "core", "ui", "images", "themes", "studio-1973")

PREVIEW_DIR = None


# ------------------------------------------------------------------------ noise


def blur_wrap(field, sigma_y, sigma_x):
    """Gaussian blur by FFT, so the result tiles."""
    fy = np.fft.fftfreq(field.shape[0])[:, None]
    fx = np.fft.fftfreq(field.shape[1])[None, :]
    kernel = np.exp(-2.0 * (math.pi**2) * ((sigma_y * fy) ** 2 + (sigma_x * fx) ** 2))
    return np.real(np.fft.ifft2(np.fft.fft2(field) * kernel))


def unit(field):
    return (field - field.mean()) / (field.std() + 1e-9)


def noise(rng, shape, sigma_y, sigma_x):
    return unit(blur_wrap(rng.standard_normal(shape), sigma_y, sigma_x))


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def ramp(t, stops):
    """Piecewise-linear colour ramp: stops is [(position, (r, g, b)), ...], positions ascending."""
    t = np.clip(t, 0.0, 1.0)
    out = np.zeros(t.shape + (3,))
    positions = [p for p, _ in stops]
    colours = np.array([c for _, c in stops])
    for channel in range(3):
        out[..., channel] = np.interp(t, positions, colours[:, channel])
    return out


def normals_from_height(height, strength):
    dy = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * 0.5 * strength
    dx = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * 0.5 * strength
    n = np.stack([-dx, dy, np.ones_like(height)], axis=-1)
    return n / np.linalg.norm(n, axis=-1, keepdims=True)


def downsample(rgb, factor):
    h, w, c = rgb.shape
    return rgb.reshape(h // factor, factor, w // factor, factor, c).mean(axis=(1, 3))


# ------------------------------------------------------------------------- wood


def veneer(rng, height, width, rings, colours, figure, pore_density, ray_flecks, lines=1.0):
    """
    Flat-sawn veneer, the grain running left to right. A growth-ring phase climbs down the
    sheet and is pushed about by slow, long noise, which bends the rings into the flowing
    stripes and the odd cathedral arch of a sliced log. Each ring is pale, open earlywood
    fading into a dark, dense latewood line; the pores are short dark dashes along the grain,
    thickest in the earlywood. A broad lighter/darker wander and the lacquer's chatoyance
    (fibres catching the light in bands across the grain) go on top. `lines` sets how dark the
    latewood lines draw against the rest (1 for flat-sawn figure, less for a fine straight grain,
    whose lines are thinner and softer).
    """
    y, _ = np.mgrid[0:height, 0:width].astype(np.float64)
    shape = (height, width)
    # The rings' bend: long along the grain, a few rings tall; and a slower, bigger sweep.
    bend = noise(rng, shape, height * 0.05, width * 0.16)
    sweep = noise(rng, shape, height * 0.18, width * 0.35)
    wobble = noise(rng, shape, 4.0, width * 0.03)
    phase = rings * y / height + figure * (1.3 * bend + 2.2 * sweep) + 0.07 * wobble
    ring = phase - np.floor(phase)
    # Earlywood (0) to latewood (1): a slow climb, then a narrow dark line, then the next ring.
    late = smoothstep(0.55, 0.95, ring) * (1.0 - smoothstep(0.95, 1.0, ring))
    late = late**2.2
    # Finer growth lines between the rings, fainter, as every season leaves several.
    sub = phase * 3.0 + 0.35 * noise(rng, shape, 1.5, width * 0.02)
    sub = sub - np.floor(sub)
    thin = smoothstep(0.78, 0.97, sub) * (1.0 - smoothstep(0.97, 1.0, sub))
    # Fibre streaks, very fine and long, and the pores: fine dashes along the grain.
    fibre = noise(rng, shape, 0.5, 26.0)
    pores = noise(rng, shape, 0.45, 7.0)
    pore_mask = smoothstep(1.9, 3.0, -pores) * (pore_density * (1.1 - 0.7 * late))
    # Broad tone: some boards of the flitch came out darker, some warmer.
    tone = noise(rng, shape, height * 0.3, width * 0.5)
    hue = noise(rng, shape, height * 0.4, width * 0.7)

    t = 0.64 - lines * (0.46 * late + 0.12 * thin) + 0.045 * fibre + 0.05 * tone
    rgb = ramp(t, colours)
    # A warmer (redder) or cooler (greyer) cast here and there, as walnut has.
    rgb *= (1.0 + 0.035 * hue[..., None] * np.array([1.0, -0.2, -0.9]))
    rgb *= (1.0 - 0.35 * np.clip(pore_mask, 0.0, 1.0))[..., None]
    if ray_flecks:
        # Small satin flecks across the grain, where the cut crossed the rays.
        fleck = noise(rng, shape, 3.0, 1.2)
        rgb *= (1.0 + 0.05 * smoothstep(1.8, 3.0, fleck))[..., None]
    # Chatoyance: bands of sheen across the grain that shift with the fibres.
    shimmer = noise(rng, shape, height * 0.02, width * 0.012)
    rgb *= (1.0 + 0.05 * np.tanh(shimmer))[..., None]
    return np.clip(rgb, 0.0, 1.0)


WALNUT = [
    (0.0, (0.105, 0.058, 0.032)),
    (0.25, (0.19, 0.11, 0.062)),
    (0.5, (0.30, 0.18, 0.10)),
    (0.75, (0.40, 0.25, 0.145)),
    (1.0, (0.47, 0.31, 0.18)),
]

TEAK = [
    (0.0, (0.22, 0.12, 0.055)),
    (0.3, (0.38, 0.22, 0.10)),
    (0.6, (0.53, 0.33, 0.16)),
    (1.0, (0.64, 0.43, 0.23)),
]


def make_walnut(rng):
    """Walnut, drawn at 512 x 256: rich, dark, with flowing figure. Also a copy turned upright."""
    rgb = veneer(rng, 512, 1024, rings=16, colours=WALNUT, figure=1.0, pore_density=0.9, ray_flecks=False)
    save("walnut.jpg", rgb)
    save("walnut-v.jpg", np.ascontiguousarray(np.transpose(rgb, (1, 0, 2))[:, ::-1]))


def make_walnut_fine(rng):
    """
    Quarter-sawn walnut, drawn at 512 x 256, for the units (a dialog's cap, the effect panel's
    cheeks): the tight, straight, even grain of a cabinet's veneer, with the satin ray flecks a
    quarter cut shows, where the flowing figure of walnut.jpg belongs on the big console
    panels. Also a copy turned upright.
    """
    rgb = veneer(rng, 512, 1024, rings=44, colours=WALNUT, figure=0.3, pore_density=0.8, ray_flecks=True, lines=0.5)
    save("walnut-fine.jpg", rgb)
    save("walnut-fine-v.jpg", np.ascontiguousarray(np.transpose(rgb, (1, 0, 2))[:, ::-1]))


def make_teak(rng):
    """Teak, drawn at 512 x 256: paler, oranger, straighter grain, the oily satin of an oiled finish."""
    rgb = veneer(rng, 512, 1024, rings=26, colours=TEAK, figure=0.3, pore_density=1.1, ray_flecks=True)
    save("teak.jpg", rgb)


# ---------------------------------------------------------------------- leather


def shade(normals, colour, light, ambient, gloss, shininess):
    light = np.asarray(light, dtype=np.float64)
    light /= np.linalg.norm(light)
    half = light + np.array([0.0, 0.0, 1.0])
    half /= np.linalg.norm(half)
    diffuse = np.clip(normals @ light, 0.0, 1.0)
    spec = np.clip(normals @ half, 0.0, 1.0) ** shininess
    if colour.ndim == 1:
        colour = colour[None, None, :]
    return colour * (ambient + (1.0 - ambient) * diffuse)[..., None] + gloss * spec[..., None]


def make_leather(rng):
    """
    Saddle-tan leather, drawn at 256 x 256: a fine pebbled grain with crease lines wandering
    through it, and darker, burnished patches where hands rest. The stylesheet adds the padding's
    roll (a gradient) and the stitching (stitch.png) over it.
    """
    size = 512
    shape = (size, size)
    grain = noise(rng, shape, 1.6, 1.6)
    cells = np.tanh(1.4 * grain)
    crease_a = np.abs(noise(rng, shape, 6.0, 6.0))
    crease_b = np.abs(noise(rng, shape, 14.0, 10.0))
    creases = smoothstep(0.0, 0.12, crease_a) * smoothstep(0.0, 0.08, crease_b)
    surface = 1.0 * cells + 0.8 * creases
    normals = normals_from_height(surface, 0.8)
    burnish = noise(rng, shape, 40.0, 60.0)
    tone = noise(rng, shape, 12.0, 12.0)
    base_colour = np.array([0.48, 0.24, 0.095])
    colour = base_colour[None, None, :] * (1.0 + 0.04 * tone[..., None] - 0.06 * np.clip(burnish, 0, None)[..., None])
    rgb = shade(normals, colour, (-0.2, 0.8, 0.6), 0.55, 0.06, 16.0)
    rgb *= (0.86 + 0.14 * creases)[..., None]
    save("leather.jpg", np.clip(rgb, 0.0, 1.0))


def make_stitch():
    """
    One stitch of cream waxed thread, 16 x 8 px (drawn at 8 x 4, repeated along the seam):
    a slanted, rounded thread lying in its two needle holes, with a shadow under it.
    """
    ss = 4
    w, h = 16 * ss, 8 * ss
    y, x = np.mgrid[0:h, 0:w].astype(np.float64) + 0.5
    # The thread runs from (3, 5.2) to (12, 2.8) in 16 x 8 units, a little slanted.
    x0, y0, x1, y1 = 3.0 * ss, 5.2 * ss, 12.0 * ss, 2.8 * ss
    dx, dy = x1 - x0, y1 - y0
    length = math.hypot(dx, dy)
    t = np.clip(((x - x0) * dx + (y - y0) * dy) / (length * length), 0.0, 1.0)
    px, py = x0 + t * dx, y0 + t * dy
    dist = np.hypot(x - px, y - py)
    radius = 1.25 * ss
    thread = np.clip(1.0 - dist / radius, 0.0, 1.0)
    alpha = smoothstep(0.0, 0.25, thread)
    # Rounded: lit along its upper edge, darker under it; and twisted, so faint diagonal bands.
    across = ((y - py) / radius)
    light = 0.82 - 0.22 * across + 0.04 * np.sin((x + y) * 1.4 / ss)
    colour = np.array([0.93, 0.85, 0.66])
    rgb = colour[None, None, :] * np.clip(light, 0.4, 1.05)[..., None]
    # The needle holes at each end, and a soft shadow down and a touch right of the thread.
    sx, sy = x - 0.6 * ss, y - 0.9 * ss
    ts = np.clip(((sx - x0) * dx + (sy - y0) * dy) / (length * length), 0.0, 1.0)
    sdist = np.hypot(sx - (x0 + ts * dx), sy - (y0 + ts * dy))
    shadow = 0.55 * smoothstep(1.9 * ss, 0.6 * ss, sdist)
    holes = np.zeros_like(x)
    for hx, hy in ((x0, y0), (x1, y1)):
        holes = np.maximum(holes, smoothstep(1.3 * ss, 0.5 * ss, np.hypot(x - hx, y - hy)))
    under = np.maximum(shadow, 0.8 * holes)
    out_alpha = alpha + under * (1.0 - alpha)
    out_rgb = (rgb * alpha[..., None] + np.array([0.08, 0.035, 0.01])[None, None, :] * (under * (1.0 - alpha))[..., None])
    out_rgb /= np.maximum(out_alpha, 1e-6)[..., None]
    rgba = np.concatenate([out_rgb, out_alpha[..., None]], axis=2)
    rgba = downsample(rgba * np.concatenate([out_alpha[..., None]] * 3 + [np.ones_like(out_alpha)[..., None]], axis=2), ss)
    # Un-premultiply after averaging, so the thread's edge doesn't darken.
    rgba[..., :3] /= np.maximum(rgba[..., 3:4], 1e-6)
    save("stitch.png", np.clip(rgba, 0.0, 1.0))


# ------------------------------------------------------------------------ output


def write_png(path, rgb_or_rgba):
    """A plain PNG writer for previews, so plain Python needs only numpy."""
    data = np.clip(np.round(rgb_or_rgba * 255.0), 0, 255).astype(np.uint8)
    height, width, channels = data.shape
    colour_type = 6 if channels == 4 else 2
    raw = b"".join(b"\x00" + data[row].tobytes() for row in range(height))

    def chunk(kind, payload):
        body = kind + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", width, height, 8, colour_type, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    print("wrote", path)


def save(filename, pixels):
    if PREVIEW_DIR:
        os.makedirs(PREVIEW_DIR, exist_ok=True)
        write_png(os.path.join(PREVIEW_DIR, os.path.splitext(filename)[0] + ".png"), pixels)
        return
    sys.path.insert(0, os.path.join(HERE, "..", "studio-theme-assets"))
    import studio_2003 as base  # noqa: E402  (Blender only)

    if pixels.shape[2] == 3:
        pixels = base.opaque(pixels)
        base.save_image(filename, pixels, jpeg_quality=90, out=OUT)
    else:
        base.save_image(filename, pixels, out=OUT)


def make_walnut_job():
    make_walnut(np.random.default_rng(1973))


def make_walnut_fine_job():
    make_walnut_fine(np.random.default_rng(1976))


def make_teak_job():
    make_teak(np.random.default_rng(1974))


def make_leather_job():
    make_leather(np.random.default_rng(1975))
    make_stitch()


JOBS = {"walnut": make_walnut_job, "walnut-fine": make_walnut_fine_job, "teak": make_teak_job, "leather": make_leather_job}

if __name__ == "__main__":
    in_blender = "blender" in os.path.basename(sys.argv[0]).lower()
    if "--" in sys.argv:
        args = sys.argv[sys.argv.index("--") + 1 :]
    else:
        args = [] if in_blender else sys.argv[1:]
    if "--preview" in args:
        index = args.index("--preview")
        PREVIEW_DIR = args[index + 1]
        args = args[:index] + args[index + 2 :]
    else:
        import bpy  # noqa: F401  (writing JPEG and PNG goes through Blender)

        bpy.ops.wm.read_factory_settings(use_empty=True)
    unknown = [name for name in args if name not in JOBS]
    if unknown:
        raise SystemExit(f"unknown job(s): {', '.join(unknown)}; expected {', '.join(JOBS)}")
    for name in args or JOBS:
        JOBS[name]()
