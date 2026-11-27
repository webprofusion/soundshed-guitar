#pragma once

// The buttons and switches pages share: square icon buttons, on/off switches, underlined tabs, a
// drop-down's field and a parameter's toggle. Measurements are the web UI's at its compact density.

#include "ui/widgets/ControlSkin.h"
#include "ui/widgets/Graphics.h"
#include "ui/widgets/Pressable.h"
#include "ui/widgets/SwitchMotion.h"

#include <cmath>
#include <cstdio>
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
        Tool,  // the preset toolbar's keys (.preset-action-btn): no face until hovered
        Edged, // the header's gear (.nav-settings-btn): an edge and no face
        Pill,  // the control bar's chevrons and mute (.output-quick-mute-btn): 18 x 16 pills
        Well,  // the control bar's EQ (.eq-icon-btn): a sunk square that glows when on
        Accent,
        Chip,    // the effect header's Presets (.default-effect-shell-chip)
        Primary, // a dialog's confirming button: filled with the accent
        Slate,   // a model slot's previous, next and clear
        Tempo,   // the footer's tempo (.bpm-display): a display where the theme draws one
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

    /// The colour it lights in, where not the accent (muted output's coral).
    IconButton& LitColour(el::color colour)
    {
        mLitColour = colour;
        mHasLitColour = true;
        return *this;
    }

    /// Unsaved: the red tint of Save with changes to keep (.preset-action-btn-unsaved).
    IconButton& Unsaved(bool unsaved)
    {
        mUnsaved = unsaved;
        return *this;
    }

    IconButton& Disabled(bool disabled)
    {
        Enabled(!disabled);
        return *this;
    }

    /// With touch sizes, the size it is hit over (44 unless set; never under 36).
    IconButton& TouchTarget(float target)
    {
        mTouchTarget = std::max(36.0f, target);
        return *this;
    }

    el::view_limits limits(const el::basic_context& ctx) const override
    {
        const float w = FaceWidth(ctx.canvas);

        // With touch sizes it is hit over 44 px across, and as tall as its bar allows up to 44,
        // its face drawn as ever in the middle.
        if (metrics::Touch())
        {
            const float hit = metrics::Hit(w, mTouchTarget);
            return {{hit, mH}, {hit, metrics::Hit(mH, mTouchTarget)}};
        }

        return {{w, mH}, {w, mH}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = metrics::Touch() ? metrics::Face(ctx.bounds, FaceWidth(cnv), mH) : ctx.bounds;
        float radius = 8.0f;
        el::color fill = el::rgba(0, 0, 0, 0), line = el::rgba(0, 0, 0, 0);
        el::color ink = mTheme.Ink();

        // A theme that draws its own keys (Studio 1973's cream caps) draws these as its web UI's.
        const auto& skin = mTheme.Controls();
        const auto* key = SkinnedKey(skin);

        if (key != nullptr)
        {
            const auto& face = mLit && mLook != Look::Tempo ? skin.backdrops.keyOn : SkinnedFace(skin);
            const auto at = DrawKey(cnv, mTheme, b, *key, face, mHover && mEnabled, mDown);
            ink = Theme::Argb(mLook == Look::Tempo ? skin.tempo.text.colour : key->ink);

            // A chosen ink is for the theme's own surfaces (its secondary text on a dark bar,
            // say) and can vanish on a key drawn as the web UI's: a pale legend on a cream
            // cap. It is kept only where it reads against the face as the key's own ink does.
            if (mHasInk && !mLit && mLook != Look::Tempo && SameSide(mInk, ink))
            {
                ink = mInk;
            }

            DrawContent(cnv, at, mEnabled ? ink : ink.opacity(0.35f), mLook == Look::Tempo ? &skin.tempo : nullptr);
            return;
        }

        switch (mLook)
        {
        case Look::Ghost:
        case Look::Tempo:

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
            line = mTheme.ModalRule();
            break;
        case Look::Edged:
            radius = 6.0f;
            fill = mHover ? mTheme.Text().opacity(0.06f) : fill;
            line = mTheme.Border();
            ink = mTheme.TextSecondary();
            break;
        case Look::Pill:
            radius = b.height() * 0.5f;
            fill = mHover ? mTheme.Text().opacity(0.06f) : mTheme.Elevated();
            line = mHover ? mTheme.Accent() : mTheme.Border();
            ink = mHover ? mTheme.Text() : mTheme.TextSecondary();
            break;
        case Look::Well:
            radius = 6.0f;
            fill = mTheme.Text().opacity(mHover ? 0.15f : 0.05f);
            line = mTheme.Border();
            ink = mHover ? mTheme.TextSecondary() : mTheme.TextMuted();
            break;
        case Look::Tool:

            if (mHover && mEnabled)
            {
                fill = mTheme.Text().opacity(0.12f);
                ink = mTheme.Text();
            }

            break;
        case Look::Primary:
            radius = 6.0f;
            fill = mTheme.Accent().level(mHover ? 1.06f : 0.92f);
            line = mTheme.Accent();
            ink = el::rgba(255, 255, 255, 255);
            break;
        case Look::Chip:
            radius = 6.0f;
            fill = mTheme.DarkOr(mHover ? el::rgba(40, 40, 50, 255) : el::rgba(30, 30, 38, 255),
                                 mHover ? mTheme.Elevated() : mTheme.Card());
            line = mTheme.DarkOr(el::rgba(38, 38, 48, 255), mTheme.Border());
            ink = mTheme.DarkOr(el::rgba(235, 240, 250, 242), mTheme.Text());
            break;
        case Look::Slate:
            radius = 7.0f;
            fill = mTheme.DarkOr(mHover ? el::rgba(72, 78, 98, 255) : el::rgba(56, 61, 79, 255),
                                 mHover ? mTheme.Elevated() : mTheme.Card());
            line = mTheme.DarkOr(el::rgba(255, 255, 255, 18), mTheme.BorderStrong());
            ink = mTheme.DarkOr(el::rgba(203, 203, 220, 255), mTheme.Text());
            break;
        case Look::Accent:
            fill = mTheme.Accent().opacity(mHover ? 0.22f : 0.14f);
            line = mTheme.Accent().opacity(0.64f);
            ink = mTheme.Text();
            break;
        }

        if (mLit)
        {
            const auto lit = mHasLitColour ? mLitColour : mTheme.Accent();
            fill = lit.opacity(0.16f);
            line = lit.opacity(mLook == Look::Edged || mLook == Look::Pill ? 0.7f : 0.55f);
            ink = lit;

            if (mLook == Look::Edged || mLook == Look::Well)
            {
                fill = mTheme.Text().opacity(mLook == Look::Well ? 0.15f : 0.06f);
                line = mLook == Look::Well ? mTheme.Border() : lit;
            }
            else if (mLook == Look::Pill)
            {
                fill = mTheme.Elevated();
            }
        }

        // Lit takes the accent over a chosen ink: a tile that is on reads as on.
        if (mHasInk && !mLit)
        {
            ink = mInk;
        }

        if (mUnsaved && !mLit)
        {
            fill = mTheme.Error().opacity(mHover && mEnabled ? 0.28f : 0.18f);
            line = mTheme.Error().opacity(0.38f);
            ink = mTheme.Error();
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
            auto state = cnv.new_state();

            // The EQ on glows in the accent (box-shadow 0 0 8px).
            if (mLit && mLook == Look::Well)
            {
                cnv.shadow_style(el::point{0.0f, 0.0f}, 8.0f, mTheme.Accent().opacity(0.8f));
            }

            cnv.fill_style(fill);
            cnv.fill_round_rect(b, radius);
        }

        if (line.alpha > 0.0f)
        {
            cnv.stroke_style(line);
            cnv.line_width(1.0f);
            cnv.stroke_round_rect(b.inset(0.5f, 0.5f), radius);
        }

        DrawContent(cnv, b, ink, nullptr);
    }

  private:
    /// Whether two inks are both dark or both light, so both read on the same face.
    [[nodiscard]] static bool SameSide(el::color a, el::color b)
    {
        const auto luma = [](el::color c) { return 0.2126f * c.red + 0.7152f * c.green + 0.0722f * c.blue; };
        return (luma(a) < 0.5f) == (luma(b) < 0.5f);
    }

    /// The theme's key for this look, where it draws its own (its web UI's toolbar keys, the
    /// arrows, the confirming key, a chip, the tempo's display; lit, its latched key).
    [[nodiscard]] const KeySkin* SkinnedKey(const ControlSkin& skin) const
    {
        const KeySkin* key = nullptr;

        switch (mLook)
        {
        case Look::Boxed:
        case Look::Tool:
        case Look::Edged:
            key = &skin.keys.tool;
            break;
        case Look::Soft:
            key = &skin.keys.nav;
            break;
        case Look::Primary:
        case Look::Accent:
            key = &skin.keys.primary;
            break;
        case Look::Chip:
        case Look::Slate:
            key = &skin.keys.plain;
            break;
        case Look::Tempo:
            key = &skin.tempo.face;
            break;
        case Look::Ghost:
            break;
        }

        if (key != nullptr && mLit && mLook != Look::Tempo)
        {
            key = &skin.keys.on;
        }

        return key != nullptr && key->drawn ? key : nullptr;
    }

    [[nodiscard]] const SurfaceBackdrop& SkinnedFace(const ControlSkin& skin) const
    {
        const auto& b = skin.backdrops;

        switch (mLook)
        {
        case Look::Boxed:
        case Look::Tool:
        case Look::Edged:
            return b.keyTool;
        case Look::Soft:
            return b.keyNav;
        case Look::Primary:
        case Look::Accent:
            return b.keyPrimary;
        case Look::Tempo:
            return b.tempo;
        default:
            return b.keyPlain;
        }
    }

    /// The icon, label and caret in `b`, centred as one group; a display's label glows.
    void DrawContent(el::canvas& cnv, el::rect b, el::color ink, const DisplaySkin* display) const
    {
        const auto font = Font(Weight::SemiBold).size(LabelSize());
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
            const el::rect at{x, b.top, x + textW + 1.0f, b.bottom};

            if (display != nullptr)
            {
                DrawDisplayText(cnv, mLabel, font, *display, ink, at, HAlign::Left);
            }
            else
            {
                FillText(cnv, mLabel, font, ink, at, HAlign::Left);
            }

            x += textW;
        }

        if (mCaret)
        {
            mIcons.Draw(cnv, "ui-caret", el::rect{x + 2.0f, cy - 5.0f, x + 2.0f + caretW, cy + 5.0f},
                        ink.opacity(ink.alpha * 0.8f));
        }
    }

    /// A label is read to work the button: 11 px at the least with touch sizes.
    [[nodiscard]] float LabelSize() const
    {
        return metrics::Readable(mLabelSize);
    }

    /// The width it draws at: its size, or what its label needs.
    [[nodiscard]] float FaceWidth(el::canvas& cnv) const
    {
        float w = mW;

        if (!mLabel.empty())
        {
            const float text = el::measure_text(cnv, mLabel, Font(Weight::SemiBold).size(LabelSize())).x;
            w = std::max(w, (mIcon.empty() ? 0.0f : mIconSize + 6.0f) + text + (mCaret ? 12.0f : 0.0f) + 20.0f);
        }
        else if (mCaret)
        {
            w += 12.0f;
        }

        return w;
    }

    const Theme& mTheme;
    Icons& mIcons;
    std::string mIcon, mLabel;
    Look mLook;
    float mW = 28.0f, mH = 28.0f, mIconSize = 14.0f, mLabelSize = 10.0f;
    float mTouchTarget = metrics::kTouchTarget;
    bool mCaret = false, mLit = false, mHasInk = false, mUnsaved = false, mHasLitColour = false;
    el::color mInk, mLitColour;
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
        return {{metrics::Hit(40.0f), 24.0f}, {metrics::Hit(40.0f), metrics::Hit(24.0f, 36.0f)}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = metrics::Touch() ? metrics::Face(ctx.bounds, 40.0f, 24.0f) : ctx.bounds;
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

    /// The header's tabs (.nav-tab): bold, spaced `tracking` em, muted until hovered or chosen.
    TextTab& Nav(float tracking)
    {
        mNav = true;
        mTracking = tracking;
        return *this;
    }

    el::view_limits limits(const el::basic_context& ctx) const override
    {
        const float w = TrackedWidth(ctx.canvas, mLabel, TabFont(), mTracking) + mPadding;
        return {{w, mHeight}, {w, mHeight}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const auto rest = mNav ? mTheme.TextMuted() : mTheme.TextSecondary();
        const auto ink = mActive ? mTheme.Accent() : (mHover ? mTheme.Text() : rest);
        FillTrackedText(cnv, mLabel, TabFont(), ink, el::rect{b.left, b.top, b.right, b.bottom - 2.0f}, HAlign::Centre,
                        mTracking);

        if (mActive)
        {
            cnv.fill_style(mTheme.Accent());
            cnv.fill_rect(el::rect{b.left, b.bottom - 2.0f, b.right, b.bottom});
        }
    }

  private:
    [[nodiscard]] el::font_descr TabFont() const
    {
        return Font(Weight::SemiBold).size(mSize);
    }

    const Theme& mTheme;
    std::string mLabel;
    bool mActive;
    bool mNav = false;
    float mSize, mPadding, mHeight;
    float mTracking = 0.0f;
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
        const auto& text = mTheme.Controls().tabs.text;
        const float w = metrics::Hit(
            TrackedWidth(ctx.canvas, mLabel, ControlFont(text, LabelSize()), LabelSize(), text.tracking) + 16.0f);
        return {{w, 26.0f}, {w, metrics::Hit(26.0f, 40.0f)}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        // A touch-sized hit area keeps the label and its underline at the foot, where they sit.
        const auto b = metrics::Touch() ? el::rect{ctx.bounds.left, std::max(ctx.bounds.top, ctx.bounds.bottom - 26.0f),
                                                   ctx.bounds.right, ctx.bounds.bottom}
                                        : ctx.bounds;
        // The web UI's .node-param-tab as the theme draws it: spaced capitals, the chosen one in
        // its colour over a line as wide as its label.
        const auto& tabs = mTheme.Controls().tabs;
        const auto& text = mActive ? tabs.active : tabs.text;
        const auto ink = !mActive && mHover ? mTheme.Text() : Theme::Argb(text.colour);
        DrawControlText(cnv, mLabel, text, ink, el::rect{b.left, b.top, b.right, b.bottom - 3.0f}, HAlign::Centre,
                        LabelSize());

        if (mActive)
        {
            const float w = TrackedWidth(cnv, mLabel, ControlFont(text, LabelSize()), LabelSize(), text.tracking);
            const float cx = (b.left + b.right) * 0.5f, line = std::max(1.0f, tabs.lineWidth);
            cnv.fill_style(Theme::Argb(tabs.line));
            cnv.fill_rect(el::rect{cx - w * 0.5f, b.bottom - line, cx + w * 0.5f, b.bottom});
        }
    }

  private:
    [[nodiscard]] static float LabelSize()
    {
        return metrics::Readable(10.0f);
    }

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
        return {{120.0f, 26.0f}, {el::full_extent, metrics::Hit(26.0f, 36.0f)}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = metrics::Touch() ? metrics::Face(ctx.bounds, ctx.bounds.width(), 26.0f) : ctx.bounds;
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

/// An empty model or IR slot (.resource-picker-label.is-empty): a dashed accent drop zone that
/// says what to do, "+ Choose an IR", the file a drop takes, and an accent Browse. An optional
/// slot (the cabinet's second IR) is asked for quietly: a faint dashed line and "+ Add a second IR".
/// A tap opens the library.
class EmptySlotField final : public Pressable
{
  public:
    EmptySlotField(const Theme& theme, std::string prompt, std::string hint, bool optional)
        : mTheme(theme), mPrompt(std::move(prompt)), mHint(std::move(hint)), mOptional(optional)
    {
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.role = AccessRole::Button;
        info.name = mHint.empty() ? mPrompt : mPrompt + ", " + mHint;
        return info;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{120.0f, 26.0f}, {el::full_extent, el::full_extent}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = metrics::Touch() ? metrics::Face(ctx.bounds, ctx.bounds.width(), 26.0f) : ctx.bounds;
        const auto accent = mTheme.Accent();
        const auto ink = mTheme.Ink();

        if (!mOptional)
        {
            cnv.fill_style(accent.opacity(mHover ? 0.13f : 0.07f));
            cnv.fill_round_rect(b, 6.0f);
        }

        cnv.stroke_style(mOptional ? (mHover ? accent.opacity(0.6f) : ink.opacity(0.22f))
                                   : (mHover ? accent : accent.opacity(0.6f)));
        cnv.line_width(1.0f);
        StrokeDashedRoundRect(cnv, b.inset(0.5f, 0.5f), 6.0f, 4.0f, 3.0f);

        // The plus, 16 px in the accent (12 px in the text's colour, optional).
        const float cy = b.top + b.height() * 0.5f;
        const float arm = mOptional ? 5.0f : 6.0f;
        const float px = b.left + (mOptional ? 10.0f : 12.0f) + arm;
        const auto plusColour = mOptional ? (mHover ? ink : mTheme.TextSecondary()) : accent;
        cnv.stroke_style(plusColour);
        cnv.line_width(1.6f);
        cnv.begin_path();
        cnv.move_to({px - arm, cy});
        cnv.line_to({px + arm, cy});
        cnv.move_to({px, cy - arm});
        cnv.line_to({px, cy + arm});
        cnv.stroke();

        float right = b.right - 4.0f;
        const float left = px + arm + 10.0f;
        const auto font = Font(mOptional ? Weight::Medium : Weight::SemiBold).size(12.0f);
        const float promptWidth = MeasureTextWidth(mPrompt, font);
        const auto actionFont = Font(Weight::SemiBold).size(11.0f);
        const float actionWidth = MeasureTextWidth("Browse", actionFont) + 24.0f;

        // Browse, a button-shape at the right (.resource-picker-label-action), where the prompt
        // still reads whole beside it: the whole zone opens the library anyway.
        if (!mOptional && right - actionWidth - 10.0f - left >= promptWidth)
        {
            const el::rect action{right - actionWidth, b.top + 3.0f, right, b.bottom - 3.0f};
            cnv.fill_style(accent);
            cnv.fill_round_rect(action, 4.0f);
            FillText(cnv, "Browse", actionFont, mTheme.OnAccent(), action, HAlign::Centre);
            right = action.left - 10.0f;
        }

        const auto promptColour = mOptional && !mHover ? mTheme.TextSecondary() : ink;
        FillText(cnv, Ellipsize(cnv, mPrompt, font, std::max(0.0f, right - left)), font, promptColour,
                 el::rect{left, b.top, right, b.bottom}, HAlign::Left);

        // The drop hint after the prompt where the line has room for it.
        if (!mOptional && !mHint.empty())
        {
            const auto hintFont = Font(Weight::Regular).size(10.0f);
            const float hintLeft = left + promptWidth + 8.0f;

            if (right - hintLeft >= 60.0f)
            {
                FillText(cnv, Ellipsize(cnv, mHint, hintFont, right - hintLeft), hintFont, mTheme.TextMuted(),
                         el::rect{hintLeft, b.top, right, b.bottom}, HAlign::Left);
            }
        }
    }

  private:
    const Theme& mTheme;
    std::string mPrompt;
    std::string mHint;
    bool mOptional;
};

/// The effect header's peak rail (.default-effect-shell-meter): a 4 px strip filled green to red
/// with the node's peak over -48..0 dBFS, faded with no reading, glowing red while clipped. A tap
/// shows or hides the DSP status, as the web UI's meter toggle does.
class PeakRail final : public Pressable
{
  public:
    struct Reading
    {
        bool inactive = true;
        bool clipped = false;
        int hundredths = 0; // the fill in hundredths: finer steps are sub-pixel
        int peakTenths = 0;
        int headroomTenths = 0;

        bool operator==(const Reading&) const = default;
    };

    void Set(const Reading& reading)
    {
        mReading = reading;
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.role = AccessRole::Button;
        info.name = NameOr("Toggle DSP status");

        if (mReading.inactive)
        {
            info.value = "No diagnostics data for this node";
        }
        else
        {
            char text[96];
            std::snprintf(text, sizeof text, "Node peak: %.1f dBFS, Headroom: %.1f dB", mReading.peakTenths / 10.0,
                          mReading.headroomTenths / 10.0);
            info.value = text;
        }

        return info;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{24.0f, 16.0f}, {el::full_extent, 16.0f}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const float cy = ctx.bounds.top + ctx.bounds.height() * 0.5f;
        const el::rect strip{ctx.bounds.left, cy - 2.0f, ctx.bounds.right, cy + 2.0f};
        auto group = cnv.new_group(mReading.inactive ? 0.45f : 1.0f);

        if (mReading.clipped)
        {
            auto state = cnv.new_state();
            cnv.shadow_style(el::point{0.0f, 0.0f}, 9.0f, el::rgba(255, 94, 94, 40));
            cnv.fill_style(el::rgba(255, 110, 110, 31));
            cnv.fill_round_rect(strip.inset(-1.0f, -1.0f), 3.0f);
        }

        const float fill = strip.width() * static_cast<float>(mReading.hundredths) / 100.0f;

        if (fill >= 0.5f)
        {
            // The gradient spans the filled length, as the web's background-size scales it.
            cycfi::artist::canvas::linear_gradient ramp{el::point{strip.left, cy}, el::point{strip.left + fill, cy}};
            ramp.add_color_stop(0.0f, el::rgba(72, 222, 170, 140));
            ramp.add_color_stop(1.0f, el::rgba(255, 94, 94, 235));
            cnv.fill_style(ramp);
            cnv.fill_round_rect(el::rect{strip.left, strip.top, strip.left + fill, strip.bottom}, 2.0f);
        }
    }

  private:
    Reading mReading;
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
        // The web UI's .toggle-switch between the group's label and value, as the theme draws them
        // (the same sizes as the effect editor's knobs beside it).
        const auto& skin = mTheme.Controls();
        DrawControlText(cnv, mName, skin.label, el::rect{b.left, b.top, b.right, b.top + 12.0f}, HAlign::Centre,
                        metrics::Readable(skin.label.size + 1.0f));
        const float cx = (b.left + b.right) * 0.5f;
        const float w = skin.switchOff.width, h = skin.switchOff.height;
        const el::rect track{cx - w * 0.5f, b.top + 31.0f - h * 0.5f, cx + w * 0.5f, b.top + 31.0f + h * 0.5f};
        DrawSwitch(cnv, track, skin, mMotion.Position(mOn, DrawnArea()), mHover);
        DrawControlText(cnv, mOn ? "On" : "Off", skin.value, el::rect{b.left, b.bottom - 13.0f, b.right, b.bottom},
                        HAlign::Centre, skin.value.size + 1.0f);
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

/// The little switch beside a Controls strip label (GATE, EQ, DOUBLER): 22 by 12. With touch
/// sizes the Controls cell gives it its label to draw (Title), so a tap on either flips it, as
/// a label does a checkbox.
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

    /// The label drawn before it, as one control with it, at `size`.
    MiniSwitch& Title(std::string title, float size, el::color ink)
    {
        mTitle = std::move(title);
        mTitleSize = size;
        mTitleInk = ink;
        return *this;
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.role = AccessRole::Toggle;
        info.toggled = mOn ? 1 : 0;
        return info;
    }

    el::view_limits limits(const el::basic_context& ctx) const override
    {
        if (!mTitle.empty())
        {
            const float w = TitleWidth(ctx.canvas) + kTitleGap + 22.0f;
            return {{w, 12.0f}, {el::full_extent, metrics::Hit(12.0f, 36.0f)}};
        }

        return {{metrics::Hit(22.0f, metrics::kTouchFloor), 12.0f}, {metrics::Hit(22.0f), metrics::Hit(12.0f, 36.0f)}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        el::rect b = ctx.bounds;

        if (!mTitle.empty())
        {
            // The label and the switch, centred as one.
            const float titleW = TitleWidth(cnv);
            const float left = b.left + std::max(0.0f, (b.width() - titleW - kTitleGap - 22.0f) * 0.5f);
            FillText(cnv, mTitle, Font(Weight::SemiBold).size(mTitleSize), mTitleInk,
                     el::rect{left, b.top, left + titleW + 1.0f, b.bottom}, HAlign::Left);
            const float cy = b.top + b.height() * 0.5f;
            b = el::rect{left + titleW + kTitleGap, cy - 6.0f, left + titleW + kTitleGap + 22.0f, cy + 6.0f};
        }
        else if (metrics::Touch())
        {
            b = metrics::Face(b, 22.0f, 12.0f);
        }

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

    static constexpr float kTitleGap = 5.0f;

    [[nodiscard]] float TitleWidth(el::canvas& cnv) const
    {
        return el::measure_text(cnv, mTitle, Font(Weight::SemiBold).size(mTitleSize)).x;
    }

    const Theme& mTheme;
    bool mOn;
    SwitchMotion mMotion;
    std::string mTitle;
    float mTitleSize = 9.0f;
    el::color mTitleInk;
};
} // namespace nanoq::ui
