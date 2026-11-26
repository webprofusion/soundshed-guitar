#include "dsp/MixerTelemetry.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace guitarfx
{
void MixerTelemetry::Accumulate(Span& span, const float* left, const float* right, int numSamples) noexcept
{
    if (numSamples <= 0)
    {
        return;
    }

    for (const float* channel : {left, right})
    {
        if (!channel)
        {
            continue;
        }

        float peak = 0.0f;
        double sumSquares = 0.0;

        for (int i = 0; i < numSamples; ++i)
        {
            const float value = channel[i];
            const float absValue = std::abs(value);
            peak = std::max(peak, absValue);
            sumSquares += static_cast<double>(value) * static_cast<double>(value);

            if (absValue > 1.0f)
            {
                ++span.clipCount;
            }
        }

        span.peak = std::max(span.peak, static_cast<double>(peak));
        span.sumSquares += sumSquares;
        span.samples += static_cast<std::uint64_t>(numSamples);
    }

    span.frames += static_cast<std::uint64_t>(numSamples);
}

MixerTelemetry::LevelStats MixerTelemetry::ReadLevels(const AtomicLevelStats& source) noexcept
{
    LevelStats stats;
    stats.peak = source.peak.load(std::memory_order_relaxed);
    stats.rms = source.rms.load(std::memory_order_relaxed);
    stats.clipCount = source.clipCount.load(std::memory_order_relaxed);
    return stats;
}

void MixerTelemetry::Record(Stage stage, const float* left, const float* right, int numSamples) noexcept
{
    AtomicLevelStats* destination = nullptr;

    switch (stage)
    {
    case Stage::RawInput:
        destination = &mRawInputLevels;
        break;
    case Stage::Input:
        destination = &mInputLevels;
        break;
    case Stage::Output:
        destination = &mOutputLevels;
        break;
    default:
        return;
    }

    // A snapshot since the last block, or a span nobody has read for a long while, starts a
    // new span. A snapshot landing between this check and the stores below moves this one
    // block out of the next span; at 20 Hz that is a rare, single block.
    Span& span = destination->span;
    const std::uint32_t taken = mSnapshotsTaken.load(std::memory_order_acquire);

    if (taken != span.taken || span.frames >= kMaxSpanFrames)
    {
        span = Span{};
        span.taken = taken;
    }

    Accumulate(span, left, right, numSamples);
    destination->peak.store(span.peak, std::memory_order_relaxed);
    destination->rms.store(span.samples > 0 ? std::sqrt(span.sumSquares / static_cast<double>(span.samples)) : 0.0,
                           std::memory_order_relaxed);
    destination->clipCount.store(span.clipCount, std::memory_order_relaxed);
}

MixerTelemetry::Snapshot MixerTelemetry::GetSnapshot() const noexcept
{
    Snapshot snapshot;
    snapshot.rawInput = ReadLevels(mRawInputLevels);
    snapshot.input = ReadLevels(mInputLevels);
    snapshot.output = ReadLevels(mOutputLevels);
    mSnapshotsTaken.fetch_add(1, std::memory_order_release);
    return snapshot;
}

MixerTelemetry::NodeSignalLevel MixerTelemetry::ToSnapshotNode(const SignalGraphExecutor::NodeSignalLevel& entry,
                                                               std::string_view scope, const std::string& presetId)
{
    NodeSignalLevel node;
    node.scope = scope;
    node.presetId = presetId;
    node.nodeId = entry.nodeId;
    node.nodeType = entry.nodeType;
    node.channelCount = entry.channelCount;
    node.levels.peak = entry.peak;
    node.levels.rms = entry.rms;
    node.levels.clipCount = entry.clipCount;
    node.analyzer = entry.analyzer;
    return node;
}
} // namespace guitarfx
