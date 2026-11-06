#pragma once

// Areas and small marks that are drawn rather than pressed: the effect view's inset panels and
// header lights, an architecture badge, the stage's backdrop, and a probe for where something drew.

#include "ui/Drawn.h"
#include "ui/widgets/Graphics.h"
#include "ui/widgets/Layout.h"
#include "ui/widgets/Widgets.h"

#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace nanoq::ui
{
/// What it wraps, telling `onBounds` where it was drawn (view coordinates): for hanging a
/// drop-down from a control that does not record that itself.
class BoundsProbe final : public el::proxy_base
{
  public:
    BoundsProbe(el::element_ptr subject, std::function<void(el::rect)> onBounds)
        : mSubject(std::move(subject)), mOnBounds(std::move(onBounds))
    {
    }

    el::element& subject() override
    {
        return *mSubject;
    }

    const el::element& subject() const override
    {
        return *mSubject;
    }

    void draw(const el::context& ctx) override
    {
        if (mOnBounds)
        {
            mOnBounds(ctx.bounds);
        }

        proxy_base::draw(ctx);
    }

  private:
    el::element_ptr mSubject;
    std::function<void(el::rect)> mOnBounds;
};

/// A loaded model's architecture as a small green pill (.node-architecture-badge, "A2").
class ArchBadge final : public el::element
{
  public:
    /// `size` and `height` for a larger chip of the same look: the editor's Calibrated
    /// (.default-effect-shell-chip-calibration).
    explicit ArchBadge(std::string text, float size = 8.0f, float height = 13.0f)
        : mText(std::move(text)), mSize(size), mHeight(height)
    {
    }

    el::view_limits limits(const el::basic_context& ctx) const override
    {
        const float w = el::measure_text(ctx.canvas, mText, Font(Weight::SemiBold).size(mSize)).x + mHeight - 1.0f;
        return {{w, mHeight}, {w, el::full_extent}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const float cy = ctx.bounds.top + ctx.bounds.height() * 0.5f;
        const float half = mHeight * 0.5f;
        const el::rect pill{ctx.bounds.left, cy - half, ctx.bounds.right, cy + half};
        cnv.fill_style(el::rgba(56, 118, 98, 71));
        cnv.fill_round_rect(pill, half);
        cnv.stroke_style(el::rgba(128, 230, 188, 92));
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(pill.inset(0.5f, 0.5f), half);
        FillText(cnv, mText, Font(Weight::SemiBold).size(mSize), el::rgba(186, 255, 226, 255), pill, HAlign::Centre);
    }

  private:
    std::string mText;
    float mSize, mHeight;
};

/// The effect header's light (.default-effect-shell-led): green while signal passes, red when
/// it is quiet, grey when the effect is off.
class HeaderLed final : public el::element, public Drawn
{
  public:
    HeaderLed(bool on, bool live) : mOn(on), mLive(live)
    {
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{14.0f, 14.0f}, {14.0f, el::full_extent}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto c = cycfi::artist::center_point(ctx.bounds);
        const el::color colour = !mOn    ? el::rgba(110, 110, 124, 255)
                                 : mLive ? el::rgba(72, 224, 120, 255)
                                         : el::rgba(255, 92, 80, 255);
        auto state = cnv.new_state();

        if (mOn)
        {
            cnv.shadow_style(el::point{0.0f, 0.0f}, 6.0f, colour.opacity(0.7f));
        }

        cycfi::artist::canvas::radial_gradient dot{el::point{c.x - 1.0f, c.y - 1.0f}, 0.0f, c, 4.0f};
        dot.add_color_stop(0.0f, colour.level(1.3f));
        dot.add_color_stop(1.0f, colour.level(0.85f));
        cnv.fill_style(dot);
        cnv.add_circle(cycfi::artist::circle(c, 4.0f));
        cnv.fill();
    }

    bool Set(bool on, bool live)
    {
        if (on == mOn && live == mLive)
        {
            return false;
        }

        mOn = on;
        mLive = live;
        return true;
    }

  private:
    bool mOn, mLive;
};

/// The inset panels of the effect view (.default-effect-section, .node-param-group-block): a
/// dark gradient sunk into the surface, with a rim, rounded 10 px; a group's title, when it has
/// one, in small capitals at the top.
class InsetBox final : public el::element
{
  public:
    InsetBox(const Theme& theme, std::string title = {}) : mTheme(theme), mTitle(Upper(std::move(title)))
    {
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds.inset(0.5f, 0.5f);
        const bool light = mTheme.IsLight();
        {
            auto state = cnv.new_state();
            cycfi::artist::canvas::linear_gradient fill{b.top_left(), b.bottom_left()};
            fill.add_color_stop(0.0f, light ? el::rgba(0, 0, 0, 14) : el::rgba(4, 6, 11, 158));
            fill.add_color_stop(1.0f, light ? el::rgba(0, 0, 0, 8) : el::rgba(9, 11, 18, 107));
            cnv.fill_style(fill);
            cnv.fill_round_rect(b, 10.0f);
            // The inset shade along the top edge.
            cycfi::artist::canvas::linear_gradient shade{b.top_left(), el::point{b.left, b.top + 6.0f}};
            shade.add_color_stop(0.0f, el::rgba(0, 0, 0, light ? 20 : 90));
            shade.add_color_stop(1.0f, el::rgba(0, 0, 0, 0));
            cnv.fill_style(shade);
            cnv.fill_round_rect(b, 10.0f);
        }
        cnv.stroke_style(light ? el::rgba(0, 0, 0, 30) : el::rgba(0, 0, 0, 107));
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(b, 10.0f);
        cnv.stroke_style(el::rgba(255, 255, 255, light ? 60 : 13));
        cnv.begin_path();
        cnv.move_to(b.left + 10.0f, b.bottom + 0.5f);
        cnv.line_to(b.right - 10.0f, b.bottom + 0.5f);
        cnv.stroke();

        if (!mTitle.empty())
        {
            FillText(cnv, mTitle, Font(Weight::SemiBold).size(10.5f),
                     light ? mTheme.TextSecondary() : el::rgba(220, 227, 243, 200),
                     el::rect{b.left, b.top + 6.0f, b.right, b.top + 20.0f}, HAlign::Centre);
        }
    }

  private:
    const Theme& mTheme;
    std::string mTitle;
};

/// A plain filled area with a soft radial highlight at its top, as the chain stage's
/// (.signal-path-scroll's radial-gradient over --bg-secondary).
class StageBackdrop final : public el::element
{
  public:
    explicit StageBackdrop(const Theme& theme) : mTheme(theme)
    {
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        cnv.fill_style(mTheme.Bar());
        cnv.fill_rect(b);

        if (mTheme.IsLight())
        {
            return;
        }

        auto state = cnv.new_state();
        const auto c = cycfi::artist::center_point(b);
        cycfi::artist::canvas::radial_gradient glow{c, 0.0f, c, std::max(b.width(), b.height()) * 0.6f};
        glow.add_color_stop(0.0f, el::rgba(255, 255, 255, 13));
        glow.add_color_stop(1.0f, el::rgba(255, 255, 255, 0));
        cnv.fill_style(glow);
        cnv.fill_rect(b);
    }

  private:
    const Theme& mTheme;
};
} // namespace nanoq::ui
