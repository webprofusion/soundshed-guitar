#pragma once

// The pieces of the frame around the stage, drawn as the web UI's compact landscape layout draws
// them (css/compact/rail.css, css/control-bar/, css/footer.css): the rail's stage tabs, the preset
// name's pill, scene chips, the footer's A/B switch, the round + over the chain and the bar between
// the full layout's chain and editor.

#include "ui/Drawn.h"
#include "ui/input/Focus.h"
#include "ui/widgets/Buttons.h"
#include "ui/widgets/InlineEdit.h"
#include "ui/widgets/Pressable.h"

#include <algorithm>
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
        auto b = Face(ctx.bounds);
        // A theme with a display of its own (Studio's LCD, Studio 1973's amber glass) draws its
        // window, and its lettering in the display's ink and glow.
        const auto& display = mTheme.Controls().presetDisplay;
        const bool skinned = display.face.drawn;
        const el::color displayInk = Theme::Argb(display.text.colour);

        if (skinned)
        {
            b = DrawKey(cnv, mTheme, b, display.face, mTheme.Controls().backdrops.presetDisplay, false, false);

            if (mHover)
            {
                cnv.stroke_style(displayInk.opacity(0.3f));
                cnv.line_width(1.0f);
                cnv.stroke_round_rect(b.inset(1.5f, 1.5f), std::max(0.0f, display.face.box.radius - 1.0f));
            }
        }
        else
        {
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
            // Unsaved changes edge it in the accent (.preset-selector.has-unsaved-changes).
            cnv.stroke_style(mDirty   ? mTheme.Accent().opacity(0.45f)
                             : mHover ? mTheme.Text().opacity(0.14f)
                                      : mTheme.Text().opacity(0.06f));
            cnv.line_width(1.0f);
            cnv.stroke_round_rect(b.inset(0.5f, 0.5f), 10.0f);
        }

        const el::color ink = skinned ? displayInk : mTheme.Ink();
        const el::color soft = skinned ? displayInk.opacity(0.72f) : mTheme.TextSecondary();
        const auto heart = HeartRect(b);
        // The heart in the theme's favourite colour when set or hovered, else soft at .7.
        mIcons.Draw(cnv, mFavourite ? "heart-filled" : "heart", heart,
                    mFavourite || mHoverHeart ? (skinned ? ink : mTheme.PresetFavorite()) : soft.opacity(0.7f));

        float right = b.right - 12.0f;
        const float nameLeft = heart.right + 14.0f;

        // The status chip (.preset-selector-status): the source, and "Unsaved" in the accent
        // while there are changes to keep; 9 px bold, padded 2 x 7, radius 4.
        const std::string badge =
            mDirty ? (mBadge.empty() ? std::string("Unsaved") : mBadge + " \xE2\x80\xA2 Unsaved") : mBadge;

        if (!badge.empty())
        {
            const auto badgeFont = Font(Weight::SemiBold).size(9.0f);
            constexpr float kTracking = 0.06f;
            const float w = TrackedWidth(cnv, badge, badgeFont, kTracking) + 14.0f;
            const float cy = b.top + b.height() * 0.5f;
            const el::rect chip{b.right - 8.0f - w, cy - 8.0f, b.right - 8.0f, cy + 8.0f};

            if (chip.left - 10.0f - nameLeft >= mBadgeNameRoom)
            {
                cnv.fill_style(skinned ? displayInk.opacity(0.12f) : mTheme.Text().opacity(0.06f));
                cnv.fill_round_rect(chip, 4.0f);
                FillTrackedText(cnv, badge, badgeFont, mDirty ? (skinned ? ink : mTheme.Accent()) : soft, chip,
                                HAlign::Centre, kTracking);
                right = chip.left - 10.0f;
            }
        }

        // The name (.preset-chooser-label): 11 px bold, tracked .05em.
        constexpr float kNameTracking = 0.05f;
        const auto font = skinned ? ControlFont(display.text, 11.0f) : Font(Weight::SemiBold).size(11.0f);
        const el::rect text{nameLeft, b.top, right, b.bottom};
        // Room for the tracking too: it adds .05em after each character.
        const float trackingRoom = kNameTracking * 11.0f * static_cast<float>(std::min<std::size_t>(mName.size(), 64));
        const auto name = Ellipsize(cnv, mName, font, std::max(0.0f, text.width() - trackingRoom));

        if (skinned)
        {
            DrawDisplayText(cnv, name, font, display, ink, text, HAlign::Left);
        }
        else
        {
            FillTrackedText(cnv, name, font, mHover && !mHoverHeart ? mTheme.Text() : ink, text, HAlign::Left,
                            kNameTracking);
        }
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
    /// Renamed in place (double click or F2, as sceneBadges.ts): the name typed, Enter or
    /// leaving keeps it, Esc drops it. `onRenaming` hears the field open and close.
    std::function<void(const std::string&)> onRename;
    std::function<void(bool)> onRenaming;
    /// Dragged sideways to a new place: the pointer's x as it moves, and on release.
    std::function<void(float x, bool done)> onDrag;
    /// Alt+Left and Alt+Right: one place earlier or later.
    std::function<void(int step)> onMove;

    /// A drop mark at its left (-1) or right (1) edge, or none (0), while a chip is dragged.
    void Mark(int side)
    {
        if (side != mMark)
        {
            mMark = side;
            RepaintSoon(DrawnArea());
        }
    }

    /// Where it was last drawn, for a drag to find its place among the chips.
    [[nodiscard]] el::rect DrawnBounds() const
    {
        return mDrawn;
    }

    [[nodiscard]] bool Renaming() const
    {
        return mEdit.Active();
    }

    bool wants_focus() const override
    {
        return Pressable::wants_focus() || mEdit.Active();
    }

    bool end_focus() override
    {
        // Going elsewhere keeps what was typed, as the web UI's field does on blur.
        FinishRename(true);
        return Pressable::end_focus();
    }

    bool text(const el::context& ctx, el::text_info info) override
    {
        if (!mEdit.Char(info))
        {
            return false;
        }

        ctx.view.refresh(ctx);
        return true;
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.role = mAdd ? AccessRole::Button : AccessRole::Tab;
        info.name = NameOr(mAdd ? "Add scene" : "Scene " + mLabel);
        info.selected = mActive;
        return info;
    }

    // Delete removes the scene (as its x does), the Menu key opens its menu, F2 renames it and
    // Alt+Left/Right moves it.
    bool key(const el::context& ctx, el::key_info k) override
    {
        if (mEdit.Active())
        {
            switch (mEdit.Key(k))
            {
            case InlineEdit::Outcome::Commit:
                FinishRename(true);
                return true;
            case InlineEdit::Outcome::Cancel:
                FinishRename(false);
                return true;
            case InlineEdit::Outcome::Used:
                ctx.view.refresh(ctx);
                return true;
            case InlineEdit::Outcome::Unused:
                return false;
            }
        }

        if (k.action == el::key_action::press && k.key == el::key_code::f2 && onRename)
        {
            BeginRename(ctx);
            return true;
        }

        if ((k.action == el::key_action::press || k.action == el::key_action::repeat) && (k.modifiers & el::mod_alt) &&
            (k.key == el::key_code::left || k.key == el::key_code::right) && onMove)
        {
            onMove(k.key == el::key_code::left ? -1 : 1);
            return true;
        }

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

        // Renaming, it is a field with room to type.
        if (mEdit.Active())
        {
            const float field = std::max(w, 120.0f);
            return {{field, 18.0f}, {field, metrics::Hit(18.0f)}};
        }

        return {{w, 18.0f}, {w, metrics::Hit(18.0f)}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx, ctx.bounds.inset(-4.0f, 0.0f));
        mDrawn = ctx.bounds;
        auto& cnv = ctx.canvas;
        auto dragState = cnv.new_state();

        // Dragged, it follows the pointer.
        if (mDragging)
        {
            cnv.translate(mDragDx, 0.0f);
        }

        const auto b = FaceRect(ctx.bounds, cnv);
        const auto font = Font(Weight::SemiBold).size(LabelSize());

        // Where a dragged chip would land: an accent bar at this one's edge.
        if (mMark != 0)
        {
            const float x = mMark < 0 ? b.left - 3.0f : b.right + 2.0f;
            cnv.fill_style(mTheme.Accent());
            cnv.fill_round_rect(el::rect{x, b.top, x + 2.0f, b.bottom}, 1.0f);
        }

        if (mEdit.Active())
        {
            mEdit.Draw(cnv, metrics::Touch() ? b : ctx.bounds, mTheme, font);
            return;
        }

        // The add chip (.scene-badge-add): a dashed edge over the raised face at 60%, muted until
        // hovered.
        if (mAdd)
        {
            cnv.fill_style(mTheme.Elevated().opacity(0.6f));
            cnv.fill_round_rect(b, 9.0f);
            cnv.stroke_style(mHover ? mTheme.BorderStrong() : mTheme.Border().opacity(0.7f));
            cnv.line_width(1.0f);
            StrokeDashedRoundRect(cnv, b.inset(0.5f, 0.5f), 8.5f, 3.0f, 2.0f);
            FillText(cnv, mLabel, font, mHover ? mTheme.TextSecondary() : mTheme.TextMuted(), b, HAlign::Centre);
            return;
        }

        const el::color fill = mActive ? mTheme.Accent().opacity(0.12f)
                                       : mTheme.DarkOr(el::rgba(38, 38, 46, mHover ? 230 : 153),
                                                       mHover ? mTheme.Elevated() : mTheme.Card());
        const el::color line =
            mActive ? mTheme.Accent().opacity(0.55f) : mTheme.DarkOr(el::rgba(38, 38, 48, 178), mTheme.Border());
        const el::color ink = mActive ? mTheme.Accent() : (mHover ? mTheme.TextSecondary() : mTheme.TextMuted());
        cnv.fill_style(fill);
        cnv.fill_round_rect(b, 9.0f);
        cnv.stroke_style(line);
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(b.inset(0.5f, 0.5f), 9.0f);

        // The name stops at 160 px with an ellipsis (.scene-badge-label).
        el::rect text{b.left + 8.0f, b.top, b.right - (onRemove ? kCloseSize + 4.0f : 8.0f), b.bottom};
        FillText(cnv, Ellipsize(cnv, mLabel, font, std::min(kLabelMax, text.width())), font, ink, text, HAlign::Left);

        // The x (.scene-badge-close): a 14 px circle at .55, red with a red wash under the pointer.
        if (onRemove)
        {
            const el::point c{b.right - 2.0f - kCloseSize * 0.5f, b.top + b.height() * 0.5f};
            const el::color cross = mHoverClose ? mTheme.Error() : ink.opacity(0.55f);

            if (mHoverClose)
            {
                cnv.fill_style(mTheme.Error().opacity(0.18f));
                cnv.add_circle(cycfi::artist::circle(c, kCloseSize * 0.5f));
                cnv.fill();
            }

            cnv.stroke_style(cross);
            cnv.line_width(1.2f);
            cnv.begin_path();
            cnv.move_to(c.x - 2.5f, c.y - 2.5f);
            cnv.line_to(c.x + 2.5f, c.y + 2.5f);
            cnv.move_to(c.x + 2.5f, c.y - 2.5f);
            cnv.line_to(c.x - 2.5f, c.y + 2.5f);
            cnv.stroke();
        }
    }

    bool cursor(const el::context& ctx, el::point p, el::cursor_tracking status) override
    {
        const bool close = onRemove && status != el::cursor_tracking::leaving && !metrics::Touch() &&
                           p.x > ctx.bounds.right - (kCloseSize + 4.0f);

        if (close != mHoverClose)
        {
            mHoverClose = close;
            ctx.view.refresh(ctx);
        }

        return Pressable::cursor(ctx, p, status);
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

        if (mEdit.Active())
        {
            return btn.state == el::mouse_button::left;
        }

        // A double click renames it in place.
        if (btn.state == el::mouse_button::left && btn.down && btn.num_clicks >= 2 && onRename && !mAdd)
        {
            BeginRename(ctx);
            return true;
        }

        if (btn.state == el::mouse_button::left && btn.down)
        {
            mDownAt = btn.pos;
            mDragging = false;
        }

        // Let go after a drag: it lands, and is not a click.
        if (btn.state == el::mouse_button::left && !btn.down && mDragging)
        {
            mDragging = false;
            mDragDx = 0.0f;
            mDown = false;
            ctx.view.refresh(ctx);

            if (onDrag)
            {
                const auto drop = onDrag; // it may rebuild the row this sits in
                drop(btn.pos.x, true);
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

    void drag(const el::context& ctx, el::mouse_button btn) override
    {
        if (!mDown || !onDrag || mEdit.Active() || (!mDragging && std::abs(btn.pos.x - mDownAt.x) < 5.0f))
        {
            return;
        }

        LongPress::Cancel();
        mDragging = true;
        mDragDx = btn.pos.x - mDownAt.x;
        onDrag(btn.pos.x, false);
        ctx.view.refresh(ctx);
    }

  private:
    void BeginRename(const el::context& ctx)
    {
        mEdit.Begin(mLabel);

        if (onRenaming)
        {
            onRenaming(true);
        }

        ctx.view.layout();
        ctx.view.refresh();
        ctx.view.post([self = weak_from_this(), &view = ctx.view] {
            if (const auto chip = self.lock())
            {
                FocusInView(view, *chip);
            }
        });
    }

    void FinishRename(bool keep)
    {
        if (!mEdit.Active())
        {
            return;
        }

        const auto typed = mEdit.Value();
        mEdit.End();
        RepaintSoon(DrawnArea());
        const auto rename = onRename;
        const auto renaming = onRenaming;

        if (renaming)
        {
            renaming(false);
        }

        if (keep && rename && !typed.empty() && typed != mLabel)
        {
            rename(typed);
        }
    }

    static constexpr float kRemoveZone = 32.0f;
    static constexpr float kCloseSize = 14.0f;
    static constexpr float kLabelMax = 160.0f;

    [[nodiscard]] float FaceWidth(el::canvas& cnv) const
    {
        const float text = el::measure_text(cnv, mLabel, Font(Weight::SemiBold).size(LabelSize())).x;

        // The add chip: padded 6 either side, at least as wide as it is tall.
        if (mAdd)
        {
            return std::max(18.0f, text + 12.0f);
        }

        return std::min(kLabelMax, text) + 16.0f + (onRemove ? kCloseSize - 2.0f : 0.0f);
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
        return mAdd && mLabel == "+" ? 12.0f : metrics::Readable(10.0f);
    }

    const Theme& mTheme;
    std::string mLabel;
    bool mActive, mAdd;
    bool mHoverClose = false;
    bool mDragging = false;
    int mMark = 0;
    float mDragDx = 0.0f;
    el::point mDownAt;
    el::rect mDrawn;
    InlineEdit mEdit;
};

/// The chevron that folds the control bar or the chain (.control-bar-collapse-btn,
/// .signal-path-collapse-btn): muted at .38, .75 under the pointer, pointing up while open; folded,
/// it points down with the bar's name beside it.
class CollapseToggle final : public Pressable
{
  public:
    CollapseToggle(const Theme& theme, Icons& icons, std::string label, bool collapsed)
        : mTheme(theme), mIcons(icons), mLabel(std::move(label)), mCollapsed(collapsed)
    {
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.name = NameOr((mCollapsed ? "Expand " : "Collapse ") + Lower(mLabel));
        return info;
    }

    el::view_limits limits(const el::basic_context& ctx) const override
    {
        const float w = mCollapsed ? 26.0f + TrackedWidth(ctx.canvas, mLabel, LabelFont(), 0.08f) : 24.0f;
        return {{w, 16.0f}, {w, 16.0f}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const auto ink = mTheme.TextMuted().opacity(mHover || HasFocus() ? 0.75f : 0.38f);
        const float cy = b.top + b.height() * 0.5f;
        mIcons.Draw(cnv, mCollapsed ? "ui-caret" : "ui-caret-up",
                    el::rect{b.left + 6.0f, cy - 6.0f, b.left + 18.0f, cy + 6.0f}, ink);

        if (mCollapsed)
        {
            FillTrackedText(cnv, mLabel, LabelFont(), ink, el::rect{b.left + 22.0f, b.top, b.right, b.bottom},
                            HAlign::Left, 0.08f);
        }
    }

  private:
    [[nodiscard]] static el::font_descr LabelFont()
    {
        return Font(Weight::SemiBold).size(10.0f);
    }

    const Theme& mTheme;
    Icons& mIcons;
    std::string mLabel;
    bool mCollapsed;
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

    /// Whether this half is the live chain; the Options sheet's pair follows it without a rebuild.
    AbButton& Active(bool active)
    {
        mActive = active;
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
            cnv.fill_style(mTheme.DarkOr(el::rgba(79, 51, 42, 255), mTheme.AccentTint()));
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
        // Its glow and growth reach past its bounds.
        NoteDrawn(ctx, ctx.bounds.inset(-8.0f, -8.0f));
        auto& cnv = ctx.canvas;
        const auto c = cycfi::artist::center_point(ctx.bounds);
        // Under the pointer it grows a tenth and fills with the accent's gradient, glowing, its +
        // white (.signal-path-add-menu-trigger:hover).
        const bool hot = mHover && !metrics::Touch();
        const float r = (mSize * 0.5f - 1.0f) * (hot ? 1.1f : 1.0f);
        {
            auto state = cnv.new_state();
            cycfi::artist::canvas::linear_gradient fill{el::point{c.x, c.y - r}, el::point{c.x, c.y + r}};

            if (hot)
            {
                cnv.shadow_style(el::point{0.0f, 0.0f}, 12.0f, el::rgba(224, 120, 72, 128));
                fill.add_color_stop(0.0f, mTheme.Accent());
                fill.add_color_stop(1.0f, el::rgba(200, 88, 48, 255));
            }
            else
            {
                fill.add_color_stop(0.0f, mTheme.DarkOr(el::rgba(58, 58, 68, 255), mTheme.Elevated()));
                fill.add_color_stop(1.0f, mTheme.Surface());
            }

            cnv.fill_style(fill);
            cnv.add_circle(cycfi::artist::circle(c, r));
            cnv.fill();
        }
        cnv.stroke_style(hot ? mTheme.Accent() : mTheme.ButtonBorder());
        cnv.line_width(1.7f);
        cnv.add_circle(cycfi::artist::circle(c, r));
        cnv.stroke();
        const float arm = r * 0.4f;
        cnv.stroke_style(hot ? el::rgba(255, 255, 255, 255) : mTheme.TextSecondary());
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
class SplitHandle final : public el::element, public Drawn, public Focusable
{
  public:
    explicit SplitHandle(const Theme& theme) : mTheme(theme)
    {
    }

    /// The two sizes the web UI's handle switches between: full cards, or icon tiles.
    std::function<void(bool full)> onMode;

    /// Read as the chain's size, as the web UI's handle (role separator) is.
    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info;
        info.role = AccessRole::Slider;
        info.name = NameOr("Signal chain size");
        info.value = mFull ? "Full" : "Compact";
        info.position = mFull ? 1.0 : 0.0;
        return info;
    }

    SplitHandle& Full(bool full)
    {
        mFull = full;
        return *this;
    }

    bool Activate(const el::rect&) override
    {
        return SetMode(true);
    }

    bool wants_focus() const override
    {
        return FocusWanted(onMode != nullptr);
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

    /// Up, Left and Home make it compact; Down, Right, End, Enter and Space full.
    bool key(const el::context&, el::key_info k) override
    {
        if (k.action != el::key_action::press && k.action != el::key_action::repeat)
        {
            return false;
        }

        switch (k.key)
        {
        case el::key_code::up:
        case el::key_code::left:
        case el::key_code::home:
            return SetMode(false);
        case el::key_code::down:
        case el::key_code::right:
        case el::key_code::end:
        case el::key_code::enter:
        case el::key_code::kp_enter:
        case el::key_code::space:
            return SetMode(true);
        default:
            return false;
        }
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{0.0f, 8.0f}, {el::full_extent, 8.0f}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const float y = b.top + b.height() * 0.5f;
        cnv.stroke_style(mTheme.Text().opacity(0.06f));
        cnv.line_width(1.0f);
        cnv.begin_path();
        cnv.move_to(b.left, y);
        cnv.line_to(b.right, y);
        cnv.stroke();
        // .signal-path-resize-handle: a 48 x 3 grip, the accent under the pointer or held.
        const float cx = (b.left + b.right) * 0.5f;
        cnv.fill_style(mHover || mDragging || HasFocus() ? mTheme.Accent() : mTheme.TextMuted().opacity(0.5f));
        cnv.fill_round_rect(el::rect{cx - 24.0f, y - 1.5f, cx + 24.0f, y + 1.5f}, 1.5f);
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (btn.state != el::mouse_button::left)
        {
            return false;
        }

        // A double click gives the chain its full cards.
        if (btn.down && btn.num_clicks >= 2)
        {
            mDragging = false;
            SetMode(true);
            return true;
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
    bool SetMode(bool full)
    {
        if (!onMode)
        {
            return false;
        }

        const auto mode = onMode; // it rebuilds the stage this sits in
        mode(full);
        return true;
    }

    const Theme& mTheme;
    float mStartY = 0.0f;
    bool mDragging = false;
    bool mHover = false;
    bool mFull = true;
};
} // namespace nanoq::ui
