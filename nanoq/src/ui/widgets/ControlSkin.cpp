// See ControlSkin.h.

#include "ui/widgets/ControlSkin.h"

#include "ui/widgets/Graphics.h"

#include <algorithm>
#include <cmath>

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
} // namespace

void DrawControlBox(canvas& cnv, el::rect r, const ControlBox& box, float scale, float radius)
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

    if (Visible(box.fillTop) || Visible(box.fillBottom))
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

el::font_descr ControlFont(const ControlText& text, float size)
{
    const auto weight = text.weight >= 600 ? Weight::SemiBold : text.weight >= 500 ? Weight::Medium : Weight::Regular;
    return Font(weight).size(size > 0.0f ? size : text.size);
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
constexpr float kPi = 3.14159265f;
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
