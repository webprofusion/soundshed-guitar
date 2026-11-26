// The mono-output makeup (audio.dsp.monoOutputUnity, MultiPresetMixer::SetMonoOutputUnity).
//
// A rig at the default centre pan is 3 dB down on each side, the equal-power law. A mono output
// folds the two sides, so those 3 dB used to reach it too; with the makeup on, a centred rig is at
// its input level there, and stereo outputs are as they were. The standalone follows the setting.
// A plugin instance takes it only when it is new and then keeps its own value in the project, so
// the setting never changes the level of a project already saved.

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "IPluginHost.h"
#include "PluginController.h"
#include "dsp/LevelTargets.h"
#include "dsp/MultiPresetMixer.h"
#include "presets/PresetStorage.h"
#include "presets/PresetTypes.h"
#include "resources/ResourceLibrary.h"

namespace fs = std::filesystem;
using namespace guitarfx;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 64;
const float kCentre = static_cast<float>(std::sqrt(0.5));

int gFailures = 0;

void Expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << what << std::endl;
        ++gFailures;
    }
}

bool Near(float a, float b, float tolerance = 1e-4f)
{
    return std::fabs(a - b) < tolerance;
}

Preset EmptyPreset(const std::string& id)
{
    Preset preset;
    preset.id = id;
    preset.name = id;
    GraphNode in;
    in.id = "__input__";
    in.type = kNodeTypeInput;
    GraphNode out;
    out.id = "__output__";
    out.type = kNodeTypeOutput;
    GraphEdge edge;
    edge.from = in.id;
    edge.to = out.id;
    preset.graph.nodes = {in, out};
    preset.graph.edges = {edge};
    return preset;
}

/// A few blocks of a constant left and right; the last output sample of each side. A null right
/// output is what a one-channel bus hands over.
std::pair<float, float> Run(MultiPresetMixer& mixer, float left, float right, bool rightOutput)
{
    std::vector<float> inL(kBlock, left), inR(kBlock, right), outL(kBlock, 0.0f), outR(kBlock, -9.0f);
    float* inputs[2] = {inL.data(), inR.data()};
    float* outputs[2] = {outL.data(), rightOutput ? outR.data() : nullptr};

    for (int i = 0; i < 4; ++i)
    {
        mixer.Process(inputs, outputs, kBlock);
    }

    return {outL.back(), outR.back()};
}

void TestMixer()
{
    MultiPresetMixer mixer;
    ResourceLibrary lib;
    mixer.SetResourceLibrary(&lib);
    mixer.Prepare(kSampleRate, kBlock);
    mixer.AddActivePreset(EmptyPreset("empty"), "empty", "empty");
    Expect(!mixer.IsMonoOutputUnity(), "the makeup is off by default");

    // Stereo output: the pan law stands, on or off.
    auto [l, r] = Run(mixer, 0.2f, 0.6f, true);
    Expect(Near(l, 0.2f * kCentre) && Near(r, 0.6f * kCentre), "stereo output is the centre pan law, makeup off");
    mixer.SetMonoOutputUnity(true);
    std::tie(l, r) = Run(mixer, 0.2f, 0.6f, true);
    Expect(Near(l, 0.2f * kCentre) && Near(r, 0.6f * kCentre), "stereo output is the centre pan law, makeup on");

    // Mono output: the fold of the two sides, 3 dB down without the makeup, at the inputs' own
    // level with it.
    mixer.SetMonoOutputUnity(false);
    mixer.SetAudioChannelCounts(2, 1);
    std::tie(l, r) = Run(mixer, 0.2f, 0.6f, false);
    Expect(Near(l, 0.4f * kCentre), "mono output without the makeup is the fold, 3 dB down");
    mixer.SetMonoOutputUnity(true);
    std::tie(l, r) = Run(mixer, 0.2f, 0.6f, false);
    Expect(Near(l, 0.4f), "mono output with the makeup is the fold at the inputs' level");
    std::tie(l, r) = Run(mixer, 0.5f, 0.5f, false);
    Expect(Near(l, 0.5f), "a mono signal reaches a mono output at its input level");

    // The output meter reads what the one channel gets.
    // A snapshot first: a reading covers every block since the last one, and the stereo runs
    // above were metered too.
    mixer.SetSignalDiagnosticsEnabled(true);
    (void)mixer.GetSignalDiagnosticsSnapshot();
    std::tie(l, r) = Run(mixer, 0.5f, 0.5f, false);
    const auto snapshot = mixer.GetSignalDiagnosticsSnapshot();
    Expect(Near(static_cast<float>(snapshot.output.peak), 0.5f, 1e-3f),
           "the output meter reads the made-up level (" + std::to_string(snapshot.output.peak) + ")");
    mixer.SetSignalDiagnosticsEnabled(false);

    // The limiter holds the ceiling on what the mono output gets, makeup included.
    mixer.SetLimiterEnabled(true);
    const float ceiling = static_cast<float>(DbToLinearGain(GetOutputProtectionCeilingDbfs()));
    std::tie(l, r) = Run(mixer, 0.95f, 0.95f, false);
    Expect(l <= ceiling + 1e-5f, "the limiter's ceiling holds after the makeup (" + std::to_string(l) + ")");
    mixer.SetLimiterEnabled(false);

    // Back to a stereo output: untouched again.
    mixer.SetAudioChannelCounts(2, 2);
    std::tie(l, r) = Run(mixer, 0.2f, 0.6f, true);
    Expect(Near(l, 0.2f * kCentre) && Near(r, 0.6f * kCentre), "a stereo output again ignores the makeup");
}

