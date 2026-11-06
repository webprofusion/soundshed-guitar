#pragma once

// The pieces of the compact frame, drawn as the web UI's compact landscape layout draws them
// (css/compact/rail.css, css/control-bar/, css/footer.css): the rail's stage tabs, the square icon
// buttons of the preset bar and footer, the preset name's pill, scene chips, the effect header's
// on/off switch, underlined tabs, the footer's A/B switch and the round + that floats over the
// chain. Measurements are the web UI's at its compact density.

#include "ui/Drawn.h"
#include "ui/Focus.h"
#include "ui/Graphics.h"
#include "ui/Layout.h"
#include "ui/SwitchMotion.h"
#include "ui/Widgets.h"

#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace nanoq::ui
{
/// Hover and press tracking shared by the clickable pieces here: a left click released inside
/// runs `onClick`. With focus (Focus.h), Enter or Space runs it too, and the Menu key or
/// Shift+F10 opens `onMenu`. Read as a button unless a subclass says what it is.
class Pressable : public el::element, public Drawn, public Focusable, public TapTarget
{
  public:
    std::function<void()> onClick;
    /// A right click (a long press on a touch screen), at a point in view coordinates: the
    /// control's context menu, such as MIDI Learn.
    std::function<void(el::point at)> onMenu;

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info;
        info.name = mAccessName;
        info.enabled = mEnabled;
        return info;
    }

    bool Activate(const el::rect& at) override
    {
        if (!mEnabled || !onClick)
        {
            return false;
        }

        mBounds = at;
        Pressed();
        const auto run = onClick; // it may rebuild the region this sits in
        run();
        return true;
    }

    bool wants_focus() const override
    {
        return FocusWanted(mEnabled && (onClick != nullptr || onMenu != nullptr));
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
        if (IsMenuKey(k) && onMenu)
        {
            mBounds = ctx.bounds;
            onMenu(cycfi::artist::center_point(ctx.bounds));
            return true;
        }

        if (IsPressKey(k))
        {
            return Activate(ctx.bounds);
        }

        return false;
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (btn.state == el::mouse_button::right && onMenu)
        {
            if (btn.down)
            {
                onMenu(btn.pos);
            }

            return true;
        }

        if (!mEnabled || btn.state != el::mouse_button::left)
        {
            return false;
        }

        if (btn.down)
        {
            mDown = true;
            ctx.view.refresh(ctx);
            return true;
        }

        const bool fire = mDown && ctx.bounds.includes(btn.pos);
        mDown = false;
        mBounds = ctx.bounds;
        ctx.view.refresh(ctx);

        if (fire && onClick)
        {
            Pressed();
            onClick();
        }

        return true;
    }

    /// Where it was when last clicked, in view coordinates: a menu it opens hangs from it.
    [[nodiscard]] el::rect Bounds() const
    {
        return mBounds;
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
        return true;
    }

    void Enabled(bool enabled)
    {
        mEnabled = enabled;
    }

  protected:
    /// Just before `onClick` runs, from a click or a key: a switch notes where it was for the
    /// slide to its new state (SwitchMotion).
    virtual void Pressed()
    {
    }

    bool mHover = false, mDown = false, mEnabled = true;
    el::rect mBounds;
};

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
        FillText(cnv, mLabel, Font(Weight::SemiBold).size(9.0f), ink,
                 el::rect{b.left, b.top + 28.0f, b.right, b.top + 40.0f}, HAlign::Centre);
    }

  private:
    const Theme& mTheme;
    Icons& mIcons;
    std::string mIcon, mLabel;
    bool mActive;
};

/// A square (or, with a label, wider) icon button. The looks are the web UI's: `Ghost` is the
/// preset bar's (.preset-action-btn), `Soft` its previous/next (.preset-nav-btn), `Boxed` the
/// footer's text buttons and the Controls tab, `Accent` a tinted one (.preset-action-btn-primary).
class IconButton final : public Pressable
{
  public:
    enum class Look
    {
        Ghost,
        Soft,
        Boxed,
        Accent,
        Chip,    // the effect header's Presets (.default-effect-shell-chip)
        Primary, // a dialog's confirming button: filled with the accent
        Slate,   // a model slot's previous, next and clear
    };

    IconButton(const Theme& theme, Icons& icons, std::string icon, Look look = Look::Ghost)
        : mTheme(theme), mIcons(icons), mIcon(std::move(icon)), mLook(look)
    {
    }

