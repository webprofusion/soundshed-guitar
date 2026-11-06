#pragma once

// The effect picker's rows, and the chip that follows an effect dragged out of it onto the chain.

#include "ui/widgets/Graphics.h"
#include "ui/widgets/Pressable.h"

#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace nanoq::ui
{
/// A category in the effect chooser's left column (.fx-selector-category): its icon in its
/// colour, its name in capitals, and how many effects it holds; the open one tinted.
class CategoryRow final : public Pressable
{
  public:
    CategoryRow(const Theme& theme, Icons& icons, std::string icon, std::string label, int count, el::color tint,
                bool active)
        : mTheme(theme), mIcons(icons), mIcon(std::move(icon)), mLabel(Upper(std::move(label))), mCount(count),
          mTint(tint), mActive(active)
    {
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.role = AccessRole::Tab;
        info.name = NameOr(mLabel);
        info.value = std::to_string(mCount);
        info.selected = mActive;
        return info;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{100.0f, 30.0f}, {el::full_extent, 30.0f}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;

        if (mActive || mHover)
        {
            cnv.fill_style(mActive ? mTheme.Accent().opacity(0.16f) : mTheme.Text().opacity(0.05f));
            cnv.fill_round_rect(b, 6.0f);

            if (mActive)
            {
                cnv.stroke_style(mTheme.Accent().opacity(0.5f));
                cnv.line_width(1.0f);
                cnv.stroke_round_rect(b.inset(0.5f, 0.5f), 6.0f);
            }
        }

        const float cy = b.top + b.height() * 0.5f;
        mIcons.Draw(cnv, mIcon, el::rect{b.left + 10.0f, cy - 8.0f, b.left + 26.0f, cy + 8.0f}, mTint);
        const auto font = Font(Weight::SemiBold).size(10.5f);
        FillText(cnv, mLabel, font, mActive ? mTheme.Text() : mTheme.TextSecondary(),
                 el::rect{b.left + 34.0f, b.top, b.right - 34.0f, b.bottom}, HAlign::Left);
        const auto countText = std::to_string(mCount);
        const auto countFont = Font(Weight::SemiBold).size(9.0f);
        const float w = std::max(18.0f, el::measure_text(cnv, countText, countFont).x + 10.0f);
        const el::rect pill{b.right - 8.0f - w, cy - 8.0f, b.right - 8.0f, cy + 8.0f};
        cnv.fill_style(mActive ? mTheme.Accent().opacity(0.35f)
                               : mTheme.Pick(el::rgba(255, 255, 255, 18), mTheme.Text().opacity(0.07f)));
        cnv.fill_round_rect(pill, 8.0f);
        FillText(cnv, countText, countFont, mTheme.TextSecondary(), pill, HAlign::Centre);
    }

  private:
    const Theme& mTheme;
    Icons& mIcons;
    std::string mIcon, mLabel;
    int mCount;
    el::color mTint;
    bool mActive;
};

/// An effect in the chooser's list (.fx-selector-item): its artwork or icon, its name and its
/// category, on a raised card; marked when it is the effect being replaced.
class EffectRow final : public Pressable
{
  public:
    EffectRow(const Theme& theme, Icons& icons, std::string icon, std::shared_ptr<cycfi::artist::image> thumb,
              std::string name, std::string category, el::color tint, bool current)
        : mTheme(theme), mIcons(icons), mIcon(std::move(icon)), mThumb(std::move(thumb)), mName(std::move(name)),
          mCategory(std::move(category)), mTint(tint), mCurrent(current)
    {
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.role = AccessRole::ListItem;
        info.name = NameOr(mName);
        info.value = mCategory + (mFavourite ? ", favourite" : "");
        info.selected = mCurrent;
        return info;
    }

    // F stars it, as a click on its star does.
    bool key(const el::context& ctx, el::key_info k) override
    {
        if (onStar && k.action == el::key_action::press && k.key == el::key_code::f && k.modifiers == 0)
        {
            onStar();
            return true;
        }

        return Pressable::key(ctx, k);
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{160.0f, 41.0f}, {el::full_extent, 41.0f}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        cnv.fill_style(mTheme.Pick(mHover ? el::rgba(40, 40, 50, 255) : el::rgba(30, 30, 38, 255),
                                   mHover ? mTheme.Elevated() : mTheme.Card()));
        cnv.fill_round_rect(b, 6.0f);
        cnv.stroke_style(mCurrent ? mTheme.Accent().opacity(0.6f)
                                  : mTheme.Pick(el::rgba(255, 255, 255, mHover ? 28 : 14), mTheme.Border()));
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(b.inset(0.5f, 0.5f), 6.0f);

        const el::rect picture{b.left + 10.0f, b.top + 7.0f, b.left + 46.0f, b.bottom - 7.0f};

        if (mThumb)
        {
            auto state = cnv.new_state();
            cnv.begin_path();
            cnv.add_round_rect(picture, 3.0f);
            cnv.clip();
            DrawImageCovering(cnv, *mThumb, picture);
        }
        else
        {
            const auto c = cycfi::artist::center_point(picture);
            mIcons.Draw(cnv, mIcon, el::rect{c.x - 9.0f, c.y - 9.0f, c.x + 9.0f, c.y + 9.0f}, mTint);
        }

        // The name stops short of the badge and the star.
        const float nameRight = b.right - (mBadge != Badge::None ? 54.0f : 38.0f) - (mCurrent ? 40.0f : 0.0f);
        const auto nameFont = Font(Weight::SemiBold).size(12.0f);
        FillText(cnv, Ellipsize(cnv, mName, nameFont, nameRight - b.left - 56.0f), nameFont, mTheme.Ink(),
                 el::rect{b.left + 56.0f, b.top + 6.0f, nameRight, b.top + 22.0f}, HAlign::Left);
        FillText(cnv, mCategory, Font(Weight::Regular).size(9.5f), mTheme.TextMuted(),
                 el::rect{b.left + 56.0f, b.top + 22.0f, b.right - 70.0f, b.bottom - 5.0f}, HAlign::Left);
        const float labelRight = onStar ? b.right - 34.0f : b.right - 12.0f;

        // Being dragged out: outlined in the accent, where it was.
        if (mDragging)
        {
            cnv.stroke_style(mTheme.Accent());
            cnv.stroke_round_rect(b.inset(0.5f, 0.5f), 6.0f);
        }

        // What it carries (.fx-item-badge): a folder for a model or IR, a box for a composite.
        if (mBadge != Badge::None && !mCurrent)
        {
            const float cy = b.top + b.height() * 0.5f;
            const el::rect r{labelRight - 16.0f, cy - 7.0f, labelRight - 2.0f, cy + 7.0f};

            if (mBadge == Badge::Resource)
            {
                mIcons.Draw(cnv, "ui-folder", r, mTheme.TextSecondary());
            }
            else
            {
                cnv.fill_style(mTheme.Accent().opacity(0.85f));
                cnv.fill_round_rect(r.inset(2.5f, 2.5f), 2.0f);
                cnv.stroke_style(mTheme.Ink().opacity(0.5f));
                cnv.line_width(1.0f);
                cnv.begin_path();
                cnv.move_to(r.left + 2.5f, r.top + 6.0f);
                cnv.line_to(r.right - 2.5f, r.top + 6.0f);
                cnv.stroke();
            }
        }

        if (mCurrent)
        {
            FillText(cnv, "CURRENT", Font(Weight::SemiBold).size(9.0f), mTheme.Accent(),
                     el::rect{labelRight - 58.0f, b.top, labelRight, b.bottom}, HAlign::Right);
        }

        // The star (.fx-favorite-toggle): the accent when starred, faint until hovered otherwise.
        if (onStar)
        {
            mIcons.Draw(cnv, "star", StarRect(b),
                        mFavourite ? mTheme.Accent()
                                   : (mHover ? mTheme.TextSecondary() : mTheme.TextMuted().opacity(0.6f)));
        }
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (onStar && btn.state == el::mouse_button::left && StarRect(ctx.bounds).inset(-6.0f, -6.0f).includes(btn.pos))
        {
            if (!btn.down)
            {
                onStar();
            }

            return true;
        }

        if (btn.state == el::mouse_button::left)
        {
            if (btn.down)
            {
                mDownAt = btn.pos;
                mDragging = false;
            }
            else if (mDragging)
            {
                // A drag ends where it is let go, not in a click.
                mDragging = false;
                mDown = false;
                ctx.view.refresh(ctx);

                if (onDrag)
                {
                    onDrag(mDownAt, btn.pos, true);
                }

                return true;
            }
        }

        return Pressable::click(ctx, btn);
    }

    void drag(const el::context& ctx, el::mouse_button btn) override
    {
        if (!mDown || !onDrag)
        {
            return;
        }

        if (!mDragging)
        {
            if (std::hypot(btn.pos.x - mDownAt.x, btn.pos.y - mDownAt.y) < 5.0f)
            {
                return;
            }

            mDragging = true;
            ctx.view.refresh(ctx);
        }

        onDrag(mDownAt, btn.pos, false);
    }

    /// Shows the star, filled in when `favourite`; a click on it calls `onStar`.
    void Star(bool favourite, std::function<void()> toggle)
    {
        mFavourite = favourite;
        onStar = std::move(toggle);
    }

    enum class Badge
    {
        None,
        Resource,  // it needs a model or an IR
        Composite, // a composite channel strip
    };

    void SetBadge(Badge badge)
    {
        mBadge = badge;
    }

    /// Dragged out (onto the chain): `onDrag(from, to, done)` in view coordinates. A drag is not
    /// a click.
    std::function<void(el::point from, el::point to, bool done)> onDrag;

  private:
    static el::rect StarRect(const el::rect& b)
    {
        const float cy = b.top + b.height() * 0.5f;
        return el::rect{b.right - 28.0f, cy - 8.0f, b.right - 12.0f, cy + 8.0f};
    }

    const Theme& mTheme;
    Icons& mIcons;
    std::string mIcon;
    std::shared_ptr<cycfi::artist::image> mThumb;
    std::string mName, mCategory;
    el::color mTint;
    bool mCurrent;
    bool mFavourite = false;
    std::function<void()> onStar;
    Badge mBadge = Badge::None;
    el::point mDownAt;
    bool mDragging = false;
};

/// What is being dragged, named in a chip by the pointer over everything: a layer of the view that
/// no click reaches (an effect dragged out of the FX Library onto the chain).
class DragGhost final : public el::element
{
  public:
    DragGhost(const Theme& theme, std::string label) : mTheme(theme), mLabel(std::move(label))
    {
    }

    void MoveTo(el::point at)
    {
        mAt = at;
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto font = Font(Weight::SemiBold).size(12.0f);
        const float w = MeasureTextWidth(mLabel, font) + 24.0f;
        const el::rect chip{mAt.x + 14.0f, mAt.y + 10.0f, mAt.x + 14.0f + w, mAt.y + 38.0f};
        {
            auto state = cnv.new_state();
            cnv.shadow_style(el::point{0.0f, 4.0f}, 12.0f, el::rgba(0, 0, 0, 120));
            cnv.fill_style(mTheme.Elevated());
            cnv.fill_round_rect(chip, 8.0f);
        }
        cnv.stroke_style(mTheme.Accent());
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(chip.inset(0.5f, 0.5f), 8.0f);
        FillText(cnv, mLabel, font, mTheme.Ink(), chip, HAlign::Centre);
    }

    el::element* hit_test(const el::context&, el::point, bool, bool) override
    {
        return nullptr;
    }

  private:
    const Theme& mTheme;
    std::string mLabel;
    el::point mAt;
};
} // namespace nanoq::ui
