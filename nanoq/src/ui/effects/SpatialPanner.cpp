// SpatialPanner.cpp - the 3D Spatial node's panner (see SpatialPanner.h).

#include "ui/effects/SpatialPanner.h"

#include "ui/widgets/TextDraw.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace nanoq::ui
{
namespace
{
constexpr double kDeg = 3.14159265358979323846 / 180.0;
constexpr double kDistanceMin = 0.2;
constexpr double kDistanceMax = 10.0;
constexpr std::size_t kTrailLength = 24;

// The web UI's --spatial-* defaults.
const el::color kSource = el::rgba(72, 168, 224, 242);
const el::color kSourceBehind = el::rgba(150, 122, 208, 217);

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

/// 0 dead ahead, 1 directly behind.
double Backness(double azimuth, double elevation)
{
    return 0.5 - 0.5 * std::cos(azimuth * kDeg) * std::cos(elevation * kDeg);
}

double Wrap(double degrees)
{
    while (degrees > 180.0)
    {
        degrees -= 360.0;
    }

    while (degrees <= -180.0)
    {
        degrees += 360.0;
    }

    return degrees;
}
} // namespace

el::view_limits SpatialPanner::limits(const el::basic_context&) const
{
    return {{240.0f, 160.0f}, {el::full_extent, el::full_extent}};
}

SpatialPanner::Layout SpatialPanner::ComputeLayout(const el::rect& b) const
{
    constexpr float pad = 10.0f;
    const float width = b.width(), height = b.height();
    const float arcPaneWidth = std::min(96.0f, std::max(60.0f, width * 0.26f));
    const float arcPaneLeft = b.left + width - arcPaneWidth;
    const float radarSize = std::min(arcPaneLeft - b.left - pad * 2.0f, height - pad * 2.0f);
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

void SpatialPanner::SetPosition(const SpatialPoint& position)
{
    if (mDragging == Target::None)
    {
        mPosition = position;
    }
}

void SpatialPanner::SetLive(const guitarfx::uiclient::SpatialPosition* live)
{
    if (live == nullptr || !live->moving)
    {
        mLive.reset();
        mTrail.clear();
        return;
    }

    mLive = SpatialPoint{live->azimuth, live->elevation, live->distance};
    mTrail.push_back(*mLive);

    while (mTrail.size() > kTrailLength)
    {
        mTrail.pop_front();
    }
}

void SpatialPanner::draw(const el::context& ctx)
{
    NoteDrawn(ctx);
    auto& cnv = ctx.canvas;
    const auto b = ctx.bounds;
    const auto L = ComputeLayout(b);
    const el::color grid = mTheme.Ink().opacity(0.10f);
    const el::color label = mTheme.Ink().opacity(0.62f);
    const el::color muted = mTheme.Ink().opacity(0.28f);
    const auto font = Font(Weight::SemiBold).size(9.0f);

    cnv.fill_style(mTheme.Pick(el::rgba(0, 0, 0, 70), el::rgba(255, 255, 255, 120)));
    cnv.add_round_rect(b, 6.0f);
    cnv.fill();

    const auto radarPoint = [&L](double azimuth, double distance) {
        const float r = L.radarInner + (L.radarOuter - L.radarInner) * static_cast<float>(DistanceToNorm(distance));
        return el::point{L.radarCx + r * static_cast<float>(std::sin(azimuth * kDeg)),
                         L.radarCy - r * static_cast<float>(std::cos(azimuth * kDeg))};
    };
    const auto arcPoint = [&L](double elevation) {
        const double a = std::clamp(elevation, -90.0, 90.0) * kDeg;
        return el::point{L.arcX + L.arcR * static_cast<float>(std::cos(a)),
                         L.arcCy - L.arcR * static_cast<float>(std::sin(a))};
    };

    // Distance rings, and the axes through the head.
    cnv.stroke_style(grid);
    cnv.line_width(1.0f);

    for (const double metres : {0.5, 1.0, 2.0, 5.0, 10.0})
    {
        const float r = L.radarInner + (L.radarOuter - L.radarInner) * static_cast<float>(DistanceToNorm(metres));
        cnv.add_circle(cycfi::artist::circle(el::point{L.radarCx, L.radarCy}, r));
        cnv.stroke();
        char text[16];
        std::snprintf(text, sizeof(text), "%gm", metres);

        if (r > 14.0f)
        {
            FillText(cnv, text, Font().size(8.0f), muted,
                     el::rect{L.radarCx + 3.0f, L.radarCy - r, L.radarCx + 40.0f, L.radarCy - r + 11.0f}, HAlign::Left);
        }
    }

    cnv.begin_path();
    cnv.move_to(L.radarCx - L.radarOuter, L.radarCy);
    cnv.line_to(L.radarCx + L.radarOuter, L.radarCy);
    cnv.move_to(L.radarCx, L.radarCy - L.radarOuter);
    cnv.line_to(L.radarCx, L.radarCy + L.radarOuter);
    cnv.stroke();

    // The head, nose up.
    cnv.fill_style(mTheme.Ink().opacity(0.32f));
    cnv.add_circle(cycfi::artist::circle(el::point{L.radarCx, L.radarCy}, std::max(5.0f, L.radarInner * 0.55f)));
    cnv.fill();
    FillText(cnv, "FRONT", font, label,
             el::rect{L.radarCx - 30.0f, L.radarCy - L.radarOuter - 11.0f, L.radarCx + 30.0f, L.radarCy - L.radarOuter},
             HAlign::Centre);
    FillText(cnv, mSpeakers ? "BEHIND *" : "BEHIND", font, mSpeakers ? muted : label,
             el::rect{L.radarCx - 30.0f, L.radarCy + L.radarOuter, L.radarCx + 30.0f, L.radarCy + L.radarOuter + 11.0f},
             HAlign::Centre);
    FillText(
        cnv, "L", font, label,
        el::rect{L.radarCx - L.radarOuter - 12.0f, L.radarCy - 6.0f, L.radarCx - L.radarOuter - 2.0f, L.radarCy + 6.0f},
        HAlign::Right);
    FillText(
        cnv, "R", font, label,
        el::rect{L.radarCx + L.radarOuter + 2.0f, L.radarCy - 6.0f, L.radarCx + L.radarOuter + 12.0f, L.radarCy + 6.0f},
        HAlign::Left);

    // The height arc: dimmed in speaker mode, where height is not delivered.
    const float arcAlpha = mSpeakers ? 0.45f : 1.0f;
    cnv.stroke_style(mTheme.Ink().opacity(0.18f * arcAlpha));
    cnv.begin_path();
    cnv.arc(L.arcX, L.arcCy, L.arcR, static_cast<float>(-90.0 * kDeg), static_cast<float>(90.0 * kDeg));
    cnv.stroke();
    FillText(cnv, "UP", font, label.opacity(0.62f * arcAlpha),
             el::rect{L.arcX + 6.0f, L.arcCy - L.arcR - 12.0f, L.arcX + 60.0f, L.arcCy - L.arcR}, HAlign::Left);
    FillText(cnv, "DOWN", font, label.opacity(0.62f * arcAlpha),
             el::rect{L.arcX + 6.0f, L.arcCy + L.arcR, L.arcX + 60.0f, L.arcCy + L.arcR + 12.0f}, HAlign::Left);

    const SpatialPoint shown = mLive.value_or(mPosition);
    const double back = Backness(shown.azimuth, shown.elevation);
    const el::color source = back > 0.5 ? kSourceBehind : kSource;

    // The trail of where it has been heard.
    for (std::size_t i = 0; i < mTrail.size(); ++i)
    {
        const float age = static_cast<float>(i + 1) / static_cast<float>(mTrail.size());
        cnv.fill_style(kSource.opacity(0.30f * age));
        cnv.add_circle(cycfi::artist::circle(radarPoint(mTrail[i].azimuth, mTrail[i].distance), 2.5f));
        cnv.fill();
    }

    // The set position as a ring when the heard one has moved off it.
    if (mLive)
    {
        cnv.stroke_style(mTheme.Ink().opacity(0.38f));
        cnv.line_width(1.5f);
        cnv.add_circle(cycfi::artist::circle(radarPoint(mPosition.azimuth, mPosition.distance), 7.0f));
        cnv.stroke();
    }

    // The source on the radar, with its shadow line to the head, and on the arc.
    const auto at = radarPoint(shown.azimuth, shown.distance);
    cnv.stroke_style(source.opacity(0.35f));
    cnv.line_width(1.0f);
    cnv.begin_path();
    cnv.move_to(L.radarCx, L.radarCy);
    cnv.line_to(at.x, at.y);
    cnv.stroke();
    cnv.fill_style(source);
    cnv.add_circle(cycfi::artist::circle(at, mDragging == Target::Radar ? 8.0f : 7.0f));
    cnv.fill();
    cnv.fill_style(source.opacity(source.alpha * arcAlpha));
    cnv.add_circle(cycfi::artist::circle(arcPoint(shown.elevation), mDragging == Target::Arc ? 7.0f : 6.0f));
    cnv.fill();

    char readout[64];
    std::snprintf(readout, sizeof(readout), "%+.0f\xC2\xB0  %+.0f\xC2\xB0  %.2f m", shown.azimuth, shown.elevation,
                  shown.distance);
    FillText(cnv, readout, Font(Weight::SemiBold).size(10.0f), label,
             el::rect{b.left + 8.0f, b.bottom - 16.0f, b.right - 8.0f, b.bottom - 3.0f}, HAlign::Right);
}

void SpatialPanner::MoveTo(const Layout& L, el::point p)
{
    if (mDragging == Target::Radar)
    {
        const double dx = p.x - L.radarCx, dy = L.radarCy - p.y;
        const double r = std::hypot(dx, dy);
        mPosition.azimuth = Wrap(std::atan2(dx, dy) / kDeg);
        mPosition.distance = NormToDistance((r - L.radarInner) / std::max(1.0f, L.radarOuter - L.radarInner));
    }
    else if (mDragging == Target::Arc)
    {
        const double dy = L.arcCy - p.y;
        mPosition.elevation =
            std::clamp(std::asin(std::clamp(dy / std::max(1.0f, L.arcR), -1.0, 1.0)) / kDeg, -90.0, 90.0);
    }
}

bool SpatialPanner::click(const el::context& ctx, el::mouse_button btn)
{
    if (btn.state != el::mouse_button::left)
    {
        return false;
    }

    const auto L = ComputeLayout(ctx.bounds);

    if (btn.down)
    {
        if (btn.num_clicks == 2)
        {
            mDragging = Target::None;
            mPosition = SpatialPoint{};

            if (onChange)
            {
                onChange(mPosition, true);
            }

            ctx.view.refresh(ctx);
            return true;
        }

        mDragging = btn.pos.x >= L.arcPaneLeft ? Target::Arc : Target::Radar;
        MoveTo(L, btn.pos);

        if (onChange)
        {
            onChange(mPosition, false);
        }

        ctx.view.refresh(ctx);
        return true;
    }

    if (mDragging != Target::None)
    {
        mDragging = Target::None;

        if (onChange)
        {
            onChange(mPosition, true);
        }

        ctx.view.refresh(ctx);
    }

    return true;
}

void SpatialPanner::drag(const el::context& ctx, el::mouse_button btn)
{
    if (mDragging == Target::None)
    {
        return;
    }

    MoveTo(ComputeLayout(ctx.bounds), btn.pos);

    if (onChange)
    {
        onChange(mPosition, false);
    }

    ctx.view.refresh(ctx);
}
} // namespace nanoq::ui
