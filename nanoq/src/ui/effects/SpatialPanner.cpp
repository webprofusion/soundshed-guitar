// SpatialPanner.cpp - the 3D Spatial node's panner (see SpatialPanner.h).

#include "ui/effects/SpatialPanner.h"

#include "dsp/FiniteCheck.h"
#include "ui/widgets/ControlSkin.h"
#include "ui/widgets/Graphics.h"
#include "ui/widgets/TextDraw.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace nanoq::ui
{
namespace
{
using cycfi::artist::canvas;
using cycfi::artist::circle;

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
constexpr double kDistanceMin = 0.2;
constexpr double kDistanceMax = 10.0;
constexpr double kDistanceDefault = 1.5;
constexpr std::size_t kTrailLength = 96;
constexpr float kPuckRadius = 8.0f;
constexpr float kLabelLine = 11.0f; // a 9 px label's line

el::color Rgba(int r, int g, int b, float a)
{
    return el::color{static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f, static_cast<float>(b) / 255.0f,
                     a};
}

/// `colour` with its alpha scaled by `k`, as the canvas's globalAlpha draws it.
el::color Dim(el::color colour, float k)
{
    return colour.opacity(colour.alpha * k);
}

// The web UI's --spatial-* colours (variables.css). They are the same in every theme, over a
// canvas that is dark in every theme.
const el::color kGrid = Rgba(255, 255, 255, 0.10f);
const el::color kHead = Rgba(255, 255, 255, 0.32f);
const el::color kLabel = Rgba(255, 255, 255, 0.62f);
const el::color kLabelMuted = Rgba(255, 255, 255, 0.28f);
const el::color kAnchor = Rgba(255, 255, 255, 0.38f);
const el::color kSource = Rgba(72, 168, 224, 0.95f);
const el::color kSourceBehind = Rgba(150, 122, 208, 0.85f);
const el::color kTrail = Rgba(72, 168, 224, 0.30f);
const el::color kShadow = Rgba(0, 0, 0, 0.35f);
const el::color kFocus = Rgba(120, 200, 255, 0.9f);

const char* const kHelp =
    "Drag the radar to pan and set distance, drag the arc for height. Shift for fine, double-click to reset.";

double DistanceToNorm(double distance)
{
    const double d = std::clamp(distance, kDistanceMin, kDistanceMax);
    return (std::log(d) - std::log(kDistanceMin)) / (std::log(kDistanceMax) - std::log(kDistanceMin));
}

double NormToDistance(double norm)
{
    return std::exp(std::log(kDistanceMin) +
                    std::clamp(norm, 0.0, 1.0) * (std::log(kDistanceMax) - std::log(kDistanceMin)));
}

/// 0 dead ahead, 1 directly behind, exactly 0.5 overhead.
double Backness(double azimuth, double elevation)
{
    return 0.5 - 0.5 * std::cos(azimuth * kDeg) * std::cos(elevation * kDeg);
}

/// To (-180, 180], so the shortest way round is always taken.
double WrapDegrees(double degrees)
{
    double d = std::fmod(degrees, 360.0);

    if (d > 180.0)
    {
        d -= 360.0;
    }
    else if (d <= -180.0)
    {
        d += 360.0;
    }

    return d;
}

/// A position made safe to draw and send: the azimuth wrapped, the height and distance clamped,
/// and anything that is not a number back at its default.
SpatialPoint Sanitise(const SpatialPoint& p)
{
    return {guitarfx::IsFinite(p.azimuth) ? WrapDegrees(p.azimuth) : 0.0,
            guitarfx::IsFinite(p.elevation) ? std::clamp(p.elevation, -90.0, 90.0) : 0.0,
            guitarfx::IsFinite(p.distance) ? std::clamp(p.distance, kDistanceMin, kDistanceMax) : kDistanceDefault};
}

/// JavaScript's Math.round: halves round up.
long long JsRound(double value)
{
    return static_cast<long long>(std::floor(value + 0.5));
}

enum class Baseline
{
    Top,
    Middle,
    Bottom
};

/// A 9 px label placed as the canvas places fillText: `align` across `x`, `baseline` at `y`.
void Label(canvas& cnv, const std::string& text, el::color colour, float x, float y, HAlign align, Baseline baseline)
{
    constexpr float kRoom = 200.0f;
    const float top = baseline == Baseline::Top      ? y
                      : baseline == Baseline::Bottom ? y - kLabelLine
                                                     : y - kLabelLine / 2.0f;
    const float left = align == HAlign::Left ? x : align == HAlign::Right ? x - kRoom : x - kRoom / 2.0f;
    FillText(cnv, text, Font().size(9.0f), colour, el::rect{left, top, left + kRoom, top + kLabelLine}, align);
}

/// A circle stroked in dashes `on` long with `off` between, from the right round clockwise, as
/// the canvas's setLineDash draws one.
void DashedCircle(canvas& cnv, el::point centre, float radius, float on, float off)
{
    const float length = 2.0f * static_cast<float>(kPi) * radius;

    for (float s = 0.0f; s < length; s += on + off)
    {
        const float e = std::min(s + on, length);
        cnv.begin_path();
        cnv.arc(centre.x, centre.y, radius, s / radius, e / radius);
        cnv.stroke();
    }
}

/// A line from `a` to `b` stroked in dashes, as above.
void DashedLine(canvas& cnv, el::point a, el::point b, float on, float off)
{
    const float length = std::hypot(b.x - a.x, b.y - a.y);

    if (length <= 0.0f)
    {
        return;
    }

    const float ux = (b.x - a.x) / length, uy = (b.y - a.y) / length;
    cnv.begin_path();

    for (float s = 0.0f; s < length; s += on + off)
    {
        const float e = std::min(s + on, length);
        cnv.move_to(a.x + ux * s, a.y + uy * s);
        cnv.line_to(a.x + ux * e, a.y + uy * e);
    }

    cnv.stroke();
}

el::font_descr HelpFont()
{
    return Font().size(10.0f);
}

/// How many lines the help takes at `width`, broken at spaces as the browser breaks it.
int HelpLines(float width)
{
    const std::string text = kHelp;
    const auto font = HelpFont();
    int lines = 1;
    std::string line;
    std::size_t i = 0;

    while (i < text.size())
    {
        auto end = text.find(' ', i);

        if (end == std::string::npos)
        {
            end = text.size();
        }

        const auto word = text.substr(i, end - i);
        const auto candidate = line.empty() ? word : line + " " + word;

        if (!line.empty() && MeasureTextWidth(candidate, font) > width)
        {
            ++lines;
            line = word;
        }
        else
        {
            line = candidate;
        }

        i = end + 1;
    }

    return lines;
}
} // namespace

// ── SpatialPanner ───────────────────────────────────────────────────────────

el::view_limits SpatialPanner::limits(const el::basic_context&) const
{
    return {{100.0f, 100.0f}, {el::full_extent, el::full_extent}};
}

SpatialPanner::Layout SpatialPanner::ComputeLayout(const el::rect& b)
{
    constexpr float pad = 10.0f;
    const float width = b.width(), height = b.height();
    const float arcPaneWidth = std::min(96.0f, std::max(60.0f, width * 0.26f));
    const float arcPaneLeft = b.left + width - arcPaneWidth;
    const float radarSize = std::min(width - arcPaneWidth - pad * 2.0f, height - pad * 2.0f);
    const float radarOuter = radarSize / 2.0f;
    return {b.left + pad + radarOuter,
            b.top + height / 2.0f,
            std::min(22.0f, radarOuter * 0.28f),
            radarOuter,
            arcPaneLeft + arcPaneWidth * 0.34f,
            b.top + height / 2.0f,
            std::min(arcPaneWidth * 0.55f, height / 2.0f - pad - 8.0f),
            arcPaneLeft};
}

void SpatialPanner::SetAnchor(const SpatialPoint& position)
{
    if (mDragging == Target::None)
    {
        mPosition = Sanitise(position);
    }
}

void SpatialPanner::SetLive(const guitarfx::uiclient::SpatialPosition* live)
{
    if (live == nullptr)
    {
        mLive.reset();
        mTrail.clear();
        return;
    }

    // The puck follows any position the node sends; the trail only grows while it is moving,
    // by one point per message however often this is synced.
    const bool fresh = !mLive || live->serial != mLiveSerial;
    mLiveSerial = live->serial;
    const SpatialPoint heard{live->azimuth, live->elevation, live->distance};
    mLive = Live{heard, live->itdUs, live->ildDb, live->moving};

    if (!live->moving)
    {
        mTrail.clear();
    }
    else if (fresh)
    {
        mTrail.push_back(heard);

        while (mTrail.size() > kTrailLength)
        {
            mTrail.pop_front();
        }
    }
}

void SpatialPanner::draw(const el::context& ctx)
{
    NoteDrawn(ctx);
    auto& cnv = ctx.canvas;
    const auto b = ctx.bounds;
    auto state = cnv.new_state();

    // The canvas (.spatial-panner-canvas): dark in every theme, rounded 4 px, and what is drawn
    // on it clipped to that.
    cnv.begin_path();
    cnv.add_round_rect(b, 4.0f);
    cnv.clip();
    cnv.fill_style(Rgba(16, 18, 24, 0.8f));
    cnv.fill_rect(b);

    const auto L = ComputeLayout(b);

    if (L.radarOuter <= 4.0f || L.arcR <= 4.0f)
    {
        return;
    }

    const SpatialPoint shown = mLive ? Sanitise(mLive->position) : mPosition;
    DrawRadar(cnv, L, shown);
    DrawElevation(cnv, L, shown);
    DrawReadouts(cnv, b, shown);

    if (HasFocus())
    {
        cnv.stroke_style(kFocus);
        cnv.line_width(1.5f);
        cnv.begin_path();
        cnv.add_rect(b.inset(0.75f, 0.75f));
        cnv.stroke();
    }
}

void SpatialPanner::DrawRadar(canvas& cnv, const Layout& L, const SpatialPoint& shown) const
{
    const el::point centre{L.radarCx, L.radarCy};
    const auto radiusFor = [&L](double distance) {
        return L.radarInner + (L.radarOuter - L.radarInner) * static_cast<float>(DistanceToNorm(distance));
    };
    const auto radarPoint = [&L, &radiusFor](double azimuth, double distance) {
        // Screen up is straight ahead; +azimuth turns clockwise, to the listener's right.
        const float r = radiusFor(distance);
        return el::point{L.radarCx + r * static_cast<float>(std::sin(azimuth * kDeg)),
                         L.radarCy - r * static_cast<float>(std::cos(azimuth * kDeg))};
    };

    // Distance rings, labelled where there is room for it.
    cnv.line_width(1.0f);

    for (const double metres : {0.5, 1.0, 2.0, 5.0, 10.0})
    {
        const float r = radiusFor(metres);
        cnv.stroke_style(kGrid);
        cnv.begin_path();
        cnv.add_circle(circle(centre, r));
        cnv.stroke();

        if (r > L.radarInner + 12.0f)
        {
            char text[16];
            std::snprintf(text, sizeof(text), "%gm", metres);
            Label(cnv, text, kLabelMuted, L.radarCx + 3.0f, L.radarCy - r + 6.0f, HAlign::Left, Baseline::Middle);
        }
    }

    // Cross hairs.
    cnv.stroke_style(kGrid);
    cnv.begin_path();
    cnv.move_to(L.radarCx - L.radarOuter, L.radarCy);
    cnv.line_to(L.radarCx + L.radarOuter, L.radarCy);
    cnv.move_to(L.radarCx, L.radarCy - L.radarOuter);
    cnv.line_to(L.radarCx, L.radarCy + L.radarOuter);
    cnv.stroke();

    // In speaker mode the rear half of the radar is a position that cannot honestly be rendered,
    // so it is knocked back rather than drawn as if it worked.
    if (mSpeakers)
    {
        cnv.fill_style(kShadow);
        cnv.begin_path();
        cnv.arc(L.radarCx, L.radarCy, L.radarOuter, 0.0f, static_cast<float>(kPi));
        cnv.close_path();
        cnv.fill();
    }

    // The listener: a head looking up the screen.
    const float head = L.radarInner * 0.55f;
    cnv.stroke_style(kHead);
    cnv.line_width(1.5f);
    cnv.begin_path();
    cnv.add_circle(circle(centre, head));
    cnv.stroke();
    cnv.begin_path();
    cnv.move_to(L.radarCx - 3.0f, L.radarCy - head);
    cnv.line_to(L.radarCx, L.radarCy - head - 5.0f);
    cnv.line_to(L.radarCx + 3.0f, L.radarCy - head);
    cnv.stroke();

    Label(cnv, "FRONT", kLabel, L.radarCx, L.radarCy - L.radarOuter - 1.0f, HAlign::Centre, Baseline::Top);
    Label(cnv, mSpeakers ? "BEHIND *" : "BEHIND", kLabel, L.radarCx, L.radarCy + L.radarOuter + 1.0f, HAlign::Centre,
          Baseline::Bottom);
    Label(cnv, "L", kLabel, L.radarCx - L.radarOuter - 2.0f, L.radarCy, HAlign::Right, Baseline::Middle);
    Label(cnv, "R", kLabel, L.radarCx + L.radarOuter + 2.0f, L.radarCy, HAlign::Left, Baseline::Middle);

    // The trail of where it has been heard, oldest faintest.
    if (mTrail.size() > 1)
    {
        cnv.line_width(1.5f);

        for (std::size_t i = 1; i < mTrail.size(); ++i)
        {
            const auto a = radarPoint(mTrail[i - 1].azimuth, mTrail[i - 1].distance);
            const auto b = radarPoint(mTrail[i].azimuth, mTrail[i].distance);
            cnv.stroke_style(Dim(kTrail, static_cast<float>(i) / static_cast<float>(mTrail.size()) * 0.8f));
            cnv.begin_path();
            cnv.move_to(a.x, a.y);
            cnv.line_to(b.x, b.y);
            cnv.stroke();
        }
    }

    // While motion runs the set position and the heard one differ, so both show: the dashed
    // ring is the position dragged to, the filled puck what is heard.
    if (mLive && mLive->moving)
    {
        cnv.stroke_style(kAnchor);
        cnv.line_width(1.5f);
        DashedCircle(cnv, radarPoint(mPosition.azimuth, mPosition.distance), kPuckRadius - 1.0f, 3.0f, 3.0f);
    }

    // The puck: further away reads as smaller, behind as a different, dimmer colour.
    const auto at = radarPoint(shown.azimuth, shown.distance);
    const double back = Backness(shown.azimuth, shown.elevation);
    const float radius = kPuckRadius * static_cast<float>(1.25 - 0.5 * DistanceToNorm(shown.distance));
    cnv.fill_style(kShadow);
    cnv.begin_path();
    cnv.add_circle(circle(el::point{at.x, at.y + 2.0f}, radius * 0.9f));
    cnv.fill();
    cnv.fill_style(Dim(back > 0.5 ? kSourceBehind : kSource, static_cast<float>(1.0 - 0.35 * back)));
    cnv.begin_path();
    cnv.add_circle(circle(at, radius));
    cnv.fill();

    if (mHover == Target::Radar || mDragging == Target::Radar)
    {
        cnv.stroke_style(kFocus);
        cnv.line_width(1.5f);
        cnv.begin_path();
        cnv.add_circle(circle(at, radius + 3.0f));
        cnv.stroke();
    }
}

void SpatialPanner::DrawElevation(canvas& cnv, const Layout& L, const SpatialPoint& shown) const
{
    // Elevation is a pinna cue. On loudspeakers there is barely any of it, so the whole pane is
    // dimmed rather than implying a height that is not being delivered.
    const float k = mSpeakers ? 0.35f : 1.0f;

    cnv.stroke_style(Dim(kGrid, k));
    cnv.line_width(1.0f);
    cnv.begin_path();
    cnv.arc(L.arcX, L.arcCy, L.arcR, static_cast<float>(-kPi / 2.0), static_cast<float>(kPi / 2.0));
    cnv.stroke();

    // Ear level, and the ground the source casts its shadow onto.
    cnv.begin_path();
    cnv.move_to(L.arcX - 4.0f, L.arcCy);
    cnv.line_to(L.arcX + L.arcR + 4.0f, L.arcCy);
    cnv.stroke();

    Label(cnv, "UP", Dim(kLabelMuted, k), L.arcX + 6.0f, L.arcCy - L.arcR - 2.0f, HAlign::Centre, Baseline::Bottom);
    Label(cnv, "DOWN", Dim(kLabelMuted, k), L.arcX + 6.0f, L.arcCy + L.arcR + 2.0f, HAlign::Centre, Baseline::Top);

    cnv.stroke_style(Dim(kHead, k));
    cnv.line_width(1.5f);
    cnv.begin_path();
    cnv.add_circle(circle(el::point{L.arcX, L.arcCy}, 5.0f));
    cnv.stroke();

    const double a = std::clamp(shown.elevation, -90.0, 90.0) * kDeg;
    const el::point point{L.arcX + L.arcR * static_cast<float>(std::cos(a)),
                          L.arcCy - L.arcR * static_cast<float>(std::sin(a))};

    // The drop to ear level is what makes the two panes read as one object.
    cnv.stroke_style(Dim(kGrid, k));
    cnv.line_width(1.0f);
    DashedLine(cnv, point, el::point{point.x, L.arcCy}, 2.0f, 3.0f);

    const double back = Backness(shown.azimuth, shown.elevation);
    cnv.fill_style(Dim(back > 0.5 ? kSourceBehind : kSource, k));
    cnv.begin_path();
    cnv.add_circle(circle(point, 6.0f));
    cnv.fill();

    if (mHover == Target::Arc || mDragging == Target::Arc)
    {
        cnv.stroke_style(Dim(kFocus, k));
        cnv.line_width(1.5f);
        cnv.begin_path();
        cnv.add_circle(circle(point, 9.0f));
        cnv.stroke();
    }
}

void SpatialPanner::DrawReadouts(canvas& cnv, const el::rect& b, const SpatialPoint& shown) const
{
    std::vector<std::string> lines;
    char text[64];
    std::snprintf(text, sizeof(text), "%lld\xC2\xB0  %s%lld\xC2\xB0", JsRound(shown.azimuth),
                  shown.elevation >= 0.0 ? "+" : "", JsRound(shown.elevation));
    lines.emplace_back(text);
    std::snprintf(text, sizeof(text), "%.2f m", shown.distance);
    lines.emplace_back(text);

    if (mLive)
    {
        std::snprintf(text, sizeof(text), "ITD %lld \xC2\xB5s", JsRound(mLive->itdUs));
        lines.emplace_back(text);
        std::snprintf(text, sizeof(text), "ILD %s%.1f dB", mLive->ildDb >= 0.0 ? "+" : "", mLive->ildDb);
        lines.emplace_back(text);
    }

    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        Label(cnv, lines[i], kLabel, b.left + 4.0f, b.top + 4.0f + 11.0f * static_cast<float>(i), HAlign::Left,
              Baseline::Top);
    }
}

