#include "ui/widgets/SwitchMotion.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

namespace nanoq::ui
{
namespace
{
constexpr double kSlideSeconds = 0.2;
// How soon after a press a new switch in the same place still slides: a rebuild lands well
// inside it.
constexpr double kHandOverSeconds = 0.6;

double Now()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

/// Windows' "Show animations in Windows" (Settings, Accessibility, Visual effects). Read once a
/// second, not on every draw.
bool ReducedMotion()
{
#if defined(_WIN32)
    static double checked = -10.0;
    static bool reduced = false;
    const double now = Now();

    if (now - checked > 1.0)
    {
        checked = now;
        BOOL animate = TRUE;
        reduced = SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0) && !animate;
    }

    return reduced;
#else
    return false;
#endif
}

struct Pressed
{
    el::point centre;
    float position = 0.0f;
    double at = -10.0;
};

Pressed& LastPress()
{
    static Pressed pressed;
    return pressed;
}

struct Moving
{
    el::rect area;
    double until;
};

std::vector<Moving>& MovingAreas()
{
    static std::vector<Moving> moving;
    return moving;
}

float Ease(float t)
{
    // Ease out: quick from the press, settling at the end.
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}
} // namespace

float SwitchMotion::Position(bool on, const std::optional<el::rect>& area)
{
    const float target = on ? 1.0f : 0.0f;
    const double now = Now();

    if (!mStarted)
    {
        mStarted = true;
        mFrom = mTo = target;
        const auto& pressed = LastPress();

        if (area && now - pressed.at < kHandOverSeconds)
        {
            const auto c = cycfi::artist::center_point(*area);

            if (std::abs(c.x - pressed.centre.x) < 3.0f && std::abs(c.y - pressed.centre.y) < 3.0f &&
                std::abs(pressed.position - target) > 0.01f)
            {
                mFrom = pressed.position;
                mStart = now;
            }
        }
    }
    else if (target != mTo)
    {
        // From where it is drawn now, which is part way if it was already sliding.
        const float t = static_cast<float>(std::clamp((now - mStart) / kSlideSeconds, 0.0, 1.0));
        mFrom = mFrom + (mTo - mFrom) * Ease(t);
        mTo = target;
        mStart = now;
    }

    const float t = static_cast<float>(std::clamp((now - mStart) / kSlideSeconds, 0.0, 1.0));

    if (t >= 1.0f || ReducedMotion() || mFrom == mTo)
    {
        mFrom = mTo;
        return mTo;
    }

    if (area)
    {
        MovingAreas().push_back({*area, mStart + kSlideSeconds});
    }

    return mFrom + (mTo - mFrom) * Ease(t);
}

void SwitchMotion::NoteToggled(const std::optional<el::rect>& area) const
{
    if (!area)
    {
        return;
    }

    auto& pressed = LastPress();
    pressed.centre = cycfi::artist::center_point(*area);
    pressed.position = mTo;
    pressed.at = Now();
}

void SwitchMotion::Tick(el::view& view)
{
    auto& moving = MovingAreas();

    if (moving.empty())
    {
        return;
    }

    // Each sliding switch noted its area as it drew; redraw them all once more, and the ones
    // still sliding note themselves again as they do.
    auto areas = std::move(moving);
    moving.clear();
    const double now = Now();

    for (const auto& m : areas)
    {
        if (m.until + 0.05 >= now)
        {
            view.refresh(m.area);
        }
    }
}

el::color SwitchMotion::Mix(el::color off, el::color on, float t)
{
    return el::color{off.red + (on.red - off.red) * t, off.green + (on.green - off.green) * t,
                     off.blue + (on.blue - off.blue) * t, off.alpha + (on.alpha - off.alpha) * t};
}
} // namespace nanoq::ui
