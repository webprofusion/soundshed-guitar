#pragma once

// Lists that scroll by dragging. Elements' scroller needs a wheel or its thin scroll bar, and a
// touch screen under X has neither: a finger reaches the view as the left mouse button, so all it
// can do on a list is press what is under it. DragScroller is that scroller, and it adds a drag on
// the list's content, in place of Elements' behind VScroll and HScroll (Layout.h).
//
// Everything in a list stays pressable. The press goes to the control under it as ever, and the
// list watches the pointer (DragGesture). Once it has moved far enough along the list, the list
// lets go of the control, by releasing it far from anywhere, and the press is a scroll from there.
//
// Only a TapTarget gives way like that. Anything else keeps its drag, so a knob still turns, and a
// list scrolls from the gaps round it. A row that can be dragged out of its list (EffectRow) is a
// TapTarget too: a drag across the list still reaches it. A chain card with touch sizes is one
// that a hold picks up (HoldToDrag, TouchHold.h): a swipe sooner scrolls the chain, and once the
// finger has rested on it the list lets the press go to the card.
//
// On Linux, where no one can tell a finger from a mouse, and on iOS, where every pointer is a
// finger. Off elsewhere, so a mouse drag on a list does what it always did:
// SOUNDSHED_NANOQ_DRAGSCROLL=1 turns it on anywhere, for testing, and 0 turns it off.

#include "ui/widgets/DragGesture.h"

#include <elements.hpp>

namespace nanoq::ui
{
namespace el = cycfi::elements;

/// A control that only cares for taps, so a list may scroll from a drag that starts on it. Marks
/// the class; a control that acts on the press rather than the release must not take it, because
/// a scroll starting there would have pressed it already. A control's release has to do nothing
/// when it comes from far away: see ReleasedAway.
class TapTarget
{
  public:
    virtual ~TapTarget() = default;

    /// Whether a list may scroll from a drag that starts here, now. A control that is a TapTarget
    /// only some of the time (a chain card, with touch sizes) says no otherwise.
    [[nodiscard]] virtual bool GivesWay() const
    {
        return true;
    }

    /// For a control a hold picks up: once TouchHold says the press was held, the list gives the
    /// drag to it rather than scrolling. False for a control that is only ever tapped.
    [[nodiscard]] virtual bool HoldToDrag() const
    {
        return false;
    }
};

/// Where a list releases a press it has taken over. iOS's host lets go at the same place when a
/// second finger turns a press into a scroll.
constexpr float kReleaseAway = -1e6f;

/// The press was given up, not let go of. Only a control that acts on any release needs this: one
/// that checks where the release is has no need, as nothing is there.
[[nodiscard]] inline bool ReleasedAway(const el::mouse_button& btn)
{
    return btn.pos.x <= kReleaseAway;
}

/// Whether a drag on a list scrolls it.
[[nodiscard]] bool DragScrollEnabled();

/// The device is driven by a finger first: iOS, and the kiosk (SOUNDSHED_KIOSK). Auto then keeps
/// the compact frame to a larger window (Shell::WantsWide). SOUNDSHED_NANOQ_TOUCH=1 turns it on
/// anywhere, for testing, and 0 turns it off.
[[nodiscard]] bool TouchFirst();

/// Elements' scroller, scrolled by a drag on its content as well. Holds no pointer into what it
/// scrolls from one event to the next, as that can be rebuilt during a press.
class DragScroller : public el::scroller_base
{
  public:
    explicit DragScroller(int traits) : el::scroller_base(traits), mVertical((traits & el::no_vscroll) == 0)
    {
    }

    bool click(const el::context& ctx, el::mouse_button btn) override;
    void drag(const el::context& ctx, el::mouse_button btn) override;

  private:
    [[nodiscard]] float Along(el::point p) const
    {
        return mVertical ? p.y : p.x;
    }

    [[nodiscard]] float Across(el::point p) const
    {
        return mVertical ? p.x : p.y;
    }

    /// How much of the content doesn't fit, along the list.
    [[nodiscard]] float Overflow(const el::context& ctx);
    [[nodiscard]] bool OnScrollBar(const el::context& ctx, el::point p) const;
    /// Can the press at `p` become a scroll of something? Asked after it reached the content.
    /// `holdToDrag` is set when it landed on a control a hold picks up.
    [[nodiscard]] bool PressGivesWay(const el::context& ctx, el::point p, bool& holdToDrag);
    void ScrollTo(const el::context& ctx, double align);

    bool mVertical;
    bool mArmed = false;      // this press may become a scroll of this list
    bool mGivesWay = true;    // what the press landed on lets go of a drag: for a list round this one
    double mPressAlign = 0.0; // where the list was at the press, 0..1
    bool mHoldToDrag = false; // the press is on a control a hold picks up (TouchHold.h)
    DragGesture mGesture;
};
} // namespace nanoq::ui
