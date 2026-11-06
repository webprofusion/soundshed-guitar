#pragma once

// The metronome card's own controls, drawn as the web UI draws them (css/modals/metronome.css):
// the tempo's arrows and readout, the round TAP and transport buttons, and the beat strip.
// Metronome.cpp lays them out in the shared dialog frame (ui/dialogs/DialogChrome.h).

#include "ui/dialogs/DialogChrome.h"
#include "ui/Drawn.h"
#include "ui/widgets/Widgets.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace nanoq::ui::metronome
{
using cycfi::artist::canvas;

/// One of the tempo's arrows (.metronome-bpm-btn), up or down. Shift steps by a tenth.
class TempoArrow final : public Pressable
{
  public:
    TempoArrow(const Theme& theme, bool up) : mTheme(theme), mUp(up)
    {
        onClick = [this] {
            if (onStep)
            {
                onStep(mFine);
            }
        };
    }

    /// Called on a click, with whether Shift was held.
    std::function<void(bool fine)> onStep;

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.name = NameOr(mUp ? "Tempo up" : "Tempo down");
        return info;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{28.0f, 24.0f}, {28.0f, 24.0f}};
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        mFine = (btn.modifiers & el::mod_shift) != 0;
        return Pressable::click(ctx, btn);
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const float fade = mEnabled ? 1.0f : 0.6f;
        cnv.fill_style((mHover && mEnabled ? mTheme.Elevated() : mTheme.Tertiary()).opacity(fade));
        cnv.fill_round_rect(b, 6.0f);
        cnv.stroke_style(mTheme.BorderStrong().opacity(fade));
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(b.inset(0.5f, 0.5f), 6.0f);
        const auto c = cycfi::artist::center_point(b);
        const float tip = mUp ? -3.5f : 3.5f;
        cnv.fill_style((mHover && mEnabled ? mTheme.Text() : mTheme.TextSecondary()).opacity(fade));
        cnv.begin_path();
        cnv.move_to(c.x, c.y + tip);
        cnv.line_to(c.x + 4.5f, c.y - tip);
        cnv.line_to(c.x - 4.5f, c.y - tip);
        cnv.close_path();
        cnv.fill();
    }

  private:
    const Theme& mTheme;
    bool mUp;
    bool mFine = false;
};

/// The round buttons: TAP (.metronome-tap) and the transport (.metronome-play). An outlined
/// circle that lights its rim under the pointer and shrinks a little while pressed; the
/// transport's rim and glyph take the accent while the click runs.
class RoundButton final : public Pressable
{
  public:
    RoundButton(const Theme& theme, Icons& icons, float diameter, std::string label)
        : mTheme(theme), mIcons(icons), mDiameter(diameter), mLabel(std::move(label))
    {
    }

    /// The glyph instead of a label: "ui-play" or "ui-stop".
    void Icon(std::string icon)
    {
        mIcon = std::move(icon);
    }

    void Active(bool active)
    {
        mActive = active;
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.name = NameOr(mLabel.empty() ? IconButton::IconMeaning(mIcon) : mLabel);
        info.selected = mActive;
        return info;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{mDiameter, mDiameter}, {mDiameter, mDiameter}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto c = cycfi::artist::center_point(ctx.bounds);
        const float fade = mEnabled ? 1.0f : 0.6f;
        const float r = (mDiameter * 0.5f - 1.0f) * (mDown && mEnabled ? 0.94f : 1.0f);
        const bool lit = mEnabled && (mHover || mDown);
        cnv.fill_style(mTheme.Bar().opacity(fade));
        cnv.add_circle(cycfi::artist::circle(c, r));
        cnv.fill();
        cnv.stroke_style((mActive || lit ? mTheme.Accent() : mTheme.BorderStrong()).opacity(fade));
        cnv.line_width(2.0f);
        cnv.add_circle(cycfi::artist::circle(c, r));
        cnv.stroke();

        if (!mIcon.empty())
        {
            const float half = 9.0f;
            mIcons.Draw(cnv, mIcon, el::rect{c.x - half, c.y - half, c.x + half, c.y + half},
                        (mActive ? mTheme.Accent() : mTheme.Text()).opacity(fade));
            return;
        }

        FillText(cnv, mLabel, Font(Weight::SemiBold).size(metrics::kSmallText),
                 (lit ? mTheme.Text() : mTheme.TextSecondary()).opacity(fade),
                 el::rect{c.x - r, c.y - r, c.x + r, c.y + r}, HAlign::Centre);
    }

