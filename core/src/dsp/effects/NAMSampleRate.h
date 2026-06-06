#pragma once

#include <cmath>

namespace guitarfx
{

inline constexpr double kDefaultNamModelSampleRate = 48000.0;

inline double ResolveNamModelProcessingSampleRate(double expectedSampleRate, double hostSampleRate)
{
  (void)hostSampleRate;
  return expectedSampleRate > 0.0 ? expectedSampleRate : kDefaultNamModelSampleRate;
}

inline bool NeedsNamRuntimeResampling(double modelProcessingSampleRate, double hostSampleRate)
{
  if (modelProcessingSampleRate <= 0.0 || hostSampleRate <= 0.0)
    return true;

  // NAM models and host processing are effectively integer-Hz domains.
  const long long modelHz = static_cast<long long>(std::llround(modelProcessingSampleRate));
  const long long hostHz = static_cast<long long>(std::llround(hostSampleRate));
  return modelHz != hostHz;
}

} // namespace guitarfx