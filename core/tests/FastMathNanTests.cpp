/**
 * @file FastMathNanTests.cpp
 * @brief NaN and infinity handling that has to hold in the fast floating-point builds.
 *
 * Release and RelWithDebInfo compile the core with /fp:fast (MSVC) or -ffast-math (clang,
 * which is what the Android build uses); Debug has neither. -ffast-math lets the optimiser
 * assume no value is ever NaN or infinite, so std::isnan and std::isfinite fold to constants;
 * /fp:fast keeps those but lets a comparison with a NaN come out equal. Code that leaned on
 * either worked in Debug and quietly did nothing in the app -- the Input/Output node gains
 * were the first casualty (BoundaryNodeGainTests).
 *
 * So these cases pass in a Debug build whatever the code does. Run them from a Release build:
 * MSVC's catches NaN comparisons, and clang's -- core/build-clangcl, see
 * docs/agent-quickstart.md -- has the Android build's floating-point semantics, which are the
 * stricter of the two.
 */

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "IPluginHost.h"
#include "PluginController.h"
#include "controller/TelemetryPublisher.h"
#include "controller/internal/ControllerUtils.h"
#include "controller/internal/SettingsKeys.h"
#include "dsp/FiniteCheck.h"
#include "dsp/LevelTargets.h"
#include "dsp/MultiPresetMixer.h"
#include "dsp/RealtimeConvolver.h"
#include "dsp/SignalGraphExecutor.h"
#include "dsp/effects/BuiltinEffects.h"
#include "dsp/effects/FlangerEffect.h"
#include "dsp/effects/GraphicEQEffect.h"
#include "dsp/effects/NoiseGateEffect.h"
#include "dsp/effects/ParametricEQEffect.h"
#include "presets/PresetStorage.h"
#include "presets/PresetTypes.h"
#include "resources/ResourceLibrary.h"

namespace fs = std::filesystem;
using namespace guitarfx;
using namespace guitarfx::controller_detail;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 256;
constexpr double kToleranceDb = 0.25;
constexpr const char* kPresetId = "fast-math-nan-preset";

// Long enough to outlast a preset swap's crossfade; a step only has to let new gains land.
constexpr int kSettleBlocks = 1000;
constexpr int kStepBlocks = 40;
constexpr int kMeasureBlocks = 20;

// The NAM model saturates at higher drive, which squashes the calibration trims these measure.
constexpr float kNamDrive = 0.01f;

using ProcessFn = std::function<bool(float**, float**, int)>;

/// Carries input_level_dbu=18.995 and output_level_dbu=13.195, so calibration at 12 dBu trims
/// -6.995 dB before the model and +1.195 dB after it.
fs::path CalibratedNamModelPath()
{
    return fs::path(GUITARFX_TEST_RESOURCES_DIR) / "assets" / "amps" / "Guitar" / "A2" /
           "[AMP] MESA.MKVII-90W-CH1-CLN Factory Bright Clean - BLEND #1.nam";
}

/// This file is built with the same floating-point flags as the code it tests, so its own
/// verdicts have to survive them. This is the same volatile read FiniteCheck.h falls back on,
/// written out again so the tests do not take the code under test as their own oracle.
bool OracleIsFinite(float value)
{
    volatile std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    return (bits & 0x7fffffffu) < 0x7f800000u;
}

bool OracleIsFinite(double value)
{
    volatile std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
    return (bits & 0x7fffffffffffffffull) < 0x7ff0000000000000ull;
}

/// The non-finite inputs come from strtod rather than std::numeric_limits for the same reason.
/// Under -ffast-math a NaN or infinity *constant* is itself something the optimiser may assume
/// away, and clang does: written as constants, they never reached the code under test.
double RuntimeDouble(const char* text)
{
    return std::strtod(text, nullptr);
}

float RuntimeFloat(const char* text)
{
    return std::strtof(text, nullptr);
}

std::string FormatNumber(double value)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << value;
    return out.str();
}

bool Report(const std::string& what, bool passed, const std::string& detail = {})
{
    std::cout << (passed ? "[PASS] " : "[FAIL] ") << what;

    if (!detail.empty())
    {
        std::cout << " (" << detail << ")";
    }

    std::cout << "\n";
    return passed;
}

