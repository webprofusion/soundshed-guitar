#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"

#include "dsp/EffectGuids.h"
#include "dsp/MultiPresetMixer.h"
#include "dsp/effects/BuiltinEffects.h"
#include "dsp/effects/OptimizedNAMAmpEffect.h"
#include "util/PathEncoding.h"

namespace
{
namespace fs = std::filesystem;

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 512;
constexpr int kWarmupBlocks = 8;
constexpr double kCalibrationLevelDbu = 12.0;
constexpr double kDbfsTolerance = 1.0e-3;
constexpr double kSampleMaxAbsTolerance = 1.0e-4;
constexpr double kSampleRmsTolerance = 1.0e-5;
constexpr double kPi = 3.14159265358979323846;

struct ModelMetadata
{
    bool loadOk = false;
    bool hasInputLevel = false;
    bool hasOutputLevel = false;
    bool hasLoudness = false;
    std::optional<double> inputLevelDbu;
    std::optional<double> outputLevelDbu;
    std::optional<double> loudness;
    std::string error;
};

struct Measurement
{
    bool ok = false;
    std::string error;
    double leftDbfs = -200.0;
    double rightDbfs = -200.0;
    std::vector<float> leftSamples;
};

struct DiffMetrics
{
    double maxAbs = 0.0;
    double rms = 0.0;
};

double RmsDbfs(const std::vector<float>& buffer)
{
    if (buffer.empty())
    {
        return -200.0;
    }

    double sumSquares = 0.0;
    for (const float sample : buffer)
    {
        sumSquares += static_cast<double>(sample) * static_cast<double>(sample);
    }

    const double rms = std::sqrt(sumSquares / static_cast<double>(buffer.size()));
    if (rms < 1.0e-12 || !std::isfinite(rms))
    {
        return -200.0;
    }

    return 20.0 * std::log10(rms);
}

DiffMetrics ComputeDiffMetrics(const std::vector<float>& a, const std::vector<float>& b)
{
    DiffMetrics metrics;
    if (a.size() != b.size() || a.empty())
    {
        return metrics;
    }

    double sumSquares = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
    {
        const double diff = static_cast<double>(a[i]) - static_cast<double>(b[i]);
        metrics.maxAbs = std::max(metrics.maxAbs, std::abs(diff));
        sumSquares += diff * diff;
    }

    metrics.rms = std::sqrt(sumSquares / static_cast<double>(a.size()));
    return metrics;
}

std::vector<fs::path> DiscoverModels(const fs::path& root)
{
    std::vector<fs::path> models;
    if (!fs::exists(root))
    {
        return models;
    }

    for (const auto& entry : fs::recursive_directory_iterator(root))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".nam")
        {
            models.push_back(entry.path());
        }
    }

    std::sort(models.begin(), models.end());
    return models;
}

ModelMetadata ReadModelMetadata(const fs::path& modelPath)
{
    ModelMetadata metadata;

    try
    {
        auto dsp = nam::get_dsp(modelPath);
        if (!dsp)
        {
            metadata.error = "nam::get_dsp returned null";
            return metadata;
        }

        metadata.loadOk = true;
        metadata.hasInputLevel = dsp->HasInputLevel();
        metadata.hasOutputLevel = dsp->HasOutputLevel();
        metadata.hasLoudness = dsp->HasLoudness();

        if (metadata.hasInputLevel)
        {
            metadata.inputLevelDbu = dsp->GetInputLevel();
        }
        if (metadata.hasOutputLevel)
        {
            metadata.outputLevelDbu = dsp->GetOutputLevel();
        }
        if (metadata.hasLoudness)
        {
            metadata.loudness = dsp->GetLoudness();
        }
    }
    catch (const std::exception& ex)
    {
        metadata.error = ex.what();
    }
    catch (...)
    {
        metadata.error = "unknown exception";
    }

    return metadata;
}

