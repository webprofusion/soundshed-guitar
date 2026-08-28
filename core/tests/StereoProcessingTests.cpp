#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/BuiltinEffects.h"

namespace
{
namespace fs = std::filesystem;

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 256;
constexpr int kTotalBlocks = 48;
constexpr int kTotalSamples = kBlockSize * kTotalBlocks;
constexpr double kPi = 3.14159265358979323846;

struct ModelResource
{
    fs::path path;
    std::string title;
};

struct TestResources
{
    fs::path resourcesDir;
    std::vector<ModelResource> models;
};

struct StereoBuffer
{
    std::vector<float> left;
    std::vector<float> right;
};

using EffectSetup = std::function<bool(guitarfx::EffectProcessor&, const TestResources&)>;

std::vector<float> GenerateSine(double frequency, double amplitude)
{
    std::vector<float> buffer(static_cast<size_t>(kTotalSamples), 0.0f);

    for (int i = 0; i < kTotalSamples; ++i)
    {
        const double phase = 2.0 * kPi * frequency * static_cast<double>(i) / kSampleRate;
        buffer[static_cast<size_t>(i)] = static_cast<float>(amplitude * std::sin(phase));
    }

    return buffer;
}

std::vector<float> GeneratePulsedSine(double frequency, double amplitude, int onBlockCount, int offBlockCount)
{
    std::vector<float> buffer(static_cast<size_t>(kTotalSamples), 0.0f);
    const int cycleBlocks = std::max(1, onBlockCount + offBlockCount);

    for (int i = 0; i < kTotalSamples; ++i)
    {
        const int blockIndex = i / kBlockSize;
        const bool isOnBlock = (blockIndex % cycleBlocks) < onBlockCount;

        if (!isOnBlock)
        {
            continue;
        }

        const double phase = 2.0 * kPi * frequency * static_cast<double>(i) / kSampleRate;
        buffer[static_cast<size_t>(i)] = static_cast<float>(amplitude * std::sin(phase));
    }

    return buffer;
}

std::vector<float> GenerateSilence()
{
    return std::vector<float>(static_cast<size_t>(kTotalSamples), 0.0f);
}

double PeakAbs(const std::vector<float>& buffer, size_t startIndex)
{
    double peak = 0.0;

    for (size_t i = std::min(startIndex, buffer.size()); i < buffer.size(); ++i)
    {
        peak = std::max(peak, std::abs(static_cast<double>(buffer[i])));
    }

    return peak;
}

double MaxAbsDiff(const std::vector<float>& a, const std::vector<float>& b, size_t startIndex)
{
    const size_t begin = std::min({startIndex, a.size(), b.size()});
    double peak = 0.0;

    for (size_t i = begin; i < a.size() && i < b.size(); ++i)
    {
        peak = std::max(peak, std::abs(static_cast<double>(a[i] - b[i])));
    }

    return peak;
}

TestResources LoadTestResources(const fs::path& resourcesDir)
{
    const fs::path libraryPath = resourcesDir / "data" / "audiofx-models.json";
    std::ifstream input(libraryPath);

    if (!input)
    {
        throw std::runtime_error("Failed to open audiofx-models.json");
    }

    const auto json = nlohmann::json::parse(input);

    if (!json.is_array())
    {
        throw std::runtime_error("audiofx-models.json must be an array");
    }

    TestResources resources;
    resources.resourcesDir = resourcesDir;

    for (const auto& entry : json)
    {
        const fs::path modelPath = resourcesDir / entry.value("filePath", "");

        if (modelPath.empty() || !fs::exists(modelPath))
        {
            continue;
        }

        resources.models.push_back({modelPath, entry.value("title", modelPath.filename().string())});

        if (resources.models.size() >= 2)
        {
            break;
        }
    }

    if (resources.models.empty())
    {
        throw std::runtime_error("No NAM test models were found in the test resources");
    }

    return resources;
}

std::unique_ptr<guitarfx::EffectProcessor> CreatePreparedEffect(const std::string& effectType, const EffectSetup& setup,
                                                                const TestResources& resources)
{
    auto effect = guitarfx::EffectRegistry::Instance().Create(effectType);

    if (!effect)
    {
        std::cerr << "ERROR: Failed to create effect type '" << effectType << "'\n";
        return nullptr;
    }

    effect->Prepare(kSampleRate, kBlockSize);

    if (!setup(*effect, resources))
    {
        std::cerr << "ERROR: Failed to configure effect type '" << effectType << "'\n";
        return nullptr;
    }

    effect->Reset();
    return effect;
}

StereoBuffer RenderEffect(guitarfx::EffectProcessor& effect, const std::vector<float>& inputLeft,
                          const std::vector<float>& inputRight)
{
    StereoBuffer rendered;
    rendered.left.resize(static_cast<size_t>(kTotalSamples), 0.0f);
    rendered.right.resize(static_cast<size_t>(kTotalSamples), 0.0f);

    std::vector<float> blockInLeft(static_cast<size_t>(kBlockSize), 0.0f);
    std::vector<float> blockInRight(static_cast<size_t>(kBlockSize), 0.0f);
    std::vector<float> blockOutLeft(static_cast<size_t>(kBlockSize), 0.0f);
    std::vector<float> blockOutRight(static_cast<size_t>(kBlockSize), 0.0f);

    float* inputs[2] = {blockInLeft.data(), blockInRight.data()};
    float* outputs[2] = {blockOutLeft.data(), blockOutRight.data()};

    for (int block = 0; block < kTotalBlocks; ++block)
    {
        const int offset = block * kBlockSize;
        std::copy_n(inputLeft.data() + offset, kBlockSize, blockInLeft.data());
        std::copy_n(inputRight.data() + offset, kBlockSize, blockInRight.data());
        std::fill(blockOutLeft.begin(), blockOutLeft.end(), 0.0f);
        std::fill(blockOutRight.begin(), blockOutRight.end(), 0.0f);

        effect.Process(inputs, outputs, kBlockSize);

        std::copy_n(blockOutLeft.data(), kBlockSize, rendered.left.data() + offset);
        std::copy_n(blockOutRight.data(), kBlockSize, rendered.right.data() + offset);
    }

    return rendered;
}

size_t GetAnalysisStart(const guitarfx::EffectProcessor& effect, int extraBlocks = 4)
{
    const size_t latency = static_cast<size_t>(std::max(0, effect.GetLatencySamples()));
    const size_t warmup = static_cast<size_t>(extraBlocks * kBlockSize);
    return std::min(static_cast<size_t>(kTotalSamples), latency + warmup);
}

bool RunLeakIsolationTest(const std::string& label, const std::string& effectType, const EffectSetup& setup,
                          const TestResources& resources, double leakRatioLimit, double leakAbsoluteLimit,
                          double minActivePeak, int extraWarmupBlocks = 4)
{
    const auto inputSignal = GenerateSine(220.0, 0.35);
    const auto silent = GenerateSilence();

    auto leftDriven = CreatePreparedEffect(effectType, setup, resources);
    auto rightDriven = CreatePreparedEffect(effectType, setup, resources);

    if (!leftDriven || !rightDriven)
    {
        return false;
    }

    const auto renderLeftDriven = RenderEffect(*leftDriven, inputSignal, silent);
    const auto renderRightDriven = RenderEffect(*rightDriven, silent, inputSignal);
    const size_t analysisStart =
        std::max(GetAnalysisStart(*leftDriven, extraWarmupBlocks), GetAnalysisStart(*rightDriven, extraWarmupBlocks));

    const double leftActivePeak = PeakAbs(renderLeftDriven.left, analysisStart);
    const double rightLeakPeak = PeakAbs(renderLeftDriven.right, analysisStart);
    const double rightActivePeak = PeakAbs(renderRightDriven.right, analysisStart);
    const double leftLeakPeak = PeakAbs(renderRightDriven.left, analysisStart);

    const bool pass = leftActivePeak > minActivePeak && rightActivePeak > minActivePeak &&
                      rightLeakPeak <= std::max(leakAbsoluteLimit, leftActivePeak * leakRatioLimit) &&
                      leftLeakPeak <= std::max(leakAbsoluteLimit, rightActivePeak * leakRatioLimit);

    std::cout << std::left << std::setw(44) << label << (pass ? "PASS" : "FAIL") << "  activeL=" << leftActivePeak
              << " leakR=" << rightLeakPeak << " activeR=" << rightActivePeak << " leakL=" << leftLeakPeak << "\n";
    return pass;
}

bool RunChannelIndependenceTest(const std::string& label, const std::string& effectType, const EffectSetup& setup,
                                const TestResources& resources, const std::vector<float>& leftInput,
                                const std::vector<float>& rightQuietInput, const std::vector<float>& rightHotInput,
                                double diffRatioLimit, double diffAbsoluteLimit, bool requireLeftActivity,
                                int extraWarmupBlocks = 4)
{
    auto quietEffect = CreatePreparedEffect(effectType, setup, resources);
    auto hotEffect = CreatePreparedEffect(effectType, setup, resources);

    if (!quietEffect || !hotEffect)
    {
        return false;
    }

    const auto quietRender = RenderEffect(*quietEffect, leftInput, rightQuietInput);
    const auto hotRender = RenderEffect(*hotEffect, leftInput, rightHotInput);
    const size_t analysisStart =
        std::max(GetAnalysisStart(*quietEffect, extraWarmupBlocks), GetAnalysisStart(*hotEffect, extraWarmupBlocks));

    const double leftPeakQuiet = PeakAbs(quietRender.left, analysisStart);
    const double leftPeakHot = PeakAbs(hotRender.left, analysisStart);
    const double referencePeak = std::max(leftPeakQuiet, leftPeakHot);
    const double leftDifference = MaxAbsDiff(quietRender.left, hotRender.left, analysisStart);

    bool pass = leftDifference <= std::max(diffAbsoluteLimit, referencePeak * diffRatioLimit);

    if (requireLeftActivity)
    {
        pass = pass && referencePeak > 1.0e-3;
    }

    std::cout << std::left << std::setw(44) << label << (pass ? "PASS" : "FAIL") << "  peakQuiet=" << leftPeakQuiet
              << " peakHot=" << leftPeakHot << " maxDiff=" << leftDifference << "\n";
    return pass;
}

bool LoadSingleNamModel(guitarfx::EffectProcessor& effect, const TestResources& resources)
{
    return effect.LoadResource(resources.models.front().path);
}

bool LoadBlendModels(guitarfx::EffectProcessor& effect, const TestResources& resources)
{
    if (resources.models.size() < 2)
    {
        return false;
    }

    std::vector<guitarfx::ResourceRef> refs(2);
    refs[0].parameterId = "gain";
    refs[0].parameterValue = 0.0;
    refs[1].parameterId = "gain";
    refs[1].parameterValue = 1.0;

    std::vector<fs::path> paths = {resources.models[0].path, resources.models[1].path};

    if (!effect.LoadResources(refs, paths))
    {
        return false;
    }

    effect.SetParam("blend", 0.35);
    return true;
}
} // namespace

