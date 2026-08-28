#pragma once

#include <algorithm>
#include <cmath>

namespace guitarfx::drive_output_limiter
{
inline float SoftClipNearCeiling(float sample, float knee = 0.92f, float ceiling = 1.0f)
{
    const float safeKnee = std::clamp(knee, 0.0f, ceiling - 1.0e-4f);
    const float magnitude = std::abs(sample);

    if (magnitude <= safeKnee)
    {
        return sample;
    }

    const float range = std::max(ceiling - safeKnee, 1.0e-4f);
    const float excess = (magnitude - safeKnee) / range;
    const float softened = safeKnee + range * std::tanh(excess);
    return std::copysign(std::min(softened, ceiling), sample);
}
} // namespace guitarfx::drive_output_limiter