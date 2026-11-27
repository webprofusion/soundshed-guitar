#pragma once

// What joins the cards of the chain strip (ChainStrip.cpp): the cable between two cards with its +,
// the rails where a parallel block splits and joins, and the cable that runs back under a wrapped
// line. Each paints from the theme; the strip decides where they go.

#include "ui/chain/NodeCard.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nanoq::ui
{
/// A cable between two things in a row. With `onAdd` it carries a + in its middle that inserts
/// an effect there; `stretch` lets it run on to fill a short branch.
class Connector final : public el::element, public Drawn, public Focusable
{
  public:
    /// `onAdd` is told where the + is (view coordinates): the chooser opens from it.
    Connector(const Theme& theme, std::function<void(el::rect plus)> onAdd, bool stretch = false)
        : mTheme(theme), mOnAdd(std::move(onAdd)), mStretch(stretch)
    {
    }

    // Its + is a button: add an effect here.
    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info;
        info.name = NameOr("Add effect here");
        return info;
    }

    bool Activate(const el::rect& at) override
    {
        if (!mOnAdd)
        {
            return false;
        }

        const auto add = mOnAdd;
        add(PlusRect(at));
        return true;
    }

    bool wants_focus() const override
    {
        return FocusWanted(mOnAdd != nullptr);
    }

    void begin_focus(focus_request) override
    {
        NoteFocus(true);
    }

    bool end_focus() override
    {
        NoteFocus(false);
        return true;
    }

    bool key(const el::context& ctx, el::key_info k) override
    {
        return IsPressKey(k) && Activate(ctx.bounds);
    }

    /// The graph edge this cable draws, so a dragged card can be dropped on it.
    void Edge(std::shared_ptr<HitMap> hits, guitarfx::GraphEdge edge)
    {
        mHits = std::move(hits);
        mEdge = std::move(edge);
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{kConnectorWidth, mRowHeight}, {mStretch ? el::full_extent : kConnectorWidth, el::full_extent}};
    }

    /// The row it sits in: the cards' (kCardHeight), or the tiles' (kIconRowHeight).
    void RowHeight(float height)
    {
        mRowHeight = height;
    }

    /// A parallel branch's cable: its route, live or not, in the branch's colour (parallel.css
    /// sets --signal-route-color and --signal-route-hot to it).
    void Tint(el::color colour)
    {
        mTint = colour;
    }

    /// The node whose signal this cable carries (its edge's source), or "" for a drawing-only one.
    [[nodiscard]] std::string Source() const
    {
        return mEdge ? mEdge->from : std::string();
    }

    /// Lights the cable with the signal (uiclient's SignalFlow): green, with pulses running down
    /// it, `phase` (0..1) along their 18 px spacing. Returns true when it needs a redraw.
    bool SetFlow(bool live, float phase)
    {
        const bool changed = live != mLive || (live && phase != mPhase);
        mLive = live;
        mPhase = phase;
        return changed;
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx, ctx.bounds.inset(-14.0f, -4.0f)); // the hovered + and its glow reach past it
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const float y = b.top + b.height() * 0.5f;

        if (mHits && mEdge)
        {
            mHits->NoteCable(*mEdge, b);
        }

        const bool dropHere = mHits && mEdge && mHits->hoverEdge && *mHits->hoverEdge == *mEdge;

        // A live cable (.signal-live): the hot route colour with a glow, and bright pulses
        // running toward what it feeds.
        if (mLive && !dropHere)
        {
            const auto hot = mTint.value_or(el::rgba(72, 224, 120, 255));
            auto state = cnv.new_state();
            cnv.line_cap(artist::canvas::round);
            cnv.stroke_style(hot.opacity(0.25f));
            cnv.line_width(7.0f);
            cnv.begin_path();
            cnv.move_to(b.left + 1.5f, y);
            cnv.line_to(b.right - 1.5f, y);
            cnv.stroke();
            cnv.stroke_style(hot);
            cnv.line_width(3.0f);
            cnv.begin_path();
            cnv.move_to(b.left + 1.5f, y);
            cnv.line_to(b.right - 1.5f, y);
            cnv.stroke();
            cnv.add_rect(el::rect{b.left + 1.5f, y - 2.0f, b.right - 1.5f, y + 2.0f});
            cnv.clip();
            constexpr float kSpacing = 18.0f;

            for (float x = b.left - kSpacing + mPhase * kSpacing; x < b.right; x += kSpacing)
            {
                artist::canvas::linear_gradient pulse{el::point{x, y}, el::point{x + kSpacing, y}};
                pulse.add_color_stop(0.0f, hot.opacity(0.0f));
                pulse.add_color_stop(0.5f, el::rgba(220, 255, 230, 230));
                pulse.add_color_stop(1.0f, hot.opacity(0.0f));
                cnv.fill_style(pulse);
                cnv.fill_rect(el::rect{x, y - 2.0f, x + kSpacing, y + 2.0f});
            }
        }

        // The cable (.signal-connector: 3 px, the route colour at 55%) with an arrowhead into
        // what it feeds, in the full colour; the accent where a dragged card would land.
        const auto route = dropHere ? mTheme.Accent()
                           : mTint  ? *mTint
                           : mLive  ? el::rgba(72, 224, 120, 255)
                                    : CableColour(mTheme);

        if (!mLive || dropHere)
        {
            cnv.stroke_style(route.opacity(0.55f));
            cnv.line_width(3.0f);
            cnv.line_cap(artist::canvas::round);
            cnv.begin_path();
            cnv.move_to(b.left + 1.5f, y);
            cnv.line_to(b.right - 1.5f, y);
            cnv.stroke();
        }

        cnv.stroke_style(route);
        cnv.line_width(1.5f);
        cnv.begin_path();
        cnv.move_to(b.right - 4.5f, y - 3.5f);
        cnv.line_to(b.right - 1.0f, y);
        cnv.line_to(b.right - 4.5f, y + 3.5f);
        cnv.stroke();

        if (!mOnAdd)
        {
            return;
        }

        const el::point c = PlusCentre(b);

        if (mHover || mHoverCable)
        {
            // The add button (.signal-add-btn) comes up while the cable is hovered: 24 px, raised,
            // a bold +. Under the pointer itself it grows a tenth into the accent's gradient,
            // glowing, its + white.
            const float r = mHover ? 11.0f * 1.1f : 11.0f;
            auto state = cnv.new_state();
            artist::canvas::linear_gradient fill{el::point{c.x, c.y - r}, el::point{c.x, c.y + r}};

            if (mHover)
            {
                cnv.shadow_style(el::point{0.0f, 0.0f}, 12.0f, el::rgba(224, 120, 72, 128));
                fill.add_color_stop(0.0f, mTheme.Accent());
                fill.add_color_stop(1.0f, el::rgba(200, 88, 48, 255));
            }
            else
            {
                fill.add_color_stop(0.0f, mTheme.DarkOr(el::rgba(58, 58, 68, 255), mTheme.ButtonBg()));
                fill.add_color_stop(1.0f, mTheme.Surface());
            }

            cnv.fill_style(fill);
            cnv.add_circle(artist::circle(c, r));
            cnv.fill();
            cnv.shadow_style(el::point{0.0f, 0.0f}, 0.0f, el::rgba(0, 0, 0, 0));
            cnv.stroke_style(mHover ? mTheme.Accent()
                                    : mTheme.DarkOr(el::rgba(74, 74, 88, 255), mTheme.ButtonBorder()));
            cnv.line_width(2.0f);
            cnv.add_circle(artist::circle(c, r));
            cnv.stroke();
            cnv.stroke_style(mHover ? el::rgba(255, 255, 255, 255) : mTheme.TextSecondary());
            cnv.line_width(2.0f);
            cnv.begin_path();
            cnv.move_to(c.x - 4.5f, c.y);
            cnv.line_to(c.x + 4.5f, c.y);
            cnv.move_to(c.x, c.y - 4.5f);
            cnv.line_to(c.x, c.y + 4.5f);
            cnv.stroke();
            return;
        }

        // At rest, a small ringed + on the cable (.signal-connector-wrapper::after).
        cnv.fill_style(Mix(mTheme.Bar(), el::rgba(0, 0, 0, 0), 0.72f));
        cnv.add_circle(artist::circle(c, 5.5f));
        cnv.fill();
        cnv.stroke_style(route.opacity(0.68f * 0.7f));
        cnv.line_width(1.0f);
        cnv.add_circle(artist::circle(c, 4.5f));
        cnv.stroke();
        cnv.stroke_style(route.opacity(0.7f));
        cnv.begin_path();
        cnv.move_to(c.x - 2.5f, c.y);
        cnv.line_to(c.x + 2.5f, c.y);
        cnv.move_to(c.x, c.y - 2.5f);
        cnv.line_to(c.x, c.y + 2.5f);
        cnv.stroke();
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (!mOnAdd || btn.state != el::mouse_button::left)
        {
            return false;
        }

        if (!btn.down && HitsPlus(ctx, btn.pos))
        {
            const auto add = mOnAdd;
            add(PlusRect(ctx.bounds));
        }

        return HitsPlus(ctx, btn.pos);
    }

    bool cursor(const el::context& ctx, el::point p, el::cursor_tracking status) override
    {
        const bool inside = mOnAdd && status != el::cursor_tracking::leaving && !metrics::Touch();
        const bool hover = mOnAdd && status != el::cursor_tracking::leaving && HitsPlus(ctx, p);
        const bool cable = inside && ctx.bounds.includes(p);

        if (hover != mHover || cable != mHoverCable)
        {
            mHover = hover;
            mHoverCable = cable;
            ctx.view.refresh(ctx);
        }

        return false;
    }

    bool wants_control() const override
    {
        return static_cast<bool>(mOnAdd);
    }

  private:
    bool mLive = false;
    float mPhase = 0.0f;

    /// Where the + sits: two fifths along the cable (--signal-add-anchor-x: 40%).
    static el::point PlusCentre(const el::rect& b)
    {
        return el::point{b.left + kConnectorWidth * 0.4f, b.top + b.height() * 0.5f};
    }

    static el::rect PlusRect(const el::rect& b)
    {
        const auto c = PlusCentre(b);
        return el::rect{c.x - 11.0f, c.y - 11.0f, c.x + 11.0f, c.y + 11.0f};
    }

    bool HitsPlus(const el::context& ctx, el::point p) const
    {
        const auto c = PlusCentre(ctx.bounds);

        // With touch sizes, the cable's whole width over 36 px of its height.
        if (metrics::Touch())
        {
            return p.x >= ctx.bounds.left && p.x <= ctx.bounds.left + kConnectorWidth && std::abs(p.y - c.y) <= 18.0f;
        }

        return std::abs(p.x - c.x) <= 14.0f && std::abs(p.y - c.y) <= 14.0f;
    }

    const Theme& mTheme;
    std::function<void(el::rect)> mOnAdd;
    bool mStretch;
    bool mHover = false, mHoverCable = false;
    std::shared_ptr<HitMap> mHits;
    std::optional<guitarfx::GraphEdge> mEdge;
    float mRowHeight = kCardHeight;
    std::optional<el::color> mTint;
};

