#pragma once

// The controls Nano Q draws itself: text, buttons, a parameter slider and a level bar.
//
// Soundshed Nano is an owner-drawn UI (nothing it shows is a stock widget), and the same is
// true here: each control is a small Elements element that paints from the theme and reports
// gestures through a callback. Layout - rows, columns, margins, scrolling - is Elements' own.

#include "ui/Theme.h"

#include <elements.hpp>

#include <functional>
#include <memory>
#include <string>

namespace nanoq::ui
{
/// A line of text. Truncated with an ellipsis when the space is short; it never wraps.
class Text : public el::element
{
public:
    enum class Align
    {
        Left,
        Centre,
        Right
    };

    Text(const Theme& theme, std::string text, float size = metrics::kTextSize, Weight weight = Weight::Regular);

    el::view_limits limits(const el::basic_context& ctx) const override;
    void draw(const el::context& ctx) override;

    Text& Set(std::string text);
    Text& Colour(el::color c);
    Text& Alignment(Align a);

    /// With true, the text asks for its full width and the row shrinks nothing around it;
    /// with false (the default) it takes what is left and truncates.
    Text& Fixed(bool fixed);

    [[nodiscard]] const std::string& Value() const
    {
        return mText;
    }

private:
    const Theme& mTheme;
    std::string mText;
    float mSize;
    Weight mWeight;
    el::color mColour;
    Align mAlign = Align::Left;
    bool mFixed = false;
};

/// A push button or, with Toggle(), a switch. Text, or text with a leading dot for a state.
class Button : public el::element
{
public:
    enum class Style
    {
        Plain,    // an outlined control
        Primary,  // the one thing that confirms: filled with the accent
        Flat,     // a label that reacts: a tab, a chip
    };

    Button(const Theme& theme, std::string text, std::function<void()> onClick = {}, Style style = Style::Plain);

    el::view_limits limits(const el::basic_context& ctx) const override;
    void draw(const el::context& ctx) override;
    bool click(const el::context& ctx, el::mouse_button btn) override;
    bool cursor(const el::context& ctx, el::point p, el::cursor_tracking status) override;
    bool wants_control() const override
    {
        return true;
    }

    Button& Set(std::string text);
    Button& On(bool on);
    Button& Selected(bool selected);
    Button& Enabled(bool enabled);
    Button& MinWidth(float w);
    Button& Dot(bool show);
    Button& TextSize(float size);

    [[nodiscard]] bool IsOn() const
    {
        return mOn;
    }

private:
    const Theme& mTheme;
    std::string mText;
    std::function<void()> mOnClick;
    Style mStyle;
    bool mOn = false;
    bool mSelected = false;
    bool mEnabled = true;
    bool mDot = false;
    bool mHover = false;
    bool mDown = false;
    float mMinWidth = 0.0f;
    float mSize = metrics::kTextSize;
};

/// One effect parameter: its name, its value as text, and a track to drag. The value is
/// normalised 0..1 (the control does not know the parameter's range); the owner formats it.
class ParamSlider : public el::element
{
public:
    ParamSlider(const Theme& theme, std::string name);

    el::view_limits limits(const el::basic_context& ctx) const override;
    void draw(const el::context& ctx) override;
    bool click(const el::context& ctx, el::mouse_button btn) override;
    void drag(const el::context& ctx, el::mouse_button btn) override;
    bool cursor(const el::context& ctx, el::point p, el::cursor_tracking status) override;
    bool scroll(const el::context& ctx, el::point dir, el::point p) override;
    bool wants_control() const override
    {
        return true;
    }

    /// Moves the thumb to a value the engine reports. Ignored while the user is dragging it.
    void Update(double normalised, std::string valueText);
    [[nodiscard]] bool Dragging() const
    {
        return mDragging;
    }

    /// Called with each new normalised value while the user drags.
    std::function<void(double normalised)> onChange;
    /// Called on a double click: the owner resets to the default.
    std::function<void()> onReset;

private:
    void Move(const el::context& ctx, el::point p);

    const Theme& mTheme;
    std::string mName;
    std::string mValueText;
    double mValue = 0.0;
    bool mDragging = false;
    bool mHover = false;
};

/// One effect parameter as a rotary knob, as the web app draws it: the name over the knob, the
/// value text under it. Drag up or down to turn (200 px for the full travel), wheel to nudge,
/// double click to reset. The value is normalised 0..1, as for ParamSlider.
class ParamKnob : public el::element
{
public:
    ParamKnob(const Theme& theme, std::string name);

