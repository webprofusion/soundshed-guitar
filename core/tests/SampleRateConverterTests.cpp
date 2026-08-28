#include "dsp/BlockSincResampler.h"
#include "dsp/effects/NAMOversampling.h"
#include "dsp/effects/NAMSampleRate.h"
#include "dsp/effects/NAMSlimmableSettings.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <vector>

namespace
{
std::atomic<std::size_t> gAllocationCount{0};
}

void* operator new(std::size_t size)
{
    gAllocationCount.fetch_add(1, std::memory_order_relaxed);

    if (void* pointer = std::malloc(std::max<std::size_t>(size, 1)))
    {
        return pointer;
    }

    throw std::bad_alloc();
}

void* operator new[](std::size_t size)
{
    return ::operator new(size);
}

void operator delete(void* pointer) noexcept
{
    std::free(pointer);
}

void operator delete[](void* pointer) noexcept
{
    std::free(pointer);
}

void operator delete(void* pointer, std::size_t) noexcept
{
    std::free(pointer);
}

void operator delete[](void* pointer, std::size_t) noexcept
{
    std::free(pointer);
}

namespace
{
constexpr double kPi = 3.14159265358979323846;

class TrackingNamDSP final : public ::nam::DSP
{
  public:
    TrackingNamDSP() : ::nam::DSP(1, 1, 48000.0)
    {
    }

    void Reset(double sampleRate, int maxBufferSize) override
    {
        resetSampleRate = sampleRate;
        resetBlockSize = maxBufferSize;
        ++resetCount;
    }

    void SetTimeScale(int scale) override
    {
        timeScale = scale;
    }

    void process(NAM_SAMPLE** input, NAM_SAMPLE** output, int numFrames) override
    {
        ++processCount;
        processedFrames += numFrames;
        std::copy_n(input[0], numFrames, output[0]);
    }