  private:
    const Theme& mTheme;
    Icons& mIcons;
    float mDiameter;
    std::string mLabel;
    std::string mIcon;
    bool mActive = false;
};

/// The tempo in large figures (.metronome-bpm-input). The wheel and, with focus, the arrow keys
/// step it; a double click (or Enter, or a digit) types a tempo in its place, as the web UI's
/// number field takes one.
class TempoReadout final : public el::element, public Drawn, public Focusable
{
  public:
    explicit TempoReadout(const Theme& theme) : mTheme(theme)
    {
    }

    std::function<void(int steps)> onWheel;
    /// What the field opens with, and what was typed into it.
    std::function<std::string()> editText;
    std::function<void(const std::string& typed)> onTyped;

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info;
        info.role = mEdit.Active() ? AccessRole::Edit : AccessRole::Slider;
        info.name = NameOr("Tempo, BPM");
        info.value = mEdit.Active() ? mEdit.Value() : mText;
        info.enabled = mEnabled;
        return info;
    }

    [[nodiscard]] bool Editing() const override
    {
        return mEdit.Active();
    }

    bool wants_focus() const override
    {
        return FocusWanted(mEnabled) || mEdit.Active();
    }

    void begin_focus(focus_request) override
    {
        NoteFocus(true);
    }

    bool end_focus() override
    {
        InlineEdit::Leave(mEdit, onTyped, [this] { RepaintSoon(DrawnArea()); });
        NoteFocus(false);
        return true;
    }

    bool key(const el::context& ctx, el::key_info k) override
    {
        bool changed = false;
        const bool used = InlineEdit::FieldKey(mEdit, k, editText, onTyped, changed);

        if (changed)
        {
            ctx.view.refresh(ctx);
        }

        if (used || mEdit.Active())
        {
            return used;
        }

        if ((k.action == el::key_action::press || k.action == el::key_action::repeat) && onWheel &&
            (k.key == el::key_code::up || k.key == el::key_code::down))
        {
            onWheel(k.key == el::key_code::up ? 1 : -1);
            return true;
        }

        return false;
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

    void Set(std::string text, bool enabled)
    {
        mText = std::move(text);
        mEnabled = enabled;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{150.0f, 58.0f}, {150.0f, 58.0f}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);

        if (mEdit.Active())
        {
            mEdit.Draw(ctx.canvas, ctx.bounds, mTheme, Font(Weight::SemiBold).size(40.0f));
            return;
        }

        FillText(ctx.canvas, mText, Font(Weight::SemiBold).size(48.0f), mTheme.Text(), ctx.bounds, HAlign::Left);
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (mEnabled && btn.down && btn.state == el::mouse_button::left && btn.num_clicks == 2 && editText && onTyped)
        {
            // The tempo turns into a field where it is, and takes focus for the keys once this
            // click is done with.
            mEdit.Begin(editText());
            ctx.view.refresh(ctx);
            ctx.view.post([self = weak_from_this(), &view = ctx.view] {
                if (const auto readout = self.lock())
                {
                    FocusInView(view, *readout);
                }
            });
        }

        return mEnabled;
    }

    bool scroll(const el::context&, el::point dir, el::point) override
    {
        if (!mEnabled || dir.y == 0.0f || !ParamKnob::sWheelAdjusts || !onWheel)
        {
            return false;
        }

        onWheel(dir.y > 0.0f ? 1 : -1);
        return true;
    }

    bool wants_control() const override
    {
        return true;
    }

  private:
    const Theme& mTheme;
    std::string mText = "120.0";
    bool mEnabled = true;
    InlineEdit mEdit;
};

/// The beat strip (.metro-beats): a dot per beat, sized and shaded by its accent, ticks under
/// it for the subdivision, and a wider gap where a group starts (the 3 of 7/8 as 2+2+3). A click
/// steps a beat's accent; the beat the engine just played is lit in the accent for a moment.
class BeatStrip final : public el::element, public Drawn
{
  public:
    explicit BeatStrip(const Theme& theme) : mTheme(theme)
    {
    }

    /// Called with the beat clicked.
    std::function<void(std::size_t beat)> onBeat;

    void Set(std::string pattern, std::vector<bool> heads, int ticks, bool editable)
    {
        mPattern = std::move(pattern);
        mHeads = std::move(heads);
        mTicks = ticks;
        mEditable = editable;
    }