    el::view_limits limits(const el::basic_context& ctx) const override;
    void draw(const el::context& ctx) override;
    bool click(const el::context& ctx, el::mouse_button btn) override;
    void drag(const el::context& ctx, el::mouse_button btn) override;
    bool cursor(const el::context& ctx, el::point p, el::cursor_tracking status) override;
    bool scroll(const el::context& ctx, el::point dir, el::point p) override;
    bool wants_control() const override
    {
        return true;
    }

    /// Moves the knob to a value the engine reports. Ignored while the user is dragging it.
    void Update(double normalised, std::string valueText);
    [[nodiscard]] bool Dragging() const
    {
        return mDragging;
    }

    /// Sets the value text at once, e.g. from onChange, so it follows the knob during a drag.
    void SetValueText(std::string valueText)
    {
        mValueText = std::move(valueText);
    }

    /// Where the knob's marker sits when at rest (0..1), drawn as a tick: the default value.
    void DefaultPosition(double normalised)
    {
        mDefault = normalised;
    }

    std::function<void(double normalised)> onChange;
    std::function<void()> onReset;

private:
    const Theme& mTheme;
    std::string mName;
    std::string mValueText;
    double mValue = 0.0;
    double mDefault = 0.0;
    double mDragStartValue = 0.0;
    float mDragStartY = 0.0f;
    bool mDragging = false;
    bool mHover = false;
};

/// A small muted capital heading that starts a group of controls, with a rule after it.
class GroupHeading : public el::element
{
public:
    GroupHeading(const Theme& theme, std::string text);

    el::view_limits limits(const el::basic_context& ctx) const override;
    void draw(const el::context& ctx) override;

private:
    const Theme& mTheme;
    std::string mText;
};

/// A horizontal level bar: -60 dB to 0 dB, with a clip tint.
class MeterBar : public el::element
{
public:
    explicit MeterBar(const Theme& theme);

    el::view_limits limits(const el::basic_context& ctx) const override;
    void draw(const el::context& ctx) override;

    /// Returns true if the bar changed enough to be worth a redraw.
    bool Set(double peakDb, bool clipped);

private:
    const Theme& mTheme;
    double mPeakDb = -120.0;
    bool mClipped = false;
};

/// One row of a list: text at the left, a muted secondary text at the right. With `heading` it
/// is a caption and does nothing when tapped.
class ListRow : public el::element
{
public:
    ListRow(const Theme& theme, std::string text, std::string secondary, bool selected, bool heading,
            std::function<void()> onClick);

    el::view_limits limits(const el::basic_context& ctx) const override;
    void draw(const el::context& ctx) override;
    bool click(const el::context& ctx, el::mouse_button btn) override;
    bool cursor(const el::context& ctx, el::point p, el::cursor_tracking status) override;
    bool wants_control() const override
    {
        return !mHeading;
    }

    /// A right click (a long press on touch screens, later) asks for the row's own menu.
    std::function<void()> onMenu;

private:
    const Theme& mTheme;
    std::string mText, mSecondary;
    bool mSelected, mHeading;
    std::function<void()> mOnClick;
    bool mHover = false, mDown = false;
};

/// A rounded, filled area that takes clicks, so nothing behind it hears them: the face of a pop-up.
class Surface : public el::element
{
public:
    Surface(el::color fill, el::color line, float radius) : mFill(fill), mLine(line), mRadius(radius) {}

    void draw(const el::context& ctx) override;
    bool click(const el::context&, el::mouse_button) override
    {
        return true;
    }
    bool wants_control() const override
    {
        return true;
    }

private:
    el::color mFill, mLine;
    float mRadius;
};

/// A full-view dimmed backdrop that calls `onClick` when tapped.
class Backdrop : public el::element
{
public:
    explicit Backdrop(std::function<void()> onClick) : mOnClick(std::move(onClick)) {}

    void draw(const el::context& ctx) override;
    bool click(const el::context& ctx, el::mouse_button btn) override;
    bool wants_control() const override
    {
        return true;
    }

private:
    std::function<void()> mOnClick;
    bool mDown = false;
};

/// A transient message: a title and, under it, a line of detail. It takes no clicks, so what is
/// under it stays usable while it shows.
class Toast : public el::element
{
public:
    Toast(const Theme& theme, std::string title, std::string detail, bool isError);

    el::view_limits limits(const el::basic_context& ctx) const override;
    void draw(const el::context& ctx) override;

private:
    const Theme& mTheme;
    std::string mTitle, mDetail;
    bool mError;
};

/// A rounded panel behind another element.
[[nodiscard]] el::element_ptr Panel(el::color fill, float radius, el::element_ptr content);

/// A fixed-size spacer.
[[nodiscard]] el::element_ptr Gap(float width, float height);
} // namespace nanoq::ui
