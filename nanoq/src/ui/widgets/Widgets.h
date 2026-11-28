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
#include <optional>
#include <string>
#include <vector>

namespace nanoq::ui
{
class Icons;

/// A line of text. Truncated with an ellipsis when the space is short; it never wraps.
///
/// A screen reader reads it where it is a heading: a title (Weight::Title), or one marked with
/// AccessHeading (a section's label). Other lines are the labels of the controls beside them,
/// which name those controls already.
class Text : public el::element, public Drawn, public AccessStatic
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
    /// Letter-spacing in em, as CSS letter-spacing: the web UI's small capitals.
    Text& Tracking(float em);

    [[nodiscard]] const std::string& Value() const
    {
        return mText;
    }

    /// Read as a heading of `level` (1 the highest), or with 0 not read at all, whatever it
    /// looks like.
    Text& AccessHeading(int level)
    {
        mHeading = level;
        return *this;
    }

    [[nodiscard]] std::optional<AccessInfo> DescribeStatic() const override;

  private:
    const Theme& mTheme;
    std::string mText;
    float mSize;
    Weight mWeight;
    int mHeading = -1; // -1: by how it looks
    el::color mColour;
    Align mAlign = Align::Left;
    bool mFixed = false;
    float mTracking = 0.0f;
};

/// Text wrapped at word breaks to a fixed width, as tall as its lines. A screen reader reads it.
class Paragraph : public el::element, public AccessStatic
{
  public:
    Paragraph(const Theme& theme, std::string text, float width, float size = metrics::kTextSize);

    el::view_limits limits(const el::basic_context& ctx) const override;
    void draw(const el::context& ctx) override;

    Paragraph& Colour(el::color c);
    /// The face's weight; Regular unless set.
    Paragraph& Weighted(Weight weight);
    /// How tall its lines are, before it is drawn: for sizing the dialog it sits in.
    [[nodiscard]] float Height() const;

    [[nodiscard]] std::optional<AccessInfo> DescribeStatic() const override
    {
        if (mText.empty())
        {
            return std::nullopt;
        }

        AccessInfo info;
        info.role = AccessRole::Label;
        info.name = mText;
        return info;
    }

  private:
    [[nodiscard]] std::vector<std::string> Lines(cycfi::artist::canvas& cnv) const;

    const Theme& mTheme;
    std::string mText;
    float mWidth;
    float mSize;
    el::color mColour;
    Weight mWeight = Weight::Regular;
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

/// Where an arrow, Page Up or Down, Home or End takes a control at `value` (0..1): a hundredth a
/// step (a thousandth with Shift), or one position of `steps`, and a tenth a page. None for any
/// other key.
[[nodiscard]] std::optional<double> KeyedPosition(const el::key_info& k, double value, int steps);
/// Delete or Backspace: puts a control back to its default.
[[nodiscard]] bool IsResetKey(const el::key_info& k);

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
    /// A press on the value (or Enter with focus): the owner asks for a typed one. With it set the
    /// value is underlined, as the web's typed value fields.
    std::function<void()> onEditValue;

  private:
    void Move(const el::context& ctx, el::point p);
    float mValueLeft = 0.0f; // where the value text starts, from the last draw

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

    /// The control bar's knob (.control-bar .knob-control): 38 px with no name over it (the bar
    /// draws the label), its 12 px value 4 px under it.
    ParamKnob& ControlBar(bool bar)
    {
        mBar = bar;
        return *this;
    }

    /// Greyed, and left alone by the pointer, while what it sets is off (.knob-control.disabled:
    /// the gate's threshold with the gate off).
    ParamKnob& Dimmed(bool dimmed)
    {
        if (dimmed != mDimmed)
        {
            mDimmed = dimmed;
            RepaintSoon(DrawnArea());
        }

        return *this;
    }

