#pragma once

// The pieces of the frame around the stage, drawn as the web UI's compact landscape layout draws
// them (css/compact/rail.css, css/control-bar/, css/footer.css): the rail's stage tabs, the preset
// name's pill, scene chips, the footer's A/B switch, the round + over the chain and the bar between
// the full layout's chain and editor.

#include "ui/Drawn.h"
#include "ui/widgets/Buttons.h"
#include "ui/widgets/Pressable.h"

#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace nanoq::ui
{
/// A stage on the rail: its icon over a small capital label, tinted with the accent when it is
/// the stage on show (.nav-tab.nav-rail-stage).
class RailTab final : public Pressable
{
  public:
    RailTab(const Theme& theme, Icons& icons, std::string icon, std::string label, bool active)
        : mTheme(theme), mIcons(icons), mIcon(std::move(icon)), mLabel(Upper(std::move(label))), mActive(active)
    {
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.role = AccessRole::Tab;
        info.name = NameOr(mLabel);
        info.selected = mActive;
        return info;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{49.0f, 46.0f}, {49.0f, 46.0f}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;

        if (mActive || mHover)
        {
            cnv.fill_style(mActive ? mTheme.Accent().opacity(0.18f) : mTheme.Text().opacity(0.05f));
            cnv.fill_round_rect(b, 8.0f);
        }

        const auto ink = mActive ? mTheme.Accent() : (mHover ? mTheme.TextSecondary() : mTheme.TextMuted());
        const float cx = (b.left + b.right) * 0.5f;
        mIcons.Draw(cnv, mIcon, el::rect{cx - 9.0f, b.top + 7.0f, cx + 9.0f, b.top + 25.0f}, ink);
        FillText(cnv, mLabel, Font(Weight::SemiBold).size(metrics::Readable(9.0f)), ink,
                 el::rect{b.left, b.top + 28.0f, b.right, b.top + 40.0f}, HAlign::Centre);
    }

  private:
    const Theme& mTheme;
    Icons& mIcons;
    std::string mIcon, mLabel;
    bool mActive;
};

/// The preset's name in its inset pill, with the favourite heart at its left
/// (.preset-selector). A tap on the heart toggles it; anywhere else opens the presets.
class PresetPill final : public Pressable
{
  public:
    PresetPill(const Theme& theme, Icons& icons, std::string name, bool favourite, bool dirty)
        : mTheme(theme), mIcons(icons), mName(std::move(name)), mFavourite(favourite), mDirty(dirty)
    {
    }

    std::function<void()> onFavourite;

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.name = NameOr("Preset");
        info.value = mName + (mDirty ? ", unsaved changes" : "") + (mFavourite ? ", favourite" : "");
        return info;
    }

    /// The full layout's selector is taller, with the preset's source (User, Factory) at its right.
    PresetPill& Height(float h)
    {
        mHeight = h;
        return *this;
    }

    /// `nameRoom`: the badge shows only where the name keeps at least this much after it (the
    /// compact bar's pill narrows with the window). Decided as it draws, from its own width.
    PresetPill& Badge(std::string badge, float nameRoom = 0.0f)
    {
        mBadge = std::move(badge);
        mBadgeNameRoom = nameRoom;
        return *this;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{160.0f, mHeight}, {el::full_extent, metrics::Hit(mHeight)}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = Face(ctx.bounds);
        cnv.fill_style(mTheme.IsLight() ? mTheme.Input() : el::rgba(17, 17, 24, 255));
        cnv.fill_round_rect(b, 10.0f);
        {
            // The inset shadow along the top edge.
            auto state = cnv.new_state();
            cycfi::artist::canvas::linear_gradient shade{b.top_left(), el::point{b.left, b.top + 8.0f}};
            shade.add_color_stop(0.0f, el::rgba(0, 0, 0, mTheme.IsLight() ? 30 : 110));
            shade.add_color_stop(1.0f, el::rgba(0, 0, 0, 0));
            cnv.fill_style(shade);
            cnv.fill_round_rect(b, 10.0f);
        }
        cnv.stroke_style(mHover ? mTheme.Text().opacity(0.14f) : mTheme.Text().opacity(0.06f));
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(b.inset(0.5f, 0.5f), 10.0f);

        const auto heart = HeartRect(b);
        mIcons.Draw(cnv, mFavourite ? "heart-filled" : "heart", heart,
                    mFavourite ? mTheme.Accent() : (mHoverHeart ? mTheme.Text() : mTheme.TextSecondary()));

        float right = b.right - 12.0f;
        const float nameLeft = heart.right + 14.0f;

        if (!mBadge.empty())
        {
            const auto badgeFont = Font(Weight::SemiBold).size(9.5f);
            const float w = el::measure_text(cnv, mBadge, badgeFont).x + 14.0f;
            const float cy = b.top + b.height() * 0.5f;
            const el::rect chip{b.right - 9.0f - w, cy - 9.0f, b.right - 9.0f, cy + 9.0f};

            if (chip.left - 10.0f - nameLeft >= mBadgeNameRoom)
            {
                cnv.fill_style(mTheme.Text().opacity(0.08f));
                cnv.fill_round_rect(chip, 5.0f);
                FillText(cnv, mBadge, badgeFont, mTheme.TextSecondary(), chip, HAlign::Centre);
                right = chip.left - 10.0f;
            }
        }

        const auto font = Font(Weight::SemiBold).size(mHeight > 30.0f ? 13.0f : 12.0f);
        const el::rect text{nameLeft, b.top, right, b.bottom};
        std::string name = mDirty ? mName + " *" : mName;
        FillText(cnv, Ellipsize(cnv, name, font, text.width()), font, mTheme.Ink(), text, HAlign::Left);
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (btn.state == el::mouse_button::left && !btn.down && HeartZone(ctx.bounds).includes(btn.pos))
        {
            mDown = false;

            if (onFavourite)
            {
                onFavourite();
            }

            return true;
        }

        return Pressable::click(ctx, btn);
    }

    bool cursor(const el::context& ctx, el::point p, el::cursor_tracking status) override
    {
        const bool heart = status != el::cursor_tracking::leaving && HeartZone(ctx.bounds).includes(p);

        if (heart != mHoverHeart)
        {
            mHoverHeart = heart;
            ctx.view.refresh(ctx);
        }

        return Pressable::cursor(ctx, p, status);
    }

  private:
    /// Where it is drawn: its bounds, or with touch sizes its height's face in the middle of them.
    [[nodiscard]] el::rect Face(const el::rect& bounds) const
    {
        return metrics::Touch() ? metrics::Face(bounds, bounds.width(), mHeight) : bounds;
    }

    /// Where a tap favourites it: round the heart, or with touch sizes the pill's whole height from
    /// its left edge to past the heart.
    [[nodiscard]] el::rect HeartZone(const el::rect& bounds) const
    {
        const auto heart = HeartRect(Face(bounds));

        if (metrics::Touch())
        {
            return el::rect{bounds.left, bounds.top, heart.right + 10.0f, bounds.bottom};
        }

        return heart.inset(-6.0f, -6.0f);
    }

    static el::rect HeartRect(const el::rect& b)
    {
        const float cy = b.top + b.height() * 0.5f;
        return el::rect{b.left + 13.0f, cy - 7.0f, b.left + 27.0f, cy + 7.0f};
    }

    const Theme& mTheme;
    Icons& mIcons;
    std::string mName, mBadge;
    bool mFavourite, mDirty;
    bool mHoverHeart = false;
    float mHeight = 28.0f;
    float mBadgeNameRoom = 0.0f;
};

