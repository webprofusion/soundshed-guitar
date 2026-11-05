#pragma once

// Is a press on a list a tap, a scroll, or neither? A finger on glass always wanders a bit, so
// nothing is decided until it has moved kDragSlop. Then the direction settles it, once: along the
// list is a scroll, and across it belongs to whatever was pressed, such as a row dragged out of
// the list. Which way it went stays settled for the rest of the press, so a scroll cannot turn
// into a drag of a row halfway, nor the other way round.

#include <cmath>

namespace nanoq::ui
{
/// How far a finger moves before a tap has become a drag.
constexpr float kDragSlop = 10.0f;

class DragGesture
{
  public:
    enum class State
    {
        Waiting,   // still a tap
        Scrolling, // moved along the list
        Passed,    // moved across it: none of the list's business
    };

    /// `along` and `across` are the press's position on the list's axis and on the other one.
    void Press(float along, float across)
    {
        mAlong = along;
        mAcross = across;
        mState = State::Waiting;
    }

    /// The pointer is now here. Returns what the press has become.
    State Move(float along, float across)
    {
        if (mState == State::Waiting)
        {
            const float moved = std::abs(along - mAlong);
            const float aside = std::abs(across - mAcross);

            if (std::hypot(moved, aside) >= kDragSlop)
            {
                mState = moved >= aside ? State::Scrolling : State::Passed;
            }
        }

        return mState;
    }

    /// How far the content goes from where it was at the press, to stay under the pointer at
    /// `along`: the list's offset grows as the pointer moves back up it.
    [[nodiscard]] float ScrolledBy(float along) const
    {
        return mAlong - along;
    }

    [[nodiscard]] State Current() const
    {
        return mState;
    }

  private:
    float mAlong = 0.0f;
    float mAcross = 0.0f;
    State mState = State::Waiting;
};
} // namespace nanoq::ui