double LevelDb(double measured, double reference)
{
    return 20.0 * std::log10(std::max(measured / reference, 1.0e-12));
}

/// Both levels go through the oracle before any arithmetic: a NaN could otherwise pass the
/// tolerance comparison under the same flags that produced it.
bool LevelsAreUsable(double measured, double reference)
{
    return OracleIsFinite(measured) && OracleIsFinite(reference) && reference > 1.0e-9;
}

bool ExpectLevel(double measured, double reference, double expectedDb, const std::string& what)
{
    if (!LevelsAreUsable(measured, reference))
    {
        return Report(what, false, "a level is not a finite number");
    }

    const double measuredDb = LevelDb(measured, reference);
    return Report(what, std::abs(measuredDb - expectedDb) <= kToleranceDb,
                  "expected " + FormatNumber(expectedDb) + " dB, measured " + FormatNumber(measuredDb) + " dB");
}

bool ExpectLevelMoved(double measured, double reference, double minimumDb, const std::string& what)
{
    if (!LevelsAreUsable(measured, reference))
    {
        return Report(what, false, "a level is not a finite number");
    }

    const double measuredDb = LevelDb(measured, reference);
    return Report(what, std::abs(measuredDb) >= minimumDb,
                  "at least " + FormatNumber(minimumDb) + " dB, measured " + FormatNumber(measuredDb) + " dB");
}

class TestHost final : public IPluginHost
{
  public:
    explicit TestHost(fs::path userDataPath) : mUserDataPath(std::move(userDataPath))
    {
    }

    void SendMessageToUI(const std::string&) override
    {
    }

    void BrowseFileAsync(BrowseFileType, const std::string&,
                         std::function<void(const BrowseFileResult&)> callback) override
    {
        callback(BrowseFileResult{});
    }

    void SaveFileAsync(BrowseFileType, const std::string&, const std::string&,
                       std::function<void(const BrowseFileResult&)> callback) override
    {
        callback(BrowseFileResult{});
    }

    void RunOnMainThread(std::function<void()> fn) override
    {
        fn();
    }

    [[nodiscard]] fs::path GetUserDataPath() const override
    {
        return mUserDataPath;
    }

    [[nodiscard]] fs::path GetBundledAssetsPath() const override
    {
        return mUserDataPath;
    }

    [[nodiscard]] double GetSampleRate() const override
    {
        return kSampleRate;
    }

    [[nodiscard]] int GetBlockSize() const override
    {
        return kBlock;
    }

  private:
    fs::path mUserDataPath;
};

void SetSettingsEnvRoot(const fs::path& root)
{
#ifdef _WIN32
    _putenv_s("APPDATA", root.string().c_str());
#else
    setenv("HOME", root.string().c_str(), 1);
#endif
}

/// Plays a sine through `process` and reports the left output's RMS over the last
/// `measureBlocks` blocks. The phase carries on between runs, so steps join up.
class SineRig
{
  public:
    SineRig(ProcessFn process, float amplitude) : mProcess(std::move(process)), mAmplitude(amplitude)
    {
    }

    double Run(int blocks, int measureBlocks)
    {
        constexpr double kTwoPi = 6.283185307179586;
        const double phaseStep = kTwoPi * 440.0 / kSampleRate;
        double sum = 0.0;
        int count = 0;

        for (int block = 0; block < blocks; ++block)
        {
            for (int i = 0; i < kBlock; ++i)
            {
                const auto sample = static_cast<float>(mAmplitude * std::sin(mPhase));
                mInL[static_cast<std::size_t>(i)] = sample;
                mInR[static_cast<std::size_t>(i)] = sample;
                mPhase = std::fmod(mPhase + phaseStep, kTwoPi);
            }

            std::fill(mOutL.begin(), mOutL.end(), 0.0f);
            std::fill(mOutR.begin(), mOutR.end(), 0.0f);
            float* inputs[] = {mInL.data(), mInR.data()};
            float* outputs[] = {mOutL.data(), mOutR.data()};

            if (!mProcess(inputs, outputs, kBlock) || block < blocks - measureBlocks)
            {
                continue;
            }

            for (const float sample : mOutL)
            {
                sum += static_cast<double>(sample) * sample;
                ++count;
            }
        }

        return count > 0 ? std::sqrt(sum / count) : 0.0;
    }

