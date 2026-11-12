#pragma once

// The controls Nano Q draws itself: text, buttons, a parameter slider and a level bar.
//
// Soundshed Nano is an owner-drawn UI (nothing it shows is a stock widget), and the same is
// true here: each control is a small Elements element that paints from the theme and reports
// gestures through a callback. Layout - rows, columns, margins, scrolling - is Elements' own.

#include "ui/widgets/DragScroll.h"
#include "ui/Drawn.h"
#include "ui/input/Focus.h"
#include "ui/widgets/InlineEdit.h"
#include "ui/widgets/Theme.h"
#include "ui/widgets/TextDraw.h"

#include <elements.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nanoq::ui
{
class Icons;

/// A line of text. Truncated with an ellipsis when the space is short; it never wraps.
class Text : public el::element, public Drawn
{
  public:
    using Align = HAlign;

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

/// Text wrapped at word breaks to a fixed width, as tall as its lines.
class Paragraph : public el::element
{
  public:
    Paragraph(const Theme& theme, std::string text, float width, float size = metrics::kTextSize);

    el::view_limits limits(const el::basic_context& ctx) const override;
    void draw(const el::context& ctx) override;

    Paragraph& Colour(el::color c);

  private:
    [[nodiscard]] std::vector<std::string> Lines(cycfi::artist::canvas& cnv) const;

    const Theme& mTheme;
    std::string mText;
    float mWidth;
    float mSize;
    el::color mColour;
};

/// A push button or, with Toggle(), a switch. Text, or text with a leading dot for a state.
class Button : public el::element, public Drawn, public Focusable, public TapTarget
{
  public:
    enum class Style
    {
        Plain,   // an outlined control
        Primary, // the one thing that confirms: filled with the accent
        Flat,    // a label that reacts: a tab, a chip
    };

    Button(const Theme& theme, std::string text, std::function<void()> onClick = {}, Style style = Style::Plain);

    el::view_limits limits(const el::basic_context& ctx) const override;
    void draw(const el::context& ctx) override;
    bool click(const el::context& ctx, el::mouse_button btn) override;
    bool cursor(const el::context& ctx, el::point p, el::cursor_tracking status) override;
    bool key(const el::context& ctx, el::key_info k) override;

    bool wants_control() const override
    {
        return true;
    }

    bool wants_focus() const override
    {
        return FocusWanted(mEnabled && mOnClick != nullptr);
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

    [[nodiscard]] AccessInfo Describe() const override;
    bool Activate(const el::rect& at) override;

    Button& Set(std::string text);

    /// What a click does, for a button made before its action could be (one that reads the
    /// dialog it sits in).
    Button& OnClick(std::function<void()> onClick)
    {
        mOnClick = std::move(onClick);
        return *this;
    }

    Button& On(bool on);
    Button& Selected(bool selected);
    Button& Enabled(bool enabled);
    Button& MinWidth(float w);
    Button& Dot(bool show);
    Button& TextSize(float size);
    /// Takes the room it is given, from MinWidth up, and cuts its text to fit, where by default
    /// a button is as wide as its text.
    Button& Flexible(bool flexible);
    Button& Alignment(HAlign align);

    /// With touch sizes: drawn `h` tall in the middle of a taller hit area (a strip's buttons).
    Button& FaceHeight(float h)
    {
        mFaceHeight = h;
        return *this;
    }

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
    bool mFlexible = false;
    HAlign mAlign = HAlign::Centre;
    float mFaceHeight = 0.0f;
};

/// One effect parameter: its name, its value as text, and a track to drag. The value is
/// normalised 0..1 (the control does not know the parameter's range); the owner formats it.
/// With focus the arrows move it (Shift finely), Page Up and Down by a tenth, Home and End to
/// its ends, Delete to its default.
class ParamSlider : public el::element, public Drawn, public Focusable
{
  public:
    ParamSlider(const Theme& theme, std::string name);

    el::view_limits limits(const el::basic_context& ctx) const override;
    void draw(const el::context& ctx) override;
    bool click(const el::context& ctx, el::mouse_button btn) override;
    void drag(const el::context& ctx, el::mouse_button btn) override;
    bool cursor(const el::context& ctx, el::point p, el::cursor_tracking status) override;
    bool scroll(const el::context& ctx, el::point dir, el::point p) override;
    bool key(const el::context& ctx, el::key_info k) override;

    bool wants_control() const override
    {
        return true;
    }

    bool wants_focus() const override
    {
        return FocusWanted(true);
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

    [[nodiscard]] AccessInfo Describe() const override;
    bool SetPosition(double normalised) override;

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
    /// Called when a drag ends: the owner commits what the drag only previewed.
    std::function<void()> onRelease;

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
///
/// With focus the arrows turn it (Shift finely), Page Up and Down by a tenth, Home and End to its
/// ends, Delete to its default. A value is typed in place, where its value text is: a double click
/// on that text, Enter, F2, or just typing a number opens the field (InlineEdit).
class ParamKnob : public el::element, public Drawn, public Focusable
{
  public:
    ParamKnob(const Theme& theme, std::string name);

    el::view_limits limits(const el::basic_context& ctx) const override;
    void draw(const el::context& ctx) override;
    bool click(const el::context& ctx, el::mouse_button btn) override;
    void drag(const el::context& ctx, el::mouse_button btn) override;
    bool cursor(const el::context& ctx, el::point p, el::cursor_tracking status) override;
    bool scroll(const el::context& ctx, el::point dir, el::point p) override;
    bool key(const el::context& ctx, el::key_info k) override;
    bool text(const el::context& ctx, el::text_info info) override;

    bool wants_control() const override
    {
        return true;
    }

    // Focus while a value is typed into it, even from a pointer's double click.
    bool wants_focus() const override
    {
        return FocusWanted(true) || mEdit.Active();
    }

    void begin_focus(focus_request) override
    {
        NoteFocus(true);
    }

    bool end_focus() override;

    [[nodiscard]] AccessInfo Describe() const override;
    bool SetPosition(double normalised) override;

    [[nodiscard]] bool Editing() const override
    {
        return mEdit.Active();
    }

    /// How many positions it has (an enum's choices), so one arrow is one step; 0 for a smooth
    /// control, where one is a hundredth.
    ParamKnob& KeySteps(int steps)
    {
        mKeySteps = steps;
        return *this;
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

    /// Just the knob, this wide, with no name over it or value under it: a settings flyout's
    /// row, which has its label and value beside it.
    ParamKnob& Bare(float diameter)
    {
        mBare = diameter;
        return *this;
    }

    /// The effect editor's size (.node-param-knob): a 40 px knob in a 74 px cell, its name and
    /// value a size up. The default is the 34 px knob of the control bar and the flyouts.
    ParamKnob& Large(bool large)
    {
        mLarge = large;
        return *this;
    }

    /// A narrower cell than the knob's own (60 px, 74 large), for a row short of room. The name
    /// and value are drawn to the width given.
    ParamKnob& CellWidth(float width)
    {
        mCellWidth = width;
        return *this;
    }

    std::function<void(double normalised)> onChange;
    std::function<void()> onReset;
    /// A right click (a long press on a touch screen), at a point in view coordinates: the
    /// knob's context menu, MIDI Learn.
    std::function<void(el::point at)> onMenu;
    /// A value can be typed in: `editText` is what the field opens with (the value without its
    /// unit), and `onTyped` hears what was entered. Without them a double click on the value
    /// text resets too.
    std::function<std::string()> editText;
    std::function<void(const std::string& typed)> onTyped;

    /// Whether a wheel over a knob or slider turns it: the shared ui.mouseWheelAdjustsControls,
    /// on unless turned off, kept current by the shell.
    static inline bool sWheelAdjusts = true;

  private:
    /// Turns it to `normalised` as a drag would, and redraws it.
    void Turn(const el::context& ctx, double normalised);
    /// A finger held still on it (LongPress.h): the press was for its menu, not a turn, so what
    /// the finger's wander turned goes back, and the menu opens at `at`.
    void HeldForMenu(el::point at);
    void BeginEdit(std::string text);
    void FinishEdit(bool commit);
    [[nodiscard]] el::rect ValueBox(const el::rect& b) const;

    const Theme& mTheme;
    std::string mName;
    std::string mValueText;
    double mValue = 0.0;
    double mDefault = 0.0;
    float mBare = 0.0f; // the diameter of a bare knob; 0 for the named one
    bool mLarge = false;
    float mCellWidth = 0.0f; // 0: the knob's own cell width
    int mKeySteps = 0;
    InlineEdit mEdit;
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

/// A level bar: -60 dB to 0 dB, with a clip tint. Horizontal, or with Vertical() the web UI's
/// Controls strip meter: a column of segments under the peak in dB.
class MeterBar : public el::element, public Drawn
{
  public:
    explicit MeterBar(const Theme& theme);

    MeterBar& Vertical(bool vertical)
    {
        mVertical = vertical;
        return *this;
    }

    el::view_limits limits(const el::basic_context& ctx) const override;
    void draw(const el::context& ctx) override;

    /// Returns true if the bar changed enough to be worth a redraw.
    bool Set(double peakDb, bool clipped);

  private:
    const Theme& mTheme;
    double mPeakDb = -120.0;
    bool mClipped = false;
    bool mVertical = false;
};

/// One row of a list: text at the left, a muted secondary text at the right. With `heading` it
/// is a caption and does nothing when tapped.
class ListRow : public el::element, public Focusable, public TapTarget
{
  public:
    ListRow(const Theme& theme, std::string text, std::string secondary, bool selected, bool heading,
            std::function<void()> onClick);

    el::view_limits limits(const el::basic_context& ctx) const override;
    void draw(const el::context& ctx) override;
    bool click(const el::context& ctx, el::mouse_button btn) override;
    bool cursor(const el::context& ctx, el::point p, el::cursor_tracking status) override;
    bool key(const el::context& ctx, el::key_info k) override;

    bool wants_control() const override
    {
        return !mHeading;
    }

    bool wants_focus() const override
    {
        return FocusWanted(!mHeading && mOnClick != nullptr);
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

    [[nodiscard]] AccessInfo Describe() const override;
    bool Activate(const el::rect& at) override;

    /// A right click (a long press on touch screens, later) asks for the row's own menu.
    std::function<void()> onMenu;

    /// An icon at the left, in `tint`.
    ListRow& Icon(Icons* icons, std::string name, el::color tint);

  private:
    const Theme& mTheme;
    std::string mText, mSecondary;
    Icons* mIcons = nullptr;
    std::string mIcon;
    el::color mTint;
    bool mSelected, mHeading;
    std::function<void()> mOnClick;
    bool mHover = false, mDown = false;
};

/// A rounded, filled area that takes clicks, so nothing behind it hears them: the face of a pop-up.
class Surface : public el::element
{
  public:
    Surface(el::color fill, el::color line, float radius) : mFill(fill), mLine(line), mRadius(radius)
    {
    }

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
    explicit Backdrop(std::function<void()> onClick) : mOnClick(std::move(onClick))
    {
    }

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
