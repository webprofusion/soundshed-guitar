#pragma once

#include "NAM/dsp.h"
#include "NAM/slimmable.h"

#include <algorithm>
#include <atomic>
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

inline std::atomic<double>& NamSlimmableSizeStorage()
{
  static std::atomic<double> size{kNamSlimmableSizeDefault};
  return size;
}

inline double GetGlobalNamSlimmableSize()
{
  return NamSlimmableSizeStorage().load(std::memory_order_acquire);
}

inline void SetGlobalNamSlimmableSize(double value)
{
  NamSlimmableSizeStorage().store(SanitizeNamSlimmableSize(value), std::memory_order_release);
}

inline bool ApplyGlobalNamSlimmableSize(::nam::DSP* dsp)
{
  if (!dsp)
    return false;

  auto* slimmable = dynamic_cast<::nam::SlimmableModel*>(dsp);
  if (!slimmable)
    return false;

  slimmable->SetSlimmableSize(GetGlobalNamSlimmableSize());
  return true;
}

} // namespace guitarfx