  private:
    ProcessFn mProcess;
    float mAmplitude;
    double mPhase = 0.0;
    std::vector<float> mInL = std::vector<float>(kBlock);
    std::vector<float> mInR = std::vector<float>(kBlock);
    std::vector<float> mOutL = std::vector<float>(kBlock);
    std::vector<float> mOutR = std::vector<float>(kBlock);
};

/// FiniteCheck.h's IsFinite across every class of value, built with this file's floating-point
/// flags -- which, in a clang build, fold every other way of asking.
bool TestIsFiniteClassifiesEveryKindOfValue()
{
    std::cout << "\nIsFinite\n";
    bool passed = true;

    const auto expect = [&passed](const char* text, bool finite) {
        const bool agrees = IsFinite(RuntimeDouble(text)) == finite && IsFinite(RuntimeFloat(text)) == finite;
        passed = Report(std::string(text) + (finite ? " is finite" : " is not finite"), agrees) && passed;
    };

    for (const char* text : {"0", "-0", "1e-40", "-3e38", "0.25"})
    {
        expect(text, true);
    }

    for (const char* text : {"nan", "-nan", "inf", "-inf"})
    {
        expect(text, false);
    }

    passed = Report("the largest double is finite", IsFinite(RuntimeDouble("1.7976931348623157e308"))) && passed;
    passed = Report("a float that overflowed to infinity is not", !IsFinite(RuntimeFloat("1e39"))) && passed;
    return passed;
}

/// A node that did not run is left out of the performance stats, so the UI shows a blank for it
/// rather than a time. "Did not run" used to be a NaN in the node's timing slot, filtered out
/// with std::isfinite -- which -ffast-math folds to true, so the NaN went out as a time.
bool TestNodeThatDidNotRunReportsNoTime()
{
    std::cout << "\nPer-node timings\n";
    RegisterAllEffects();

    SignalGraph graph;
    graph.nodes.push_back({"in", kNodeTypeInput, "", "Input", true});
    graph.nodes.push_back({"active", "gain", "utility", "Active", true});
    graph.nodes.push_back({"bypassed", "gain", "utility", "Bypassed", false});
    graph.nodes.push_back({"out", kNodeTypeOutput, "", "Output", true});
    graph.edges.push_back({"in", "active", 0, 0, 1.0});
    graph.edges.push_back({"active", "bypassed", 0, 0, 1.0});
    graph.edges.push_back({"bypassed", "out", 0, 0, 1.0});

    SignalGraphExecutor executor;
    executor.SetGraph(graph);
    executor.Prepare(kSampleRate, kBlock);
    executor.SetSignalDiagnosticsEnabled(true);

    SineRig rig(
        [&executor](float** inputs, float** outputs, int numSamples) {
            executor.Process(inputs, outputs, numSamples);
            return true;
        },
        0.25f);
    rig.Run(4, 1);

    const auto stats = executor.GetPerformanceStats();
    const auto active = stats.nodeProcessingTimesUs.find("active");
    const auto bypassed = stats.nodeProcessingTimesUs.find("bypassed");
    const bool bypassedAbsent = bypassed == stats.nodeProcessingTimesUs.end();

    bool passed = Report("a node that ran reports a time", active != stats.nodeProcessingTimesUs.end() &&
                                                               OracleIsFinite(active->second) && active->second >= 0.0);
    passed = Report("a bypassed node reports no time", bypassedAbsent,
                    bypassedAbsent ? std::string{} : "reported " + std::to_string(bypassed->second)) &&
             passed;
    return passed;
}

