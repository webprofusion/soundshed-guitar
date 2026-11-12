#pragma once

// A finger resting on a control that a hold picks up: a chain card, with touch sizes. A swipe
// that starts on the card sooner than kHoldSeconds scrolls the chain (DragScroller, TapTarget's
// HoldToDrag) instead of moving the card, which made a full chain impossible to scroll on a touch
// screen; held that long first, the card is picked up and lifts, so the finger knows it can move
// it now. The web UI's chain does the same (pointerDrag.ts TOUCH_DRAG_HOLD_MS).
//
// One press at a time, as LongPress: the control arms it when pressed, the view's root
// (KeyRouter) tells it where the pointer goes and lets go of it after the release, and the
// shell's tick reports the moment the hold takes, so the lifted card is drawn then and not at the
// first move.

#include <chrono>
#include <cmath>

namespace nanoq::ui
{
class TouchHold
{
  public:
    /// Short of LongPress::kHoldSeconds, the hold that opens the card's menu.
    static constexpr double kHoldSeconds = 0.25;
    /// A finger on glass wanders: this far is still holding still.
    static constexpr float kSlop = 8.0f;

    /// A press that a hold picks up began at (`x`, `y`).
    static void Press(float x, float y)
    {
        auto& s = State();
        s.armed = true;
        s.held = false;
        s.x = x;
        s.y = y;
        s.since = Clock::now();
    }

    /// The pointer is now at (`x`, `y`): moved too far before the hold took, the press is a scroll.
    static void Move(float x, float y)
    {
        auto& s = State();

        if (s.armed && !s.held && std::hypot(x - s.x, y - s.y) > kSlop)
        {
            s.armed = false;
        }
    }

    /// Whether the press being made has been held long enough to pick its control up.
    [[nodiscard]] static bool Held()
    {
        return State().held;
    }

    /// The press is over, whoever took its release.
    static void Reset()
    {
        auto& s = State();
        s.armed = false;
        s.held = false;
    }

    /// From the UI tick: true once, the moment a press has been held long enough.
    static bool Tick()
    {
        auto& s = State();

        if (!s.armed || s.held || std::chrono::duration<double>(Clock::now() - s.since).count() < kHoldSeconds)
        {
            return false;
        }

        s.held = true;
        return true;
    }

  private:
    using Clock = std::chrono::steady_clock;

    struct Pending
    {
        bool armed = false;
        bool held = false;
        float x = 0.0f, y = 0.0f;
        Clock::time_point since;
    };

    static Pending& State()
    {
        static Pending state;
        return state;
    }
};
} // namespace nanoq::ui