    /// Read as its label, or for an icon alone what the icon stands for; with a caret it opens a
    /// list, and lit it is pressed.
    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.role = mCaret ? AccessRole::ComboBox : AccessRole::Button;
        info.name = NameOr(mLabel.empty() ? IconMeaning(mIcon) : mLabel);
        info.selected = mLit;
        return info;
    }

    /// What an icon on a button means, for one with no label to read.
    [[nodiscard]] static std::string IconMeaning(const std::string& icon)
    {
        static const std::pair<const char*, const char*> kMeanings[] = {
            {"ui-prev", "Previous"},
            {"ui-next", "Next"},
            {"ui-play", "Play"},
            {"ui-stop", "Stop"},
            {"ui-loop", "Loop"},
            {"ui-record", "Record"},
            {"ui-save", "Save"},
            {"ui-new", "New preset"},
            {"ui-random", "Random preset"},
            {"ui-swap", "Copy the chain to the other slot"},
            {"ui-more", "More"},
            {"ui-gear", "Settings"},
            {"ui-folder", "Folder"},
            {"ui-trash", "Delete"},
            {"ui-share", "Share"},
            {"ui-edit", "Edit"},
            {"pencil", "Rename"},
            {"ui-download", "Download"},
            {"ui-upload", "Upload"},
            {"ui-up", "Up"},
            {"ui-speaker", "Mute"},
            {"ui-mute", "Unmute"},
            {"ui-tuner", "Tuner"},
            {"ui-midi", "MIDI and automation"},
            {"ui-pads", "Setlists"},
            {"ui-metronome", "Metronome"},
            {"ui-jam", "Jam"},
            {"ui-jump-back", "Back to start"},
            {"ui-wrap", "Wrap"},
            {"ui-box", "Composite"},
            {"ui-caret", "Open"},
            {"ui-undo", "Undo"},
            {"ui-redo", "Redo"},
            {"ui-copy", "Copy"},
            {"ui-close", "Close"},
            {"x", "Close"},
            {"star", "Favourite"},
            {"sliders", "Controls"},
            {"meter", "DSP status"},
        };

        for (const auto& [name, meaning] : kMeanings)
        {
            if (icon == name)
            {
                return meaning;
            }
        }

        // An icon not listed: its name, without the prefix.
        auto words = icon.rfind("ui-", 0) == 0 ? icon.substr(3) : icon;

        for (auto& ch : words)
        {
            ch = ch == '-' ? ' ' : ch;
        }

        return words;
    }

    IconButton& Size(float w, float h)
    {
        mW = w;
        mH = h;
        return *this;
    }

    IconButton& IconSize(float s)
    {
        mIconSize = s;
        return *this;
    }

    IconButton& Label(std::string label, float size = 10.0f)
    {
        mLabel = std::move(label);
        mLabelSize = size;
        return *this;
    }

    /// A small down-pointing caret after the icon: the button opens a menu.
    IconButton& Caret(bool caret)
    {
        mCaret = caret;
        return *this;
    }

    /// Lit: the accent tint of a pressed toggle (an open sheet, the tuner on).
    IconButton& Lit(bool lit)
    {
        mLit = lit;
        return *this;
    }

    IconButton& Ink(el::color ink)
    {
        mInk = ink;
        mHasInk = true;
        return *this;
    }

    IconButton& Disabled(bool disabled)
    {
        Enabled(!disabled);
        return *this;
    }

    el::view_limits limits(const el::basic_context& ctx) const override
    {
        float w = mW;

        if (!mLabel.empty())
        {
            const float text = el::measure_text(ctx.canvas, mLabel, Font(Weight::SemiBold).size(mLabelSize)).x;
            w = std::max(w, (mIcon.empty() ? 0.0f : mIconSize + 6.0f) + text + (mCaret ? 12.0f : 0.0f) + 20.0f);
        }
        else if (mCaret)
        {
            w += 12.0f;
        }

        return {{w, mH}, {w, mH}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        float radius = 8.0f;
        el::color fill = el::rgba(0, 0, 0, 0), line = el::rgba(0, 0, 0, 0);
        el::color ink = mTheme.Ink();

        switch (mLook)
        {
        case Look::Ghost:

            if (mHover || mLit)
            {
                fill = mTheme.Text().opacity(0.06f);
            }

            break;
        case Look::Soft:
            fill = mTheme.Text().opacity(mHover ? 0.09f : 0.05f);
            line = mTheme.Text().opacity(0.08f);
            ink = mTheme.TextSecondary();
            break;
        case Look::Boxed:
            radius = 6.0f;
            fill = mHover ? mTheme.Elevated() : mTheme.Bar();
            line = mTheme.Pick(el::rgba(58, 58, 72, 255), mTheme.BorderStrong());
            break;
        case Look::Primary:
            radius = 6.0f;
            fill = mTheme.Accent().level(mHover ? 1.06f : 0.92f);
            line = mTheme.Accent();
            ink = el::rgba(255, 255, 255, 255);
            break;
        case Look::Chip:
            radius = 6.0f;
            fill = mTheme.Pick(mHover ? el::rgba(40, 40, 50, 255) : el::rgba(30, 30, 38, 255),
                               mHover ? mTheme.Elevated() : mTheme.Card());
            line = mTheme.Pick(el::rgba(38, 38, 48, 255), mTheme.Border());
            ink = mTheme.Pick(el::rgba(235, 240, 250, 242), mTheme.Text());
            break;
        case Look::Slate:
            radius = 7.0f;
            fill = mTheme.Pick(mHover ? el::rgba(72, 78, 98, 255) : el::rgba(56, 61, 79, 255),
                               mHover ? mTheme.Elevated() : mTheme.Card());
            line = mTheme.Pick(el::rgba(255, 255, 255, 18), mTheme.BorderStrong());
            ink = mTheme.Pick(el::rgba(203, 203, 220, 255), mTheme.Text());
            break;
        case Look::Accent:
            fill = mTheme.Accent().opacity(mHover ? 0.22f : 0.14f);
            line = mTheme.Accent().opacity(0.64f);
            ink = mTheme.Text();
            break;
        }

        if (mLit)
        {
            fill = mTheme.Accent().opacity(0.16f);
            line = mTheme.Accent().opacity(0.55f);
            ink = mTheme.Accent();
        }

        // Lit takes the accent over a chosen ink: a tile that is on reads as on.
        if (mHasInk && !mLit)
        {
            ink = mInk;
        }

        if (!mEnabled)
        {
            ink = ink.opacity(0.35f);
        }

        if (mDown)
        {
            fill = fill.alpha > 0.0f ? fill.level(0.8f) : mTheme.Text().opacity(0.04f);
        }

        if (fill.alpha > 0.0f)
        {
            cnv.fill_style(fill);
            cnv.fill_round_rect(b, radius);
        }

        if (line.alpha > 0.0f)
        {
            cnv.stroke_style(line);
            cnv.line_width(1.0f);
            cnv.stroke_round_rect(b.inset(0.5f, 0.5f), radius);
        }

        // Icon, label and caret, centred as one group.
        const auto font = Font(Weight::SemiBold).size(mLabelSize);
        const float textW = mLabel.empty() ? 0.0f : el::measure_text(cnv, mLabel, font).x;
        const float iconW = mIcon.empty() ? 0.0f : mIconSize;
        const float gap = (!mIcon.empty() && !mLabel.empty()) ? 6.0f : 0.0f;
        const float caretW = mCaret ? 10.0f : 0.0f;
        const float total = iconW + gap + textW + (mCaret ? 2.0f + caretW : 0.0f);
        float x = b.left + (b.width() - total) * 0.5f;
        const float cy = b.top + b.height() * 0.5f;

        if (!mIcon.empty())
        {
            mIcons.Draw(cnv, mIcon, el::rect{x, cy - mIconSize * 0.5f, x + mIconSize, cy + mIconSize * 0.5f}, ink);
            x += iconW + gap;
        }

        if (!mLabel.empty())
        {
            FillText(cnv, mLabel, font, ink, el::rect{x, b.top, x + textW + 1.0f, b.bottom}, HAlign::Left);
            x += textW;
        }

        if (mCaret)
        {
            mIcons.Draw(cnv, "ui-caret", el::rect{x + 2.0f, cy - 5.0f, x + 2.0f + caretW, cy + 5.0f},
                        ink.opacity(ink.alpha * 0.8f));
        }
    }

  private:
    const Theme& mTheme;
    Icons& mIcons;
    std::string mIcon, mLabel;
    Look mLook;
    float mW = 28.0f, mH = 28.0f, mIconSize = 14.0f, mLabelSize = 10.0f;
    bool mCaret = false, mLit = false, mHasInk = false;
    el::color mInk;
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

    PresetPill& Badge(std::string badge)
    {
        mBadge = std::move(badge);
        return *this;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{160.0f, mHeight}, {el::full_extent, mHeight}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
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

        if (!mBadge.empty())
        {
            const auto badgeFont = Font(Weight::SemiBold).size(9.5f);
            const float w = el::measure_text(cnv, mBadge, badgeFont).x + 14.0f;
            const float cy = b.top + b.height() * 0.5f;
            const el::rect chip{b.right - 9.0f - w, cy - 9.0f, b.right - 9.0f, cy + 9.0f};
            cnv.fill_style(mTheme.Text().opacity(0.08f));
            cnv.fill_round_rect(chip, 5.0f);
            FillText(cnv, mBadge, badgeFont, mTheme.TextSecondary(), chip, HAlign::Centre);
            right = chip.left - 10.0f;
        }

        const auto font = Font(Weight::SemiBold).size(mHeight > 30.0f ? 13.0f : 12.0f);
        const el::rect text{heart.right + 14.0f, b.top, right, b.bottom};
        std::string name = mDirty ? mName + " *" : mName;
        FillText(cnv, Ellipsize(cnv, name, font, text.width()), font, mTheme.Ink(), text, HAlign::Left);
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (btn.state == el::mouse_button::left && !btn.down &&
            HeartRect(ctx.bounds).inset(-6.0f, -6.0f).includes(btn.pos))
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
        const bool heart =
            status != el::cursor_tracking::leaving && HeartRect(ctx.bounds).inset(-6.0f, -6.0f).includes(p);

        if (heart != mHoverHeart)
        {
            mHoverHeart = heart;
            ctx.view.refresh(ctx);
        }

        return Pressable::cursor(ctx, p, status);
    }

  private:
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
        const float w = mAdd ? 20.0f
                             : el::measure_text(ctx.canvas, mLabel, Font(Weight::SemiBold).size(10.0f)).x + 16.0f +
                                   (onRemove ? 12.0f : 0.0f);
        return {{w, 18.0f}, {w, 18.0f}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
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

        const auto font = Font(Weight::SemiBold).size(mAdd ? 12.0f : 10.0f);

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

        if (onRemove && btn.state == el::mouse_button::left && !btn.down && btn.pos.x > ctx.bounds.right - 18.0f)
        {
            mDown = false;
            onRemove();
            return true;
        }

        return Pressable::click(ctx, btn);
    }

  private:
    const Theme& mTheme;
    std::string mLabel;
    bool mActive, mAdd;
};

