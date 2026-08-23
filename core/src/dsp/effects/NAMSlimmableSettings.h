#pragma once

#include "NAM/dsp.h"
#include "NAM/slimmable.h"

#include <algorithm>
#include <cmath>

namespace guitarfx
{

constexpr double kNamSlimmableSizeDefault = 1.0;
constexpr double kNamSlimmableSizeMin = 0.0;
constexpr double kNamSlimmableSizeMax = 1.0;

inline double SanitizeNamSlimmableSize(double value)
{
  if (!std::isfinite(value))
    return kNamSlimmableSizeDefault;
  return std::clamp(value, kNamSlimmableSizeMin, kNamSlimmableSizeMax);
}

// Slimmable size is owned per NAM node rather than by a process-wide global. A DAW
// loads every plugin instance into one process, so shared static storage would force
// one quality tier across every instance in the project. Each effect keeps its own
// value, delivered as node config by PluginController.
inline bool ApplyNamSlimmableSize(::nam::DSP* dsp, double size)
{
  if (!dsp)
    return false;

  auto* slimmable = dynamic_cast<::nam::SlimmableModel*>(dsp);
  if (!slimmable)
    return false;

  slimmable->SetSlimmableSize(SanitizeNamSlimmableSize(size));
  return true;
}

} // namespace guitarfx