Measurement MeasureReferencePath(const fs::path& modelPath, bool useCalibration, double calibrationDbu,
                                 double inputAmplitude)
{
    Measurement m;

    try
    {
        auto dsp = nam::get_dsp(modelPath);
        if (!dsp)
        {
            m.error = "reference: nam::get_dsp returned null";
            return m;
        }

        const std::optional<double> modelInput =
            dsp->HasInputLevel() ? std::optional<double>(dsp->GetInputLevel()) : std::nullopt;
        const std::optional<double> modelOutput =
            dsp->HasOutputLevel() ? std::optional<double>(dsp->GetOutputLevel()) : std::nullopt;

        double inputGainDb = 0.0;
        double outputGainDb = 0.0;
        if (useCalibration && modelInput.has_value())
        {
            inputGainDb += (calibrationDbu - *modelInput);
        }
        if (useCalibration && modelOutput.has_value())
        {
            outputGainDb += (*modelOutput - calibrationDbu);
        }

        const double inputGain = std::pow(10.0, inputGainDb / 20.0);
        const double outputGain = std::pow(10.0, outputGainDb / 20.0);

        dsp->Reset(kSampleRate, kBlockSize);

        std::vector<NAM_SAMPLE> in(static_cast<size_t>(kBlockSize), static_cast<NAM_SAMPLE>(0.0));
        std::vector<NAM_SAMPLE> out(static_cast<size_t>(kBlockSize), static_cast<NAM_SAMPLE>(0.0));
        std::vector<float> outMono(static_cast<size_t>(kBlockSize), 0.0f);

        for (int block = 0; block < kWarmupBlocks; ++block)
        {
            for (int i = 0; i < kBlockSize; ++i)
            {
                const double t = static_cast<double>(block * kBlockSize + i) / kSampleRate;
                const double signal = inputAmplitude * std::sin(2.0 * kPi * 220.0 * t);
                in[static_cast<size_t>(i)] = static_cast<NAM_SAMPLE>(signal * inputGain);
            }

            NAM_SAMPLE* inPtrs[1] = {in.data()};
            NAM_SAMPLE* outPtrs[1] = {out.data()};
            dsp->process(inPtrs, outPtrs, kBlockSize);

            for (int i = 0; i < kBlockSize; ++i)
            {
                outMono[static_cast<size_t>(i)] = static_cast<float>(out[static_cast<size_t>(i)] * outputGain);
            }
        }

        m.leftDbfs = RmsDbfs(outMono);
        m.rightDbfs = m.leftDbfs;
        m.leftSamples = outMono;
        m.ok = true;
        return m;
    }
    catch (const std::exception& ex)
    {
        m.error = std::string("reference exception: ") + ex.what();
        return m;
    }
    catch (...)
    {
        m.error = "reference unknown exception";
        return m;
    }
}

