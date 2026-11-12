#pragma once

// The frame every dialog shares, as the web UI draws its modals (css/modals/shell.css): a
// rounded card with a raised header band holding the title and a close cross, the body under
// it, and, when the dialog has actions, a raised footer band with its buttons at the right.
// Also the two small controls dialogs use for a yes or no: the checkbox and the switch.
// Shell::ShowDialog puts the pieces together.

#include "ui/widgets/Buttons.h"

#include <string>
#include <vector>
#include <utility>

namespace nanoq::ui
{
inline constexpr float kDialogHeaderHeight = 56.0f;
inline constexpr float kDialogFooterHeight = 60.0f;
inline constexpr float kDialogRadius = 12.0f;

/// The card (.modal-content): the dialog's colour, the header band (.modal-header) with a rule
/// under it, and the footer band (.modal-footer) with a rule over it when `footer` is not 0.
class DialogFace final : public el::element
{
  public:
    DialogFace(const Theme& theme, float footer, float header = kDialogHeaderHeight)
        : mTheme(theme), mHeader(header), mFooter(footer)
    {
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        cnv.fill_style(mTheme.ModalBg());
        cnv.fill_round_rect(b, kDialogRadius);
        {
            auto state = cnv.new_state();
            cnv.add_round_rect(b, kDialogRadius);
            cnv.clip();
            cnv.fill_style(mTheme.ModalRaised());

            if (mHeader > 0.0f)
            {
                cnv.fill_rect(el::rect{b.left, b.top, b.right, b.top + mHeader});
            }

            if (mFooter > 0.0f)
            {
                cnv.fill_rect(el::rect{b.left, b.bottom - mFooter, b.right, b.bottom});
            }
        }
        cnv.fill_style(mTheme.ModalRule());

        if (mHeader > 0.0f)
        {
            cnv.fill_rect(el::rect{b.left, b.top + mHeader - 1.0f, b.right, b.top + mHeader});
        }

        if (mFooter > 0.0f)
        {
            cnv.fill_rect(el::rect{b.left, b.bottom - mFooter, b.right, b.bottom - mFooter + 1.0f});
        }

        cnv.stroke_style(mTheme.ModalBorder());
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(b.inset(0.5f, 0.5f), kDialogRadius);
    }

  private:
    const Theme& mTheme;
    float mHeader, mFooter;
};

/// The header's close cross (.modal-close): muted, on a faint square under the pointer.
class CloseCross final : public Pressable
{
  public:
    explicit CloseCross(const Theme& theme) : mTheme(theme)
    {
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.name = NameOr("Close");
        return info;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        // Touch sizes: hit over 40 px, drawn the same 28 in the middle.
        const float size = metrics::Hit(28.0f, 40.0f);
        return {{size, size}, {size, size}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = metrics::Face(ctx.bounds, 28.0f, 28.0f);

        if (mHover)
        {
            cnv.fill_style(mTheme.ModalText().opacity(0.08f));
            cnv.fill_round_rect(b, 6.0f);
        }

        const auto c = cycfi::artist::center_point(b);
        constexpr float kArm = 4.5f;
        cnv.stroke_style(mHover ? mTheme.ModalText() : mTheme.ModalTextMuted());
        cnv.line_width(2.0f);
        cnv.line_cap(cycfi::artist::canvas::round);
        cnv.begin_path();
        cnv.move_to(c.x - kArm, c.y - kArm);
        cnv.line_to(c.x + kArm, c.y + kArm);
        cnv.move_to(c.x + kArm, c.y - kArm);
        cnv.line_to(c.x - kArm, c.y + kArm);
        cnv.stroke();
    }

  private:
    const Theme& mTheme;
};

/// A checkbox with its label (an input type=checkbox in the web UI): a rounded square, filled
/// with the accent and ticked when checked, and the label after it. A tap anywhere on it
/// flips it; `onClick` decides what that means, then the owner calls Set().
class CheckBox final : public Pressable
{
  public:
    CheckBox(const Theme& theme, std::string label, float size = metrics::kSmallText)
        : mTheme(theme), mLabel(std::move(label)), mSize(size)
    {
    }

    void Set(bool checked)
    {
        mChecked = checked;
    }

    [[nodiscard]] bool Checked() const
    {
        return mChecked;
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.role = AccessRole::CheckBox;
        info.name = NameOr(mLabel);
        info.toggled = mChecked ? 1 : 0;
        return info;
    }