/// A scene's chip (.scene-badge): its name, and an x to remove it when `onRemove` is set. The
/// playing scene is tinted with the accent.
class SceneChip final : public Pressable
{
  public:
    SceneChip(const Theme& theme, std::string label, bool active, bool add = false)
        : mTheme(theme), mLabel(std::move(label)), mActive(active), mAdd(add)
    {
    }

    std::function<void()> onRemove;
    std::function<void()> onMenu;

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.role = mAdd ? AccessRole::Button : AccessRole::Tab;
        info.name = NameOr(mAdd ? "Add scene" : "Scene " + mLabel);
        info.selected = mActive;
        return info;
    }

    // Delete removes the scene (as its x does), the Menu key opens its menu.
    bool key(const el::context& ctx, el::key_info k) override
    {
        if (IsMenuKey(k) && onMenu)
        {
            onMenu();
            return true;
        }

        if (k.action == el::key_action::press && k.key == el::key_code::_delete && onRemove)
        {
            onRemove();
            return true;
        }

        return Pressable::key(ctx, k);
    }

    el::view_limits limits(const el::basic_context& ctx) const override
    {
        // With touch sizes the chip is hit over its row's height, and its x over a zone of its
        // own past the face, kRemoveZone wide, so a tap on the name never removes the scene.
        const float face = FaceWidth(ctx.canvas);
        const float w = !metrics::Touch() ? face : onRemove ? face + kRemoveZone - 12.0f : metrics::Hit(face);
        return {{w, 18.0f}, {w, metrics::Hit(18.0f)}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = FaceRect(ctx.bounds, cnv);
        const el::color fill =
            mActive ? mTheme.Accent().opacity(0.12f)
                    : mTheme.Pick(el::rgba(38, 38, 46, mHover ? 230 : 153), mHover ? mTheme.Elevated() : mTheme.Card());
        const el::color line =
            mActive ? mTheme.Accent().opacity(0.55f) : mTheme.Pick(el::rgba(38, 38, 48, 178), mTheme.Border());
        const el::color ink = mActive ? mTheme.Accent() : (mHover ? mTheme.TextSecondary() : mTheme.TextMuted());
        cnv.fill_style(fill);
        cnv.fill_round_rect(b, 9.0f);
        cnv.stroke_style(line);
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(b.inset(0.5f, 0.5f), 9.0f);

        const auto font = Font(Weight::SemiBold).size(LabelSize());

        if (mAdd)
        {
            FillText(cnv, "+", font, ink, b, HAlign::Centre);
            return;
        }

        el::rect text{b.left + 8.0f, b.top, b.right - (onRemove ? 18.0f : 8.0f), b.bottom};
        FillText(cnv, mLabel, font, ink, text, HAlign::Left);

        if (onRemove)
        {
            const el::point c{b.right - 10.0f, b.top + b.height() * 0.5f};
            cnv.stroke_style(ink.opacity(0.8f));
            cnv.line_width(1.2f);
            cnv.begin_path();
            cnv.move_to(c.x - 2.5f, c.y - 2.5f);
            cnv.line_to(c.x + 2.5f, c.y + 2.5f);
            cnv.move_to(c.x + 2.5f, c.y - 2.5f);
            cnv.line_to(c.x - 2.5f, c.y + 2.5f);
            cnv.stroke();
        }
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (btn.state == el::mouse_button::right)
        {
            if (btn.down && onMenu)
            {
                onMenu();
            }

            return true;
        }

        // Held, a finger's press renames it, as a right click does (LongPress.h).
        if (btn.state == el::mouse_button::left && btn.down && onMenu)
        {
            LongPress::Press(btn.pos, onMenu);
        }

        const float removeZone = metrics::Touch() ? kRemoveZone : 18.0f;

        if (onRemove && btn.state == el::mouse_button::left && !btn.down && btn.pos.x > ctx.bounds.right - removeZone &&
            ctx.bounds.includes(btn.pos) && mDown && !LongPress::Fired())
        {
            mDown = false;
            onRemove();
            return true;
        }

        return Pressable::click(ctx, btn);
    }

  private:
    static constexpr float kRemoveZone = 32.0f;

    [[nodiscard]] float FaceWidth(el::canvas& cnv) const
    {
        return mAdd ? 20.0f
                    : el::measure_text(cnv, mLabel, Font(Weight::SemiBold).size(LabelSize())).x + 16.0f +
                          (onRemove ? 12.0f : 0.0f);
    }

    /// Where the chip is drawn: its bounds, or with touch sizes an 18 px face centred in them
    /// (at the left of a chip with an x, whose zone runs on past it, and of the add chip).
    [[nodiscard]] el::rect FaceRect(const el::rect& bounds, el::canvas& cnv) const
    {
        if (!metrics::Touch())
        {
            return bounds;
        }

        const float w = FaceWidth(cnv);
        const auto face = metrics::Face(bounds, w, 18.0f);

        // The add chip's face at the left of its hit area too, so it sits the same gap from the
        // last scene's zone as the scenes do from each other.
        if (onRemove || mAdd)
        {
            return el::rect{bounds.left, face.top, bounds.left + w, face.bottom};
        }

        return face;
    }

    [[nodiscard]] float LabelSize() const
    {
        return mAdd ? 12.0f : metrics::Readable(10.0f);
    }

    const Theme& mTheme;
    std::string mLabel;
    bool mActive, mAdd;
};

