#pragma once

/**
 * OfflineRenderSupport.h — Rendering audio through the mixer faster than real
 * time, for demo previews and riff export.
 *
 * Offline rendering borrows the live mixer, so it has to leave it exactly as it
 * found it: the prepare scope inside restores the previous sample rate and
 * block size even when the render throws. It also runs the graph past the end
 * of the input so reverb and delay tails are captured, then trims the silence
 * that padding leaves behind.
 */

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace guitarfx
{
class MultiPresetMixer;
}

namespace guitarfx::controller_detail
{
struct OfflineRenderBuffer
{
    std::string id;
    std::string title;
    double sampleRate = 0.0;
    std::vector<std::vector<float>> channelSamples;
};

/// True for a sample rate the offline renderer will accept.
[[nodiscard]] bool IsSupportedDemoRenderSampleRate(double sampleRate);

/// Picks the render rate from the request, falling back to the host rate.
/// On an unsupported request returns 0 and sets `error`.
[[nodiscard]] double ResolveDemoRenderSampleRate(const nlohmann::json& payload, double hostSampleRate,
                                                 std::string& error);

[[nodiscard]] std::string BuildDemoRenderSuggestedFilename(const std::string& requestedName, double renderSampleRate);

/// Index one past the last frame louder than `threshold`, once at least
/// `requiredQuietFrames` of silence follow it.
[[nodiscard]] std::size_t FindTrailingAudibleFrameCount(const std::vector<float>& left, const std::vector<float>& right,
                                                        float threshold, std::size_t requiredQuietFrames);

void TrimOfflineRenderBufferTrailingSilence(OfflineRenderBuffer& buffer, float threshold,
                                            std::size_t requiredQuietFrames);

/// Decodes and resamples source bytes into a buffer ready for the mixer.
/// On failure returns nullopt and sets `error`.
[[nodiscard]] std::optional<OfflineRenderBuffer> PrepareOfflineRenderBuffer(const std::vector<std::uint8_t>& bytes,
                                                                            double targetSampleRate,
                                                                            const std::string& id,
                                                                            const std::string& title,
                                                                            std::string& error);

/// Runs `source` through `mixer` faster than real time, writing stereo output
/// into renderedLeft/renderedRight. Takes `dspMutex` for the duration and
/// restores the mixer to `restoreSampleRate`/`restoreBlockSize` before
/// returning, including when the render throws. Returns false if nothing was
/// rendered.
[[nodiscard]] bool RenderBufferThroughMixer(MultiPresetMixer& mixer, std::mutex& dspMutex,
                                            const OfflineRenderBuffer& source, int blockSize, double restoreSampleRate,
                                            int restoreBlockSize, double tempoBpm, std::vector<float>& renderedLeft,
                                            std::vector<float>& renderedRight);

/// Per-bin peak magnitudes across both channels, normalised to [0, 1], for
/// drawing a waveform overview. Returns an empty array for empty or
/// mismatched channels.
[[nodiscard]] nlohmann::json BuildWaveformPeaks(const std::vector<float>& left, const std::vector<float>& right,
                                                std::size_t bins);
} // namespace guitarfx::controller_detail