/// The effect header's on/off switch (.default-effect-shell-toggle): an accent pill reading ON
/// with its thumb at the right, or a grey one reading OFF with it at the left.
class PowerSwitch final : public Pressable
{
  public:
    PowerSwitch(const Theme& theme, bool on) : mTheme(theme), mOn(on)
    {
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.role = AccessRole::Toggle;
        info.name = NameOr("Effect on");
        info.toggled = mOn ? 1 : 0;
        return info;
    }

    void Set(bool on)
    {
        mOn = on;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{40.0f, 24.0f}, {40.0f, 24.0f}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const float radius = b.height() * 0.5f;
        // Sliding: the grey track under the accent one, which fades in as the thumb goes right.
        const float on = mMotion.Position(mOn, DrawnArea());

        if (on < 1.0f)
        {
            cnv.fill_style(mTheme.ToggleBg());
            cnv.fill_round_rect(b, radius);
        }

        if (on > 0.0f)
        {
            cycfi::artist::canvas::linear_gradient fill{b.top_left(), b.bottom_left()};
            fill.add_color_stop(0.0f, mTheme.Accent().level(1.05f).opacity(on));
            fill.add_color_stop(1.0f, mTheme.Accent().level(0.82f).opacity(on));
            cnv.fill_style(fill);
            cnv.fill_round_rect(b, radius);
        }

        const float thumbR = radius - 4.0f;
        const el::point thumb{b.left + radius + (b.width() - 2.0f * radius) * on, b.top + radius};
        cnv.fill_style(el::rgba(255, 255, 255, 255).opacity((200.0f + 55.0f * on) / 255.0f));
        cnv.add_circle(cycfi::artist::circle(thumb, thumbR));
        cnv.fill();

        const bool lit = on >= 0.5f;
        const auto font = Font(Weight::SemiBold).size(7.5f);
        const el::rect label = lit ? el::rect{b.left + 3.0f, b.top, thumb.x - thumbR, b.bottom}
                                   : el::rect{thumb.x + thumbR, b.top, b.right - 3.0f, b.bottom};
        FillText(cnv, lit ? "ON" : "OFF", font, el::rgba(255, 255, 255, lit ? 240 : 170), label, HAlign::Centre);
    }