/// Where a parallel block splits (or joins), between the splitter's (mixer's) card and the box of
/// branches: each branch's route in its colour at 72% (parallel.css), out of the card's edge and
/// round a 12 px elbow into the branch. The outer two meet their neighbours halfway across the
/// gap; a middle one is a T. They reach on past the rail, over the box's margin and padding, to
/// where the branch's first (last) cable starts.
class Rail final : public el::element
{
  public:
    Rail(int lanes, bool join) : mLanes(lanes), mJoin(join)
    {
    }

    /// Each lane's height: the cards' (kCardHeight), or the tiles' (kIconRowHeight).
    void RowHeight(float height)
    {
        mRowHeight = height;
    }

    /// Lanes of their own heights, top first, with each branch's padding: a lane holding a nested
    /// split is as tall as its branches. Unset, every lane is RowHeight.
    void LaneHeights(std::vector<float> heights)
    {
        mLaneHeights = std::move(heights);
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        const float h = Height();
        return {{kRailWidth, h}, {kRailWidth, el::full_extent}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const float top = b.top + std::max(0.0f, (b.height() - Height()) * 0.5f);
        const float x = mJoin ? b.right - 1.5f : b.left + 1.5f; // the spine, at the card's edge
        const float inside = mJoin ? b.left - kBranchReach : b.right + kBranchReach;

        // Each lane's middle, where its route runs, and the middle of the gap under it.
        std::vector<float> middles, gaps;
        float laneTop = top;

        for (int i = 0; i < mLanes; ++i)
        {
            const float h = LaneHeight(i);
            middles.push_back(laneTop + h * 0.5f);
            gaps.push_back(laneTop + h + kLaneGap * 0.5f);
            laneTop += h + kLaneGap;
        }

        auto state = cnv.new_state();
        cnv.line_width(3.0f);
        cnv.line_cap(artist::canvas::butt);
        cnv.line_join(artist::canvas::round_join);

        for (int i = 0; i < mLanes; ++i)
        {
            const float m = middles[static_cast<std::size_t>(i)];
            cnv.stroke_style(BranchColour(i).opacity(0.72f));
            cnv.begin_path();

            if (mLanes == 1)
            {
                cnv.move_to(mJoin ? b.right : b.left, m);
                cnv.line_to(inside, m);
            }
            else if (i == 0 || i == mLanes - 1)
            {
                // An elbow from the spine, halfway into the gap toward its neighbour.
                const float end = i == 0 ? gaps.front() : gaps[static_cast<std::size_t>(i - 1)];
                const float r = std::min(12.0f, std::abs(end - m));
                cnv.move_to(x, end);
                cnv.arc_to(el::point{x, m}, el::point{inside, m}, r);
                cnv.line_to(inside, m);
            }
            else
            {
                cnv.move_to(x, gaps[static_cast<std::size_t>(i - 1)]);
                cnv.line_to(x, gaps[static_cast<std::size_t>(i)]);
                cnv.move_to(x, m);
                cnv.line_to(inside, m);
            }

            cnv.stroke();
        }
    }

  private:
    [[nodiscard]] float LaneHeight(int lane) const
    {
        return lane < static_cast<int>(mLaneHeights.size()) ? mLaneHeights[static_cast<std::size_t>(lane)] : mRowHeight;
    }

    [[nodiscard]] float Height() const
    {
        float height = static_cast<float>(std::max(0, mLanes - 1)) * kLaneGap;

        for (int i = 0; i < mLanes; ++i)
        {
            height += LaneHeight(i);
        }

        return height;
    }

    int mLanes;
    bool mJoin;
    float mRowHeight = kCardHeight;
    std::vector<float> mLaneHeights;
};

/// The box the branches sit in (.parallel-branches): a faint wash and a dashed border of the
/// medium border colour at 55%, radius 8.
class BranchBox final : public el::element
{
  public:
    explicit BranchBox(const Theme& theme) : mTheme(theme)
    {
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds.inset(0.5f, 0.5f);
        cnv.fill_style(el::rgba(90, 90, 104, 20));
        cnv.add_round_rect(b, 7.5f);
        cnv.fill();
        cnv.stroke_style(mTheme.BorderStrong().opacity(0.55f));
        cnv.line_width(1.0f);
        StrokeDashedRoundRect(cnv, b, 7.5f, 4.0f, 3.0f);
    }