/// One half of the footer's A/B switch (.footer-ab-btn).
class AbButton final : public Pressable
{
  public:
    AbButton(const Theme& theme, std::string label, bool active)
        : mTheme(theme), mLabel(std::move(label)), mActive(active)
    {
    }

    /// With touch sizes, where its 24 px face sits across its wider hit area: 1 at the right (A,
    /// which meets B in the middle of the pair), 0 at the left (B).
    AbButton& FaceAt(float along)
    {
        mFaceAt = along;
        return *this;
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.role = AccessRole::Tab;
        info.name = NameOr("Chain " + mLabel);
        info.selected = mActive;
        return info;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{metrics::Hit(24.0f, metrics::kTouchFloor), 21.0f},
                {metrics::Hit(24.0f, metrics::kTouchFloor), metrics::Hit(21.0f)}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        auto b = ctx.bounds;

        if (metrics::Touch())
        {
            const auto face = metrics::Face(b, 24.0f, 21.0f);
            const float left = b.left + (b.width() - 24.0f) * mFaceAt;
            b = el::rect{left, face.top, left + 24.0f, face.bottom};
        }

        if (mActive)
        {
            cnv.fill_style(mTheme.Pick(el::rgba(79, 51, 42, 255), mTheme.AccentTint()));
            cnv.fill_round_rect(b, 4.0f);
            cnv.stroke_style(mTheme.Accent().opacity(0.62f));
            cnv.line_width(1.0f);
            cnv.stroke_round_rect(b.inset(0.5f, 0.5f), 4.0f);
        }
        else if (mHover)
        {
            cnv.fill_style(mTheme.Text().opacity(0.06f));
            cnv.fill_round_rect(b, 4.0f);
        }

        FillText(cnv, mLabel, Font(Weight::SemiBold).size(11.0f), mActive ? mTheme.Ink() : mTheme.TextSecondary(), b,
                 HAlign::Centre);
    }