/// A non-finite calibration level means no calibration, so the amp sounds exactly as it does with
/// useCalibration off. The std::isfinite that recognised one folds to true under -ffast-math: the
/// NaN was stored as the level, and the auto gains landed on the +/-24 dB clamp instead of unity.
bool TestNamNonFiniteCalibrationLevelMeansNone()
{
    std::cout << "\nNAM calibration level, at the executor\n";
    const fs::path model = CalibratedNamModelPath();

    if (!fs::exists(model))
    {
        std::cout << "[SKIP] calibrated NAM model not found: " << model.string() << "\n";
        return true;
    }

    RegisterAllEffects();
    ResourceLibrary library;
    LibraryResource resource;
    resource.type = "nam";
    resource.id = "fast-math-calibrated-nam";
    resource.name = "Calibrated NAM";
    resource.filePath = model;
    library.AddResource(resource);

    SignalGraph graph;
    graph.nodes.push_back({"in", kNodeTypeInput, "", "Input", true});
    GraphNode amp{"amp", "amp_nam", "amp", "NAM", true};
    amp.resources = {ResourceRef{"nam", resource.id, {}, ""}};
    amp.params["useCalibration"] = 1.0;
    amp.params["calibrationInputLevel"] = 12.0;
    graph.nodes.push_back(amp);
    graph.nodes.push_back({"out", kNodeTypeOutput, "", "Output", true});
    graph.edges.push_back({"in", "amp", 0, 0, 1.0});
    graph.edges.push_back({"amp", "out", 0, 0, 1.0});

    SignalGraphExecutor executor;
    executor.SetResourceLibrary(&library);
    executor.SetGraph(graph);
    executor.Prepare(kSampleRate, kBlock);

    SineRig rig(
        [&executor](float** inputs, float** outputs, int numSamples) {
            executor.Process(inputs, outputs, numSamples);
            return true;
        },
        kNamDrive);

    const double calibrated = rig.Run(kStepBlocks, kMeasureBlocks);
    executor.SetNodeParam("amp", "useCalibration", 0.0);
    const double uncalibrated = rig.Run(kStepBlocks, kMeasureBlocks);
    executor.SetNodeParam("amp", "useCalibration", 1.0);

    executor.SetNodeParam("amp", "calibrationInputLevel", RuntimeDouble("nan"));
    const double nanLevel = rig.Run(kStepBlocks, kMeasureBlocks);
    executor.SetNodeParam("amp", "calibrationInputLevel", 12.0);
    executor.SetNodeParam("amp", "calibrationInputLevel", RuntimeDouble("inf"));
    const double infiniteLevel = rig.Run(kStepBlocks, kMeasureBlocks);

    bool passed = ExpectLevelMoved(calibrated, uncalibrated, 3.0, "calibration at 12 dBu moves the level (control)");
    passed = ExpectLevel(nanLevel, uncalibrated, 0.0, "a NaN calibration level applies none") && passed;
    passed = ExpectLevel(infiniteLevel, uncalibrated, 0.0, "an infinite calibration level applies none") && passed;
    return passed;
}

Preset BuildNamPreset()
{
    Preset preset;
    preset.id = kPresetId;
    preset.name = "Fast-math NAM calibration";
    preset.version = 2;
    preset.category = "Test";

    GraphNode in;
    in.id = "__input__";
    in.type = kNodeTypeInput;

    GraphNode out;
    out.id = "__output__";
    out.type = kNodeTypeOutput;

    GraphNode amp;
    amp.id = "amp";
    amp.type = "amp_nam";
    amp.category = "amp";
    ResourceRef modelRef;
    modelRef.resourceType = "nam";
    modelRef.filePath = CalibratedNamModelPath();
    amp.resources = {modelRef};
    amp.params["useCalibration"] = 1.0;

    preset.graph.nodes = {in, out, amp};
    preset.graph.edges = {{"__input__", "amp", 0, 0, 1.0}, {"amp", "__output__", 0, 0, 1.0}};
    return preset;
}

void SendToController(PluginController& controller, const nlohmann::json& message)
{
    controller.HandleUIMessage(message.dump());
}