    double resetSampleRate = 0.0;
    int resetBlockSize = 0;
    int resetCount = 0;
    int timeScale = 0;
    int processCount = 0;
    int processedFrames = 0;
};

void GenerateSine(std::vector<float>& buffer, double frequency, double sampleRate)
{
    for (std::size_t sampleIndex = 0; sampleIndex < buffer.size(); ++sampleIndex)
    {
        buffer[sampleIndex] =
            static_cast<float>(0.5 * std::sin(2.0 * kPi * frequency * static_cast<double>(sampleIndex) / sampleRate));
    }
}

bool BufferIsFinite(const std::vector<float>& buffer)
{
    for (const float sample : buffer)
    {
        if (!std::isfinite(sample))
        {
            return false;
        }
    }

    return true;
}

double RmsError(const std::vector<float>& first, const std::vector<float>& second)
{
    const std::size_t count = std::min(first.size(), second.size());

    if (count == 0)
    {
        return std::numeric_limits<double>::infinity();
    }

    double sumSquares = 0.0;

    for (std::size_t sampleIndex = 0; sampleIndex < count; ++sampleIndex)
    {
        const double delta = static_cast<double>(first[sampleIndex]) - static_cast<double>(second[sampleIndex]);
        sumSquares += delta * delta;
    }

    return std::sqrt(sumSquares / static_cast<double>(count));
}

bool TestRoundTripQuality()
{
    constexpr double sourceRate = 48000.0;
    constexpr double intermediateRate = 44100.0;
    constexpr int sourceFrames = 2048;
    constexpr int intermediateFrames = 1882;

    std::vector<float> source(sourceFrames);
    std::vector<float> intermediate(intermediateFrames);
    std::vector<float> roundTrip(sourceFrames);

    GenerateSine(source, 997.0, sourceRate);

    guitarfx::BlockSincResampler downsampler;
    downsampler.Prepare(sourceRate, intermediateRate, sourceFrames);
    downsampler.ProcessFixedOutput(source.data(), static_cast<int>(source.size()), intermediate.data(),
                                   static_cast<int>(intermediate.size()));

    guitarfx::BlockSincResampler upsampler;
    upsampler.Prepare(intermediateRate, sourceRate, intermediateFrames);
    upsampler.ProcessFixedOutput(intermediate.data(), static_cast<int>(intermediate.size()), roundTrip.data(),
                                 static_cast<int>(roundTrip.size()));

    const double error = RmsError(source, roundTrip);
    std::cout << "Round-trip RMS error: " << error << "\n";
    return BufferIsFinite(intermediate) && BufferIsFinite(roundTrip) && error < 0.02;
}

bool TestFixedOutputCount()
{
    constexpr double sourceRate = 44100.0;
    constexpr double targetRate = 96000.0;
    constexpr int sourceFrames = 257;
    constexpr int targetFrames = 559;

    std::vector<float> source(sourceFrames);
    std::vector<float> target(targetFrames, 0.0f);
    GenerateSine(source, 440.0, sourceRate);

    guitarfx::BlockSincResampler resampler;
    resampler.Prepare(sourceRate, targetRate, sourceFrames);
    const int written = resampler.ProcessFixedOutput(source.data(), sourceFrames, target.data(), targetFrames);

    return written == targetFrames && BufferIsFinite(target);
}

bool TestOptimizedNamSampleRateParsing()
{
    const nlohmann::json metadataRate = nlohmann::json::parse(
        R"({"metadata":{"expected_sample_rate":"96000"},"sample_rate":48000,"config":{"sample_rate":44100}})");
    const nlohmann::json topLevelRate = nlohmann::json::parse(R"({"sample_rate":48000,"config":{}})");
    const nlohmann::json configRate = nlohmann::json::parse(R"({"config":{"sample_rate":44100}})");
    const nlohmann::json missingRate = nlohmann::json::parse(R"({"config":{}})");

    return guitarfx::nam::ReadExpectedSampleRateFromNamJson(metadataRate) == 96000.0 &&
           guitarfx::nam::ReadExpectedSampleRateFromNamJson(topLevelRate) == 48000.0 &&
           guitarfx::nam::ReadExpectedSampleRateFromNamJson(configRate) == 44100.0 &&
           guitarfx::nam::ReadExpectedSampleRateFromNamJson(missingRate) < 0.0;
}

bool TestNamDefaultProcessingRate()
{
    return guitarfx::ResolveNamModelProcessingSampleRate(44100.0, 96000.0) == 44100.0 &&
           guitarfx::ResolveNamModelProcessingSampleRate(-1.0, 96000.0) == guitarfx::kDefaultNamModelSampleRate;
}

bool TestNamOversamplingConfiguration()
{
    return guitarfx::NamOversamplingFactorFromIndex(0.0) == 1 && guitarfx::NamOversamplingFactorFromIndex(1.0) == 2 &&
           guitarfx::NamOversamplingFactorFromIndex(5.0) == 32 &&
           guitarfx::NamOversamplingFactorFromIndex(99.0) == 32 && guitarfx::NamOversamplingIndexFromFactor(1) == 0 &&
           guitarfx::NamOversamplingIndexFromFactor(8) == 3 &&
           guitarfx::ResolveNamOversampledRenderingRate(48000.0, 48000.0, 1) == 48000.0 &&
           guitarfx::ResolveNamOversampledRenderingRate(48000.0, 48000.0, 4) == 192000.0 &&
           guitarfx::ResolveNamOversampledRenderingRate(48000.0, 44100.0, 2) == 96000.0;
}

/// Sanitizing is shared by every NAM node: out-of-range or non-finite values must
/// fall back to the documented defaults rather than reaching the resampler.
bool TestNamQualitySanitizing()
{
    return guitarfx::SanitizeNamOversamplingIndex(-4.0) == 0 &&
           guitarfx::SanitizeNamOversamplingIndex(99.0) == guitarfx::kNamOversamplingMaxIndex &&
           guitarfx::SanitizeNamOversamplingIndex(std::numeric_limits<double>::quiet_NaN()) ==
               guitarfx::kNamOversamplingIndexDefault &&
           guitarfx::SanitizeNamAntiAliasPhaseIndex(7.0) == guitarfx::kNamAntiAliasPhaseMaxIndex &&
           guitarfx::SanitizeNamAntiAliasPhaseIndex(std::numeric_limits<double>::infinity()) ==
               guitarfx::kNamAntiAliasPhaseIndexDefault &&
           guitarfx::SanitizeNamSlimmableSize(-1.0) == guitarfx::kNamSlimmableSizeMin &&
           guitarfx::SanitizeNamSlimmableSize(5.0) == guitarfx::kNamSlimmableSizeMax &&
           guitarfx::SanitizeNamSlimmableSize(std::numeric_limits<double>::quiet_NaN()) ==
               guitarfx::kNamSlimmableSizeDefault;
}

/// Oversampling settings are owned per node, not by a process-wide global: two
/// processors configured differently must stay independent. This is what lets two
/// plugin instances in one DAW project run at different quality tiers.
bool TestNamOversamplingIsPerProcessor()
{
    constexpr int blockSize = 64;
    constexpr double hostRate = 48000.0;

    TrackingNamDSP modelOff;
    TrackingNamDSP modelUp;
    guitarfx::NamOversamplingProcessor processorOff;
    guitarfx::NamOversamplingProcessor processorUp;

    processorOff.Prepare(modelOff, hostRate, hostRate, blockSize, guitarfx::NamOversamplingFactorFromIndex(0),
                         guitarfx::NamAntiAliasPhaseFromIndex(0));
    processorUp.Prepare(modelUp, hostRate, hostRate, blockSize, guitarfx::NamOversamplingFactorFromIndex(2),
                        guitarfx::NamAntiAliasPhaseFromIndex(2));

    // Preparing the second processor must not disturb the first.
    if (processorOff.GetTimeScale() != 1 || processorOff.IsResamplingActive())
    {
        return false;
    }

    if (processorUp.GetTimeScale() != 4 || !processorUp.IsResamplingActive())
    {
        return false;
    }

    // Linear-phase reports latency; the untouched off-tier processor still reports none.
    if (processorUp.GetLatencySamples() <= 0 || processorOff.GetLatencySamples() != 0)
    {
        return false;
    }

    // And the models really were configured to different rendering rates.
    return modelOff.timeScale == 1 && modelUp.timeScale == 4 &&
           processorUp.GetRenderingSampleRate() > processorOff.GetRenderingSampleRate();
}

bool TestNamDryDelay()
{
    guitarfx::NamDryDelay delay;
    delay.Prepare(3, 8);

    std::vector<float> firstBlock{1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
    delay.Process(firstBlock.data(), static_cast<int>(firstBlock.size()));
    const std::vector<float> expectedFirst{0.0f, 0.0f, 0.0f, 1.0f, 2.0f};

    if (firstBlock != expectedFirst)
    {
        return false;
    }

    std::vector<float> secondBlock{6.0f, 7.0f};
    delay.Process(secondBlock.data(), static_cast<int>(secondBlock.size()));
    const std::vector<float> expectedSecond{3.0f, 4.0f};

    if (secondBlock != expectedSecond)
    {
        return false;
    }

    delay.Reset();
    std::vector<float> resetBlock{8.0f, 9.0f, 10.0f};
    delay.Process(resetBlock.data(), static_cast<int>(resetBlock.size()));
    return resetBlock == std::vector<float>({0.0f, 0.0f, 0.0f});
}

bool TestNamOversamplingProcessor()
{
    constexpr int blockSize = 64;
    TrackingNamDSP model;
    guitarfx::NamOversamplingProcessor processor;
    processor.Prepare(model, 48000.0, 48000.0, blockSize, 4, dsp::EAntiAliasFilterPhase::MinimumPhaseCascadedFIR);

    if (model.timeScale != 4 || model.resetSampleRate != 192000.0 || model.resetBlockSize != 257 ||
        processor.GetRenderingSampleRate() != 192000.0 || processor.GetTimeScale() != 4)
    {
        return false;
    }

    std::vector<NAM_SAMPLE> input(blockSize);
    std::vector<NAM_SAMPLE> output(blockSize, static_cast<NAM_SAMPLE>(0.0));

    for (int sampleIndex = 0; sampleIndex < blockSize; ++sampleIndex)
    {
        input[static_cast<std::size_t>(sampleIndex)] = static_cast<NAM_SAMPLE>(sampleIndex) / blockSize;
    }

    const std::size_t allocationsBeforeProcess = gAllocationCount.load(std::memory_order_relaxed);
    processor.Process(model, input.data(), output.data(), blockSize);
    const std::size_t allocationsAfterProcess = gAllocationCount.load(std::memory_order_relaxed);

    if (allocationsAfterProcess != allocationsBeforeProcess)
    {
        return false;
    }

    if (model.processCount <= 0 || model.processedFrames < blockSize)
    {
        return false;
    }

    for (const NAM_SAMPLE sample : output)
    {
        if (!std::isfinite(static_cast<double>(sample)))
        {
            return false;
        }
    }

    processor.Reset(model);

    if (model.resetCount != 2 || model.timeScale != 4)
    {
        return false;
    }

    TrackingNamDSP linearModel;
    guitarfx::NamOversamplingProcessor linearProcessor;
    linearProcessor.Prepare(linearModel, 48000.0, 48000.0, blockSize, 2,
                            dsp::EAntiAliasFilterPhase::LinearCascadedFIRShort);

    if (linearProcessor.GetLatencySamples() <= 0)
    {
        return false;
    }

    std::fill(output.begin(), output.end(), static_cast<NAM_SAMPLE>(0.0));
    const std::size_t allocationsBeforeLinear = gAllocationCount.load(std::memory_order_relaxed);
    linearProcessor.Process(linearModel, input.data(), output.data(), blockSize);

    if (gAllocationCount.load(std::memory_order_relaxed) != allocationsBeforeLinear)
    {
        return false;
    }

    TrackingNamDSP fractionalModel;
    guitarfx::NamOversamplingProcessor fractionalProcessor;
    fractionalProcessor.Prepare(fractionalModel, 48000.0, 44100.0, blockSize, 1,
                                dsp::EAntiAliasFilterPhase::MinimumPhaseCascadedFIR);
    const std::size_t allocationsBeforeFractional = gAllocationCount.load(std::memory_order_relaxed);
    fractionalProcessor.Process(fractionalModel, input.data(), output.data(), blockSize);
    return gAllocationCount.load(std::memory_order_relaxed) == allocationsBeforeFractional;
}
} // namespace

int main()
{
    const bool roundTripOk = TestRoundTripQuality();
    const bool fixedOutputOk = TestFixedOutputCount();
    const bool optimizedNamSampleRateParsingOk = TestOptimizedNamSampleRateParsing();
    const bool namDefaultProcessingRateOk = TestNamDefaultProcessingRate();
    const bool namOversamplingConfigurationOk = TestNamOversamplingConfiguration();
    const bool namQualitySanitizingOk = TestNamQualitySanitizing();
    const bool namPerProcessorOk = TestNamOversamplingIsPerProcessor();
    const bool namDryDelayOk = TestNamDryDelay();
    const bool namOversamplingProcessorOk = TestNamOversamplingProcessor();

    if (!roundTripOk)
    {
        std::cerr << "Sample-rate converter round-trip quality test failed\n";
    }

    if (!fixedOutputOk)
    {
        std::cerr << "Sample-rate converter fixed output count test failed\n";
    }

    if (!optimizedNamSampleRateParsingOk)
    {
        std::cerr << "Optimized NAM sample-rate metadata parsing test failed\n";
    }

    if (!namDefaultProcessingRateOk)
    {
        std::cerr << "NAM default processing-rate test failed\n";
    }

    if (!namOversamplingConfigurationOk)
    {
        std::cerr << "NAM oversampling configuration test failed\n";
    }

    if (!namQualitySanitizingOk)
    {
        std::cerr << "NAM quality sanitizing test failed\n";
    }

    if (!namPerProcessorOk)
    {
        std::cerr << "NAM per-processor oversampling independence test failed\n";
    }

    if (!namDryDelayOk)
    {
        std::cerr << "NAM dry-delay alignment test failed\n";
    }

    if (!namOversamplingProcessorOk)
    {
        std::cerr << "NAM oversampling processor test failed\n";
    }

    return (roundTripOk && fixedOutputOk && optimizedNamSampleRateParsingOk && namDefaultProcessingRateOk &&
            namOversamplingConfigurationOk && namQualitySanitizingOk && namPerProcessorOk && namDryDelayOk &&
            namOversamplingProcessorOk)
               ? 0
               : 1;
}
