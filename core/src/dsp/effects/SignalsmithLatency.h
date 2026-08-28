#pragma once

#include "signalsmith-stretch.h"

#include <algorithm>

namespace guitarfx
{
/**
 * Signalsmith Stretch reports latency in two halves (docs):
 *   - inputLatency():  input is supplied ahead of the internal processing time
 *   - outputLatency(): audible output lags that processing time
 *
 * For live 1:1 pitch shift (equal in/out process lengths), host delay
 * compensation and dry alignment must use the sum.
 *
 * outputLatency() also includes one extra hop when splitComputation is enabled.
 */
[[nodiscard]] inline int SignalsmithTotalLatencySamples(const signalsmith::stretch::SignalsmithStretch<float>& stretch)
{
    return static_cast<int>(stretch.inputLatency() + stretch.outputLatency());
}

[[nodiscard]] inline int SignalsmithTotalLatencySamples(const signalsmith::stretch::SignalsmithStretch<float>& a,
                                                        const signalsmith::stretch::SignalsmithStretch<float>& b)
{
    return std::max(SignalsmithTotalLatencySamples(a), SignalsmithTotalLatencySamples(b));
}
} // namespace guitarfx
