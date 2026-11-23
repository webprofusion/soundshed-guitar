// See ControlSkin.h.

#include "ui/widgets/ControlSkin.h"

#include "ui/widgets/Graphics.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace nanoq::ui
{
namespace artist = cycfi::artist;
using canvas = artist::canvas;

namespace
{
el::color Argb(std::uint32_t value)
{
    return Theme::Argb(value);
}

bool Visible(std::uint32_t argb)
{
    return (argb >> 24) != 0;
}

el::color Mix(el::color a, el::color b, float t)
{
    return el::color{a.red + (b.red - a.red) * t, a.green + (b.green - a.green) * t, a.blue + (b.blue - a.blue) * t,
                     a.alpha + (b.alpha - a.alpha) * t};
}

std::uint32_t MixArgb(std::uint32_t a, std::uint32_t b, float t)
{
    auto channel = [&](int shift) {
        const float x = static_cast<float>((a >> shift) & 0xFF), y = static_cast<float>((b >> shift) & 0xFF);
        return static_cast<std::uint32_t>(std::lround(x + (y - x) * t)) << shift;
    };
    return channel(24) | channel(16) | channel(8) | channel(0);
}

ControlBox MixBox(const ControlBox& a, const ControlBox& b, float t)
{
    if (t <= 0.0f)
    {
        return a;
    }

    if (t >= 1.0f)
    {
        return b;
    }

    ControlBox m = t < 0.5f ? a : b;
    m.fillTop = MixArgb(a.fillTop, b.fillTop, t);
    m.fillBottom = MixArgb(a.fillBottom, b.fillBottom, t);
    m.borderColour = MixArgb(a.borderColour, b.borderColour, t);
    m.ringColour = MixArgb(a.ringColour, b.ringColour, t);
    m.dropColour = MixArgb(a.dropColour, b.dropColour, t);
    m.highlight = MixArgb(a.highlight, b.highlight, t);
    m.shade = MixArgb(a.shade, b.shade, t);
    return m;
}

// A rounded box; a square one rounded all the way is a circle. CSS caps the radius at half the
// shorter side, so a pill keeps its round ends.
void AddShape(canvas& cnv, el::rect r, float radius)
{
    const float half = std::min(r.width(), r.height()) * 0.5f;

    if (radius >= half - 0.01f && std::abs(r.width() - r.height()) < 0.5f)
    {
        cnv.add_circle(artist::circle(artist::center_point(r), half));
    }
    else
    {
        cnv.add_round_rect(r, std::min(radius, half));
    }
}

// The web's inset line (`inset 0 1px 0 c`): the sliver of the box its copy `dy` lower leaves
// uncovered, inside the box.
void InsetLine(canvas& cnv, el::rect r, float radius, float dy, el::color colour)
{
    auto state = cnv.new_state();
    cnv.begin_path();
    AddShape(cnv, r, radius);
    cnv.clip();
    cnv.begin_path();
    AddShape(cnv, r, radius);
    AddShape(cnv, r.move(0.0f, dy), radius);
    cnv.fill_rule(artist::path::fill_odd_even);
    cnv.fill_style(colour);
    cnv.fill();
}

constexpr float kPi = 3.14159265358979f;

// A length along an axis `extent` long: pixels, per cent of it, or -1 for auto.
float LengthOf(float value, int unit, float extent)
{
    return unit == 0 ? value : unit == 1 ? value * 0.01f * extent : -1.0f;
}

// A stop's place along a gradient `length` long, 0 to 1.
float StopAt(const guitarfx::uiclient::theme::BackdropStop& stop, float length)
{
    return std::clamp(stop.px ? (length > 0.0f ? stop.at / length : 0.0f) : stop.at, 0.0f, 1.0f);
}

void DrawPicture(canvas& cnv, const Theme& theme, el::rect r, const BackdropLayer& layer)
{
    const auto image = theme.Picture(layer.image);

    if (!image)
    {
        return;
    }

    const auto natural = image->size();

    if (natural.x <= 0.0f || natural.y <= 0.0f)
    {
        return;
    }

    const float boxW = r.width(), boxH = r.height();
    float w = 0.0f, h = 0.0f;

    if (layer.sizeMode == 1 || layer.sizeMode == 2)
    {
        const float sx = boxW / natural.x, sy = boxH / natural.y;
        const float s = layer.sizeMode == 1 ? std::max(sx, sy) : std::min(sx, sy);
        w = natural.x * s;
        h = natural.y * s;
    }
    else
    {
        w = LengthOf(layer.width, layer.widthUnit, boxW);
        h = LengthOf(layer.height, layer.heightUnit, boxH);

        if (w < 0.0f && h < 0.0f)
        {
            w = natural.x;
            h = natural.y;
        }
        else if (w < 0.0f)
        {
            w = h * natural.x / natural.y;
        }
        else if (h < 0.0f)
        {
            h = w * natural.y / natural.x;
        }
    }

    if (w < 0.5f || h < 0.5f)
    {
        return;
    }

    const float x0 = r.left + layer.placeX * (boxW - w) + layer.offsetX;
    const float y0 = r.top + layer.placeY * (boxH - h) + layer.offsetY;

    // Tiles only where they can be seen: the box, within what is being redrawn.
    const auto seen = artist::intersection(r, cnv.clip_extent());

    if (seen.width() <= 0.0f || seen.height() <= 0.0f)
    {
        return;
    }

    const float firstX = layer.repeatX ? x0 - std::ceil((x0 - seen.left) / w) * w : x0;
    const float firstY = layer.repeatY ? y0 - std::ceil((y0 - seen.top) / h) * h : y0;
    const float lastX = layer.repeatX ? seen.right : x0 + 0.5f * w;
    const float lastY = layer.repeatY ? seen.bottom : y0 + 0.5f * h;

    for (float y = firstY; y < lastY; y += h)
    {
        for (float x = firstX; x < lastX; x += w)
        {
            cnv.draw(*image, el::rect{x, y, x + w, y + h});
        }
    }
}

// One tile of a linear gradient, filled where it overlaps `r` (the box).
void DrawLinearTile(canvas& cnv, el::rect tile, el::rect r, const BackdropLayer& layer)
{
    const auto part = artist::intersection(tile, r);

    if (part.width() <= 0.0f || part.height() <= 0.0f)
    {
        return;
    }

    // A repeating gradient straight across or down (a dial's scale ticks, an LCD's pixel grid):
    // its stops' pattern again every period along the tile, each band between two stops solid
    // where they are one colour (hard stops) and graded where not.
    const int n = layer.stopCount;
    const float turns = layer.angle / 90.0f;

    if (layer.repeating && n >= 2 && std::abs(turns - std::round(turns)) < 0.001f)
    {
        const int quarter = ((static_cast<int>(std::lround(turns)) % 4) + 4) % 4; // 0 up, 1 right, 2 down, 3 left
        const bool across = quarter == 1 || quarter == 3;
        const float length = across ? tile.width() : tile.height();
        const auto at = [&](const guitarfx::uiclient::theme::BackdropStop& s) { return s.px ? s.at : s.at * length; };
        const float first = at(layer.stops[0]);
        const float period = at(layer.stops[n - 1]) - first;

        if (period >= 0.5f)
        {
            // A band from `from` to `to` along the gradient line, as a rectangle of the tile.
            const auto band = [&](float from, float to) {
                switch (quarter)
                {
                case 1:
                    return el::rect{tile.left + from, tile.top, tile.left + to, tile.bottom};
                case 3:
                    return el::rect{tile.right - to, tile.top, tile.right - from, tile.bottom};
                case 2:
                    return el::rect{tile.left, tile.top + from, tile.right, tile.top + to};
                default:
                    return el::rect{tile.left, tile.bottom - to, tile.right, tile.bottom - from};
                }
            };

            auto state = cnv.new_state();
            cnv.begin_path();
            cnv.add_rect(part);
            cnv.clip();

            for (float base = first - std::ceil(first / period) * period; base < length; base += period)
            {
                for (int i = 0; i + 1 < n; ++i)
                {
                    const float from = base + at(layer.stops[i]) - first;
                    const float to = base + at(layer.stops[i + 1]) - first;

                    if (to <= from || to <= 0.0f || from >= length)
                    {
                        continue;
                    }

                    const auto c0 = layer.stops[i].colour, c1 = layer.stops[i + 1].colour;

                    if (!Visible(c0) && !Visible(c1))
                    {
                        continue;
                    }

                    const auto r0 = band(from, to);

                    if (c0 == c1)
                    {
                        cnv.fill_style(Argb(c0));
                    }
                    else
                    {
                        const auto start = band(from, from), end = band(to, to);
                        canvas::linear_gradient g{artist::center_point(start), artist::center_point(end)};
                        g.add_color_stop(0.0f, Argb(c0));
                        g.add_color_stop(1.0f, Argb(c1));
                        cnv.fill_style(g);
                    }

                    cnv.fill_rect(r0);
                }
            }

            return;
        }
    }

    // CSS's gradient line: through the tile's centre at the angle (0 up, 90 right, 180 down),
    // as long as the tile's corners reach along it.
    const float a = layer.angle * kPi / 180.0f;
    const float dx = std::sin(a), dy = -std::cos(a);
    const float length = std::abs(tile.width() * dx) + std::abs(tile.height() * dy);
    const auto c = artist::center_point(tile);
    canvas::linear_gradient fill{el::point{c.x - dx * length * 0.5f, c.y - dy * length * 0.5f},
                                 el::point{c.x + dx * length * 0.5f, c.y + dy * length * 0.5f}};

    for (int i = 0; i < layer.stopCount; ++i)
    {
        fill.add_color_stop(StopAt(layer.stops[i], length), Argb(layer.stops[i].colour));
    }

    cnv.fill_style(fill);
    cnv.fill_rect(part);
}

void DrawLinear(canvas& cnv, el::rect r, const BackdropLayer& layer)
{
    // Sized and placed as a picture is (a band along an edge, say); auto, cover and fit are
    // the whole box, as a gradient has no size of its own.
    const float boxW = r.width(), boxH = r.height();
    const bool sized = layer.sizeMode == 0;
    float w = sized ? LengthOf(layer.width, layer.widthUnit, boxW) : -1.0f;
    float h = sized ? LengthOf(layer.height, layer.heightUnit, boxH) : -1.0f;
    w = w < 0.0f ? boxW : w;
    h = h < 0.0f ? boxH : h;

    if (w < 0.25f || h < 0.25f)
    {
        return;
    }

    const float x0 = r.left + layer.placeX * (boxW - w) + layer.offsetX;
    const float y0 = r.top + layer.placeY * (boxH - h) + layer.offsetY;

    if (!layer.repeatX && !layer.repeatY)
    {
        DrawLinearTile(cnv, el::rect{x0, y0, x0 + w, y0 + h}, r, layer);
        return;
    }

    const auto seen = artist::intersection(r, cnv.clip_extent());

    if (seen.width() <= 0.0f || seen.height() <= 0.0f)
    {
        return;
    }

    const float firstX = layer.repeatX ? x0 - std::ceil((x0 - seen.left) / w) * w : x0;
    const float firstY = layer.repeatY ? y0 - std::ceil((y0 - seen.top) / h) * h : y0;
    const float lastX = layer.repeatX ? seen.right : x0 + 0.5f * w;
    const float lastY = layer.repeatY ? seen.bottom : y0 + 0.5f * h;

    for (float y = firstY; y < lastY; y += h)
    {
        for (float x = firstX; x < lastX; x += w)
        {
            DrawLinearTile(cnv, el::rect{x, y, x + w, y + h}, r, layer);
        }
    }
}

void DrawRadial(canvas& cnv, el::rect r, const BackdropLayer& layer)
{
    // An ellipse: the gradient drawn round a unit circle, stretched to the radii.
    const float cx = r.left + layer.placeX * r.width() + layer.offsetX;
    const float cy = r.top + layer.placeY * r.height() + layer.offsetY;
    const float rx = LengthOf(layer.width, layer.widthUnit, r.width());
    const float ry = LengthOf(layer.height, layer.heightUnit, r.height());

    if (rx <= 0.0f || ry <= 0.0f)
    {
        return;
    }

    auto state = cnv.new_state();
    cnv.translate(cx, cy);
    cnv.scale(rx, ry);
    canvas::radial_gradient fill{el::point{0.0f, 0.0f}, 0.0f, el::point{0.0f, 0.0f}, 1.0f};

    for (int i = 0; i < layer.stopCount; ++i)
    {
        fill.add_color_stop(StopAt(layer.stops[i], rx), Argb(layer.stops[i].colour));
    }

    cnv.fill_style(fill);
    cnv.fill_rect(el::rect{(r.left - cx) / rx, (r.top - cy) / ry, (r.right - cx) / rx, (r.bottom - cy) / ry});
}

void DrawLayers(canvas& cnv, const Theme& theme, el::rect r, const SurfaceBackdrop& backdrop)
{
    // CSS lists the topmost layer first.
    for (int i = backdrop.count - 1; i >= 0; --i)
    {
        const auto& layer = backdrop.layers[i];

        if (layer.kind == 0)
        {
            DrawPicture(cnv, theme, r, layer);
        }
        else if (layer.kind == 1)
        {
            DrawLinear(cnv, r, layer);
        }
        else if (layer.kind == 2)
        {
            DrawRadial(cnv, r, layer);
        }
    }
}

void DrawBox(canvas& cnv, el::rect r, const ControlBox& box, float scale, float radius,
             const std::function<void(float)>& fillWith)
{
    const float rad = radius >= 0.0f ? radius : box.radius * scale;
    const float ring = box.ring * scale;

    if (Visible(box.dropColour) && (box.dropBlur > 0.0f || box.dropY != 0.0f))
    {
        auto state = cnv.new_state();
        cnv.shadow_style(el::point{0.0f, box.dropY * scale}, box.dropBlur * scale, Argb(box.dropColour));
        cnv.begin_path();
        AddShape(cnv, r, rad);
        cnv.fill_style(Visible(box.fillBottom) ? Argb(box.fillBottom) : Argb(box.dropColour).opacity(0.0f));
        cnv.fill();
    }

    if (ring > 0.0f && Visible(box.ringColour))
    {
        cnv.begin_path();
        AddShape(cnv, r.inset(-ring, -ring), rad > 0.0f ? rad + ring : 0.0f);
        cnv.fill_style(Argb(box.ringColour));
        cnv.fill();
    }

    if (fillWith)
    {
        fillWith(rad);
    }
    else if (Visible(box.fillTop) || Visible(box.fillBottom))
    {
        auto state = cnv.new_state();

        if (box.fillTop == box.fillBottom)
        {
            cnv.fill_style(Argb(box.fillTop));
        }
        else
        {
            canvas::linear_gradient fill{r.top_left(), r.bottom_left()};
            fill.add_color_stop(0.0f, Argb(box.fillTop));
            fill.add_color_stop(1.0f, Argb(box.fillBottom));
            cnv.fill_style(fill);
        }

        cnv.begin_path();
        AddShape(cnv, r, rad);
        cnv.fill();
    }

    if (Visible(box.highlight))
    {
        InsetLine(cnv, r, rad, scale, Argb(box.highlight));
    }

    if (Visible(box.shade))
    {
        InsetLine(cnv, r, rad, -scale, Argb(box.shade));
    }

    if (box.border > 0.0f && Visible(box.borderColour))
    {
        const float w = box.border * scale;
        cnv.begin_path();
        AddShape(cnv, r.inset(w * 0.5f, w * 0.5f), std::max(0.0f, rad - w * 0.5f));
        cnv.stroke_style(Argb(box.borderColour));
        cnv.line_width(w);
        cnv.stroke();
    }
}
} // namespace

void DrawControlBox(canvas& cnv, el::rect r, const ControlBox& box, float scale, float radius)
{
    DrawBox(cnv, r, box, scale, radius, {});
}

void DrawControlBox(canvas& cnv, const Theme& theme, el::rect r, const ControlBox& box, const SurfaceBackdrop& fill,
                    float scale, float radius)
{
    if (fill.count == 0)
    {
        DrawBox(cnv, r, box, scale, radius, {});
        return;
    }

    DrawBox(cnv, r, box, scale, radius, [&](float rad) { DrawBackdrop(cnv, theme, r, fill, true, rad); });
}

el::rect DrawKey(canvas& cnv, const Theme& theme, el::rect bounds, const KeySkin& key, const SurfaceBackdrop& face,
                 bool hover, bool down, std::uint32_t border)
{
    // It stands its wall proud: at rest the face fills the bounds less the wall under it. A press
    // pushes it down all but a pixel of the wall (the web's :active), a latched key sits its own
    // press lower.
    const float wall = std::max(0.0f, key.wall);
    const float press = down ? std::max(key.press, wall - 1.0f) : key.press;
    const el::rect rest{bounds.left, bounds.top, bounds.right, bounds.bottom - std::max(0.0f, wall - key.press)};
    const el::rect r = rest.move(0.0f, press - key.press);
    const float standing = std::max(0.0f, wall - press);
    const float rad = std::min(key.box.radius, std::min(r.width(), r.height()) * 0.5f);

    // The wall: the face's shape again, `standing` lower, banded down to its foot; the drop shadow
    // falls from it.
    if (standing > 0.0f && (Visible(key.wallTop) || Visible(key.wallBottom)))
    {
        auto state = cnv.new_state();
        const el::rect base = r.move(0.0f, standing);

        if (Visible(key.box.dropColour))
        {
            // CSS casts it from the face, which stands `standing` above the wall's foot.
            cnv.shadow_style(el::point{0.0f, std::max(0.0f, key.box.dropY - standing)}, key.box.dropBlur,
                             Argb(key.box.dropColour));
        }

        canvas::linear_gradient bands{el::point{base.left, r.bottom}, el::point{base.left, base.bottom}};
        bands.add_color_stop(0.0f, Argb(key.wallTop));
        bands.add_color_stop(1.0f, Argb(key.wallBottom));
        cnv.fill_style(bands);
        cnv.begin_path();
        AddShape(cnv, base, rad);
        cnv.fill();
    }

    ControlBox box = key.box;

    if (standing > 0.0f)
    {
        box.dropColour = 0; // fallen from the wall
    }

    if (Visible(border))
    {
        box.borderColour = border;
    }

    DrawControlBox(cnv, theme, r, box, face, 1.0f, rad);

    if (hover && !down)
    {
        cnv.begin_path();
        AddShape(cnv, r, rad);
        cnv.fill_style(el::rgba(255, 255, 255, 20));
        cnv.fill();
    }

    return r;
}

void DrawDisplayText(canvas& cnv, const std::string& text, const el::font_descr& font, const DisplaySkin& display,
                     el::color colour, el::rect bounds, HAlign align)
{
    auto state = cnv.new_state();

    if (Visible(display.glow) && display.glowBlur > 0.0f)
    {
        cnv.shadow_style(el::point{display.glowX, display.glowY}, display.glowBlur, Argb(display.glow));
    }

    FillText(cnv, text, font, colour, bounds, align);
}

void DrawBackdrop(canvas& cnv, const Theme& theme, el::rect r, const SurfaceBackdrop& backdrop, bool withColour,
                  float radius)
{
    if (backdrop.count == 0 && !(withColour && Visible(backdrop.colour)))
    {
        return;
    }

    auto state = cnv.new_state();
    cnv.begin_path();
    AddShape(cnv, r, radius);
    cnv.clip();

    if (withColour && Visible(backdrop.colour))
    {
        cnv.fill_style(Argb(backdrop.colour));
        cnv.fill_rect(r);
    }

    DrawLayers(cnv, theme, r, backdrop);
}

void BackdropBox::draw(const el::context& ctx)
{
    auto& cnv = ctx.canvas;
    cnv.fill_style(mColour);
    cnv.fill_rect(ctx.bounds);
    DrawBackdrop(cnv, mTheme, ctx.bounds, mTheme.Controls().backdrops.*mWhich, false);
}

el::font_descr ControlFont(const ControlText& text, float size)
{
    return FaceFor(text.family, text.weight).size(size > 0.0f ? size : text.size);
}

namespace
{
// The UTF-8 characters of `text`, one string each.
std::vector<std::string> Characters(const std::string& text)
{
    std::vector<std::string> out;

    for (std::size_t i = 0; i < text.size();)
    {
        std::size_t n = 1;

        while (i + n < text.size() && (static_cast<unsigned char>(text[i + n]) & 0xC0) == 0x80)
        {
            ++n;
        }

        out.push_back(text.substr(i, n));
        i += n;
    }

    return out;
}
} // namespace

float TrackedWidth(canvas& cnv, const std::string& text, const el::font_descr& font, float size, float tracking)
{
    const float width = el::measure_text(cnv, text, font).x;
    const auto n = Characters(text).size();
    return width + (n > 1 ? static_cast<float>(n - 1) * tracking * size : 0.0f);
}

float MeasureControlText(const std::string& text, const ControlText& skin, float size)
{
    const float px = size > 0.0f ? size : skin.size;
    const auto shown = skin.upper ? Upper(text) : text;
    const auto n = Characters(shown).size();
    return MeasureTextWidth(shown, ControlFont(skin, px)) +
           (n > 1 ? static_cast<float>(n - 1) * skin.tracking * px : 0.0f);
}

void DrawControlText(canvas& cnv, std::string text, const ControlText& skin, el::rect bounds, HAlign align, float size)
{
    DrawControlText(cnv, std::move(text), skin, Argb(skin.colour), bounds, align, size);
}

void DrawControlText(canvas& cnv, std::string text, const ControlText& skin, el::color colour, el::rect bounds,
                     HAlign align, float size)
{
    if (text.empty())
    {
        return;
    }

    const float px = size > 0.0f ? size : skin.size;
    const auto font = ControlFont(skin, px);

    if (skin.upper)
    {
        text = Upper(std::move(text));
    }

    const float gap = skin.tracking * px;

    if (gap < 0.05f)
    {
        FillText(cnv, Ellipsize(cnv, text, font, bounds.width() + 6.0f), font, colour, bounds, align);
        return;
    }

    // Spaced letters: cut to fit with the spacing counted, then set one character at a time.
    auto chars = Characters(text);
    const float room = bounds.width() + 6.0f;

    if (TrackedWidth(cnv, text, font, px, skin.tracking) > room)
    {
        static const std::string kEllipsis = "\xE2\x80\xA6";

        while (!chars.empty())
        {
            chars.pop_back();
            std::string cut;

            for (const auto& c : chars)
            {
                cut += c;
            }

            if (TrackedWidth(cnv, cut + kEllipsis, font, px, skin.tracking) <= room)
            {
                break;
            }
        }

        chars.push_back(kEllipsis);
    }

    std::string whole;

    for (const auto& c : chars)
    {
        whole += c;
    }

    const float total = TrackedWidth(cnv, whole, font, px, skin.tracking);
    float x = align == HAlign::Centre  ? bounds.left + (bounds.width() - total) * 0.5f
              : align == HAlign::Right ? bounds.right - total
                                       : bounds.left;
    auto state = cnv.new_state();
    cnv.font(font);
    cnv.fill_style(colour);
    cnv.text_align(canvas::left | canvas::middle);
    const float y = bounds.top + bounds.height() * 0.5f;

    // Each character placed where the run so far ends, so kerning inside the run is kept.
    std::string run;

    for (std::size_t i = 0; i < chars.size(); ++i)
    {
        const float at = x + (run.empty() ? 0.0f : el::measure_text(cnv, run, font).x) + gap * static_cast<float>(i);
        cnv.fill_text(chars[i], el::point{at, y});
        run += chars[i];
    }
}

namespace
{
// A 270 degree sweep, open at the bottom: 7 o'clock round to 5 o'clock.
constexpr float kKnobStart = kPi * 0.75f;
constexpr float kKnobSweep = kPi * 1.5f;
} // namespace

void DrawKnob(canvas& cnv, const Theme& theme, el::point centre, float diameter, float value)
{
    const auto& k = theme.Controls().knob;
    const float s = diameter / std::max(1.0f, k.size);
    const float r = diameter * 0.5f;
    const float cx = centre.x, cy = centre.y;
    const float v = std::clamp(value, 0.0f, 1.0f);

    // The ring: the web's conic gradient round the knob's edge, under the face. It shows in the
    // band the face leaves (its inset), or round a picture's edge where the picture is clear.
    {
        const float band = (k.inset > 0.0f ? k.inset : 4.0f) * s;
        const float arcR = r - band * 0.5f;
        auto state = cnv.new_state();
        cnv.line_width(band);
        cnv.stroke_style(theme.KnobRingTrack());
        cnv.begin_path();
        cnv.arc(cx, cy, arcR, kKnobStart, kKnobStart + kKnobSweep);
        cnv.stroke();

        if (v > 0.002f)
        {
            // Start colour to end colour across whatever the fill spans, as the web's gradient.
            const auto start = theme.KnobRingStart(), end = theme.KnobRingEnd();
            constexpr int kSteps = 24;

            for (int i = 0; i < kSteps; ++i)
            {
                const float t0 = v * static_cast<float>(i) / kSteps;
                const float t1 = v * static_cast<float>(i + 1) / kSteps;
                const float mix = static_cast<float>(i + 1) / kSteps;
                cnv.stroke_style(Mix(start, end, mix));
                cnv.begin_path();
                cnv.arc(cx, cy, arcR, kKnobStart + kKnobSweep * t0,
                        kKnobStart + kKnobSweep * t1 + (i + 1 < kSteps ? 0.01f : 0.0f));
                cnv.stroke();
            }
        }
    }

    // The face: its ring and shadow, its fill, its picture, then its inset lines and border.
    const float faceR = r - k.inset * s;
    const el::rect face{cx - faceR, cy - faceR, cx + faceR, cy + faceR};
    const float round = k.face.radius >= k.size * 0.25f ? faceR : k.face.radius * s;
    {
        ControlBox outer = k.face;
        outer.fillTop = outer.fillBottom = outer.highlight = outer.shade = 0;
        outer.border = 0.0f;
        DrawControlBox(cnv, face, outer, s, round);
    }

    if (Visible(k.radialInner) || Visible(k.radialOuter))
    {
        // CSS's circle gradient reaches its last stop at the farthest corner.
        const float fx = std::max(k.radialX, 1.0f - k.radialX), fy = std::max(k.radialY, 1.0f - k.radialY);
        const float reach = std::sqrt(fx * fx + fy * fy) * face.width();
        const el::point at{face.left + face.width() * k.radialX, face.top + face.height() * k.radialY};
        canvas::radial_gradient fill{at, 0.0f, at, reach};
        fill.add_color_stop(0.0f, Argb(k.radialInner));
        fill.add_color_stop(std::clamp(k.radialEnd, 0.01f, 1.0f), Argb(k.radialOuter));
        auto state = cnv.new_state();
        cnv.fill_style(fill);
        cnv.begin_path();
        AddShape(cnv, face, round);
        cnv.fill();
    }
    else if (Visible(k.face.fillTop) || Visible(k.face.fillBottom))
    {
        ControlBox fill = k.face;
        fill.ring = fill.border = 0.0f;
        fill.dropColour = fill.highlight = fill.shade = 0;
        DrawControlBox(cnv, face, fill, s, round);
    }

    if (const auto picture = theme.Picture(k.image))
    {
        cnv.draw(*picture, face);
    }

    {
        ControlBox lines = k.face;
        lines.fillTop = lines.fillBottom = lines.dropColour = 0;
        lines.ring = 0.0f;
        DrawControlBox(cnv, face, lines, s, round);
    }

    // The tick at six o'clock, which stays still.
    if (Visible(k.tick.colour))
    {
        const float w = k.tick.width * s, h = k.tick.height * s;
        const float bottom = cy + r - k.tick.bottom * s;
        cnv.fill_style(Argb(k.tick.colour));
        cnv.fill_round_rect(el::rect{cx - w * 0.5f, bottom - h, cx + w * 0.5f, bottom}, std::min(w, h) * 0.5f);
    }

    // The pointer: a bar from the knob's top edge down, turned about the centre.
    {
        const auto& p = k.pointer;
        const float w = p.width * s, top = -r + p.top * s, h = p.height * s;
        const float radius = std::min(p.radius * s, w * 0.5f);
        auto state = cnv.new_state();
        cnv.translate(cx, cy);
        cnv.rotate(-kPi * 0.75f + kKnobSweep * v);

        if (Visible(p.glow) && p.glowBlur > 0.0f)
        {
            cnv.shadow_style(el::point{0.0f, 0.0f}, p.glowBlur * s, Argb(p.glow));
        }

        const el::rect bar{-w * 0.5f, top, w * 0.5f, top + h};

        if (p.split >= 1.0f || p.colour == p.colourBelow)
        {
            cnv.fill_style(Argb(p.colour));
            cnv.fill_round_rect(bar, radius);
        }
        else
        {
            const float cut = top + h * p.split;
            cnv.fill_style(Argb(p.colourBelow));
            cnv.fill_round_rect(bar, radius);
            cnv.fill_style(Argb(p.colour));
            cnv.fill_round_rect(el::rect{bar.left, bar.top, bar.right, cut}, radius);
        }
    }
}

void DrawSwitch(canvas& cnv, el::rect track, const ControlSkin& skin, float on, bool hover)
{
    const auto& off = skin.switchOff;
    const auto& lit = skin.switchOn;
    const float scale = track.height() / std::max(1.0f, off.height);
    auto slot = MixBox(off.slot, lit.slot, on);

    if (hover)
    {
        const auto lift = [](std::uint32_t c) { return MixArgb(c, (c & 0xFF000000u) | 0xFFFFFFu, 0.08f); };
        slot.fillTop = lift(slot.fillTop);
        slot.fillBottom = lift(slot.fillBottom);
    }

    DrawControlBox(cnv, track, slot, scale, track.height() * 0.5f);

    const float size = (off.thumbSize + (lit.thumbSize - off.thumbSize) * on) * scale;
    const float inset = off.thumbInset * scale;
    const float travel = track.width() - 2.0f * inset - size;
    const float left = track.left + inset + travel * on;
    const float top = track.top + (track.height() - size) * 0.5f;
    DrawControlBox(cnv, el::rect{left, top, left + size, top + size}, MixBox(off.thumb, lit.thumb, on), scale,
                   size * 0.5f);
}
} // namespace nanoq::ui