    el::view_limits limits(const el::basic_context& ctx) const override
    {
        const float w = 24.0f + el::measure_text(ctx.canvas, mLabel, Font(Weight::Regular).size(mSize)).x;
        return {{w, 22.0f}, {w, 22.0f}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const float fade = mEnabled ? 1.0f : 0.5f;
        const float cy = b.top + b.height() * 0.5f;
        const el::rect box{b.left + 0.5f, cy - 8.0f, b.left + 16.5f, cy + 8.0f};

        if (mChecked)
        {
            cnv.fill_style(mTheme.Accent().level(mHover ? 1.08f : 1.0f).opacity(fade));
            cnv.fill_round_rect(box, 4.0f);
            cnv.stroke_style(el::rgba(255, 255, 255, 245).opacity(fade));
            cnv.line_width(2.0f);
            cnv.line_cap(cycfi::artist::canvas::round);
            cnv.line_join(cycfi::artist::canvas::round_join);
            cnv.begin_path();
            cnv.move_to(box.left + 4.0f, cy + 0.5f);
            cnv.line_to(box.left + 7.0f, cy + 3.5f);
            cnv.line_to(box.left + 12.0f, cy - 3.0f);
            cnv.stroke();
        }
        else
        {
            cnv.fill_style(mTheme.Bar().opacity(fade));
            cnv.fill_round_rect(box, 4.0f);
            cnv.stroke_style((mHover && mEnabled ? mTheme.Accent() : mTheme.BorderStrong()).opacity(fade));
            cnv.line_width(1.0f);
            cnv.stroke_round_rect(box, 4.0f);
        }

        const auto font = Font(Weight::Regular).size(mSize);
        const el::rect text{b.left + 24.0f, b.top, b.right, b.bottom};
        FillText(cnv, Ellipsize(cnv, mLabel, font, text.width()), font, mTheme.ModalText().opacity(fade), text,
                 HAlign::Left);
    }

  private:
    const Theme& mTheme;
    std::string mLabel;
    float mSize;
    bool mChecked = false;
};

/// A picker's trigger (the web UI's .metro-select-trigger and themed selects): the choice in a filled box with a
/// chevron at the right, its border lit under the pointer. A tap opens the list under it.
class PickerField final : public Pressable
{
  public:
    PickerField(const Theme& theme, Icons& icons) : mTheme(theme), mIcons(icons)
    {
    }

    void Set(std::string text)
    {
        mText = std::move(text);
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.role = AccessRole::ComboBox;
        info.value = mText;
        return info;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{60.0f, 32.0f}, {el::full_extent, 32.0f}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const float fade = mEnabled ? 1.0f : 0.6f;
        cnv.fill_style(mTheme.Tertiary().opacity(fade));
        cnv.fill_round_rect(b, 8.0f);
        cnv.stroke_style((mHover && mEnabled ? mTheme.Accent() : mTheme.BorderStrong()).opacity(fade));
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(b.inset(0.5f, 0.5f), 8.0f);
        const auto font = Font(Weight::Regular).size(metrics::kSmallText);
        const el::rect text{b.left + 10.0f, b.top, b.right - 30.0f, b.bottom};
        FillText(cnv, Ellipsize(cnv, mText, font, text.width()), font, mTheme.Text().opacity(fade), text, HAlign::Left);
        const float cy = b.top + b.height() * 0.5f;
        mIcons.Draw(cnv, "ui-caret", el::rect{b.right - 24.0f, cy - 7.0f, b.right - 10.0f, cy + 7.0f},
                    mTheme.TextMuted().opacity(fade));
    }

  private:
    const Theme& mTheme;
    Icons& mIcons;
    std::string mText;
};

/// A plain on/off switch (.toggle-switch): a 36 by 20 pill, the accent when on, with its white
/// thumb at that end.
class ToggleSwitch final : public Pressable
{
  public:
    explicit ToggleSwitch(const Theme& theme, bool on = false) : mTheme(theme), mOn(on)
    {
    }

    void Set(bool on)
    {
        mOn = on;
    }