  private:
    void Pressed() override
    {
        mMotion.NoteToggled(DrawnArea());
    }

    const Theme& mTheme;
    bool mOn;
    SwitchMotion mMotion;
};

/// A tab drawn as text with an accent underline when chosen (.node-param-tab).
class UnderlineTab final : public Pressable
{
  public:
    UnderlineTab(const Theme& theme, std::string label, bool active)
        : mTheme(theme), mLabel(Upper(std::move(label))), mActive(active)
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

    el::view_limits limits(const el::basic_context& ctx) const override
    {
        const float w = el::measure_text(ctx.canvas, mLabel, Font(Weight::SemiBold).size(10.0f)).x + 16.0f;
        return {{w, 26.0f}, {w, 26.0f}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const auto ink = mActive ? mTheme.Accent() : (mHover ? mTheme.Text() : mTheme.TextSecondary());
        FillText(cnv, mLabel, Font(Weight::SemiBold).size(10.0f), ink,
                 el::rect{b.left, b.top, b.right, b.bottom - 3.0f}, HAlign::Centre);

        if (mActive)
        {
            cnv.fill_style(mTheme.Accent());
            cnv.fill_round_rect(el::rect{b.left + 1.0f, b.bottom - 2.0f, b.right - 1.0f, b.bottom}, 1.0f);
        }
    }

  private:
    const Theme& mTheme;
    std::string mLabel;
    bool mActive;
};

/// One half of the footer's A/B switch (.footer-ab-btn).
class AbButton final : public Pressable
{
  public:
    AbButton(const Theme& theme, std::string label, bool active)
        : mTheme(theme), mLabel(std::move(label)), mActive(active)
    {
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
        return {{24.0f, 21.0f}, {24.0f, 21.0f}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;

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
        return {{mSize, mSize}, {mSize, mSize}};
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

/// A model or IR slot's choice (.resource-select): the loaded name on an accent-tinted field,
/// with a caret at the right. A tap opens the library.
class SelectField final : public Pressable
{
  public:
    SelectField(const Theme& theme, Icons& icons, std::string text, bool chosen, bool warn)
        : mTheme(theme), mIcons(icons), mText(std::move(text)), mChosen(chosen), mWarn(warn)
    {
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.role = AccessRole::ComboBox;
        info.name = NameOr("Choose");
        info.value = mText;
        return info;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{120.0f, 26.0f}, {el::full_extent, 26.0f}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const auto accent = mWarn ? mTheme.Error() : mTheme.Accent();
        cnv.fill_style(mChosen ? accent.opacity(mHover ? 0.3f : 0.22f).level(0.75f)
                               : (mHover ? mTheme.Elevated() : mTheme.Input()));
        cnv.fill_round_rect(b, 6.0f);
        cnv.stroke_style(mChosen ? accent.opacity(0.42f) : mTheme.Border());
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(b.inset(0.5f, 0.5f), 6.0f);
        const auto font = Font(Weight::Medium).size(12.0f);
        const el::rect text{b.left + 9.0f, b.top, b.right - 24.0f, b.bottom};
        FillText(cnv, Ellipsize(cnv, mText, font, text.width()), font, mChosen ? mTheme.Ink() : mTheme.TextSecondary(),
                 text, HAlign::Left);
        const float cy = b.top + b.height() * 0.5f;
        mIcons.Draw(cnv, "ui-caret", el::rect{b.right - 19.0f, cy - 5.0f, b.right - 9.0f, cy + 5.0f},
                    mTheme.TextSecondary());
    }

  private:
    const Theme& mTheme;
    Icons& mIcons;
    std::string mText;
    bool mChosen, mWarn;
};

/// An on/off parameter (.node-param-group with a .toggle-switch): its capital label, a small
/// switch, and On or Off under it. A tap flips it.
class ParamToggle final : public Pressable
{
  public:
    ParamToggle(const Theme& theme, std::string name) : mTheme(theme), mName(Upper(std::move(name)))
    {
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.role = AccessRole::Toggle;
        info.name = NameOr(mName);
        info.toggled = mOn ? 1 : 0;
        return info;
    }

    void Set(bool on)
    {
        mOn = on;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{60.0f, 63.0f}, {60.0f, 63.0f}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const auto label = Font(Weight::SemiBold).size(9.0f);
        FillText(cnv, Ellipsize(cnv, mName, label, b.width() + 6.0f), label,
                 mTheme.Pick(el::rgba(201, 209, 225, 184), mTheme.TextSecondary()),
                 el::rect{b.left, b.top, b.right, b.top + 12.0f}, HAlign::Centre);
        const float cx = (b.left + b.right) * 0.5f;
        const el::rect track{cx - 17.0f, b.top + 22.0f, cx + 17.0f, b.top + 40.0f};
        const float on = mMotion.Position(mOn, DrawnArea());
        cnv.fill_style(SwitchMotion::Mix(mTheme.ToggleBg().level(mHover ? 1.15f : 1.0f),
                                         mTheme.Accent().level(mHover ? 1.05f : 0.92f), on));
        cnv.fill_round_rect(track, 9.0f);
        cnv.fill_style(el::rgba(255, 255, 255, 240));
        cnv.add_circle(
            cycfi::artist::circle(el::point{track.left + 9.0f + (track.width() - 18.0f) * on, track.top + 9.0f}, 7.0f));
        cnv.fill();
        FillText(cnv, mOn ? "On" : "Off", Font(Weight::SemiBold).size(10.0f),
                 mTheme.Pick(el::rgba(231, 236, 246, 194), mTheme.Text()),
                 el::rect{b.left, b.bottom - 13.0f, b.right, b.bottom}, HAlign::Centre);
    }

  private:
    void Pressed() override
    {
        mMotion.NoteToggled(DrawnArea());
    }

    const Theme& mTheme;
    std::string mName;
    bool mOn = false;
    SwitchMotion mMotion;
};

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

/// One preset in the preset browser (the web UI's preset row): its name, a line under it with its
/// category, folder and tags, and at the right its rating (five stars, a click sets one, the
/// same one again clears it) and the favourite heart. A right click asks for its menu.
class PresetRow final : public Pressable
{
  public:
    PresetRow(const Theme& theme, Icons& icons, std::string name, std::string detail, int rating, bool favourite,
              bool selected)
        : mTheme(theme), mIcons(icons), mName(std::move(name)), mDetail(std::move(detail)), mRating(rating),
          mFavourite(favourite), mSelected(selected)
    {
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.role = AccessRole::ListItem;
        info.name = NameOr(mName);
        info.value = mDetail;

        if (mRating > 0)
        {
            info.value += (info.value.empty() ? "" : ", ") + std::to_string(mRating) + " stars";
        }

        if (mFavourite)
        {
            info.value += info.value.empty() ? "favourite" : ", favourite";
        }

        info.selected = mSelected;
        return info;
    }

    // The Menu key opens the row's menu; F favourites it and 0 to 5 rate it, as the heart and the
    // stars do.
    bool key(const el::context& ctx, el::key_info k) override
    {
        if (IsMenuKey(k) && onMenu)
        {
            onMenu();
            return true;
        }

        if (k.action == el::key_action::press && k.modifiers == 0)
        {
            if (k.key == el::key_code::f && onFavourite)
            {
                onFavourite();
                return true;
            }

            if (k.key >= el::key_code::_0 && k.key <= el::key_code::_5 && onRate)
            {
                onRate(static_cast<int>(k.key) - static_cast<int>(el::key_code::_0));
                return true;
            }
        }

        return Pressable::key(ctx, k);
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{200.0f, 44.0f}, {el::full_extent, 44.0f}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds.inset(0.0f, 1.0f);
        cnv.fill_style(mSelected ? mTheme.AccentTint()
                       : mHover  ? mTheme.Elevated().opacity(0.7f)
                                 : el::rgba(0, 0, 0, 0));
        cnv.fill_round_rect(b, metrics::kRadius);

        // Right to left: the heart, then the stars.
        const float mid = b.top + b.height() * 0.5f;
        mIcons.Draw(cnv, mFavourite ? "heart-filled" : "heart", HeartRect(b),
                    mFavourite ? mTheme.Accent() : mTheme.TextMuted());

        for (int i = 0; i < 5; ++i)
        {
            mIcons.Draw(cnv, "star", StarRect(b, i),
                        i < mRating ? mTheme.Accent() : mTheme.TextMuted().opacity(mHover ? 0.7f : 0.35f));
        }

        const float right = StarRect(b, 0).left - 12.0f;
        const auto nameFont = Font(mSelected ? Weight::SemiBold : Weight::Medium).size(metrics::kTextSize);
        const auto detailFont = Font(Weight::Regular).size(10.5f);
        const el::rect text{b.left + 12.0f, b.top + 4.0f, right, mid + 1.0f};
        FillText(cnv, Ellipsize(cnv, mName, nameFont, text.width()), nameFont, mTheme.Text(), text, HAlign::Left);

        if (!mDetail.empty())
        {
            FillText(cnv, Ellipsize(cnv, mDetail, detailFont, text.width()), detailFont, mTheme.TextMuted(),
                     el::rect{text.left, mid + 1.0f, right, b.bottom - 4.0f}, HAlign::Left);
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

        if (btn.state == el::mouse_button::left)
        {
            const auto b = ctx.bounds.inset(0.0f, 1.0f);

            if (HeartRect(b).inset(-5.0f, -8.0f).includes(btn.pos))
            {
                if (!btn.down && onFavourite)
                {
                    onFavourite();
                }

                return true;
            }

            for (int i = 0; i < 5; ++i)
            {
                if (StarRect(b, i).inset(-2.0f, -8.0f).includes(btn.pos))
                {
                    if (!btn.down && onRate)
                    {
                        onRate(i + 1 == mRating ? 0 : i + 1);
                    }

                    return true;
                }
            }
        }

        return Pressable::click(ctx, btn);
    }

    std::function<void()> onMenu, onFavourite;
    std::function<void(int rating)> onRate;

  private:
    static el::rect HeartRect(const el::rect& b)
    {
        const float mid = b.top + b.height() * 0.5f;
        return el::rect{b.right - 30.0f, mid - 8.0f, b.right - 14.0f, mid + 8.0f};
    }

    static el::rect StarRect(const el::rect& b, int index)
    {
        const float mid = b.top + b.height() * 0.5f;
        const float left = b.right - 44.0f - 5.0f * 16.0f + static_cast<float>(index) * 16.0f;
        return el::rect{left, mid - 6.5f, left + 13.0f, mid + 6.5f};
    }

    const Theme& mTheme;
    Icons& mIcons;
    std::string mName, mDetail;
    int mRating = 0;
    bool mFavourite = false, mSelected = false;
};

/// Something with a menu of its own: a right click on it calls `onMenu`; everything else goes to
/// what it wraps (a pad's button keeps its tap).
class MenuArea final : public el::proxy_base
{
  public:
    MenuArea(el::element_ptr subject, std::function<void()> menu)
        : mSubject(std::move(subject)), onMenu(std::move(menu))
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

        return proxy_base::click(ctx, btn);
    }

  private:
    el::element_ptr mSubject;

  public:
    std::function<void()> onMenu;
};

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

/// The little switch beside a Controls strip label (GATE, EQ, DOUBLER): 22 by 12.
class MiniSwitch final : public Pressable
{
  public:
    MiniSwitch(const Theme& theme, bool on) : mTheme(theme), mOn(on)
    {
    }

    void Set(bool on)
    {
        mOn = on;
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.role = AccessRole::Toggle;
        info.toggled = mOn ? 1 : 0;
        return info;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{22.0f, 12.0f}, {22.0f, 12.0f}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const float on = mMotion.Position(mOn, DrawnArea());
        cnv.fill_style(
            SwitchMotion::Mix(mTheme.ToggleBg().level(mHover ? 1.2f : 1.0f), mTheme.Accent().level(0.92f), on));
        cnv.fill_round_rect(b, 6.0f);
        cnv.fill_style(el::rgba(255, 255, 255, 235));
        cnv.add_circle(cycfi::artist::circle(el::point{b.left + 6.0f + (b.width() - 12.0f) * on, b.top + 6.0f}, 4.0f));
        cnv.fill();
    }

  private:
    void Pressed() override
    {
        mMotion.NoteToggled(DrawnArea());
    }

    const Theme& mTheme;
    bool mOn;
    SwitchMotion mMotion;
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
