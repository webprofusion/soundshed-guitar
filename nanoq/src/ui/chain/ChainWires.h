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
    Connector(const Theme& theme, std::function<void()> onAdd, bool stretch = false)
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

    bool Activate(const el::rect&) override
    {
        if (!mOnAdd)
        {
            return false;
        }

        const auto add = mOnAdd;
        add();
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
        NoteDrawn(ctx);
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
            const auto hot = el::rgba(72, 224, 120, 255);
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
        const auto route = dropHere ? mTheme.Accent() : mLive ? el::rgba(72, 224, 120, 255) : CableColour(mTheme);

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

        if (mHover)
        {
            // The add button (.signal-add-btn): 24 px, raised, a bold +.
            auto state = cnv.new_state();
            artist::canvas::linear_gradient fill{el::point{c.x, c.y - 11.0f}, el::point{c.x, c.y + 11.0f}};
            fill.add_color_stop(0.0f, mTheme.Pick(el::rgba(58, 58, 68, 255), mTheme.Elevated()));
            fill.add_color_stop(1.0f, mTheme.Pick(el::rgba(32, 32, 42, 255), mTheme.Card()));
            cnv.fill_style(fill);
            cnv.add_circle(artist::circle(c, 11.0f));
            cnv.fill();
            cnv.stroke_style(mTheme.Pick(el::rgba(74, 74, 88, 255), mTheme.BorderStrong()));
            cnv.line_width(2.0f);
            cnv.add_circle(artist::circle(c, 11.0f));
            cnv.stroke();
            cnv.stroke_style(mTheme.TextSecondary());
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
            mOnAdd();
        }

        return HitsPlus(ctx, btn.pos);
    }

    bool cursor(const el::context& ctx, el::point p, el::cursor_tracking status) override
    {
        const bool hover = mOnAdd && status != el::cursor_tracking::leaving && HitsPlus(ctx, p);

        if (hover != mHover)
        {
            mHover = hover;
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
    std::function<void()> mOnAdd;
    bool mStretch;
    bool mHover = false;
    std::shared_ptr<HitMap> mHits;
    std::optional<guitarfx::GraphEdge> mEdge;
    float mRowHeight = kCardHeight;
};

/// Where a parallel block splits (or joins): the cable from the main line meets a rail that
/// reaches each branch, with the split (join) icon on it. The split's icon selects the splitter,
/// which is where its Branches control is.
class Rail final : public el::element, public Focusable
{
  public:
    Rail(const Theme& theme, Icons& icons, int lanes, bool join, bool selected, std::function<void()> onSelect)
        : mTheme(theme), mIcons(icons), mLanes(lanes), mJoin(join), mSelected(selected), mOnSelect(std::move(onSelect))
    {
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info;
        info.role = AccessRole::ListItem;
        info.name = NameOr(mJoin ? "Mix" : "Split");
        info.value = std::to_string(mLanes) + " lanes";
        info.selected = mSelected;
        return info;
    }

    bool Activate(const el::rect&) override
    {
        if (!mOnSelect)
        {
            return false;
        }

        const auto select = mOnSelect;
        select();
        return true;
    }

    bool wants_focus() const override
    {
        return FocusWanted(mOnSelect != nullptr);
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

    /// Each lane's height: the cards' (kCardHeight), or the tiles' (kIconRowHeight).
    void RowHeight(float height)
    {
        mRowHeight = height;
    }

    /// Lanes of their own heights, top first: a lane holding a nested split is as tall as its
    /// branches. Unset, every lane is RowHeight.
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
        const float mid = b.top + b.height() * 0.5f;
        const float x = mJoin ? b.left + 8.0f : b.right - 8.0f; // the rail
        const float outside = mJoin ? b.right : b.left;
        const float inside = mJoin ? b.left : b.right;

        cnv.stroke_style(CableColour(mTheme).opacity(0.55f));
        cnv.line_width(3.0f);
        cnv.line_join(artist::canvas::round_join);
        cnv.begin_path();
        cnv.move_to(outside, mid);
        cnv.line_to(x, mid);
        // Each lane's middle, where its cable runs.
        std::vector<float> middles;
        float laneTop = top;

        for (int i = 0; i < mLanes; ++i)
        {
            const float h = LaneHeight(i);
            middles.push_back(laneTop + h * 0.5f);
            laneTop += h + kLaneGap;
        }

        if (!middles.empty())
        {
            cnv.move_to(x, middles.front());
            cnv.line_to(x, middles.back());
        }

        for (const float y : middles)
        {
            cnv.move_to(x, y);
            cnv.line_to(inside, y);
        }

        cnv.stroke();

        const el::point c{x, mid};
        cnv.fill_style(mSelected ? mTheme.Elevated() : mTheme.Card());
        cnv.add_circle(artist::circle(c, 11.0f));
        cnv.fill();
        cnv.stroke_style(mSelected ? mTheme.Accent() : (mHover && mOnSelect ? mTheme.Text() : mTheme.BorderStrong()));
        cnv.line_width(mSelected ? 2.0f : 1.0f);
        cnv.add_circle(artist::circle(c, 11.0f));
        cnv.stroke();
        mIcons.Draw(cnv, mJoin ? "parallel-join" : "parallel-split",
                    el::rect{c.x - 7.0f, c.y - 7.0f, c.x + 7.0f, c.y + 7.0f}, mTheme.TextSecondary());
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (!mOnSelect || btn.state != el::mouse_button::left)
        {
            return false;
        }

        if (!btn.down)
        {
            mOnSelect();
        }

        return true;
    }

    bool cursor(const el::context& ctx, el::point, el::cursor_tracking status) override
    {
        const bool hover = status != el::cursor_tracking::leaving;

        if (hover != mHover)
        {
            mHover = hover;
            ctx.view.refresh(ctx);
        }

        return false;
    }

    bool wants_control() const override
    {
        return static_cast<bool>(mOnSelect);
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

    const Theme& mTheme;
    Icons& mIcons;
    int mLanes;
    float mRowHeight = kCardHeight;
    std::vector<float> mLaneHeights;
    bool mJoin, mSelected;
    std::function<void()> mOnSelect;
    bool mHover = false;
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
        cnv.move_to(from, b.top);
        cnv.arc_to(el::point{from, mid}, el::point{to, mid}, 6.0f);
        cnv.arc_to(el::point{to, mid}, el::point{to, b.bottom}, 6.0f);
        cnv.line_to(to, b.bottom);
        cnv.stroke();
    }

  private:
    const Theme& mTheme;
    float mFrom, mTo;
};
} // namespace nanoq::ui