Measurement MeasureHostedNamPath(const fs::path& modelPath, bool useCalibration, double calibrationDbu,
                                 double inputAmplitude)
{
    Measurement m;

    try
    {
        guitarfx::Preset preset;
        preset.id = "nam-compare";
        preset.name = "NAM Compare";

        guitarfx::GraphNode inputNode;
        inputNode.id = "__input__";
        inputNode.type = guitarfx::kNodeTypeInput;
        inputNode.enabled = true;

        guitarfx::GraphNode namNode;
        namNode.id = "nam";
        namNode.type = guitarfx::EffectGuids::kAmpNamOptimized;
        namNode.category = "amp";
        namNode.enabled = true;
        namNode.params["mix"] = 1.0;
        namNode.params["inputGain"] = 0.0;
        namNode.params["outputGain"] = 0.0;
        namNode.params["useCalibration"] = useCalibration ? 1.0 : 0.0;
        namNode.params["calibrationInputLevel"] = calibrationDbu;

        guitarfx::ResourceRef modelRef;
        modelRef.resourceType = "nam";
        modelRef.filePath = modelPath;
        namNode.resources.push_back(modelRef);

        guitarfx::GraphNode outputNode;
        outputNode.id = "__output__";
        outputNode.type = guitarfx::kNodeTypeOutput;
        outputNode.enabled = true;

        preset.graph.nodes = {inputNode, namNode, outputNode};
        preset.graph.edges.push_back({"__input__", "nam", 0, 0, 1.0});
        preset.graph.edges.push_back({"nam", "__output__", 0, 0, 1.0});

        guitarfx::MultiPresetMixer mixer;
        mixer.SetHostControlledInput(true); // plugin-host signal path behavior
        if (!mixer.AddActivePreset(preset, preset.id, preset.name))
        {
            m.error = "hosted path: AddActivePreset failed";
            return m;
        }
        // Neutralize the mixer center pan law for direct per-channel level comparison.
        mixer.SetPresetPan(preset.id, -1.0);

        mixer.Prepare(kSampleRate, kBlockSize);

        std::vector<float> inL(static_cast<size_t>(kBlockSize), 0.0f);
        std::vector<float> inR(static_cast<size_t>(kBlockSize), 0.0f);
        std::vector<float> outL(static_cast<size_t>(kBlockSize), 0.0f);
        std::vector<float> outR(static_cast<size_t>(kBlockSize), 0.0f);

        for (int block = 0; block < kWarmupBlocks; ++block)
        {
            for (int i = 0; i < kBlockSize; ++i)
            {
                const double t = static_cast<double>(block * kBlockSize + i) / kSampleRate;
                const float signal = static_cast<float>(inputAmplitude * std::sin(2.0 * kPi * 220.0 * t));
                // Feed dual-mono into the hosted stereo bus to avoid source ambiguity.
                inL[static_cast<size_t>(i)] = signal;
                inR[static_cast<size_t>(i)] = signal;
            }

            float* inputs[2] = {inL.data(), inR.data()};
            float* outputs[2] = {outL.data(), outR.data()};
            mixer.Process(inputs, outputs, kBlockSize);
        }

        m.leftDbfs = RmsDbfs(outL);
        m.rightDbfs = RmsDbfs(outR);
        m.leftSamples = outL;
        m.ok = true;
        return m;
    }
    catch (const std::exception& ex)
    {
        m.error = std::string("hosted path exception: ") + ex.what();
        return m;
    }
    catch (...)
    {
        m.error = "hosted path unknown exception";
        return m;
    }
}

std::size_t ResolveModelLimit(std::size_t discoveredCount)
{
    const char* env = std::getenv("GUITARFX_NAM_COMPARE_MODEL_LIMIT");
    if (!env || !*env)
    {
        return std::min<std::size_t>(discoveredCount, 6);
    }

    try
    {
        const std::size_t limit = static_cast<std::size_t>(std::stoull(env));
        if (limit == 0)
        {
            return discoveredCount;
        }
        return std::min(limit, discoveredCount);
    }
    catch (...)
    {
        return discoveredCount;
    }
}

} // namespace