class TestHost final : public IPluginHost
{
  public:
    TestHost(fs::path root, bool standalone, int outputs = 2)
        : mRoot(std::move(root)), mStandalone(standalone), mOutputs(outputs)
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
        return mRoot;
    }

    [[nodiscard]] fs::path GetBundledAssetsPath() const override
    {
        return mRoot;
    }

    [[nodiscard]] double GetSampleRate() const override
    {
        return kSampleRate;
    }

    [[nodiscard]] int GetBlockSize() const override
    {
        return kBlock;
    }

    [[nodiscard]] bool IsStandalone() const override
    {
        return mStandalone;
    }

    [[nodiscard]] AudioChannelCounts GetAudioChannelCounts() const override
    {
        return {2, mOutputs};
    }

  private:
    fs::path mRoot;
    bool mStandalone;
    int mOutputs;
};

/// A fresh profile per test, keyed on this run so a concurrent run of the same exe never shares one.
fs::path Sandbox(const std::string& name)
{
    static const auto run = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 100000000);
    const auto root = fs::temp_directory_path() / ("guitarfx-mono-output-tests-" + run) / name;
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
#ifdef _WIN32
    _putenv_s("APPDATA", root.string().c_str());
    _putenv_s("LOCALAPPDATA", (root / "local").string().c_str());
#else
    setenv("HOME", root.string().c_str(), 1);
#endif
    return root;
}

void SetSetting(PluginController& controller, bool value)
{
    controller.HandleUIMessage(
        nlohmann::json{{"type", "setSetting"}, {"key", "audio.dsp.monoOutputUnity"}, {"value", value}}.dump());
}

bool SavedValue(const std::string& state, bool& value)
{
    const auto json = nlohmann::json::parse(state, nullptr, false);

    if (!json.is_object() || !json.contains("mixer") || !json["mixer"].contains("monoOutputUnity") ||
        !json["mixer"]["monoOutputUnity"].is_boolean())
    {
        return false;
    }

    value = json["mixer"]["monoOutputUnity"].get<bool>();
    return true;
}

