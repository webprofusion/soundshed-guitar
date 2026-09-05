#pragma once

#include <cstdint>
#include <limits>
#include <vector>

namespace guitarfx
{
/**
 * Analyzer measurements published for one node of a signal graph.
 *
 * Shared, rather than nested in each publisher, because both SignalGraphExecutor
 * and MultiPresetMixer hand this payload on to the UI telemetry feed. Two
 * identical-but-distinct copies meant every hop had to field-copy all of it, and
 * a field added to one side would have been silently dropped by the other.
 */
struct AnalyzerTelemetry
{
    double peakPercent = 0.0;
    double rmsPercent = 0.0;
    double rmsDbu = 0.0;
    double rmsDbv = 0.0;
    double rmsVolts = 0.0;
    bool loudnessValid = false;
    double momentaryLufs = -std::numeric_limits<double>::infinity();
    double shortTermLufs = -std::numeric_limits<double>::infinity();
    double integratedLufs = -std::numeric_limits<double>::infinity();
    bool stereo = false;
    int activeChannelCount = 0;
    std::vector<float> spectrogramBinsDb;
    double spectrogramMinDbfs = -120.0;
    double spectrogramMaxDbfs = 0.0;
    double spectrogramMinFrequencyHz = 20.0;
    double spectrogramMaxFrequencyHz = 20000.0;
    std::vector<float> barkBandsDb;
    double barkMinDbfs = -96.0;
    double barkMaxDbfs = 0.0;
    double barkMinFrequencyHz = 20.0;
    double barkMaxFrequencyHz = 15500.0;
    std::uint64_t generatedAtMs = 0;
};
} // namespace guitarfx
