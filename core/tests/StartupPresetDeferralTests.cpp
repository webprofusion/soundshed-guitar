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
 *  - Prepare() on the message thread builds it at once.
 *  - A restore that brings no preset or slots plays the start-up preset.
 */

#include <atomic>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

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

    int stateChanges = 0;
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

    // The same state restored again costs what the first restore did: neither the early instance
    // nor this one's start-up preset built anything of their own.
    gBuilt = 0;
    controller.DeserializeState(saved);
    host.Pump();
    Check(restoreBuilds == gBuilt, "and builds no more than restoring it again (" + std::to_string(restoreBuilds) +
                                       " against " + std::to_string(gBuilt.load()) + ")");
}

void TestPrepareBuilds(const fs::path& root)
{
    Host host(root);
    PluginController controller(host);
    gBuilt = 0;
    controller.Initialize();
    controller.Prepare(kSampleRate, kBlock);
    Check(gBuilt == 1 && controller.GetMixer().GetPresetCount() == 1, "Prepare on the message thread builds it");
    host.Pump();
    Check(gBuilt == 1, "and the posted task then has nothing to do");
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
    TestPrepareBuilds(root);
    TestEmptyRestorePlaysIt(root);

    EffectRegistry::Instance().Unregister(kCounterType);
    fs::remove_all(root, ec);
    std::cout << (gFailures == 0 ? "StartupPresetDeferralTests PASSED\n" : "StartupPresetDeferralTests FAILED\n");
    return gFailures == 0 ? 0 : 1;
}