int main()
{
#ifndef GUITARFX_TEST_RESOURCES_DIR
    #error "GUITARFX_TEST_RESOURCES_DIR must be defined"
#endif

    try
    {
        const auto resources = LoadTestResources(fs::path(GUITARFX_TEST_RESOURCES_DIR));

        guitarfx::RegisterAllEffects();

        std::cout << "========================================\n";
        std::cout << "Stereo Processing Tests\n";
        std::cout << "========================================\n\n";

        bool allPassed = true;

        allPassed &= RunLeakIsolationTest(
            "Doubler keeps channels isolated", "delay_doubler",
            [](guitarfx::EffectProcessor& effect, const TestResources&) {
                effect.SetParam("timeMs", 18.0);
                effect.SetParam("mix", 0.9);
                return true;
            },
            resources, 0.10, 1.0e-3, 1.0e-2, 2);

        allPassed &= RunLeakIsolationTest(
            "Octave keeps wet paths isolated", "octave",
            [](guitarfx::EffectProcessor& effect, const TestResources&) {
                effect.SetParam("octaveUp", 1.0);
                effect.SetParam("octaveDown", 0.0);
                effect.SetParam("mix", 1.0);
                return true;
            },
            resources, 0.10, 1.0e-3, 1.0e-2, 5);

        allPassed &= RunChannelIndependenceTest(
            "Limiter ignores opposite channel peaks", "limiter_brickwall",
            [](guitarfx::EffectProcessor& effect, const TestResources&) {
                effect.SetParam("ceiling", -18.0);
                effect.SetParam("release", 20.0);
                return true;
            },
            resources, GenerateSine(220.0, 0.03), GenerateSilence(), GenerateSine(880.0, 0.95), 0.10, 5.0e-3, true, 2);

        allPassed &= RunChannelIndependenceTest(
            "VCA compressor ignores opposite channel detector", "compressor_vca",
            [](guitarfx::EffectProcessor& effect, const TestResources&) {
                effect.SetParam("threshold", -24.0);
                effect.SetParam("ratio", 8.0);
                effect.SetParam("attack", 1.0);
                effect.SetParam("release", 60.0);
                effect.SetParam("mix", 1.0);
                return true;
            },
            resources, GenerateSine(220.0, 0.03), GenerateSilence(), GenerateSine(880.0, 0.85), 0.10, 5.0e-3, true, 3);

        allPassed &= RunChannelIndependenceTest(
            "Opto compressor ignores opposite channel detector", "compressor_opto",
            [](guitarfx::EffectProcessor& effect, const TestResources&) {
                effect.SetParam("threshold", -24.0);
                effect.SetParam("ratio", 6.0);
                effect.SetParam("attack", 5.0);
                effect.SetParam("release", 120.0);
                effect.SetParam("mix", 1.0);
                return true;
            },
            resources, GenerateSine(220.0, 0.03), GenerateSilence(), GenerateSine(880.0, 0.85), 0.12, 6.0e-3, true, 3);

        allPassed &= RunChannelIndependenceTest(
            "Noise gate ignores opposite channel activity", "dynamics_gate",
            [](guitarfx::EffectProcessor& effect, const TestResources&) {
                effect.SetParam("thresholdDb", -42.0);
                effect.SetParam("attackMs", 0.1);
                effect.SetParam("holdMs", 0.0);
                effect.SetParam("releaseMs", 5.0);
                return true;
            },
            resources, GeneratePulsedSine(220.0, 0.002, 1, 1), GenerateSilence(), GeneratePulsedSine(880.0, 0.4, 1, 1),
            0.10, 5.0e-4, false, 1);

        allPassed &= RunChannelIndependenceTest(
            "Auto-wah ignores opposite channel envelope", "auto_wah",
            [](guitarfx::EffectProcessor& effect, const TestResources&) {
                effect.SetParam("sensitivity", 1.0);
                effect.SetParam("mix", 1.0);
                return true;
            },
            resources, GenerateSine(220.0, 0.18), GenerateSilence(), GenerateSine(880.0, 0.9), 0.18, 2.0e-2, true, 3);

        allPassed &= RunChannelIndependenceTest(
            "Delay ducking ignores opposite channel", "delay_digital",
            [](guitarfx::EffectProcessor& effect, const TestResources&) {
                effect.SetParam("timeMs", 60.0);
                effect.SetParam("feedback", 0.25);
                effect.SetParam("mix", 1.0);
                effect.SetParam("stereoMode", 0.0);
                effect.SetParam("ducking", 1.0);
                return true;
            },
            resources, GeneratePulsedSine(220.0, 0.35, 1, 1), GenerateSilence(), GeneratePulsedSine(660.0, 0.9, 1, 1),
            0.12, 1.0e-2, true, 6);

        allPassed &= RunLeakIsolationTest(
            "Standard NAM keeps stereo separate", "amp_nam",
            [](guitarfx::EffectProcessor& effect, const TestResources& resources) {
                return LoadSingleNamModel(effect, resources);
            },
            resources, 0.20, 2.0e-2, 1.0e-2, 4);

        allPassed &= RunLeakIsolationTest(
            "Optimized NAM keeps stereo separate", "amp_nam_optimized",
            [](guitarfx::EffectProcessor& effect, const TestResources& resources) {
                return LoadSingleNamModel(effect, resources);
            },
            resources, 0.20, 2.0e-2, 1.0e-2, 4);

        allPassed &= RunLeakIsolationTest(
            "NAM FX keeps stereo separate", "fx_nam",
            [](guitarfx::EffectProcessor& effect, const TestResources& resources) {
                return LoadSingleNamModel(effect, resources);
            },
            resources, 0.20, 2.0e-2, 1.0e-2, 4);

        allPassed &= RunLeakIsolationTest(
            "NAM blend keeps stereo separate", "amp_nam_blend",
            [](guitarfx::EffectProcessor& effect, const TestResources& resources) {
                return LoadBlendModels(effect, resources);
            },
            resources, 0.20, 2.0e-2, 1.0e-2, 4);

        std::cout << "\n========================================\n";
        std::cout << "Stereo processing tests " << (allPassed ? "PASSED" : "FAILED") << "\n";
        return allPassed ? 0 : 1;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "Fatal error: " << ex.what() << "\n";
        return 1;
    }
}