SpatialPanner::Target SpatialPanner::HitTest(const Layout& L, el::point p) const
{
    if (p.x >= L.arcPaneLeft)
    {
        return Target::Arc;
    }

    if (std::hypot(p.x - L.radarCx, p.y - L.radarCy) <= L.radarOuter + 12.0f)
    {
        return Target::Radar;
    }

    return Target::None;
}

SpatialPoint SpatialPanner::FromRadar(const Layout& L, el::point p, bool fine) const
{
    const double dx = p.x - L.radarCx, dy = p.y - L.radarCy;
    const double radius = std::hypot(dx, dy);

    // Right at the centre the angle is meaningless, so the azimuth holds rather than snapping to
    // whichever side of zero the pointer is on.
    const double azimuth = radius < 2.0 ? mPosition.azimuth : WrapDegrees(std::atan2(dx, -dy) / kDeg);
    const double distance = NormToDistance((radius - L.radarInner) / std::max(1.0f, L.radarOuter - L.radarInner));

    if (fine)
    {
        return {WrapDegrees(mPosition.azimuth + WrapDegrees(azimuth - mPosition.azimuth) * 0.25), mPosition.elevation,
                mPosition.distance + (distance - mPosition.distance) * 0.25};
    }

    return {azimuth, mPosition.elevation, distance};
}