/// Settings' NAM auto input calibration switch, end to end. Off has to leave a calibrated node
/// sounding like one with useCalibration off, and on has to bring the calibration back. Off used
/// to reach the node as a NaN level, which the fast-math builds took for a real one.
bool TestControllerCalibrationSwitch()
{
    std::cout << "\nNAM auto input calibration switch, through the controller\n";

    if (!fs::exists(CalibratedNamModelPath()))
    {
        std::cout << "[SKIP] calibrated NAM model not found\n";
        return true;
    }

    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-fast-math-nan-tests";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    bool passed = true;

    {
        TestHost host(sandbox);
        PluginController controller(host);
        controller.Initialize();
        controller.Prepare(kSampleRate, kBlock);
        SendToController(controller,
                         {{"type", "loadPreset"},
                          {"presetId", kPresetId},
                          {"preset", nlohmann::json::parse(PresetStorage::SerializeToJson(BuildNamPreset()))}});

        const auto setCalibrationSwitch = [&controller](bool on) {
            SendToController(controller,
                             {{"type", "setSetting"}, {"key", kNamAutoInputCalibrationSettingKey}, {"value", on}});
        };
        const auto setUseCalibration = [&controller](bool on) {
            SendToController(controller, {{"type", "updateSignalPathNodeParam"},
                                          {"presetId", kPresetId},
                                          {"nodeId", "amp"},
                                          {"paramKey", "useCalibration"},
                                          {"value", on ? 1.0 : 0.0}});
        };

        SineRig rig([&controller](float** inputs, float** outputs,
                                  int numSamples) { return controller.ProcessAudio(inputs, outputs, numSamples); },
                    kNamDrive);

        const double calibrated = rig.Run(kSettleBlocks, kMeasureBlocks);
        setCalibrationSwitch(false);
        const double switchedOff = rig.Run(kStepBlocks, kMeasureBlocks);
        setUseCalibration(false);
        const double uncalibrated = rig.Run(kStepBlocks, kMeasureBlocks);
        setUseCalibration(true);
        setCalibrationSwitch(true);
        const double switchedOn = rig.Run(kStepBlocks, kMeasureBlocks);

        passed = ExpectLevelMoved(calibrated, uncalibrated, 3.0, "calibration is on by default (control)") && passed;
        passed = ExpectLevel(switchedOff, uncalibrated, 0.0, "switching it off leaves the node uncalibrated") && passed;
        passed = ExpectLevel(switchedOn, calibrated, 0.0, "switching it back on restores the calibration") && passed;
    }

    fs::remove_all(sandbox, ec);
    return passed;
}

/// An analyzer whose input is too quiet to have a loudness -- an interface's idle noise floor with
/// the guitar not being played -- publishes that loudness as null, which the UI shows as a dash.
/// The engine held "no loudness" as -infinity and left the JSON library to write that as null: a
/// std::isfinite check -ffast-math folds away, so it went out as a number instead.
bool TestAnalyzerPublishesNoLoudnessAsNull()
{
    std::cout << "\nAnalyzer loudness on the wire\n";
    RegisterAllEffects();

    TestHost host({});
    MultiPresetMixer mixer;
    std::vector<nlohmann::json> messages;
    TelemetryPublisher publisher(host, mixer, [&messages](const std::string& message) {
        messages.push_back(nlohmann::json::parse(message, nullptr, false));
    });

    Preset preset;
    preset.id = "analyzer";
    preset.name = "analyzer";
    preset.graph.nodes.push_back({"in", kNodeTypeInput, "", "Input", true});
    preset.graph.nodes.push_back({"analyzer", "input_analyzer", "utility", "Analyzer", true});
    preset.graph.nodes.push_back({"out", kNodeTypeOutput, "", "Output", true});
    preset.graph.edges.push_back({"in", "analyzer", 0, 0, 1.0});
    preset.graph.edges.push_back({"analyzer", "out", 0, 0, 1.0});

    mixer.Prepare(kSampleRate, kBlock);
    mixer.SetSignalDiagnosticsEnabled(true);
    const bool added = mixer.AddActivePreset(preset, preset.id, preset.id);

    const ProcessFn process = [&mixer](float** inputs, float** outputs, int numSamples) {
        mixer.Process(inputs, outputs, numSamples);
        return true;
    };

    // The analyzer's "l" array: [.., momentaryLufs, shortTermLufs, integratedLufs, ..]. It is only
    // sent while the analyzer sees a signal at all, which is why the quiet case is not silence.
    const auto publishedLevels = [&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000 / kSignalDiagnosticsRateHz + 10));
        messages.clear();
        publisher.RequestSignalDiagnostics();

        for (const auto& message : messages)
        {
            if (!message.is_discarded() && message.value("type", std::string{}) == "sldA" && message.contains("l") &&
                message["l"].is_array() && message["l"].size() > 7)
            {
                return message["l"];
            }
        }

        return nlohmann::json{};
    };

    // Half a second fills the 400 ms momentary window. -100 dBFS still counts as a signal, but its
    // mean square is below what the analyzer will turn into a loudness.
    const int halfSecondBlocks = static_cast<int>(kSampleRate / 2.0 / kBlock) + 1;
    SineRig(process, 1.0e-5f).Run(halfSecondBlocks, 1);
    const nlohmann::json quiet = publishedLevels();
    SineRig(process, 0.3f).Run(halfSecondBlocks, 1);
    const nlohmann::json tone = publishedLevels();

    if (!Report("the analyzer publishes its levels", added && !quiet.is_null() && !tone.is_null()))
    {
        return false;
    }

    bool passed = Report("too quiet to measure: momentary loudness is null", quiet[5].is_null(), quiet[5].dump());
    passed = Report("too quiet to measure: short-term loudness is null", quiet[6].is_null(), quiet[6].dump()) && passed;
    passed = Report("too quiet to measure: integrated loudness is null", quiet[7].is_null(), quiet[7].dump()) && passed;

    const bool toneMeasured = tone[5].is_number() && OracleIsFinite(tone[5].get<double>()) &&
                              tone[5].get<double>() > -40.0 && tone[5].get<double>() < 0.0;
    passed =
        Report("a tone publishes momentary loudness as a number (control)", toneMeasured, tone[5].dump()) && passed;
    return passed;
}

