#include "controller/internal/OfflineRenderSupport.h"

#include "dsp/BlockSincResampler.h"
#include "dsp/MultiPresetMixer.h"
#include "util/AudioDecoder.h"
#include "util/PathSanitizer.h"
#include "util/Wav.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace guitarfx::controller_detail
{

constexpr std::array<int, 6> kDemoRenderSampleRateOptions = {
    44100, 48000, 88200, 96000, 176400, 192000,
};

bool IsSupportedDemoRenderSampleRate(double sampleRate)
{
    if (sampleRate <= 0.0 || sampleRate > 192000.0)
    {
        return false;
    }

    const int roundedSampleRate = static_cast<int>(std::llround(sampleRate));
    if (std::abs(sampleRate - static_cast<double>(roundedSampleRate)) >= 1.0)
    {
        return false;
    }

    return std::find(kDemoRenderSampleRateOptions.begin(), kDemoRenderSampleRateOptions.end(), roundedSampleRate) !=
           kDemoRenderSampleRateOptions.end();
}

double ResolveDemoRenderSampleRate(const nlohmann::json& payload, double hostSampleRate, std::string& error)
{
    if (hostSampleRate <= 0.0)
    {
        error = "Audio device sample rate is unavailable";
        return 0.0;
    }

    const auto sampleRateIter = payload.find("renderSampleRate");
    if (sampleRateIter == payload.end() || sampleRateIter->is_null())
    {
        return hostSampleRate;
    }

    if (!sampleRateIter->is_number())
    {
        error = "Render sample rate is invalid";
        return 0.0;
    }

    const double requestedSampleRate = sampleRateIter->get<double>();
    if (requestedSampleRate <= 0.0)
    {
        return hostSampleRate;
    }

    if (!IsSupportedDemoRenderSampleRate(requestedSampleRate))
    {
        error = "Unsupported render sample rate";
        return 0.0;
    }

    return static_cast<double>(std::llround(requestedSampleRate));
}

std::string BuildDemoRenderSuggestedFilename(const std::string& requestedName, double renderSampleRate)
{
    std::string suggestedName =
        guitarfx::util::SanitizeFilename(requestedName.empty() ? std::string("demo-audio.wav") : requestedName);
    std::string lowerSuggested = suggestedName;
    std::transform(lowerSuggested.begin(), lowerSuggested.end(), lowerSuggested.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    if (lowerSuggested.size() >= 4 && lowerSuggested.compare(lowerSuggested.size() - 4, 4, ".wav") == 0)
    {
        suggestedName.erase(suggestedName.size() - 4);
    }

    if (suggestedName.empty())
    {
        suggestedName = "demo-audio";
    }

    const int roundedKilohertz = static_cast<int>(std::llround(renderSampleRate / 1000.0));
    suggestedName += "-" + std::to_string(roundedKilohertz) + ".wav";
    return suggestedName;
}

class OfflineRenderMixerPrepareScope
{
  public:
    OfflineRenderMixerPrepareScope(guitarfx::MultiPresetMixer& mixer, double renderSampleRate, int renderBlockSize,
                                   double restoreSampleRate, int restoreBlockSize)
        : mMixer(mixer), mRestoreSampleRate(restoreSampleRate), mRestoreBlockSize(restoreBlockSize)
    {
        mMixer.Prepare(renderSampleRate, renderBlockSize);
        mMixer.Reset();
    }

    ~OfflineRenderMixerPrepareScope()
    {
        try
        {
            if (mRestoreSampleRate > 0.0 && mRestoreBlockSize > 0)
            {
                mMixer.Prepare(mRestoreSampleRate, mRestoreBlockSize);
                mMixer.Reset();
            }
        }
        catch (...)
        {
        }
    }

    OfflineRenderMixerPrepareScope(const OfflineRenderMixerPrepareScope&) = delete;
    OfflineRenderMixerPrepareScope& operator=(const OfflineRenderMixerPrepareScope&) = delete;

  private:
    guitarfx::MultiPresetMixer& mMixer;
    double mRestoreSampleRate = 0.0;
    int mRestoreBlockSize = 0;
};

std::size_t FindTrailingAudibleFrameCount(const std::vector<float>& left, const std::vector<float>& right,
                                          float threshold, std::size_t requiredQuietFrames)
{
    const std::size_t frameCount = std::min(left.size(), right.size());
    if (frameCount == 0)
    {
        return 0;
    }

    std::size_t quietFrames = 0;
    for (std::size_t frame = frameCount; frame > 0; --frame)
    {
        const std::size_t index = frame - 1;
        const float peak = std::max(std::abs(left[index]), std::abs(right[index]));
        if (peak <= threshold)
        {
            ++quietFrames;
            continue;
        }

        if (quietFrames >= requiredQuietFrames)
        {
            return frame;
        }

        return frame + quietFrames;
    }

    return 0;
}

void TrimOfflineRenderBufferTrailingSilence(OfflineRenderBuffer& buffer, float threshold,
                                            std::size_t requiredQuietFrames)
{
    if (buffer.channelSamples.empty() || buffer.channelSamples.front().empty())
    {
        return;
    }

    auto& left = buffer.channelSamples[0];
    auto& right = buffer.channelSamples.size() > 1 ? buffer.channelSamples[1] : buffer.channelSamples[0];
    const std::size_t trimmedFrames = FindTrailingAudibleFrameCount(left, right, threshold, requiredQuietFrames);
    if (trimmedFrames == 0 || trimmedFrames >= left.size())
    {
        return;
    }

    for (auto& channel : buffer.channelSamples)
    {
        channel.resize(trimmedFrames);
    }
}

std::optional<OfflineRenderBuffer> PrepareOfflineRenderBuffer(const std::vector<std::uint8_t>& bytes,
                                                              double targetSampleRate, const std::string& id,
                                                              const std::string& title, std::string& error)
{
    const auto wavData = guitarfx::util::DecodeAudioBytes(bytes);
    if (!wavData)
    {
        error = "Unsupported audio format (expected WAV, AIFF, or MP3)";
        return std::nullopt;
    }

    if (targetSampleRate <= 0.0)
    {
        error = "Target sample rate is invalid";
        return std::nullopt;
    }

    auto resampled =
        guitarfx::util::ConvertToSampleRate(*wavData, targetSampleRate, guitarfx::SampleRateConversionQuality::Highest);
    if (resampled.empty() || resampled.front().empty())
    {
        error = "Audio buffer is empty";
        return std::nullopt;
    }

    std::size_t minFrames = resampled.front().size();
    for (const auto& channel : resampled)
    {
        if (channel.empty())
        {
            error = "Audio buffer is empty";
            return std::nullopt;
        }
        minFrames = std::min(minFrames, channel.size());
    }

    if (minFrames == 0)
    {
        error = "Audio buffer is empty";
        return std::nullopt;
    }

    for (auto& channel : resampled)
    {
        if (channel.size() > minFrames)
        {
            channel.resize(minFrames);
        }
    }

    OfflineRenderBuffer buffer;
    buffer.id = id;
    buffer.title = title;
    buffer.sampleRate = targetSampleRate;
    buffer.channelSamples = std::move(resampled);
    const std::size_t requiredQuietFrames =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(targetSampleRate * 0.05)));
    TrimOfflineRenderBufferTrailingSilence(buffer, 3.0e-4f, requiredQuietFrames);
    return buffer;
}

