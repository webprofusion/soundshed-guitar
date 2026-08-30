#pragma once

/**
 * PracticeToolSupport.h — the few constants and conversions shared by the two
 * halves of PracticeToolService.
 *
 * The service is split across PracticeToolService.cpp (lifecycle, loading,
 * transport, the audio-thread mix) and PracticeToolServiceRender.cpp (the
 * background render thread: stretch, crossfade, ring-fill). Both clamp speed
 * the same way and both convert seconds to frames, and those two rules
 * disagreeing is exactly the kind of drift that produces a loop that ends a
 * frame early or a speed the render thread never honours.
 */

#include <cmath>

namespace guitarfx::controller_detail
{
inline constexpr double kPracticeToolMinSpeed = 0.25;
inline constexpr double kPracticeToolMaxSpeed = 2.0;

[[nodiscard]] inline std::size_t PracticeToolSecondsToFrames(double seconds, double sampleRate)
{
    if (!(seconds > 0.0) || sampleRate <= 0.0)
    {
        return 0;
    }

    return static_cast<std::size_t>(std::llround(seconds * sampleRate));
}
} // namespace guitarfx::controller_detail