SpatialPoint SpatialPanner::FromArc(const Layout& L, el::point p, bool fine) const
{
    const double dx = std::max(0.0, static_cast<double>(p.x - L.arcX));
    const double dy = L.arcCy - p.y;
    const double raw = std::hypot(dx, dy) < 2.0 ? mPosition.elevation : std::atan2(dy, dx) / kDeg;
    const double elevation = std::clamp(raw, -90.0, 90.0);
    return {mPosition.azimuth,
            fine ? mPosition.elevation + (elevation - mPosition.elevation) * 0.25 : elevation,
            mPosition.distance};
}

void SpatialPanner::ApplyDrag(const el::context& ctx, el::point p, bool fine, bool commit)
{
    const auto L = ComputeLayout(ctx.bounds);
    mPosition = Sanitise(mDragging == Target::Arc ? FromArc(L, p, fine) : FromRadar(L, p, fine));

    if (onChange)
    {
        onChange(mPosition, commit);
    }

    ctx.view.refresh(ctx);
}

void SpatialPanner::Commit(const el::context& ctx)
{
    mPosition = Sanitise(mPosition);

    if (onChange)
    {
        onChange(mPosition, true);
    }

    ctx.view.refresh(ctx);
}

bool SpatialPanner::click(const el::context& ctx, el::mouse_button btn)
{
    if (btn.state != el::mouse_button::left)
    {
        return false;
    }

    const bool fine = (btn.modifiers & el::mod_shift) != 0;

    if (btn.down)
    {
        const auto target = HitTest(ComputeLayout(ctx.bounds), btn.pos);

        if (target == Target::None)
        {
            return false;
        }

        if (btn.num_clicks == 2)
        {
            // Only the axis double-clicked resets, so a careful distance is not thrown away by
            // re-centring the height.
            mDragging = Target::None;

            if (target == Target::Arc)
            {
                mPosition.elevation = 0.0;
            }
            else
            {
                mPosition.azimuth = 0.0;
                mPosition.distance = kDistanceDefault;
            }

            Commit(ctx);
            return true;
        }

        mDragging = target;
        ApplyDrag(ctx, btn.pos, fine, false);
        return true;
    }

    if (mDragging != Target::None)
    {
        ApplyDrag(ctx, btn.pos, fine, true);
        mDragging = Target::None;
        ctx.view.refresh(ctx);
    }

    return true;
}

