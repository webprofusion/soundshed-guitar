/**
 * @file StartupPresetDeferralTests.cpp
 * @brief A DAW instance builds the preset it starts on only if the project doesn't replace it.
 *
 * Initialize() picks a preset to start on. In a DAW the project's own state usually lands
 * straight after, so building that preset there (models, IRs, hosted plugins) was thrown away,
 * and a plugin scanner paid for it on every instance. Now it is the working copy at once and is
 * built later: by a task posted to the message thread, by Prepare() on the message thread, or at
 * the end of a restore that left nothing playing. The start-up preset here holds a stand-in
 * effect that counts how often it is built.
 *  - Left alone, the instance builds it once its message loop runs, without telling the host
 *    its state changed (it hasn't).
 *  - A project restored straight away replaces it unbuilt.
 *  - A state saved before the build, and an instance destroyed before it, both come out right.
 *    That state holds the global chain's default graphs, and restored and saved again it is the
 *    same state byte for byte, which hosts and clap-validator's state tests compare.
 *  - Prepare() leaves it to the posted task, since REAPER prepares a plugin before restoring a
 *    project into it; only a host that never runs posted tasks gets it built there.
 *  - A restore that brings no preset or slots plays the start-up preset.
 *
 * And a restore builds the preset it brings once. It used to build it as the working copy, then
 * remove that slot and build it again from the same preset for the state's slot list.
 *  - Once, with the slot's controls as the state gives them, or at their defaults where it gives
 *    none, whatever the slot had before.
 *  - Beside another slot, in the state's order.
 *  - A slot the state names differently is still built afresh, under that name.
 */

#include <atomic>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "MessageThreadTestHost.h"
#include "PluginController.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetStorage.h"
#include "storage/JsonStore.h"

namespace fs = std::filesystem;
using namespace guitarfx;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 128;
constexpr const char* kCounterType = "test-startup-counter";
constexpr const char* kStartupId = "startup-preset";

int gFailures = 0;
std::atomic<int> gBuilt{0};

void Check(bool condition, const std::string& what)
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << what << "\n";

    if (!condition)
    {
        ++gFailures;
    }
}

class CountingEffect final : public PassthroughProcessor
{
  public:
    CountingEffect()
    {
        ++gBuilt;
    }
};

class Host final : public test::PumpedTestHost
{
  public:
    explicit Host(const fs::path& root) : PumpedTestHost(root, std::this_thread::get_id(), kSampleRate, kBlock)
    {
    }

    void NotifyStateChanged() override
    {
        ++stateChanges;
    }

    [[nodiscard]] bool RunsPostedTasks() const override
    {
        return postedTasksRun;
    }

    int stateChanges = 0;
    bool postedTasksRun = true;
};

Preset MakePreset(const std::string& id, const std::string& effectType)
{
    Preset preset;
    preset.id = id;
    preset.name = id;
    GraphNode in;
    in.id = "in";
    in.type = kNodeTypeInput;
    GraphNode fx;
    fx.id = "fx";
    fx.type = effectType;
    GraphNode out;
    out.id = "out";
    out.type = kNodeTypeOutput;
    preset.graph.nodes = {in, fx, out};
    preset.graph.edges = {{"in", "fx", 0, 0, 1.0}, {"fx", "out", 0, 0, 1.0}};
    return preset;
}

nlohmann::json State(const std::string& serialized)
{
    return nlohmann::json::parse(serialized, nullptr, false);
}

void TestLeftAlone(const fs::path& root)
{
    Host host(root);
    PluginController controller(host);
    gBuilt = 0;
    controller.Initialize();
    Check(gBuilt == 0 && controller.GetMixer().GetPresetCount() == 0, "left alone: not built during Initialize");

    // Saved before the build, the state still names the preset and the slot it will have.
    const auto saved = State(controller.SerializeState());
    Check(saved.value("presetId", "") == kStartupId, "the start-up preset is the working copy already");
    Check(saved["mixer"]["activePresetIds"] == nlohmann::json::array({kStartupId}),
          "and a state saved now lists the slot it will have");
    const auto chain = saved.value("globalSignalChain", nlohmann::json::object());
    Check(chain.contains("preChainGraph") && !chain["preChainGraph"].value("nodes", nlohmann::json::array()).empty() &&
              chain.contains("postChainGraph") &&
              !chain["postChainGraph"].value("nodes", nlohmann::json::array()).empty(),
          "and holds the global chain's graphs, though nothing is built yet");

    const int changesBefore = host.stateChanges;
    host.Pump();
    Check(gBuilt == 1 && controller.GetMixer().GetPresetCount() == 1, "built once the message loop runs");
    Check(host.stateChanges == changesBefore, "without telling the host its state changed");
    host.Pump();
}