/// Plays a tone with a NaN and an infinity in it through `effect`, then checks that the output
/// has recovered a few hundred milliseconds later. Each of these is recursive -- a biquad's
/// history, a flanger's feedback -- and kept non-finite values out of that state with
/// std::isfinite, which -ffast-math folds away: one bad sample, and NaN until the node was rebuilt.
bool ExpectRecoversFromNonFiniteInput(const std::string& name, EffectProcessor& effect)
{
    constexpr int kBlocks = 60;
    constexpr int kPoisonedBlock = 2;
    constexpr int kFirstCheckedBlock = 40;
    constexpr double kTwoPi = 6.283185307179586;

    std::vector<float> inL(kBlock);
    std::vector<float> inR(kBlock);
    std::vector<float> outL(kBlock);
    std::vector<float> outR(kBlock);
    double phase = 0.0;
    bool injected = false;
    bool finite = true;
    double sum = 0.0;
    int count = 0;

    for (int block = 0; block < kBlocks; ++block)
    {
        for (int i = 0; i < kBlock; ++i)
        {
            const auto sample = static_cast<float>(0.25 * std::sin(phase));
            inL[static_cast<std::size_t>(i)] = sample;
            inR[static_cast<std::size_t>(i)] = sample;
            phase = std::fmod(phase + kTwoPi * 440.0 / kSampleRate, kTwoPi);
        }

        if (block == kPoisonedBlock)
        {
            inL[17] = inR[17] = RuntimeFloat("nan");
            inL[18] = inR[18] = RuntimeFloat("inf");
            injected = !OracleIsFinite(inL[17]) && !OracleIsFinite(inL[18]);
        }

        float* inputs[] = {inL.data(), inR.data()};
        float* outputs[] = {outL.data(), outR.data()};
        effect.Process(inputs, outputs, kBlock);

        for (std::size_t i = 0; block >= kFirstCheckedBlock && i < outL.size(); ++i)
        {
            if (!OracleIsFinite(outL[i]) || !OracleIsFinite(outR[i]))
            {
                finite = false;
                continue;
            }

            sum += static_cast<double>(outL[i]) * outL[i];
            ++count;
        }
    }

    const double rms = count > 0 ? std::sqrt(sum / count) : 0.0;
    const std::string detail = !injected ? "the non-finite input was not injected"
                               : finite  ? "rms " + FormatNumber(rms)
                                         : "still emitting non-finite samples";
    return Report(name + " recovers from a NaN and an infinity in its input", injected && finite && rms > 0.01, detail);
}