void SpatialPanner::drag(const el::context& ctx, el::mouse_button btn)
{
    if (mDragging != Target::None)
    {
        ApplyDrag(ctx, btn.pos, (btn.modifiers & el::mod_shift) != 0, false);
    }
}

bool SpatialPanner::cursor(const el::context& ctx, el::point p, el::cursor_tracking status)
{
    const auto target =
        status == el::cursor_tracking::leaving ? Target::None : HitTest(ComputeLayout(ctx.bounds), p);

    // A drag keeps its highlight wherever the pointer goes.
    if (mDragging == Target::None && target != mHover)
    {
        mHover = target;
        ctx.view.refresh(ctx);
    }

    if (target != Target::None)
    {
        el::set_cursor(el::cursor_type::hand);
    }

    return target != Target::None;
}

bool SpatialPanner::key(const el::context& ctx, el::key_info k)
{
    using el::key_code;

    if ((k.action != el::key_action::press && k.action != el::key_action::repeat) ||
        (k.modifiers & (el::mod_control | el::mod_super)) != 0 || mDragging != Target::None)
    {
        return false;
    }

    const bool fine = (k.modifiers & el::mod_shift) != 0;
    const bool alt = (k.modifiers & el::mod_alt) != 0;
    const double step = fine ? 1.0 : 5.0;
    const double distanceStep = fine ? 0.05 : 0.25;
    auto& p = mPosition;

    switch (k.key)
    {
    case key_code::left:
        p.azimuth = WrapDegrees(p.azimuth - step);
        break;
    case key_code::right:
        p.azimuth = WrapDegrees(p.azimuth + step);
        break;
    case key_code::up:
        if (alt)
        {
            p.distance = std::clamp(p.distance - distanceStep, kDistanceMin, kDistanceMax);
        }
        else
        {
            p.elevation = std::clamp(p.elevation + step, -90.0, 90.0);
        }
        break;
    case key_code::down:
        if (alt)
        {
            p.distance = std::clamp(p.distance + distanceStep, kDistanceMin, kDistanceMax);
        }
        else
        {
            p.elevation = std::clamp(p.elevation - step, -90.0, 90.0);
        }
        break;
    case key_code::home:
        p = SpatialPoint{};
        break;
    default:
        return false;
    }

    Commit(ctx);
    return true;
}

