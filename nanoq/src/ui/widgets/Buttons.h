#pragma once

// The buttons and switches pages share: square icon buttons, on/off switches, underlined tabs, a
// drop-down's field and a parameter's toggle. Measurements are the web UI's at its compact density.

#include "ui/widgets/Graphics.h"
#include "ui/widgets/Pressable.h"
#include "ui/widgets/SwitchMotion.h"

#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace nanoq::ui
{
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

    /// Another icon, for a toggle shown in place (play and stop).
    IconButton& Icon(std::string icon)
    {
        mIcon = std::move(icon);
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

/// A tab drawn as text at `size`, `padding` wider than its label, with the accent and an underline
/// when open (.nav-tab, .resource-browser-tab-btn).
class TextTab final : public Pressable
{
  public:
    TextTab(const Theme& theme, std::string label, bool active, float size, float padding, float height)
        : mTheme(theme), mLabel(std::move(label)), mActive(active), mSize(size), mPadding(padding), mHeight(height)
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
        const float w = el::measure_text(ctx.canvas, mLabel, Font(Weight::SemiBold).size(mSize)).x + mPadding;
        return {{w, mHeight}, {w, mHeight}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const auto ink = mActive ? mTheme.Accent() : (mHover ? mTheme.Text() : mTheme.TextSecondary());
        FillText(cnv, mLabel, Font(Weight::SemiBold).size(mSize), ink,
                 el::rect{b.left, b.top, b.right, b.bottom - 2.0f}, HAlign::Centre);

        if (mActive)
        {
            cnv.fill_style(mTheme.Accent());
            cnv.fill_rect(el::rect{b.left, b.bottom - 2.0f, b.right, b.bottom});
        }
    }

  private:
    const Theme& mTheme;
    std::string mLabel;
    bool mActive;
    float mSize, mPadding, mHeight;
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
} // namespace nanoq::ui
