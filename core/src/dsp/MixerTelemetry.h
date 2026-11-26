#pragma once

#include "dsp/SignalGraphExecutor.h"
#include "dsp/SignalTelemetry.h"

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace guitarfx
{
/// Owns the lock-free level readings written by the audio thread and read by the UI thread.
class MixerTelemetry
{
  public:
    struct LevelStats
    {
        double peak = 0.0;
        double rms = 0.0;
        int clipCount = 0;
    };

    struct NodeSignalLevel
    {
        using AnalyzerTelemetry = guitarfx::AnalyzerTelemetry;

        std::string scope;
        std::string presetId;
        std::string nodeId;
        std::string nodeType;
        int channelCount = 0;
        LevelStats levels;
        std::optional<AnalyzerTelemetry> analyzer;
    };

    struct Snapshot
    {
        LevelStats rawInput;
        LevelStats input;
        LevelStats output;
        std::vector<NodeSignalLevel> nodes;
    };

    enum class Stage
    {
        RawInput,
        Input,
        Output
    };

    void SetEnabled(bool enabled) noexcept
    {
        mEnabled.store(enabled, std::memory_order_release);
    }

    [[nodiscard]] bool IsEnabled() const noexcept
    {
        return mEnabled.load(std::memory_order_acquire);
    }

    /// Adds one block to the stage's reading. Audio thread only.
    void Record(Stage stage, const float* left, const float* right, int numSamples) noexcept;
    /// The levels since the last snapshot: the highest peak, the RMS and the clips over every
    /// block in between, so a transient between two 20 Hz frames still reaches the meters. A
    /// snapshot taken before any new block repeats the last one. Meant for one reader (the
    /// telemetry publisher): taking a snapshot starts the next span.
    [[nodiscard]] Snapshot GetSnapshot() const noexcept;
    [[nodiscard]] static NodeSignalLevel ToSnapshotNode(const SignalGraphExecutor::NodeSignalLevel& entry,
                                                        std::string_view scope, const std::string& presetId);

    void NoteOversizedBlock() noexcept
    {
        mOversizedBlockCount.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t GetOversizedBlockCount() const noexcept
    {
        return mOversizedBlockCount.load(std::memory_order_relaxed);
    }

  private:
    /// The audio thread's running totals for the span since the last snapshot.
    struct Span
    {
        double peak = 0.0;
        double sumSquares = 0.0;
        std::uint64_t samples = 0; // over both channels, for the RMS
        std::uint64_t frames = 0;
        int clipCount = 0;
        std::uint32_t taken = 0;
    };

    /// What the reader sees: the span so far, published after every block.
    struct AtomicLevelStats
    {
        std::atomic<double> peak{0.0};
        std::atomic<double> rms{0.0};
        std::atomic<int> clipCount{0};
        Span span; // audio thread only
    };

    /// A span the reader has left alone this long (about 1.4 s at 48 kHz, as when the editor
    /// is closed) starts again, so the next snapshot is not a peak from minutes ago.
    static constexpr std::uint64_t kMaxSpanFrames = 65536;

    static void Accumulate(Span& span, const float* left, const float* right, int numSamples) noexcept;
    [[nodiscard]] static LevelStats ReadLevels(const AtomicLevelStats& source) noexcept;

    std::atomic<bool> mEnabled{true};
    std::atomic<std::uint64_t> mOversizedBlockCount{0};
    // Bumped by each snapshot; the audio thread starts a new span when it sees it move.
    mutable std::atomic<std::uint32_t> mSnapshotsTaken{0};
    AtomicLevelStats mRawInputLevels;
    AtomicLevelStats mInputLevels;
    AtomicLevelStats mOutputLevels;
};
} // namespace guitarfx