  private:
    const Theme& mTheme;
    std::string mLabel;
    bool mActive;
    float mFaceAt = 0.5f;
};

/// The round + (.signal-add-btn, .signal-path-add-menu-trigger).
class RoundAdd final : public Pressable
{
  public:
    explicit RoundAdd(const Theme& theme, float size = 24.0f) : mTheme(theme), mSize(size)
    {
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.name = NameOr("Add effect");
        return info;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        // With touch sizes it is hit over 40 px, drawn its size in the middle.
        const float hit = metrics::Hit(mSize, metrics::kTouchFloor);
        return {{hit, hit}, {hit, hit}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto c = cycfi::artist::center_point(ctx.bounds);
        const float r = mSize * 0.5f - 1.0f;
        {
            auto state = cnv.new_state();
            cycfi::artist::canvas::linear_gradient fill{el::point{c.x, c.y - r}, el::point{c.x, c.y + r}};
            fill.add_color_stop(0.0f, mTheme.Pick(mHover ? el::rgba(78, 78, 90, 255) : el::rgba(58, 58, 68, 255),
                                                  mHover ? mTheme.Input() : mTheme.Elevated()));
            fill.add_color_stop(1.0f, mTheme.Pick(el::rgba(32, 32, 42, 255), mTheme.Card()));
            cnv.fill_style(fill);
            cnv.add_circle(cycfi::artist::circle(c, r));
            cnv.fill();
        }
        cnv.stroke_style(mHover ? mTheme.Accent() : mTheme.Pick(el::rgba(74, 74, 88, 255), mTheme.BorderStrong()));
        cnv.line_width(1.7f);
        cnv.add_circle(cycfi::artist::circle(c, r));
        cnv.stroke();
        const float arm = mSize * 0.2f;
        cnv.stroke_style(mHover ? mTheme.Text() : mTheme.Pick(el::rgba(200, 200, 216, 255), mTheme.TextSecondary()));
        cnv.line_width(2.0f);
        cnv.begin_path();
        cnv.move_to(c.x - arm, c.y);
        cnv.line_to(c.x + arm, c.y);
        cnv.move_to(c.x, c.y - arm);
        cnv.line_to(c.x, c.y + arm);
        cnv.stroke();
    }

  private:
    const Theme& mTheme;
    float mSize;
};

/// The bar between the full layout's chain and the editor under it (the web UI's
/// .signal-path-resize-handle): a grip that is dragged up or down. `onDrag` hears how far the
/// pointer has moved since the press, and `done` on release.
class SplitHandle final : public el::element
{
  public:
    explicit SplitHandle(const Theme& theme) : mTheme(theme)
    {
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{0.0f, 10.0f}, {el::full_extent, 10.0f}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const float y = b.top + b.height() * 0.5f;
        cnv.stroke_style(mTheme.Text().opacity(0.06f));
        cnv.line_width(1.0f);
        cnv.begin_path();
        cnv.move_to(b.left, y);
        cnv.line_to(b.right, y);
        cnv.stroke();
        const float cx = (b.left + b.right) * 0.5f;
        cnv.fill_style(mHover || mDragging ? mTheme.TextSecondary() : mTheme.TextMuted().opacity(0.5f));
        cnv.fill_round_rect(el::rect{cx - 18.0f, y - 2.0f, cx + 18.0f, y + 2.0f}, 2.0f);
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (btn.state != el::mouse_button::left)
        {
            return false;
        }

        if (btn.down)
        {
            mDragging = true;
            mStartY = btn.pos.y;
        }
        else if (mDragging)
        {
            mDragging = false;

            if (onDrag)
            {
                onDrag(btn.pos.y - mStartY, true);
            }

            ctx.view.refresh(ctx);
        }

        return true;
    }

    void drag(const el::context&, el::mouse_button btn) override
    {
        if (mDragging && onDrag)
        {
            onDrag(btn.pos.y - mStartY, false);
        }
    }

    bool cursor(const el::context& ctx, el::point, el::cursor_tracking status) override
    {
        const bool hover = status != el::cursor_tracking::leaving;

        if (hover != mHover)
        {
            mHover = hover;
            ctx.view.refresh(ctx);
        }

        if (hover)
        {
            el::set_cursor(el::cursor_type::v_resize);
        }

        return true;
    }

    bool wants_control() const override
    {
        return true;
    }

    std::function<void(float dy, bool done)> onDrag;

  private:
    const Theme& mTheme;
    float mStartY = 0.0f;
    bool mDragging = false;
    bool mHover = false;
};
} // namespace nanoq::ui