    [[nodiscard]] bool IsOn() const
    {
        return mOn;
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
        return {{36.0f, 20.0f}, {36.0f, 20.0f}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const float fade = mEnabled ? 1.0f : 0.5f;
        const float radius = b.height() * 0.5f;
        const float on = mMotion.Position(mOn, DrawnArea());
        cnv.fill_style(SwitchMotion::Mix(mTheme.ToggleBg().level(mHover ? 1.2f : 1.0f),
                                         mTheme.Accent().level(mHover ? 1.05f : 0.92f), on)
                           .opacity(fade));
        cnv.fill_round_rect(b, radius);
        cnv.fill_style(el::rgba(255, 255, 255, 240).opacity(fade));
        cnv.add_circle(cycfi::artist::circle(
            el::point{b.left + radius + (b.width() - 2.0f * radius) * on, b.top + radius}, radius - 3.0f));
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

/// Lines of code in a fixed-pitch face (the web UI's pre blocks): a preset's JSON, a report.
/// As tall as its lines; put it in a scroller.
class CodeBlock final : public el::element
{
  public:
    explicit CodeBlock(const Theme& theme) : mTheme(theme)
    {
    }

    void Set(const std::string& text)
    {
        mLines.clear();
        std::size_t start = 0;

        while (start <= text.size())
        {
            const auto end = text.find('\n', start);
            mLines.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));

            if (end == std::string::npos)
            {
                break;
            }

            start = end + 1;
        }
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        const float h = kLine * static_cast<float>(mLines.size()) + 16.0f;
        return {{0.0f, h}, {el::full_extent, h}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        cnv.fill_style(mTheme.Bar());
        cnv.fill_round_rect(b, 8.0f);
        const auto font = MonoFont().size(11.0f);
        const auto visible = ctx.view_bounds(); // only the lines on screen are drawn
        float y = b.top + 8.0f;

        for (const auto& line : mLines)
        {
            if (y + kLine >= visible.top && y <= visible.bottom)
            {
                FillText(cnv, line, font, mTheme.ModalTextSecondary(),
                         el::rect{b.left + 10.0f, y, b.right - 6.0f, y + kLine}, HAlign::Left);
            }

            y += kLine;
        }
    }

  private:
    static constexpr float kLine = 15.0f;
    const Theme& mTheme;
    std::vector<std::string> mLines;
};

/// Two or more choices joined into one control (the tuner's Cents | Hz, a Cards | List view
/// switch): the chosen one filled with the accent. `onSelect` hears the index tapped. With focus
/// the arrows choose the one beside it.
class Segmented final : public el::element, public Drawn, public Focusable
{
  public:
    Segmented(const Theme& theme, std::vector<std::string> options, float segmentWidth = 0.0f)
        : mTheme(theme), mOptions(std::move(options)), mSegmentWidth(segmentWidth)
    {
    }

    std::function<void(int index)> onSelect;

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info;
        info.role = AccessRole::ComboBox;
        info.name = mAccessName;
        info.value = mSelected >= 0 && mSelected < static_cast<int>(mOptions.size())
                         ? mOptions[static_cast<std::size_t>(mSelected)]
                         : std::string();
        return info;
    }

    // Enter or Space steps on round, as a two-way switch is used.
    bool Activate(const el::rect&) override
    {
        return Choose((mSelected + 1) % std::max<int>(1, static_cast<int>(mOptions.size())));
    }

    bool wants_focus() const override
    {
        return FocusWanted(onSelect != nullptr && mOptions.size() > 1);
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
        if (IsPressKey(k))
        {
            Activate(ctx.bounds);
            ctx.view.refresh(ctx);
            return true;
        }

        if (k.action != el::key_action::press && k.action != el::key_action::repeat)
        {
            return false;
        }

        int next = mSelected;

        if (k.key == el::key_code::left || k.key == el::key_code::up)
        {
            next = std::max(0, mSelected - 1);
        }
        else if (k.key == el::key_code::right || k.key == el::key_code::down)
        {
            next = std::min(static_cast<int>(mOptions.size()) - 1, mSelected + 1);
        }
        else
        {
            return false;
        }

        if (next != mSelected)
        {
            Choose(next);
            ctx.view.refresh(ctx);
        }

        return true;
    }

    void Select(int index)
    {
        mSelected = index;
    }

    el::view_limits limits(const el::basic_context& ctx) const override
    {
        float w = 0.0f;

        for (const auto& option : mOptions)
        {
            w += mSegmentWidth > 0.0f
                     ? mSegmentWidth
                     : el::measure_text(ctx.canvas, option, Font(Weight::Medium).size(13.0f)).x + 32.0f;
        }

        return {{w, 34.0f}, {w, 34.0f}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const auto font = Font(Weight::Medium).size(13.0f);
        cnv.fill_style(mTheme.Bar());
        cnv.fill_round_rect(b, 6.0f);
        float x = b.left;
        mEdges.clear();

        for (std::size_t i = 0; i < mOptions.size(); ++i)
        {
            const float w = mSegmentWidth > 0.0f ? mSegmentWidth : el::measure_text(cnv, mOptions[i], font).x + 32.0f;
            const el::rect segment{x, b.top, x + w, b.bottom};
            const bool chosen = static_cast<int>(i) == mSelected;

            if (chosen)
            {
                cnv.fill_style(mTheme.Accent());
                cnv.fill_round_rect(segment, 6.0f);
            }
            else if (static_cast<int>(i) == mHover)
            {
                cnv.fill_style(mTheme.ModalText().opacity(0.06f));
                cnv.fill_round_rect(segment, 6.0f);
            }

            FillText(cnv, mOptions[i], font, chosen ? mTheme.OnAccent() : mTheme.ModalText(), segment, HAlign::Centre);
            mEdges.push_back(x + w);
            x += w;
        }

        cnv.stroke_style(mTheme.ModalRule());
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(b.inset(0.5f, 0.5f), 6.0f);
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (!btn.down || btn.state != el::mouse_button::left)
        {
            return true;
        }

        if (const int i = Hit(ctx.bounds, btn.pos); i >= 0 && onSelect)
        {
            onSelect(i);
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
    bool Choose(int index)
    {
        if (!onSelect || index < 0 || index >= static_cast<int>(mOptions.size()))
        {
            return false;
        }

        const auto select = onSelect; // it may rebuild the region this sits in
        select(index);
        return true;
    }

    [[nodiscard]] int Hit(el::rect bounds, el::point p) const
    {
        for (std::size_t i = 0; i < mEdges.size(); ++i)
        {
            if (p.x >= bounds.left && p.x < mEdges[i])
            {
                return static_cast<int>(i);
            }
        }

        return -1;
    }

    const Theme& mTheme;
    std::vector<std::string> mOptions;
    float mSegmentWidth;
    int mSelected = 0;
    int mHover = -1;
    mutable std::vector<float> mEdges; // each segment's right edge, as last drawn
};

/// A number in a field with ▲ and ▼ beside it (the tuner's reference pitch, the web UI's
/// number inputs with their step buttons): the arrows and the wheel step it, Shift for a finer
/// step, and a double click on the number (or, with focus, Enter or a digit) types one in its
/// place. With focus the arrow keys step it too.
class Stepper final : public el::element, public Drawn, public Focusable
{
  public:
    Stepper(const Theme& theme, float width) : mTheme(theme), mWidth(width)
    {
    }

    /// +1 or -1, and whether Shift was held.
    std::function<void(int direction, bool fine)> onStep;
    /// What the field opens with, and what was typed into it.
    std::function<std::string()> editText;
    std::function<void(const std::string& typed)> onTyped;

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info;
        info.role = mEdit.Active() ? AccessRole::Edit : AccessRole::Slider;
        info.name = mAccessName;
        info.value = mEdit.Active() ? mEdit.Value() : mText;
        return info;
    }

    [[nodiscard]] bool Editing() const override
    {
        return mEdit.Active();
    }

    bool wants_focus() const override
    {
        return FocusWanted(onStep != nullptr) || mEdit.Active();
    }

    void begin_focus(focus_request) override
    {
        NoteFocus(true);
    }

    bool end_focus() override
    {
        // Leaving the field keeps what was typed.
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

        if ((k.action == el::key_action::press || k.action == el::key_action::repeat) && onStep &&
            (k.key == el::key_code::up || k.key == el::key_code::down))
        {
            onStep(k.key == el::key_code::up ? 1 : -1, (k.modifiers & el::mod_shift) != 0);
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

    void Set(std::string text)
    {
        mText = std::move(text);
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        // Touch sizes: a minus before the number and a plus after it, each hit over 40 px.
        if (metrics::Touch())
        {
            const float w = mWidth - kArrows + 2.0f * kTouchArrow;
            return {{w, kTouchArrow}, {w, kTouchArrow}};
        }

        return {{mWidth, 32.0f}, {mWidth, 32.0f}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);

        if (metrics::Touch())
        {
            DrawTouch(ctx);
            return;
        }

        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const auto field = el::rect{b.left, b.top, b.right - kArrows, b.bottom};
        cnv.fill_style(mTheme.Bar());
        cnv.fill_round_rect(b, 6.0f);
        cnv.stroke_style(mTheme.ModalRule());
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(b.inset(0.5f, 0.5f), 6.0f);
        cnv.fill_style(mTheme.ModalRule());
        cnv.fill_rect(el::rect{field.right, b.top, field.right + 1.0f, b.bottom});
        cnv.fill_rect(el::rect{field.right, b.top + b.height() * 0.5f, b.right, b.top + b.height() * 0.5f + 1.0f});

        if (mEdit.Active())
        {
            mEdit.Draw(cnv, field.inset(3.0f, 4.0f), mTheme, Font(Weight::Medium).size(14.0f));
        }
        else
        {
            FillText(cnv, mText, Font(Weight::Medium).size(14.0f), mTheme.ModalText(), field, HAlign::Centre);
        }

        for (int dir : {1, -1})
        {
            const float cy = dir > 0 ? b.top + b.height() * 0.25f : b.top + b.height() * 0.75f;
            const float cx = field.right + kArrows * 0.5f;
            const float tip = dir > 0 ? -2.5f : 2.5f;
            cnv.fill_style(mHoverArrow == dir ? mTheme.ModalText() : mTheme.ModalTextSecondary());
            cnv.begin_path();
            cnv.move_to(cx, cy + tip);
            cnv.line_to(cx + 3.5f, cy - tip);
            cnv.line_to(cx - 3.5f, cy - tip);
            cnv.close_path();
            cnv.fill();
        }
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (!btn.down || btn.state != el::mouse_button::left)
        {
            return true;
        }

        const bool fine = (btn.modifiers & el::mod_shift) != 0;

        if (const int dir = Arrow(ctx.bounds, btn.pos); dir != 0)
        {
            if (onStep)
            {
                onStep(dir, fine);
            }
        }
        else if (btn.num_clicks == 2 && editText && onTyped)
        {
            // The number turns into a field where it is; the stepper takes focus for the keys
            // once this click is done with.
            mEdit.Begin(editText());
            ctx.view.refresh(ctx);
            ctx.view.post([self = weak_from_this(), &view = ctx.view] {
                if (const auto stepper = self.lock())
                {
                    FocusInView(view, *stepper);
                }
            });
        }

        return true;
    }

    bool scroll(const el::context&, el::point dir, el::point) override
    {
        if (dir.y == 0.0f || !ParamKnob::sWheelAdjusts || !onStep)
        {
            return false;
        }

        onStep(dir.y > 0.0f ? 1 : -1, false);
        return true;
    }

    bool cursor(const el::context& ctx, el::point p, el::cursor_tracking status) override
    {
        const int hover = status == el::cursor_tracking::leaving ? 0 : Arrow(ctx.bounds, p);

        if (hover != mHoverArrow)
        {
            mHoverArrow = hover;
            ctx.view.refresh(ctx);
        }

        return false;
    }

    bool wants_control() const override
    {
        return true;
    }

  private:
    static constexpr float kArrows = 20.0f;
    static constexpr float kTouchArrow = 40.0f;

    [[nodiscard]] static int Arrow(el::rect b, el::point p)
    {
        if (metrics::Touch())
        {
            return p.x < b.left + kTouchArrow ? -1 : p.x >= b.right - kTouchArrow ? 1 : 0;
        }

        if (p.x < b.right - kArrows)
        {
            return 0;
        }

        return p.y < b.top + b.height() * 0.5f ? 1 : -1;
    }

    /// The touch layout: [-] number [+], the signs in 40 px zones at either end.
    void DrawTouch(const el::context& ctx)
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const auto field = el::rect{b.left + kTouchArrow, b.top, b.right - kTouchArrow, b.bottom};
        cnv.fill_style(mTheme.Bar());
        cnv.fill_round_rect(b, 6.0f);
        cnv.stroke_style(mTheme.ModalRule());
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(b.inset(0.5f, 0.5f), 6.0f);
        cnv.fill_style(mTheme.ModalRule());
        cnv.fill_rect(el::rect{field.left - 1.0f, b.top, field.left, b.bottom});
        cnv.fill_rect(el::rect{field.right, b.top, field.right + 1.0f, b.bottom});

        if (mEdit.Active())
        {
            mEdit.Draw(cnv, field.inset(3.0f, 4.0f), mTheme, Font(Weight::Medium).size(14.0f));
        }
        else
        {
            FillText(cnv, mText, Font(Weight::Medium).size(14.0f), mTheme.ModalText(), field, HAlign::Centre);
        }

        for (int dir : {-1, 1})
        {
            const float cx = dir > 0 ? b.right - kTouchArrow * 0.5f : b.left + kTouchArrow * 0.5f;
            const float cy = b.top + b.height() * 0.5f;
            cnv.stroke_style(mHoverArrow == dir ? mTheme.ModalText() : mTheme.ModalTextSecondary());
            cnv.line_width(2.0f);
            cnv.begin_path();
            cnv.move_to(cx - 5.0f, cy);
            cnv.line_to(cx + 5.0f, cy);

            if (dir > 0)
            {
                cnv.move_to(cx, cy - 5.0f);
                cnv.line_to(cx, cy + 5.0f);
            }

            cnv.stroke();
        }
    }

    const Theme& mTheme;
    float mWidth;
    std::string mText;
    int mHoverArrow = 0;
    InlineEdit mEdit;
};
} // namespace nanoq::ui