void TestRestoredStraightAway(const fs::path& root, const std::string& otherState)
{
    Host host(root);
    PluginController controller(host);
    gBuilt = 0;
    controller.Initialize();
    controller.DeserializeState(otherState);
    host.Pump();
    Check(gBuilt == 0, "a project restored straight away replaces the start-up preset unbuilt");
    Check(controller.GetMixer().GetPresetCount() == 1 &&
              State(controller.SerializeState()).value("presetId", "") == "other-preset",
          "and plays its own");
}

void TestSavedBeforeBuild(const fs::path& root)
{
    std::string saved;
    Host host(root);
    gBuilt = 0;
    {
        // Destroyed before its message loop runs: its build task finds it gone.
        PluginController early(host);
        early.Initialize();
        saved = early.SerializeState();
    }

    PluginController controller(host);
    controller.Initialize();
    controller.DeserializeState(saved);
    host.Pump();
    const int restoreBuilds = gBuilt;
    Check(controller.GetMixer().GetPresetCount() == 1 && controller.GetMixer().GetPresetConfig(kStartupId).has_value(),
          "a state saved before the build restores into the slot it was saved with");
    Check(controller.SerializeState() == saved, "and saved again it is the same state, byte for byte");

    // The same state restored again costs what the first restore did: neither the early instance
    // nor this one's start-up preset built anything of their own.
    gBuilt = 0;
    controller.DeserializeState(saved);
    host.Pump();
    Check(restoreBuilds == gBuilt, "and builds no more than restoring it again (" + std::to_string(restoreBuilds) +
                                       " against " + std::to_string(gBuilt.load()) + ")");
}

void TestPrepare(const fs::path& root, const std::string& otherState)
{
    {
        // REAPER opening a project: the plugin is prepared, then given the project's state.
        Host host(root);
        PluginController controller(host);
        gBuilt = 0;
        controller.Initialize();
        controller.Prepare(kSampleRate, kBlock);
        controller.DeserializeState(otherState);
        host.Pump();
        Check(gBuilt == 0 && State(controller.SerializeState()).value("presetId", "") == "other-preset",
              "prepared before a project restore, the start-up preset is still never built");
    }
    {
        Host host(root);
        PluginController controller(host);
        gBuilt = 0;
        controller.Initialize();
        controller.Prepare(kSampleRate, kBlock);
        Check(gBuilt == 0, "Prepare leaves the build to the posted task");
        host.Pump();
        Check(gBuilt == 1 && controller.GetMixer().GetPresetCount() == 1, "which builds it once");
    }
    {
        Host host(root);
        host.postedTasksRun = false;
        PluginController controller(host);
        gBuilt = 0;
        controller.Initialize();
        controller.Prepare(kSampleRate, kBlock);
        Check(gBuilt == 1 && controller.GetMixer().GetPresetCount() == 1,
              "a host that never runs posted tasks gets it built in Prepare");
    }
}

void TestEmptyRestorePlaysIt(const fs::path& root)
{
    Host host(root);
    PluginController controller(host);
    gBuilt = 0;
    controller.Initialize();
    controller.DeserializeState(R"({"version":1})");
    Check(gBuilt == 1 && controller.GetMixer().GetPresetCount() == 1,
          "a restore that brings no preset or slots plays the start-up preset");
    host.Pump();
    Check(gBuilt == 1, "built once");
}

