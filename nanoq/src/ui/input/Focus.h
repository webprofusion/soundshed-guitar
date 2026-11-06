#pragma once

// Keyboard focus, and what assistive technology is told about each control.
//
// A control the keyboard can reach derives from Focusable as well as el::element. It wants focus
// only while the keyboard is in use (FocusVisible: set by Tab and the other keys that move focus,
// cleared by the next pointer press), so a click still leaves no focus on a button and Space keeps
// its shortcut (bypass, tap tempo) for a pointer user, as it always has. Tab gives a control focus,
// KeyRouter draws the ring round it (the web UI's :focus-visible, 2 px out from the control), and
// the control's own key() does the rest: Enter or Space presses a button, the arrows turn a knob.
//
// Describe() is what a screen reader hears (AccessBridge.h), and its role and name are also how
// focus finds its control again after a region is rebuilt under it (FocusKey).

#include <elements.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nanoq::ui
{
namespace el = cycfi::elements;

enum class AccessRole
{
    Button,
    Toggle, // a switch: on or off
    CheckBox,
    Slider,   // a knob or a slider
    Tab,      // one of a row of tabs, or a choice of a segmented control
    ListItem, // a row of a list: a preset, an effect, a resource
    MenuItem,
    ComboBox, // opens a list to choose from: a picker, a select
    Edit,     // a text field
};

/// What assistive technology reads of a control.
struct AccessInfo
{
    AccessRole role = AccessRole::Button;
    std::string name;
    /// A knob's value as text ("-6.0 dB"), a picker's choice, a field's text.
    std::string value;
    /// A switch or a checkbox: 0 off, 1 on. -1 for anything else.
    int toggled = -1;
    /// The tab, segment or row that is the chosen one.
    bool selected = false;
    bool enabled = true;
    /// A knob or slider, 0..1. Negative for anything else.
    double position = -1.0;
};

class Focusable
{
  public:
    virtual ~Focusable() = default;

    [[nodiscard]] virtual AccessInfo Describe() const = 0;

    /// The control's action: Enter, Space, or a screen reader's Invoke or Toggle. `at` is where
    /// it is, in the coordinates a click on it would have (a menu it opens hangs from it).
    /// Returns whether it has one.
    virtual bool Activate(const el::rect& at)
    {
        (void)at;
        return false;
    }

    /// A knob or slider set to a position (0..1) by a screen reader.
    virtual bool SetPosition(double)
    {
        return false;
    }

    /// Typing into the control (a knob's value being entered): it keeps Esc and every other key.
    [[nodiscard]] virtual bool Editing() const
    {
        return false;
    }

    [[nodiscard]] bool HasFocus() const
    {
        return mFocused;
    }

    /// The name read for the control where Describe() has none of its own to give (an icon, a
    /// knob whose label is drawn beside it), or a better one.
    void AccessName(std::string name)
    {
        mAccessName = std::move(name);
    }

    /// Keyboard focus is showing: a key has moved focus since the last pointer press.
    [[nodiscard]] static bool FocusVisible()
    {
        return sFocusVisible;
    }

    static void SetFocusVisible(bool visible)
    {
        sFocusVisible = visible;
    }

    /// Counts every gain and loss of focus, so a watcher can tell focus moved.
    [[nodiscard]] static unsigned FocusSerial()
    {
        return sSerial;
    }

  protected:
    /// For wants_focus(): an enabled control wants focus while the keyboard is in use, and keeps
    /// the focus it has.
    [[nodiscard]] bool FocusWanted(bool enabled) const
    {
        return enabled && (sFocusVisible || mFocused);
    }

    /// For begin_focus() and end_focus().
    void NoteFocus(bool focused)
    {
        if (focused != mFocused)
        {
            mFocused = focused;
            ++sSerial;
        }
    }

    /// The name: AccessName() if one was given, else `fallback`.
    [[nodiscard]] std::string NameOr(std::string fallback) const
    {
        return mAccessName.empty() ? std::move(fallback) : mAccessName;
    }

    std::string mAccessName;

  private:
    bool mFocused = false;
    static inline bool sFocusVisible = false;
    static inline unsigned sSerial = 0;
};

/// A key that presses a button: Enter (either) or Space, not a repeat.
[[nodiscard]] inline bool IsPressKey(const el::key_info& k)
{
    return k.action == el::key_action::press && (k.modifiers & (el::mod_control | el::mod_alt | el::mod_super)) == 0 &&
           (k.key == el::key_code::enter || k.key == el::key_code::kp_enter || k.key == el::key_code::space);
}

/// A key that opens a control's context menu: the Menu key, or Shift+F10.
[[nodiscard]] inline bool IsMenuKey(const el::key_info& k)
{
    return k.action == el::key_action::press &&
           (k.key == el::key_code::menu || (k.key == el::key_code::f10 && (k.modifiers & el::mod_shift)));
}

// ── Walking the tree for focus ──────────────────────────────────────────────

/// One control focus can go to, as a walk found it.
struct FocusTarget
{
    el::element* element = nullptr;
    Focusable* focusable = nullptr; // null for an Elements text box
    el::rect bounds;                // in the walk's coordinates
    /// The composites from the walk's root down to it, each with the index of the child taken.
    std::vector<std::pair<el::composite_base*, std::size_t>> path;
    /// FocusKey(), and how many targets before this one share it.
    std::string key;
    int ordinal = 0;
};

/// The role and name: what makes a control the same control after the region holding it is
/// rebuilt.
[[nodiscard]] std::string FocusKey(const el::element& element);

/// What a control says of itself: Describe() for a Focusable, a text field's text for an
/// Elements text box.
[[nodiscard]] AccessInfo DescribeTarget(const el::element& element);

/// Every control under `root` that wants focus now, in tab order (the order the layout gives
/// them), with its bounds; `ctx` is root's own context.
[[nodiscard]] std::vector<FocusTarget> CollectFocusTargets(const el::context& ctx, el::element& root);

/// The control holding focus under `root` (following each composite's focus), or null.
[[nodiscard]] el::element* FocusedLeaf(el::element& root);

/// Calls `f` with the focused control under `root` and its context, if there is one.
void WithFocusedLeaf(const el::context& ctx, el::element& root,
                     const std::function<void(const el::context&, el::element&)>& f);

/// Moves focus under `root` to `target` (one CollectFocusTargets found under the same root),
/// ending the focus that was there, and scrolls it into view.
void FocusTargetUnder(el::view& view, el::element& root, const FocusTarget& target);

/// Whether a text box under `root` holds focus with its caret showing, or a control is having a
/// value typed into it.
[[nodiscard]] bool TypingUnder(el::element& root);

/// Gives `element` focus wherever it is in the view (the layer holding it takes focus too), for a
/// control that starts typing from a pointer's double click. Call it outside the click (posted):
/// the click's own handling ends focus on anything that did not want it when the button went
/// down. Returns whether it was found.
bool FocusInView(el::view& view, el::element& element);

/// Asks for `area` (view coordinates, a Drawn area) to be redrawn on the next tick, for a control
/// that changed where no context was at hand to redraw it (a value field closed by end_focus).
void RepaintSoon(const std::optional<el::rect>& area);
/// Redraws what RepaintSoon asked for. Each tick, on the UI thread.
void FlushRepaints(el::view& view);
} // namespace nanoq::ui