bool TestRecursiveEffectsRecoverFromNonFiniteInput()
{
    std::cout << "\nRecursive effects after a non-finite sample\n";

    ParametricEQEffect parametric;
    parametric.Prepare(kSampleRate, kBlock);
    parametric.SetParam("lowMidGain", 6.0);

    GraphicEQEffect graphic;
    graphic.Prepare(kSampleRate, kBlock);
    graphic.SetParam("band3Gain", 6.0);

    FlangerEffect flanger;
    flanger.Prepare(kSampleRate, kBlock);
    flanger.SetParam("feedback", 0.7);
    flanger.SetParam("mix", 0.5);

    // The gate's detector, gain and sidechain filter are all one-pole state. A NaN in any of
    // them compares false against every threshold, so the gate would sit shut for good rather
    // than merely emit a bad sample -- silence, with nothing in the UI to say why.
    NoiseGateEffect gate;
    gate.Prepare(kSampleRate, kBlock);
    gate.SetParam("threshold", -40.0);

    // The character delays' compander, idle gates and ducking follow the input. A NaN in any
    // detector stays there, and on the analog delay it left the repeats silent for good: the
    // line's own guard caught each NaN by writing zero. That showed in the IEEE build and not
    // under /fp:fast, which happened to compare its way out. Wet only, with a short Time and
    // some Feedback, so the check hears the repeats — at the default Mix a silenced repeat
    // path hides behind the dry signal and passes.
    AnalogDelayEffect analogDelay;
    analogDelay.Prepare(kSampleRate, kBlock);
    analogDelay.SetParam("mix", 1.0);
    analogDelay.SetParam("time", 50.0);
    analogDelay.SetParam("feedback", 0.5);

    TapeDelayEffect tapeDelay;
    tapeDelay.Prepare(kSampleRate, kBlock);
    tapeDelay.SetParam("mix", 1.0);
    tapeDelay.SetParam("time", 50.0);
    tapeDelay.SetParam("feedback", 0.5);

    // The ring modulator's DC blocker and tone filter hold state. Wet only, as above.
    RingModEffect ringMod;
    ringMod.Prepare(kSampleRate, kBlock);
    ringMod.SetParam("mix", 1.0);

    // The drive pedals hold one-poles, half-band allpasses and antialiasing memory, all
    // recursive. Wet only, and a model per family whose stages are biased (Fuzz-Tone) or cascaded.
    OverdriveEffect overdrive;
    overdrive.Prepare(kSampleRate, kBlock);

    DistortionEffect distortion;
    distortion.Prepare(kSampleRate, kBlock);
    distortion.SetParam("model", 3.0);

    FuzzEffect fuzz;
    fuzz.Prepare(kSampleRate, kBlock);
    fuzz.SetParam("model", 3.0);

    // Not recursive, but its splices are chosen by correlating the input history, where a NaN
    // would sit for as long as the search reaches back. Wet only.
    PitchShiftEffect pitchShift;
    pitchShift.Prepare(kSampleRate, kBlock);
    pitchShift.SetParam("engine", 1.0);
    pitchShift.SetParam("semitones", -5.0);
    pitchShift.Reset();

    bool passed = ExpectRecoversFromNonFiniteInput("parametric EQ", parametric);
    passed = ExpectRecoversFromNonFiniteInput("graphic EQ", graphic) && passed;
    passed = ExpectRecoversFromNonFiniteInput("flanger", flanger) && passed;
    passed = ExpectRecoversFromNonFiniteInput("noise gate", gate) && passed;
    passed = ExpectRecoversFromNonFiniteInput("analog delay", analogDelay) && passed;
    passed = ExpectRecoversFromNonFiniteInput("tape delay", tapeDelay) && passed;
    passed = ExpectRecoversFromNonFiniteInput("ring modulator", ringMod) && passed;
    passed = ExpectRecoversFromNonFiniteInput("overdrive", overdrive) && passed;
    passed = ExpectRecoversFromNonFiniteInput("distortion (Metal Zone)", distortion) && passed;
    passed = ExpectRecoversFromNonFiniteInput("fuzz (Fuzz-Tone)", fuzz) && passed;
    passed = ExpectRecoversFromNonFiniteInput("pitch shift (Low Latency)", pitchShift) && passed;
    return passed;
}