void TestRestoreBuildsOnce(const fs::path& root)
{
    Host host(root);
    PluginController controller(host);
    controller.Initialize();
    host.Pump();
    auto saved = State(controller.SerializeState());
    auto& mixer = controller.GetMixer();

    const auto restore = [&](const nlohmann::json& state) {
        gBuilt = 0;
        controller.DeserializeState(state.dump());
        host.Pump();
        return gBuilt.load();
    };

    controller.SetActivePresetMix(kStartupId, 0.3);
    Check(restore(saved) == 1, "a project restore builds its preset once");
    auto config = mixer.GetPresetConfig(kStartupId);
    Check(mixer.GetPresetCount() == 1 && config && config->mix == 1.0 && config->name == kStartupId,
          "with the slot's controls as the state gives them");

    auto noControls = saved;
    noControls["mixer"]["presets"][kStartupId] = {{"name", kStartupId}};
    controller.SetActivePresetMix(kStartupId, 0.3);
    controller.SetActivePresetPan(kStartupId, 0.5);
    controller.SetActivePresetMute(kStartupId, true);
    Check(restore(noControls) == 1, "a state naming no controls builds once too");
    config = mixer.GetPresetConfig(kStartupId);
    Check(config && config->mix == 1.0 && config->pan == 0.0 && !config->mute && !config->solo,
          "and leaves them at a new slot's defaults, not where they were");

    auto withMix = saved;
    withMix["mixer"]["presets"][kStartupId]["mix"] = 0.5;
    withMix["mixer"]["presets"][kStartupId]["solo"] = true;
    (void)restore(withMix);
    config = mixer.GetPresetConfig(kStartupId);
    Check(config && config->mix == 0.5 && config->solo, "a slot's own controls are restored");

    auto twoSlots = saved;
    twoSlots["mixer"]["activePresetIds"] = nlohmann::json::array({kStartupId, "other-preset"});
    twoSlots["mixer"]["presets"]["other-preset"] = {{"name", "other-preset"}};
    twoSlots["mixer"]["presetData"]["other-preset"] =
        State(PresetStorage::SerializeToJson(MakePreset("other-preset", "passthrough")));
    Check(restore(twoSlots) == 1, "beside another slot, it is still built once");
    Check(mixer.GetActivePresetIds() == std::vector<std::string>{kStartupId, "other-preset"},
          "and the slots keep the state's order");

    auto renamed = saved;
    renamed["mixer"]["presets"][kStartupId]["name"] = "Renamed in the project";
    Check(restore(renamed) == 2, "a slot the state names differently is built afresh");
    config = mixer.GetPresetConfig(kStartupId);
    Check(config && config->name == "Renamed in the project", "under the state's name");
}
} // namespace

int main()
{
    RegisterAllEffects();
    EffectTypeInfo info;
    info.type = kCounterType;
    info.category = "utility";
    EffectRegistry::Instance().Register(kCounterType, info, [] { return std::make_unique<CountingEffect>(); });

    const fs::path root = fs::temp_directory_path() / "guitarfx-startup-preset-deferral-tests";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    test::SetSettingsEnvRoot(root);

    std::string otherState;
    {
        // The library's only preset, so Initialize starts on it.
        Host host(root);
        PluginController setup(host);
        setup.Initialize();
        host.Pump();
        storage::JsonStore store;
        std::string error;

        if (!store.Open(root / "Soundshed Guitar" / "data" / "v1" / "soundshed.db", error))
        {
            std::cout << "[FAIL] the test can open the store: " << error << "\n";
            return 1;
        }

        for (const auto& id : store.ListIds(storage::ItemType::kPreset))
        {
            store.Remove(storage::ItemType::kPreset, id);
        }

        PresetStorage::SaveToStore(store, MakePreset(kStartupId, kCounterType));
        store.Close();

        // A project playing another preset.
        auto state = State(setup.SerializeState());
        state["preset"] = State(PresetStorage::SerializeToJson(MakePreset("other-preset", "passthrough")));
        state["presetId"] = "other-preset";
        state["mixer"]["activePresetIds"] = nlohmann::json::array({"other-preset"});
        state["mixer"]["presets"] = {{"other-preset", {{"name", "other-preset"}}}};
        state["mixer"]["presetData"] = nlohmann::json::object();
        otherState = state.dump();
        host.Pump();
    }

    TestLeftAlone(root);
    TestRestoredStraightAway(root, otherState);
    TestSavedBeforeBuild(root);
    TestPrepare(root, otherState);
    TestEmptyRestorePlaysIt(root);
    TestRestoreBuildsOnce(root);

    EffectRegistry::Instance().Unregister(kCounterType);
    fs::remove_all(root, ec);
    std::cout << (gFailures == 0 ? "StartupPresetDeferralTests PASSED\n" : "StartupPresetDeferralTests FAILED\n");
    return gFailures == 0 ? 0 : 1;
}