    /// The value centred in the cell less this much at its right, where a chevron sits beside
    /// it (.knob-value-row: the gate's value and its settings, centred as one).
    ParamKnob& ValueInset(float right)
    {
        mValueInset = right;
        return *this;
    }

    /// A point marked on the knob's travel (0..1): where a NAM Blend's model was captured
    /// (.knob-mapped-point), lit where the other knobs reach it, green where the knob is on it.
    struct Mark
    {
        double position = 0.0;
        bool selectable = false;
        bool selected = false;

        bool operator==(const Mark&) const = default;
    };

    /// The marks round the knob; set again as the values change (a live binding).
    void SetMarks(std::vector<Mark> marks);

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
    std::vector<Mark> mMarks;
    void BeginEdit(std::string text);
    void FinishEdit(bool commit);
    [[nodiscard]] el::rect ValueBox(const el::rect& b) const;

    /// The heights down the cell: the name, the gap to the knob, the knob, the gap to the value
    /// and the value.
    struct Geometry
    {
        float name, nameGap, diameter, valueGap, value;
    };
    [[nodiscard]] Geometry Layout() const;

    const Theme& mTheme;
    std::string mName;
    std::string mValueText;
    double mValue = 0.0;
    float mBare = 0.0f; // the diameter of a bare knob; 0 for the named one
    bool mLarge = false;
    bool mBar = false;
    bool mDimmed = false;
    float mValueInset = 0.0f;
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
/// Controls strip meter: a column of segments under the peak in dB, or, where the theme draws
/// its meters as needle gauges (ControlSkin::gaugeFace, Studio 1973), a VU meter reading RMS
/// with a red peak dot riding the scale under its arc.
class MeterBar : public el::element, public Drawn, public AccessStatic
{
  public:
    explicit MeterBar(const Theme& theme);

    /// What a screen reader calls it ("Input level"), which reads it as a meter in dB.
    MeterBar& Name(std::string name)
    {
        mName = std::move(name);
        return *this;
    }

    [[nodiscard]] std::optional<AccessInfo> DescribeStatic() const override;

    MeterBar& Vertical(bool vertical)
    {
        mVertical = vertical;
        return *this;
    }

    el::view_limits limits(const el::basic_context& ctx) const override;
    void draw(const el::context& ctx) override;

    /// Returns true if the bar changed enough to be worth a redraw. A gauge's needle reads
    /// `rmsDb` and moves with a VU meter's ballistics, so it eases between frames; its red peak
    /// dot jumps to `peakDb` and falls back like a peak programme meter.
    bool Set(double peakDb, bool clipped, double rmsDb = -120.0);

    /// Whether the theme draws vertical meters as gauges, and how wide one is.
    [[nodiscard]] static bool ThemeGauge(const Theme& theme);
    [[nodiscard]] static float ColumnWidth(const Theme& theme);
    /// How tall the vertical meter is: the readout over the ladder (.vu-meter, 68 px to the
    /// ladder's foot), or over the theme's gauge.
    [[nodiscard]] static float ColumnHeight(const Theme& theme);

  private:
    [[nodiscard]] bool Gauge() const
    {
        return mVertical && ThemeGauge(mTheme);
    }

    void DrawGauge(cycfi::artist::canvas& cnv, el::rect face) const;

    const Theme& mTheme;
    double mPeakDb = -120.0;
    bool mClipped = false;
    bool mVertical = false;
    float mNeedle = 0.0f;
    float mNeedleDrawn = 0.0f;
    double mNeedleAt = -1.0;
    double mPeakDotDb = -120.0;
    float mPeakDotDrawn = 0.0f;
    std::string mName;
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
    /// A second, muted line under the text (the levelling flyout's notes): the row is 44 tall.
    ListRow& Note(std::string note);
    /// The text in `ink` (an action row's accent) rather than the text colour.
    ListRow& TextInk(el::color ink);

  private:
    const Theme& mTheme;
    std::string mText, mSecondary, mNote;
    std::optional<el::color> mTextInk;
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