  private:
    const Theme& mTheme;
};

/// One branch's row (.parallel-branch): a wash from its colour at the left fading right, and a
/// border of its colour at 12% over white at 6%, radius 6.
class BranchRow final : public el::element
{
  public:
    explicit BranchRow(int branch) : mColour(BranchColour(branch))
    {
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds.inset(0.5f, 0.5f);
        const auto base = el::rgba(46, 46, 56, 255);
        artist::canvas::linear_gradient wash{el::point{b.left, b.top}, el::point{b.right, b.top}};
        wash.add_color_stop(0.0f, Mix(mColour, base, 0.05f).opacity(0.28f));
        wash.add_color_stop(0.42f, base.opacity(0.18f));
        wash.add_color_stop(1.0f, base.opacity(0.12f));
        cnv.fill_style(wash);
        cnv.add_round_rect(b, 6.0f);
        cnv.fill();
        const auto white = el::rgba(255, 255, 255, 255);
        cnv.stroke_style(Mix(mColour.opacity(1.0f), white.opacity(0.06f), 0.12f));
        cnv.line_width(1.0f);
        cnv.add_round_rect(b, 6.0f);
        cnv.stroke();
    }

  private:
    el::color mColour;
};

/// The end of a wrapped line's cable: in from the left and down (`out`), or in from above and
/// on to the right (the next line's start).
class WireEnd final : public el::element
{
  public:
    WireEnd(const Theme& theme, bool out) : mTheme(theme), mOut(out)
    {
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{kWireEndWidth, 0.0f}, {kWireEndWidth, el::full_extent}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const float mid = b.top + b.height() * 0.5f;
        const float x = b.left + kWireEndWidth * 0.5f;
        cnv.stroke_style(CableColour(mTheme).opacity(0.55f));
        cnv.line_width(3.0f);
        cnv.begin_path();

        if (mOut)
        {
            cnv.move_to(b.left, mid);
            cnv.arc_to(el::point{x, mid}, el::point{x, b.bottom}, 6.0f);
            cnv.line_to(x, b.bottom);
        }
        else
        {
            cnv.move_to(x, b.top);
            cnv.arc_to(el::point{x, mid}, el::point{b.right, mid}, 6.0f);
            cnv.line_to(b.right, mid);
        }

        cnv.stroke();
    }

  private:
    const Theme& mTheme;
    bool mOut;
};

/// The cable back under a line: down from where the line ended, across, and down into the next.
class ReturnRoute final : public el::element
{
  public:
    ReturnRoute(const Theme& theme, float fromX, float toX) : mTheme(theme), mFrom(fromX), mTo(toX)
    {
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{0.0f, kRouteHeight}, {el::full_extent, kRouteHeight}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const float mid = b.top + b.height() * 0.5f;
        const float from = b.left + mFrom, to = b.left + mTo;
        cnv.stroke_style(CableColour(mTheme).opacity(0.55f));
        cnv.line_width(3.0f);
        cnv.begin_path();
        const float bend = std::min(6.0f, b.height() * 0.5f);
        cnv.move_to(from, b.top);
        cnv.arc_to(el::point{from, mid}, el::point{to, mid}, bend);
        cnv.arc_to(el::point{to, mid}, el::point{to, b.bottom}, bend);
        cnv.line_to(to, b.bottom);
        cnv.stroke();
    }

  private:
    const Theme& mTheme;
    float mFrom, mTo;
};
} // namespace nanoq::ui
