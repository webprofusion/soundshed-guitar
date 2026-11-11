// The controls that show a parameter: ParamSlider and ParamKnob (declared in Widgets.h), and the
// keys that turn them.

#include "ui/widgets/Widgets.h"

#include "ui/widgets/Graphics.h"
#include "ui/widgets/Layout.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <optional>

namespace nanoq::ui
{
namespace
{
using cycfi::artist::canvas;
} // namespace

// ── Turning a knob or slider from the keyboard ─────────────────────────────────

namespace
{
/// Where an arrow, Page Up or Down, Home or End takes a control at `value` (0..1): a hundredth a
/// step (a thousandth with Shift), or one position of `steps`, and a tenth a page.
std::optional<double> KeyedPosition(const el::key_info& k, double value, int steps)
{
    using el::key_code;

    if ((k.action != el::key_action::press && k.action != el::key_action::repeat) ||
        (k.modifiers & (el::mod_control | el::mod_alt | el::mod_super)) != 0)
    {
        return std::nullopt;
    }

    const double step =
        steps > 1 ? 1.0 / static_cast<double>(steps - 1) : ((k.modifiers & el::mod_shift) ? 0.001 : 0.01);
    const double page = std::max(step, 0.1);
    double next = value;

    switch (k.key)
    {
    case key_code::up:
    case key_code::right:
        next = value + step;
        break;
    case key_code::down:
    case key_code::left:
        next = value - step;
        break;
    case key_code::page_up:
        next = value + page;
        break;
    case key_code::page_down:
        next = value - page;
        break;
    case key_code::home:
        next = 0.0;
        break;
    case key_code::end:
        next = 1.0;
        break;
    default:
        return std::nullopt;
    }

    next = std::clamp(next, 0.0, 1.0);

    if (steps > 1)
    {
        next = std::round(next * static_cast<double>(steps - 1)) / static_cast<double>(steps - 1);
    }

    return next;
}

bool IsResetKey(const el::key_info& k)
{
    return k.action == el::key_action::press && (k.key == el::key_code::_delete || k.key == el::key_code::backspace);
}
} // namespace

// ── ParamSlider ─────────────────────────────────────────────────────────────────

ParamSlider::ParamSlider(const Theme& theme, std::string name) : mTheme(theme), mName(std::move(name))
{
}

el::view_limits ParamSlider::limits(const el::basic_context&) const
{
    constexpr float h = 52.0f;
    return {{120.0f, h}, {el::full_extent, h}};
}

void ParamSlider::draw(const el::context& ctx)
{
    NoteDrawn(ctx);
    auto& cnv = ctx.canvas;
    const auto b = ctx.bounds;

    const auto nameFont = Font(Weight::Medium).size(metrics::kSmallText);
    const float valueWidth = el::measure_text(cnv, mValueText, nameFont).x;

    auto top = el::rect{b.left, b.top, b.right, b.top + 20.0f};
    auto nameBounds = top;
    nameBounds.right = std::max(nameBounds.left, top.right - valueWidth - 8.0f);
    FillText(cnv, Ellipsize(cnv, mName, nameFont, nameBounds.width()), nameFont, mTheme.TextSecondary(), nameBounds,
             Text::Align::Left);
    FillText(cnv, mValueText, nameFont, mTheme.Text(), top, Text::Align::Right);

    // The track, with the travel filled in the accent: the one place an effect page uses it.
    const float trackY = b.bottom - 14.0f;
    const float trackH = 6.0f;
    const auto track = el::rect{b.left + 6.0f, trackY - trackH * 0.5f, b.right - 6.0f, trackY + trackH * 0.5f};
    cnv.fill_style(mTheme.Input());
    cnv.fill_round_rect(track, trackH * 0.5f);

    const float v = static_cast<float>(std::clamp(mValue, 0.0, 1.0));
    const float x = track.left + v * track.width();
    auto travel = track;
    travel.right = std::max(track.left + trackH, x);
    cnv.fill_style(mTheme.AccentFill());
    cnv.fill_round_rect(travel, trackH * 0.5f);

    const float r = (mDragging || mHover) ? 9.0f : 8.0f;
    cnv.fill_style(mTheme.Text());
    cnv.add_circle(cycfi::artist::circle(el::point{x, trackY}, r));
    cnv.fill();
    cnv.stroke_style(mTheme.BorderStrong());
    cnv.line_width(1.0f);
    cnv.add_circle(cycfi::artist::circle(el::point{x, trackY}, r));
    cnv.stroke();
}

void ParamSlider::Move(const el::context& ctx, el::point p)
{
    const float left = ctx.bounds.left + 6.0f;
    const float width = std::max(1.0f, ctx.bounds.width() - 12.0f);
    mValue = std::clamp(static_cast<double>((p.x - left) / width), 0.0, 1.0);

    if (onChange)
    {
        onChange(mValue);
    }

    ctx.view.refresh(ctx);
}

bool ParamSlider::click(const el::context& ctx, el::mouse_button btn)
{
    if (btn.state != el::mouse_button::left)
    {
        return false;
    }

    if (btn.down)
    {
        if (btn.num_clicks == 2 && onReset)
        {
            onReset();
            return true;
        }

        mDragging = true;
        Move(ctx, btn.pos);
    }
    else
    {
        const bool wasDragging = mDragging;
        mDragging = false;
        ctx.view.refresh(ctx);

        if (wasDragging && onRelease)
        {
            onRelease();
        }
    }

    return true;
}

void ParamSlider::drag(const el::context& ctx, el::mouse_button btn)
{
    if (mDragging)
    {
        Move(ctx, btn.pos);
    }
}

bool ParamSlider::cursor(const el::context& ctx, el::point, el::cursor_tracking status)
{
    const bool hover = status != el::cursor_tracking::leaving;

    if (hover != mHover)
    {
        mHover = hover;
        ctx.view.refresh(ctx);
    }

    return false;
}

bool ParamSlider::scroll(const el::context& ctx, el::point dir, el::point)
{
    // A wheel nudges by 2%; the page's own scroller gets it when the slider is not hovered.
    if (dir.y == 0.0f || !mHover || !ParamKnob::sWheelAdjusts)
    {
        return false;
    }

    mValue = std::clamp(mValue + (dir.y > 0.0f ? 0.02 : -0.02), 0.0, 1.0);

    if (onChange)
    {
        onChange(mValue);
    }

    ctx.view.refresh(ctx);
    return true;
}

bool ParamSlider::key(const el::context& ctx, el::key_info k)
{
    if (IsResetKey(k) && onReset)
    {
        onReset();
        return true;
    }

    const auto next = KeyedPosition(k, mValue, 0);

    if (!next)
    {
        return false;
    }

    SetPosition(*next);
    ctx.view.refresh(ctx);
    return true;
}

AccessInfo ParamSlider::Describe() const
{
    AccessInfo info;
    info.role = AccessRole::Slider;
    info.name = NameOr(mName);
    info.value = mValueText;
    info.position = mValue;
    return info;
}

bool ParamSlider::SetPosition(double normalised)
{
    // A step from the keyboard is a whole gesture: previewed and committed at once.
    mValue = std::clamp(normalised, 0.0, 1.0);

    if (onChange)
    {
        onChange(mValue);
    }

    if (onRelease)
    {
        onRelease();
    }

    RepaintSoon(DrawnArea());
    return true;
}

void ParamSlider::Update(double normalised, std::string valueText)
{
    if (mDragging)
    {
        return;
    }

    mValue = normalised;
    mValueText = std::move(valueText);
}

// ── ParamKnob ───────────────────────────────────────────────────────────────────

namespace
{
// The web UI's node-param-group: a 9 px capital label, a 34 px knob, a 10 px value.
constexpr float kKnobCellWidth = 60.0f;
constexpr float kKnobNameHeight = 12.0f;
constexpr float kKnobValueHeight = 13.0f;
constexpr float kKnobDiameter = 34.0f;
constexpr float kLargeCellWidth = 74.0f;
constexpr float kLargeNameHeight = 13.0f;
constexpr float kLargeValueHeight = 15.0f;
constexpr float kLargeDiameter = 40.0f;
constexpr float kKnobDragPixels = 200.0f;
// A 270 degree sweep, open at the bottom: 7 o'clock round to 5 o'clock.
constexpr float kKnobStart = 3.14159265f * 0.75f;
constexpr float kKnobSweep = 3.14159265f * 1.5f;
} // namespace

ParamKnob::ParamKnob(const Theme& theme, std::string name) : mTheme(theme), mName(std::move(name))
{
}

el::view_limits ParamKnob::limits(const el::basic_context&) const
{
    if (mBare > 0.0f)
    {
        return {{mBare, mBare}, {mBare, mBare}};
    }

    const float w = mCellWidth > 0.0f ? mCellWidth : (mLarge ? kLargeCellWidth : kKnobCellWidth);
    const float h = mLarge ? kLargeNameHeight + kLargeDiameter + kLargeValueHeight + 4.0f
                           : kKnobNameHeight + kKnobDiameter + kKnobValueHeight + 4.0f;
    return {{w, h}, {w, h}};
}

void ParamKnob::draw(const el::context& ctx)
{
    NoteDrawn(ctx);
    auto& cnv = ctx.canvas;
    const auto b = ctx.bounds;
    const bool light = mTheme.IsLight();
    // The web UI's .node-param-label and .node-param-value.
    const auto labelFont = Font(Weight::SemiBold).size(mLarge ? 10.0f : 9.0f);
    const auto valueFont = Font(Weight::SemiBold).size(mLarge ? 12.0f : 10.0f);
    const float nameHeight = mLarge ? kLargeNameHeight : kKnobNameHeight;
    const float valueHeight = mLarge ? kLargeValueHeight : kKnobValueHeight;
    const el::color labelInk = light ? mTheme.TextSecondary() : el::rgba(201, 209, 225, 184);
    const el::color valueInk = light ? mTheme.Text() : el::rgba(231, 236, 246, 194);

    const auto nameBounds = el::rect{b.left, b.top, b.right, b.top + nameHeight};

    if (mBare <= 0.0f)
    {
        FillText(cnv, Ellipsize(cnv, Upper(mName), labelFont, nameBounds.width() + 6.0f), labelFont, labelInk,
                 nameBounds, Text::Align::Centre);
    }

    const float diameter = mBare > 0.0f ? mBare : (mLarge ? kLargeDiameter : kKnobDiameter);
    const float cx = (b.left + b.right) * 0.5f;
    const float cy = mBare > 0.0f ? (b.top + b.bottom) * 0.5f : nameBounds.bottom + 2.0f + diameter * 0.5f;
    const float r = diameter * 0.5f;
    const float v = static_cast<float>(std::clamp(mValue, 0.0, 1.0));

    // The ring: a faint track all the way round, and the travel over it, shading from the ring's
    // start colour into its end colour as the web UI's conic gradient does (from the left end, or
    // from the centre of a bipolar control: the default sitting at the middle).
    constexpr float kArcWidth = 3.0f;
    const float arcR = r - kArcWidth * 0.5f;
    {
        auto state = cnv.new_state();
        cnv.line_width(kArcWidth);
        cnv.stroke_style(mTheme.KnobRingTrack());
        cnv.begin_path();
        cnv.arc(cx, cy, arcR, 0.0f, 2.0f * 3.14159265f);
        cnv.stroke();

        const float from = (mDefault > 0.4 && mDefault < 0.6) ? static_cast<float>(mDefault) : 0.0f;
        const float lo = std::min(from, v);
        const float hi = std::max(from, v);

        if (hi - lo > 0.002f)
        {
            const auto start = mTheme.KnobRingStart(), end = mTheme.KnobRingEnd();
            constexpr int kSteps = 24;

            for (int i = 0; i < kSteps; ++i)
            {
                const float t0 = lo + (hi - lo) * static_cast<float>(i) / kSteps;
                const float t1 = lo + (hi - lo) * static_cast<float>(i + 1) / kSteps;
                // The shade follows the position on the dial, not the slice of travel drawn.
                const float mix = std::clamp(t1 / std::max(0.001f, hi), 0.0f, 1.0f);
                cnv.stroke_style(el::color{start.red + (end.red - start.red) * mix,
                                           start.green + (end.green - start.green) * mix,
                                           start.blue + (end.blue - start.blue) * mix, 1.0f});
                cnv.begin_path();
                cnv.arc(cx, cy, arcR, kKnobStart + kKnobSweep * t0, kKnobStart + kKnobSweep * t1 + 0.01f);
                cnv.stroke();
            }
        }
    }

    // The cap: a dark dome inside the ring, with a white pointer.
    const float capR = r - kArcWidth - 1.5f;
    {
        auto state = cnv.new_state();
        cycfi::artist::canvas::radial_gradient dome{el::point{cx, cy - capR * 0.4f}, 0.0f, el::point{cx, cy}, capR};
        const auto top = light ? el::rgba(250, 250, 252, 255) : el::rgba(58, 58, 68, 255);
        const auto bottom = light ? el::rgba(214, 216, 224, 255) : el::rgba(22, 22, 28, 255);
        dome.add_color_stop(0.0f, (mDragging || mHover) ? top.level(1.15f) : top);
        dome.add_color_stop(1.0f, bottom);
        cnv.fill_style(dome);
        cnv.add_circle(cycfi::artist::circle(el::point{cx, cy}, capR));
        cnv.fill();
    }
    cnv.stroke_style(light ? el::rgba(0, 0, 0, 40) : el::rgba(0, 0, 0, 140));
    cnv.line_width(1.0f);
    cnv.add_circle(cycfi::artist::circle(el::point{cx, cy}, capR));
    cnv.stroke();

    const float angle = kKnobStart + kKnobSweep * v;
    const float c = std::cos(angle), s = std::sin(angle);
    {
        auto state = cnv.new_state();
        cnv.stroke_style(light ? mTheme.Text() : el::rgba(240, 240, 248, 255));
        cnv.line_width(2.0f);
        cnv.line_cap(canvas::round);
        cnv.begin_path();
        cnv.move_to(cx + c * capR * 0.40f, cy + s * capR * 0.40f);
        cnv.line_to(cx + c * capR * 0.85f, cy + s * capR * 0.85f);
        cnv.stroke();
    }

    if (mBare > 0.0f)
    {
        return;
    }

    if (mEdit.Active())
    {
        mEdit.Draw(cnv, ValueBox(b), mTheme, valueFont);
        return;
    }

    const auto valueBounds = el::rect{b.left, b.bottom - valueHeight, b.right, b.bottom};
    FillText(cnv, Ellipsize(cnv, mValueText, valueFont, valueBounds.width() + 6.0f), valueFont, valueInk, valueBounds,
             Text::Align::Centre);
}

el::rect ParamKnob::ValueBox(const el::rect& b) const
{
    const float valueHeight = mLarge ? kLargeValueHeight : kKnobValueHeight;
    return el::rect{b.left + 1.0f, b.bottom - valueHeight - 2.0f, b.right - 1.0f, b.bottom + 1.0f};
}

void ParamKnob::BeginEdit(std::string text)
{
    mDragging = false;
    mEdit.Begin(std::move(text));
}

void ParamKnob::FinishEdit(bool commit)
{
    const auto typed = mEdit.Value();
    mEdit.End();
    RepaintSoon(DrawnArea());

    if (commit && onTyped)
    {
        const auto typedTo = onTyped; // it may rebuild the region this sits in
        typedTo(typed);
    }
}

bool ParamKnob::end_focus()
{
    // Going elsewhere keeps what was typed, as leaving the web UI's field does.
    if (mEdit.Active())
    {
        FinishEdit(true);
    }

    NoteFocus(false);
    return true;
}

void ParamKnob::Turn(const el::context& ctx, double normalised)
{
    mValue = normalised;

    if (onChange)
    {
        onChange(mValue);
    }

    ctx.view.refresh(ctx);
}

bool ParamKnob::key(const el::context& ctx, el::key_info k)
{
    using el::key_code;

    if (mEdit.Active())
    {
        switch (mEdit.Key(k))
        {
        case InlineEdit::Outcome::Commit:
            FinishEdit(true);
            ctx.view.refresh(ctx);
            return true;
        case InlineEdit::Outcome::Cancel:
            FinishEdit(false);
            ctx.view.refresh(ctx);
            return true;
        case InlineEdit::Outcome::Used:
            ctx.view.refresh(ctx);
            return true;
        case InlineEdit::Outcome::Unused:
            return false;
        }
    }

    if (IsMenuKey(k) && onMenu)
    {
        onMenu(cycfi::artist::center_point(ctx.bounds));
        return true;
    }

    const bool plain = (k.modifiers & (el::mod_control | el::mod_alt | el::mod_super)) == 0;
    const bool typeable = editText && onTyped && mBare <= 0.0f;

    if (k.action == el::key_action::press && plain && typeable)
    {
        if (k.key == key_code::enter || k.key == key_code::kp_enter || k.key == key_code::f2)
        {
            BeginEdit(editText());
            ctx.view.refresh(ctx);
            return true;
        }

        // A number typed straight away opens the field for it; the character comes as text().
        const bool digit = (k.key >= key_code::_0 && k.key <= key_code::_9 && (k.modifiers & el::mod_shift) == 0) ||
                           (k.key >= key_code::kp_0 && k.key <= key_code::kp_9);

        if (digit || k.key == key_code::minus || k.key == key_code::period || k.key == key_code::kp_subtract ||
            k.key == key_code::kp_decimal)
        {
            BeginEdit({});
            ctx.view.refresh(ctx);
            return true;
        }
    }

    if (IsResetKey(k) && onReset)
    {
        onReset();
        ctx.view.refresh(ctx);
        return true;
    }

    if (const auto next = KeyedPosition(k, mValue, mKeySteps))
    {
        Turn(ctx, *next);
        return true;
    }

    return false;
}

bool ParamKnob::text(const el::context& ctx, el::text_info info)
{
    if (!mEdit.Char(info))
    {
        return false;
    }

    ctx.view.refresh(ctx);
    return true;
}

AccessInfo ParamKnob::Describe() const
{
    AccessInfo info;
    info.role = mEdit.Active() ? AccessRole::Edit : AccessRole::Slider;
    info.name = NameOr(mName);
    info.value = mEdit.Active() ? mEdit.Value() : mValueText;
    info.position = mValue;
    return info;
}

bool ParamKnob::SetPosition(double normalised)
{
    mValue = std::clamp(normalised, 0.0, 1.0);

    if (onChange)
    {
        onChange(mValue);
    }

    RepaintSoon(DrawnArea());
    return true;
}

bool ParamKnob::click(const el::context& ctx, el::mouse_button btn)
{
    if (btn.state == el::mouse_button::right && onMenu)
    {
        if (btn.down)
        {
            onMenu(btn.pos);
        }

        return true;
    }

    if (btn.state != el::mouse_button::left)
    {
        return false;
    }

    if (btn.down)
    {
        if (btn.num_clicks == 2)
        {
            mDragging = false;

            if (editText && onTyped && mBare <= 0.0f &&
                btn.pos.y >= ctx.bounds.bottom - (mLarge ? kLargeValueHeight : kKnobValueHeight) - 2.0f)
            {
                // The value turns into a field where it is; the knob takes focus for the keys
                // once this click is done with.
                BeginEdit(editText());
                ctx.view.refresh(ctx);
                ctx.view.post([self = weak_from_this(), &view = ctx.view] {
                    if (const auto knob = self.lock())
                    {
                        FocusInView(view, *knob);
                    }
                });
            }
            else if (onReset)
            {
                onReset();
            }

            return true;
        }

        mDragging = true;
        mDragStartY = btn.pos.y;
        mDragStartValue = mValue;
    }
    else
    {
        mDragging = false;
        ctx.view.refresh(ctx);
    }

    return true;
}

void ParamKnob::drag(const el::context& ctx, el::mouse_button btn)
{
    if (!mDragging)
    {
        return;
    }

    // Up turns it up. Holding Shift slows the turn to a fifth.
    const float scale = btn.modifiers & el::mod_shift ? 5.0f : 1.0f;
    const double delta = static_cast<double>(mDragStartY - btn.pos.y) / (kKnobDragPixels * scale);
    mValue = std::clamp(mDragStartValue + delta, 0.0, 1.0);

    if (onChange)
    {
        onChange(mValue);
    }

    ctx.view.refresh(ctx);
}

bool ParamKnob::cursor(const el::context& ctx, el::point, el::cursor_tracking status)
{
    const bool hover = status != el::cursor_tracking::leaving;

    if (hover != mHover)
    {
        mHover = hover;
        ctx.view.refresh(ctx);
    }

    return false;
}

bool ParamKnob::scroll(const el::context& ctx, el::point dir, el::point)
{
    if (dir.y == 0.0f || !mHover || !sWheelAdjusts)
    {
        return false;
    }

    mValue = std::clamp(mValue + (dir.y > 0.0f ? 0.02 : -0.02), 0.0, 1.0);

    if (onChange)
    {
        onChange(mValue);
    }

    ctx.view.refresh(ctx);
    return true;
}

void ParamKnob::Update(double normalised, std::string valueText)
{
    if (mDragging)
    {
        return;
    }

    mValue = normalised;
    mValueText = std::move(valueText);
}
} // namespace nanoq::ui