bool RenderBufferThroughMixer(guitarfx::MultiPresetMixer& mixer, std::mutex& dspMutex,
                              const OfflineRenderBuffer& source, int blockSize, double restoreSampleRate,
                              int restoreBlockSize, double tempoBpm, std::vector<float>& renderedLeft,
                              std::vector<float>& renderedRight)
{
    if (source.channelSamples.empty() || source.channelSamples.front().empty())
    {
        return false;
    }

    const int safeBlockSize = std::max(32, blockSize);
    const std::size_t totalFrames = source.channelSamples.front().size();
    constexpr double kMaxTailSeconds = 8.0;
    constexpr double kTailSilenceSeconds = 0.25;
    constexpr float kTailSilencePeak = 7.5e-4f;

    const int requiredSilentBlocks = std::max(
        2, static_cast<int>(std::ceil((source.sampleRate * kTailSilenceSeconds) / static_cast<double>(safeBlockSize))));
    const int maxTailBlocks = std::max(
        requiredSilentBlocks,
        static_cast<int>(std::ceil((source.sampleRate * kMaxTailSeconds) / static_cast<double>(safeBlockSize))));

    renderedLeft.clear();
    renderedRight.clear();
    renderedLeft.reserve(totalFrames + static_cast<std::size_t>(maxTailBlocks * safeBlockSize));
    renderedRight.reserve(totalFrames + static_cast<std::size_t>(maxTailBlocks * safeBlockSize));

    std::vector<float> inputLeft(static_cast<std::size_t>(safeBlockSize), 0.0f);
    std::vector<float> inputRight(static_cast<std::size_t>(safeBlockSize), 0.0f);
    std::vector<float> outputLeft(static_cast<std::size_t>(safeBlockSize), 0.0f);
    std::vector<float> outputRight(static_cast<std::size_t>(safeBlockSize), 0.0f);
    const bool hasRightChannel = source.channelSamples.size() > 1;

    std::size_t frameOffset = 0;
    int tailBlocks = 0;
    int silentBlocks = 0;

    std::lock_guard<std::mutex> lock(dspMutex);
    OfflineRenderMixerPrepareScope renderPrepare(mixer, source.sampleRate, safeBlockSize, restoreSampleRate,
                                                 std::max(1, restoreBlockSize));

    while (frameOffset < totalFrames || tailBlocks < maxTailBlocks)
    {
        const bool feedingInput = frameOffset < totalFrames;
        const int framesThisBlock =
            feedingInput ? std::min(safeBlockSize, static_cast<int>(totalFrames - frameOffset)) : safeBlockSize;

        std::fill(inputLeft.begin(), inputLeft.end(), 0.0f);
        std::fill(inputRight.begin(), inputRight.end(), 0.0f);
        std::fill(outputLeft.begin(), outputLeft.end(), 0.0f);
        std::fill(outputRight.begin(), outputRight.end(), 0.0f);

        if (feedingInput)
        {
            std::copy_n(source.channelSamples[0].begin() + static_cast<std::ptrdiff_t>(frameOffset), framesThisBlock,
                        inputLeft.begin());
            if (hasRightChannel)
            {
                std::copy_n(source.channelSamples[1].begin() + static_cast<std::ptrdiff_t>(frameOffset),
                            framesThisBlock, inputRight.begin());
            }
            else
            {
                std::copy_n(inputLeft.begin(), framesThisBlock, inputRight.begin());
            }
        }

        float* inputPtrs[2] = {inputLeft.data(), inputRight.data()};
        float* outputPtrs[2] = {outputLeft.data(), outputRight.data()};

        mixer.SetTempo(tempoBpm);
        mixer.Process(inputPtrs, outputPtrs, framesThisBlock);

        renderedLeft.insert(renderedLeft.end(), outputLeft.begin(), outputLeft.begin() + framesThisBlock);
        renderedRight.insert(renderedRight.end(), outputRight.begin(), outputRight.begin() + framesThisBlock);

        if (feedingInput)
        {
            frameOffset += static_cast<std::size_t>(framesThisBlock);
            continue;
        }

        ++tailBlocks;

        float peak = 0.0f;
        for (int i = 0; i < framesThisBlock; ++i)
        {
            peak = std::max(peak, std::abs(outputLeft[static_cast<std::size_t>(i)]));
            peak = std::max(peak, std::abs(outputRight[static_cast<std::size_t>(i)]));
        }

        silentBlocks = (peak <= kTailSilencePeak) ? (silentBlocks + 1) : 0;
        if (silentBlocks >= requiredSilentBlocks)
        {
            break;
        }
    }

    const std::size_t requiredQuietFrames =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(source.sampleRate * 0.05)));
    const std::size_t trimmedFrames =
        FindTrailingAudibleFrameCount(renderedLeft, renderedRight, kTailSilencePeak, requiredQuietFrames);
    if (trimmedFrames > 0 && trimmedFrames < renderedLeft.size())
    {
        renderedLeft.resize(trimmedFrames);
        renderedRight.resize(trimmedFrames);
    }

    mixer.Reset();
    return !renderedLeft.empty() && renderedLeft.size() == renderedRight.size();
}

nlohmann::json BuildWaveformPeaks(const std::vector<float>& left, const std::vector<float>& right, std::size_t bins)
{
    nlohmann::json peaks = nlohmann::json::array();
    if (left.empty() || right.empty() || left.size() != right.size() || bins == 0)
    {
        return peaks;
    }

    const std::size_t totalSamples = left.size();
    const std::size_t binCount = std::min<std::size_t>(bins, totalSamples);

    for (std::size_t b = 0; b < binCount; ++b)
    {
        const std::size_t start = (b * totalSamples) / binCount;
        const std::size_t end = std::max(start + 1, ((b + 1) * totalSamples) / binCount);
        float peak = 0.0f;
        for (std::size_t i = start; i < end && i < totalSamples; ++i)
        {
            const float p = std::max(std::fabs(left[i]), std::fabs(right[i]));
            if (p > peak)
            {
                peak = p;
            }
        }
        peaks.push_back(static_cast<double>(std::clamp(peak, 0.0f, 1.0f)));
    }

    return peaks;
}

} // namespace guitarfx::controller_detail