/// The FFT convolver zeroes any non-finite sample it would output, so a bad input sample cannot
/// reach the recursive effects after a cab or reverb IR. (Its clamp happens to catch NaN too.)
bool TestConvolverOutputStaysFinite()
{
    std::cout << "\nConvolver output after a non-finite sample\n";

    // Long enough to take the partitioned FFT path rather than direct convolution.
    std::vector<float> impulse(4096);

    for (std::size_t i = 0; i < impulse.size(); ++i)
    {
        const double envelope = std::exp(-static_cast<double>(i) / 400.0);
        impulse[i] = static_cast<float>(envelope * ((i % 7 == 0) ? 0.5 : -0.25));
    }

    impulse[0] = 1.0f;

    RealtimeConvolver convolver;

    if (!Report("the convolver accepts a 4096-sample impulse", convolver.SetImpulse(impulse, kBlock)))
    {
        return false;
    }

    constexpr double kTwoPi = 6.283185307179586;
    std::vector<float> input(kBlock);
    std::vector<float> output(kBlock);
    double phase = 0.0;
    bool injected = false;
    bool finite = true;

    for (int block = 0; block < 80; ++block)
    {
        for (auto& sample : input)
        {
            sample = static_cast<float>(0.25 * std::sin(phase));
            phase = std::fmod(phase + kTwoPi * 440.0 / kSampleRate, kTwoPi);
        }

        if (block == 2)
        {
            input[17] = RuntimeFloat("nan");
            input[18] = RuntimeFloat("inf");
            injected = !OracleIsFinite(input[17]) && !OracleIsFinite(input[18]);
        }

        convolver.Process(input.data(), output.data(), kBlock);

        for (const float sample : output)
        {
            finite = finite && OracleIsFinite(sample);
        }
    }

    return Report("no non-finite sample leaves the convolver", injected && finite,
                  injected ? std::string{} : "the non-finite input was not injected");
}

/// The level-target settings fall back to their defaults for a non-finite value instead of
/// storing it or clamping it to a rail.
bool TestLevelTargetsRejectNonFiniteValues()
{
    std::cout << "\nLevel target settings\n";

    SetNominalOperatingLevelDbfs(RuntimeDouble("nan"));
    const double nominal = GetNominalOperatingLevelDbfs();
    SetOutputProtectionCeilingDbfs(RuntimeDouble("-inf"));
    const double ceiling = GetOutputProtectionCeilingDbfs();

    bool passed =
        Report("a NaN operating level falls back to the default",
               OracleIsFinite(nominal) && nominal == kDefaultNominalOperatingLevelDbfs, FormatNumber(nominal));
    passed = Report("an infinite output ceiling falls back to the default",
                    OracleIsFinite(ceiling) && ceiling == kDefaultOutputProtectionCeilingDbfs, FormatNumber(ceiling)) &&
             passed;

    SetNominalOperatingLevelDbfs(kDefaultNominalOperatingLevelDbfs);
    SetOutputProtectionCeilingDbfs(kDefaultOutputProtectionCeilingDbfs);
    return passed;
}
} // namespace

int main()
{
    std::cout << "Fast floating-point NaN handling\n";

    bool passed = TestIsFiniteClassifiesEveryKindOfValue();
    passed = TestNodeThatDidNotRunReportsNoTime() && passed;
    passed = TestNamNonFiniteCalibrationLevelMeansNone() && passed;
    passed = TestControllerCalibrationSwitch() && passed;
    passed = TestAnalyzerPublishesNoLoudnessAsNull() && passed;
    passed = TestRecursiveEffectsRecoverFromNonFiniteInput() && passed;
    passed = TestConvolverOutputStaysFinite() && passed;
    passed = TestLevelTargetsRejectNonFiniteValues() && passed;

    std::cout << (passed ? "\nAll fast-math NaN handling tests passed\n" : "\nFast-math NaN handling tests FAILED\n");
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