AccessInfo SpatialPanner::Describe() const
{
    // The web UI's aria-label for the canvas.
    const auto& p = mPosition;
    const char* side = std::abs(p.azimuth) < 1.0 ? "centre" : p.azimuth > 0.0 ? "right" : "left";
    const char* depth = std::abs(p.azimuth) > 90.0 ? "behind" : "in front";
    const char* height = std::abs(p.elevation) < 1.0 ? "at ear level" : p.elevation > 0.0 ? "above" : "below";
    char text[160];
    std::snprintf(text, sizeof(text), "%lld degrees %s, %s, %s by %lld degrees, %.2f metres away",
                  JsRound(std::abs(p.azimuth)), side, depth, height, JsRound(std::abs(p.elevation)), p.distance);

    AccessInfo info;
    info.role = AccessRole::Slider;
    info.name = NameOr("Spatial position");
    info.value = text;
    return info;
}

// ── SpatialCard ─────────────────────────────────────────────────────────────

float SpatialCard::HeightFor(const Theme&, float width)
{
    // Border and padding, the header's line, the gap, the panner, the gap, the help's lines
    // (10 px at a line height of 1.4), padding and border.
    return kPannerTop + SpatialPanner::kHeight + 6.0f +
           14.0f * static_cast<float>(HelpLines(width - 2.0f * kSide)) + 10.0f + 1.0f;
}