void TestPluginInstances()
{
    // One store, as on a user's machine.
    const auto root = Sandbox("plugin");
    std::string savedOn;

    {
        TestHost host(root, false);
        PluginController controller(host);
        controller.Initialize();
        Expect(!controller.GetMixer().IsMonoOutputUnity(), "a plugin instance starts off by default");

        SetSetting(controller, true);
        Expect(!controller.GetMixer().IsMonoOutputUnity(),
               "turning the setting on leaves the instance it was turned on in as it was");

        bool saved = true;
        Expect(SavedValue(controller.SerializeState(), saved) && !saved, "the project saves the instance's own value");
    }

    {
        TestHost host(root, false);
        PluginController controller(host);
        controller.Initialize();
        Expect(controller.GetMixer().IsMonoOutputUnity(), "an instance added after it is turned on starts on");
        savedOn = controller.SerializeState();
        bool saved = false;
        Expect(SavedValue(savedOn, saved) && saved, "and saves it on");
    }

    {
        // The setting off again: the instance that turns it off keeps its own value.
        TestHost host(root, false);
        PluginController controller(host);
        controller.Initialize();
        SetSetting(controller, false);
        Expect(controller.GetMixer().IsMonoOutputUnity(),
               "the instance keeps its value when the setting is turned off");
    }

    {
        // A new instance now starts off, and a project made with it on wins over that.
        TestHost host(root, false);
        PluginController controller(host);
        controller.Initialize();
        Expect(!controller.GetMixer().IsMonoOutputUnity(), "with the setting off, a new instance is off");
        controller.DeserializeState(savedOn);
        Expect(controller.GetMixer().IsMonoOutputUnity(), "a project saved with it on restores on");
    }

    {
        // A project saved before the setting existed has no value, and was made without it.
        auto legacy = nlohmann::json::parse(savedOn);
        legacy["mixer"].erase("monoOutputUnity");
        TestHost host(root, false);
        PluginController controller(host);
        controller.Initialize();
        SetSetting(controller, true);

        TestHost freshHost(root, false);
        PluginController fresh(freshHost);
        fresh.Initialize();
        Expect(fresh.GetMixer().IsMonoOutputUnity(), "with the setting on, a new instance is on");
        fresh.DeserializeState(legacy.dump());
        Expect(!fresh.GetMixer().IsMonoOutputUnity(), "an older project without the value restores off");
    }
}

void TestStandalone()
{
    const auto root = Sandbox("standalone");
    TestHost host(root, true);
    PluginController controller(host);
    controller.Initialize();
    Expect(!controller.GetMixer().IsMonoOutputUnity(), "the standalone starts off by default");
    SetSetting(controller, true);
    Expect(controller.GetMixer().IsMonoOutputUnity(), "the standalone follows the setting at once");
    SetSetting(controller, false);
    Expect(!controller.GetMixer().IsMonoOutputUnity(), "and back off");
}

void TestMonoOutputPluginEndToEnd()
{
    // A new instance on a mono bus with the setting on: an empty preset is at its input level.
    const auto root = Sandbox("end-to-end");

    {
        TestHost host(root, false);
        PluginController controller(host);
        controller.Initialize();
        SetSetting(controller, true);
    }

    TestHost host(root, false, 1);
    PluginController controller(host);
    controller.Initialize();
    controller.Prepare(kSampleRate, kBlock);
    const auto preset = EmptyPreset("mono-out-empty");
    controller.HandleUIMessage(nlohmann::json{{"type", "loadPreset"},
                                              {"presetId", preset.id},
                                              {"preset", nlohmann::json::parse(PresetStorage::SerializeToJson(preset))}}
                                   .dump());

    std::vector<float> inL(kBlock, 0.25f), inR(kBlock, 0.25f), out(kBlock, 0.0f);
    float* inputs[2] = {inL.data(), inR.data()};
    float* outputs[2] = {out.data(), nullptr};

    // Through the load's fade-in.
    for (int i = 0; i < 200; ++i)
    {
        controller.ProcessAudio(inputs, outputs, kBlock);
        controller.OnIdle();
    }

    Expect(Near(out.back(), 0.25f, 1e-3f),
           "an empty preset reaches a mono bus at its input level (" + std::to_string(out.back()) + ")");
}
} // namespace

int main()
{
    TestMixer();
    TestPluginInstances();
    TestStandalone();
    TestMonoOutputPluginEndToEnd();

    if (gFailures != 0)
    {
        std::cerr << gFailures << " mono output level check(s) failed" << std::endl;
        return 1;
    }

    std::cout << "Mono output level tests passed" << std::endl;
    return 0;
}