int main()
{
    using namespace guitarfx;
    std::cout << std::unitbuf; // flush as we go so a crash still leaves the progress trail
    RegisterAllEffects();

    // The hosted path adds a 5 Hz post-model DC blocker that the stock NAM reference
    // path has no equivalent of. Its phase shift alone is ~2.6 degrees at 220 Hz, far
    // more than the sample tolerances below, so disable it to compare the model paths
    // themselves rather than that deliberate difference.
    gEnableNamPostDcBlocker = false;

    const fs::path assetsRoot = fs::path(GUITARFX_TEST_RESOURCES_DIR) / "assets";
    auto models = DiscoverModels(assetsRoot);
    if (models.empty())
    {
        std::cout << "NAM reference comparison: no .nam models found under " << guitarfx::util::PathToUtf8(assetsRoot)
                  << "\n";
        return 0;
    }

    const std::size_t modelLimit = ResolveModelLimit(models.size());
    models.resize(modelLimit);

    const std::vector<double> amplitudes = {0.01, 0.10};
    const std::vector<bool> calibrationModes = {false, true};

    int passed = 0;
    int failed = 0;
    int skipped = 0;

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "============================================================\n";
    std::cout << "NAM Hosted Path vs Reference Comparison\n";
    std::cout << "Models: " << models.size() << "  SampleRate: " << kSampleRate << "  BlockSize: " << kBlockSize
              << "\n";
    std::cout << "Calibration level: " << kCalibrationLevelDbu << " dBu\n";
    std::cout << "Digital tolerances: dBFS=" << kDbfsTolerance << ", maxAbs=" << kSampleMaxAbsTolerance
              << ", rmsDiff=" << kSampleRmsTolerance << "\n";
    std::cout << "============================================================\n\n";

    for (const auto& model : models)
    {
        const ModelMetadata metadata = ReadModelMetadata(model);
        // PathToUtf8 rather than path::string(): model names in the test assets contain
        // characters (e.g. U+221A) with no code-page mapping, and string() throws on those.
        std::cout << "Model: " << guitarfx::util::PathToUtf8(model.lexically_relative(assetsRoot)) << "\n";
        if (!metadata.loadOk)
        {
            std::cout << "  metadata: FAIL (" << metadata.error << ")\n\n";
            ++failed;
            continue;
        }

        std::cout << "  metadata: input=" << (metadata.hasInputLevel ? std::to_string(*metadata.inputLevelDbu) : "n/a")
                  << " dBu, output=" << (metadata.hasOutputLevel ? std::to_string(*metadata.outputLevelDbu) : "n/a")
                  << " dBu, loudness=" << (metadata.hasLoudness ? std::to_string(*metadata.loudness) : "n/a") << "\n";

        for (const double amplitude : amplitudes)
        {
            for (const bool useCalibration : calibrationModes)
            {
                const Measurement ours = MeasureHostedNamPath(model, useCalibration, kCalibrationLevelDbu, amplitude);
                const Measurement reference =
                    MeasureReferencePath(model, useCalibration, kCalibrationLevelDbu, amplitude);

                const std::string modeLabel = useCalibration ? "calibrated" : "non-calibrated";
                if (!ours.ok || !reference.ok)
                {
                    std::cout << "    [" << modeLabel << " amp=" << amplitude << "] "
                              << "SKIP  ours=" << (ours.ok ? "ok" : ours.error)
                              << "  ref=" << (reference.ok ? "ok" : reference.error) << "\n";
                    ++skipped;
                    continue;
                }

                const double deltaLeftDb = ours.leftDbfs - reference.leftDbfs;
                const DiffMetrics diff = ComputeDiffMetrics(ours.leftSamples, reference.leftSamples);
                const bool sameDigitalOutput = std::abs(deltaLeftDb) <= kDbfsTolerance &&
                                               diff.maxAbs <= kSampleMaxAbsTolerance && diff.rms <= kSampleRmsTolerance;

                std::cout << "    [" << modeLabel << " amp=" << amplitude << "] "
                          << "ours/ref dBFS=" << ours.leftDbfs << "/" << reference.leftDbfs << ", delta=" << deltaLeftDb
                          << ", maxAbsDiff=" << diff.maxAbs << ", rmsDiff=" << diff.rms << "  "
                          << (sameDigitalOutput ? "PASS" : "FAIL") << "\n";

                if (sameDigitalOutput)
                {
                    ++passed;
                }
                else
                {
                    ++failed;
                }
            }
        }

        std::cout << "\n";
    }

    std::cout << "============================================================\n";
    std::cout << "NAM hosted/reference comparison results: " << passed << " passed, " << failed << " failed, "
              << skipped << " skipped\n";
    std::cout << "============================================================\n";

    return failed == 0 ? 0 : 1;
}
