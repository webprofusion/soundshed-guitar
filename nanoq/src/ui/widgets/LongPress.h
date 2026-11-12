#pragma once

// Press and hold for a control's right-click action (MIDI Learn on a knob, a chain card's menu, a
// scene's rename), where a finger has no right button: a Pi panel, a Windows touch screen. iOS's
// host makes its own right click from a long press (ios/elements_host/base_view.mm), so this is
// off there, and off without touch sizes, where a mouse has its right button.
//
// One press at a time, so one recogniser for the view. The control arms it when it is pressed,
// the view's root (KeyRouter) tells it where the pointer goes and lets go of it after the release,
// and the shell's tick fires it once the press has been held still for kHoldSeconds. The control
// asks Fired() at the release: a hold that fired has done its work, and the release is no tap.

#include "ui/widgets/Theme.h"

#include <elements.hpp>

#include <chrono>
#include <cmath>
#include <functional>
#include <utility>

#if defined(__APPLE__)
    #include <TargetConditionals.h>
#endif

namespace nanoq::ui
{
namespace el = cycfi::elements;

class LongPress
{
  public:
    static constexpr double kHoldSeconds = 0.5;
    /// A finger on glass wanders: this far is still holding still.
    static constexpr float kSlop = 8.0f;

    /// Whether a hold stands in for a right click here.
    [[nodiscard]] static bool Enabled()
    {
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
        return false;
#else
        return metrics::Touch();
#endif
    }

    /// A press on a control with a right-click action began at `at`: `action` runs if it is held.
    static void Press(el::point at, std::function<void()> action)
    {
        if (!Enabled() || !action)
        {
            return;
        }

        auto& s = State();
        s.armed = true;
        s.fired = false;
        s.at = at;
        s.since = Clock::now();
        s.action = std::move(action);
    }

    /// The pointer is now at `p`: moved too far, the press is a drag or a scroll, not a hold.
    static void Move(el::point p)
    {
        auto& s = State();

        if (s.armed && std::hypot(p.x - s.at.x, p.y - s.at.y) > kSlop)
        {
            s.armed = false;
            s.action = nullptr;
        }
    }

    /// The press is something else now (a drag the control started itself).
    static void Cancel()
    {
        auto& s = State();
        s.armed = false;
        s.action = nullptr;
    }

    /// Whether the press being released fired its hold: then the release is not a tap.
    [[nodiscard]] static bool Fired()
    {
        return State().fired;
    }

    /// The press is over, whoever took its release.
    static void Reset()
    {
        auto& s = State();
        s.armed = false;
        s.fired = false;
        s.action = nullptr;
    }

    /// From the UI tick: fires a press held long enough. Returns whether it fired.
    static bool Tick()
    {
        auto& s = State();

        if (!s.armed || std::chrono::duration<double>(Clock::now() - s.since).count() < kHoldSeconds)
        {
            return false;
        }

        s.armed = false;
        s.fired = true;
        const auto run = std::move(s.action); // it may open a menu, which rebuilds nothing it holds
        s.action = nullptr;
        run();
        return true;
    }

  private:
    using Clock = std::chrono::steady_clock;

    struct Pending
    {
        bool armed = false;
        bool fired = false;
        el::point at{0.0f, 0.0f};
        Clock::time_point since;
        std::function<void()> action;
    };

    static Pending& State()
    {
        static Pending state;
        return state;
    }
};
} // namespace nanoq::ui