    void Light(int beat)
    {
        mLit = beat;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{0.0f, 46.0f}, {el::full_extent, 46.0f}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const float fade = mEditable ? 1.0f : 0.55f;
        const auto cells = Cells(ctx.bounds);

        for (std::size_t i = 0; i < cells.size(); ++i)
        {
            const char level = mPattern[i];
            const bool off = level == 'S';
            const bool lit = static_cast<int>(i) == mLit;
            float size = level == 'H' ? 22.0f : level == 'M' ? 19.0f : 16.0f;
            // The dots hang from the top of the strip, as the web UI's do (align-items: flex-start).
            const el::point c{(cells[i].left + cells[i].right) * 0.5f, cells[i].top + 6.0f + size * 0.5f};

            if (static_cast<int>(i) == mHover && mEditable)
            {
                cnv.fill_style(mTheme.Accent().opacity(0.16f));
                cnv.add_circle(cycfi::artist::circle(c, size * 0.5f + 3.0f));
                cnv.fill();
            }

            if (lit)
            {
                size *= off ? 1.1f : 1.25f;
            }

            const float r = size * 0.5f;

            if (!off)
            {
                const el::color fill = lit            ? mTheme.Accent()
                                       : level == 'H' ? mTheme.Text()
                                       : level == 'M' ? mTheme.TextSecondary()
                                                      : mTheme.TextMuted();
                cnv.fill_style(fill.opacity(fade));
                cnv.add_circle(cycfi::artist::circle(c, r));
                cnv.fill();
            }
            else
            {
                cnv.stroke_style((lit ? mTheme.Accent() : mTheme.BorderStrong()).opacity(fade));
                cnv.line_width(2.0f);
                cnv.add_circle(cycfi::artist::circle(c, r - 1.0f));
                cnv.stroke();
            }

            // A tick for each subdivision after the beat itself, under the largest dot's foot.
            if (mTicks > 1)
            {
                const int count = mTicks - 1;
                const float width = static_cast<float>(count) * 3.0f + static_cast<float>(count - 1) * 3.0f;
                float x = c.x - width * 0.5f + 1.5f;
                const float y = cells[i].top + 6.0f + 22.0f + 5.0f + 2.0f;
                cnv.fill_style(mTheme.TextMuted().opacity(0.7f * fade));

                for (int t = 0; t < count; ++t, x += 6.0f)
                {
                    cnv.add_circle(cycfi::artist::circle(el::point{x, y}, 1.5f));
                }

                cnv.fill();
            }
        }
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (!mEditable || btn.state != el::mouse_button::left)
        {
            return false;
        }

        if (btn.down)
        {
            if (const int beat = Hit(ctx.bounds, btn.pos); beat >= 0 && onBeat)
            {
                onBeat(static_cast<std::size_t>(beat));
            }
        }

        return true;
    }

    bool cursor(const el::context& ctx, el::point p, el::cursor_tracking status) override
    {
        const int hover = status == el::cursor_tracking::leaving ? -1 : Hit(ctx.bounds, p);

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

  private:
    static constexpr float kCell = 26.0f;     // the largest dot and its padding
    static constexpr float kGap = 10.0f;      // between beats
    static constexpr float kGroupGap = 14.0f; // before a group's first beat, on top of the gap

    [[nodiscard]] std::vector<el::rect> Cells(el::rect bounds) const
    {
        std::vector<el::rect> cells;
        float width = 0.0f;

        for (std::size_t i = 0; i < mPattern.size(); ++i)
        {
            width += (i > 0 ? kGap : 0.0f) + (IsGroupHead(i) ? kGroupGap : 0.0f) + kCell;
        }

        float x = (bounds.left + bounds.right - width) * 0.5f;

        for (std::size_t i = 0; i < mPattern.size(); ++i)
        {
            x += (i > 0 ? kGap : 0.0f) + (IsGroupHead(i) ? kGroupGap : 0.0f);
            cells.push_back(el::rect{x, bounds.top, x + kCell, bounds.bottom});
            x += kCell;
        }

        return cells;
    }

    [[nodiscard]] bool IsGroupHead(std::size_t i) const
    {
        return i > 0 && i < mHeads.size() && mHeads[i];
    }

    [[nodiscard]] int Hit(el::rect bounds, el::point p) const
    {
        const auto cells = Cells(bounds);

        for (std::size_t i = 0; i < cells.size(); ++i)
        {
            if (p.x >= cells[i].left - kGap * 0.5f && p.x < cells[i].right + kGap * 0.5f)
            {
                return static_cast<int>(i);
            }
        }

        return -1;
    }

    const Theme& mTheme;
    std::string mPattern;
    std::vector<bool> mHeads;
    int mTicks = 1;
    bool mEditable = true;
    int mLit = -1;
    int mHover = -1;
};
} // namespace nanoq::ui::metronome