void SpatialCard::draw(const el::context& ctx)
{
    NoteDrawn(ctx);
    auto& cnv = ctx.canvas;
    const auto b = ctx.bounds;

    // .spatial-visualizer: fixed colours in every theme, as the web UI's.
    cnv.fill_style(Rgba(0, 0, 0, 0.2f));
    cnv.begin_path();
    cnv.add_round_rect(b, 6.0f);
    cnv.fill();
    cnv.stroke_style(Rgba(0x4a, 0x4a, 0x58, 1.0f));
    cnv.line_width(1.0f);
    cnv.begin_path();
    cnv.add_round_rect(b.inset(0.5f, 0.5f), 5.5f);
    cnv.stroke();

    // The header: the title in capitals on the left, the listening hint on the right.
    const auto family = mTheme.Controls().body.family;
    const ControlText title{0xFF9AA0B5, 9.0f, 600, 0.5f / 9.0f, true, family};
    const ControlText hint{0xFF7D8398, 9.0f, 500, 0.3f / 9.0f, false, family};
    const el::rect header{b.left + kSide, b.top + 11.0f, b.right - kSide, b.top + 11.0f + kLabelLine};
    const std::string hintText =
        mPanner->Speakers() ? "Speaker mode \xE2\x80\x94 height and behind are reduced" : "Best on headphones";
    const float hintWidth = std::min(header.width() * 0.7f, MeasureControlText(hintText, hint));
    DrawControlText(cnv, hintText, hint, el::rect{header.right - hintWidth, header.top, header.right, header.bottom},
                    HAlign::Right);
    DrawControlText(cnv, "Source Position", title,
                    el::rect{header.left, header.top, header.right - hintWidth - 8.0f, header.bottom}, HAlign::Left);

    // The help under the panner, as many lines as it needs.
    constexpr float kLine = 14.0f;
    const auto font = HelpFont();
    const float top = b.top + kPannerTop + SpatialPanner::kHeight + 6.0f;
    const int room = std::max(1, static_cast<int>((b.bottom - 11.0f - top) / kLine));
    const auto lines = WrapLines(cnv, kHelp, font, b.width() - 2.0f * kSide, room);

    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        const float y = top + kLine * static_cast<float>(i);
        FillText(cnv, lines[i], font, Rgba(0x7d, 0x83, 0x98, 1.0f),
                 el::rect{b.left + kSide, y, b.right - kSide, y + kLine}, HAlign::Left);
    }
}
} // namespace nanoq::ui
