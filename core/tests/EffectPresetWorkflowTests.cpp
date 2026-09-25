/**
 * EffectPresetWorkflowTests.cpp - A saved effect preset brings back everything the node was
 * set to, in another chain.
 *
 * `saveEffectPreset` with a nodeId snapshots the engine's own copy of the node: parameters,
 * resources (the NAM model, the IR) and config. `applyEffectPreset` puts all of it back on a
 * node of the same type (controller/PluginControllerEffectPresets.cpp). These run both against
 * a headless controller, loading the preset into a chain other than the one it was saved from.
 */

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "IPluginHost.h"
#include "PluginController.h"
#include "presets/PresetStorage.h"
#include "util/PathEncoding.h"

namespace fs = std::filesystem;
using guitarfx::GraphEdge;
using guitarfx::GraphNode;
using guitarfx::Preset;
namespace EffectGuids = guitarfx::EffectGuids;

namespace
{
class TestHost final : public guitarfx::IPluginHost
{
  public:
    explicit TestHost(fs::path root) : mRoot(std::move(root))
    {
    }

    void SendMessageToUI(const std::string& json) override
    {
        sent.push_back(json);
    }

    void BrowseFileAsync(guitarfx::BrowseFileType, const std::string&,
                         std::function<void(const guitarfx::BrowseFileResult&)> callback) override
    {
        callback(guitarfx::BrowseFileResult{});
    }

    void SaveFileAsync(guitarfx::BrowseFileType, const std::string&, const std::string&,
                       std::function<void(const guitarfx::BrowseFileResult&)> callback) override
    {
        callback(guitarfx::BrowseFileResult{});
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
        return 48000.0;
    }

    [[nodiscard]] int GetBlockSize() const override
    {
        return 256;
    }

    [[nodiscard]] bool IsStandalone() const override
    {
        return true;
    }

    std::vector<std::string> sent;

  private:
    fs::path mRoot;
};

int gFailures = 0;

void Expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << what << std::endl;
        ++gFailures;
    }
}

std::optional<nlohmann::json> Latest(const TestHost& host, const std::string& type)
{
    for (auto it = host.sent.rbegin(); it != host.sent.rend(); ++it)
    {
        auto parsed = nlohmann::json::parse(*it, nullptr, false);

        if (parsed.is_object() && parsed.value("type", std::string{}) == type)
        {
            return parsed;
        }
    }

    return std::nullopt;
}

void Send(guitarfx::PluginController& controller, const std::string& type, nlohmann::json payload = {})
{
    if (!payload.is_object())
    {
        payload = nlohmann::json::object();
    }

    payload["type"] = type;
    controller.HandleUIMessage(payload.dump());
}

/// A fresh profile per test, keyed on when this run started so concurrent runs never share one.
fs::path Sandbox(const std::string& name)
{
    static const auto run = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 100000000);
    const auto root = fs::temp_directory_path() / ("guitarfx-effect-preset-tests-" + run) / name;
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
#ifdef _WIN32
    _putenv_s("APPDATA", root.string().c_str());
#else
    setenv("HOME", root.string().c_str(), 1);
#endif
    return root;
}

fs::path ModelPath()
{
    return fs::path(GUITARFX_TEST_RESOURCES_DIR) / "assets" / "amps" / "Guitar" / "A2" /
           "Boosted-6505+-A2-Chugs-FullRig.nam";
}

fs::path IrPath()
{
    return fs::path(GUITARFX_TEST_RESOURCES_DIR) / "assets" / "ir" / "421 1960.wav";
}

GraphNode Node(const std::string& id, const std::string& type, std::map<std::string, double> params)
{
    GraphNode node;
    node.id = id;
    node.type = type;
    node.params = std::move(params);
    return node;
}

guitarfx::ResourceRef FileRef(const std::string& resourceType, const fs::path& path)
{
    guitarfx::ResourceRef ref;
    ref.resourceType = resourceType;
    ref.filePath = path;
    return ref;
}

/// input -> nodes... -> output, in order.
Preset Chain(const std::string& id, std::vector<GraphNode> nodes)
{
    Preset preset;
    preset.id = id;
    preset.name = id;
    preset.category = "Test";
    preset.version = 2;

    GraphNode input;
    input.id = "__input__";
    input.type = guitarfx::kNodeTypeInput;
    GraphNode output;
    output.id = "__output__";
    output.type = guitarfx::kNodeTypeOutput;

    nodes.insert(nodes.begin(), input);
    nodes.push_back(output);

    for (std::size_t i = 0; i + 1 < nodes.size(); ++i)
    {
        preset.graph.edges.push_back(GraphEdge{nodes[i].id, nodes[i + 1].id, 0, 0, 1.0});
    }

    preset.graph.nodes = std::move(nodes);
    return preset;
}

void Load(guitarfx::PluginController& controller, const Preset& preset)
{
    const auto body = nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(preset));
    Send(controller, "loadPreset", {{"preset", body}, {"presetId", preset.id}});
}

const GraphNode* LiveNode(const guitarfx::PluginController& controller, const std::string& nodeId)
{
    const auto& preset = controller.GetActivePreset();
    return preset ? preset->graph.FindNode(nodeId) : nullptr;
}

/// The saved entry named `name` for `effectType`, from the latest broadcast.
std::optional<nlohmann::json> Saved(const TestHost& host, const std::string& effectType, const std::string& name)
{
    const auto message = Latest(host, "effectPresets");

    if (!message || !message->contains("byEffectType") || !(*message)["byEffectType"].contains(effectType))
    {
        return std::nullopt;
    }

    for (const auto& entry : (*message)["byEffectType"][effectType])
    {
        if (entry.value("name", std::string{}) == name)
        {
            return entry;
        }
    }

    return std::nullopt;
}

bool HasFile(const GraphNode& node, const fs::path& path)
{
    return !node.resources.empty() && fs::path(node.resources.front().filePath) == path;
}

void TestModelAndIrComeBackInAnotherChain()
{
    TestHost host(Sandbox("model-and-ir"));
    guitarfx::PluginController controller(host);
    controller.Initialize();

    auto amp = Node("amp_1", EffectGuids::kAmpNamOptimized,
                    {{"inputGain", 3.0}, {"outputGain", -2.0}, {"useCalibration", 0.0}});
    amp.resources = {FileRef("nam", ModelPath())};
    auto cab = Node("cab_1", EffectGuids::kCabIr, {{"mix", 0.8}, {"outputGain", -1.5}});
    cab.resources = {FileRef("ir", IrPath())};
    Load(controller, Chain("p-source", {amp, cab}));

    Send(controller, "saveEffectPreset",
         {{"effectType", EffectGuids::kAmpNamOptimized},
          {"nodeId", "amp_1"},
          {"name", "Chugs"},
          {"parameters", {{"inputGain", 99.0}}}});
    Send(controller, "saveEffectPreset", {{"effectType", EffectGuids::kCabIr}, {"nodeId", "cab_1"}, {"name", "421"}});

    const auto savedAmp = Saved(host, EffectGuids::kAmpNamOptimized, "Chugs");
    const auto savedCab = Saved(host, EffectGuids::kCabIr, "421");
    Expect(savedAmp.has_value() && savedCab.has_value(), "both presets are saved and broadcast");

    if (!savedAmp || !savedCab)
    {
        return;
    }

    Expect(savedAmp->value("parameters", nlohmann::json::object()).value("inputGain", 0.0) == 3.0,
           "a save with a nodeId takes the parameters from the engine's node, not the payload");
    Expect(savedAmp->contains("resources") && (*savedAmp)["resources"].size() == 1,
           "the model is saved with the preset");
    Expect(!savedAmp->value("parameters", nlohmann::json::object()).contains("calibrationInputLevel"),
           "the injected calibration level is not saved");

    // Another chain: the same effects with no model or IR chosen, and other settings.
    Load(controller, Chain("p-target", {Node("amp_x", EffectGuids::kAmpNamOptimized, {{"inputGain", 0.0}}),
                                        Node("cab_x", EffectGuids::kCabIr, {{"mix", 1.0}})}));

    Send(controller, "applyEffectPreset",
         {{"nodeId", "amp_x"}, {"effectType", EffectGuids::kAmpNamOptimized}, {"presetId", savedAmp->value("id", "")}});
    Send(controller, "applyEffectPreset",
         {{"nodeId", "cab_x"}, {"effectType", EffectGuids::kCabIr}, {"presetId", savedCab->value("id", "")}});

    const auto* ampNode = LiveNode(controller, "amp_x");
    const auto* cabNode = LiveNode(controller, "cab_x");
    Expect(ampNode && HasFile(*ampNode, ModelPath()), "the model is recalled into the other chain");
    Expect(ampNode && ampNode->params.at("inputGain") == 3.0 && ampNode->params.at("outputGain") == -2.0,
           "the amp's parameters are recalled with it");
    Expect(ampNode && ampNode->params.at("useCalibration") == 0.0, "a calibration toggle that was off stays off");
    Expect(cabNode && HasFile(*cabNode, IrPath()), "the IR is recalled into the other chain");
    Expect(cabNode && cabNode->params.at("mix") == 0.8 && cabNode->params.at("outputGain") == -1.5,
           "the cab's parameters are recalled with it");
    Expect(controller.GetActivePreset()->id == "p-target", "the preset being edited is still the target");
}

void TestOlderEntriesLeaveTheModelAlone()
{
    TestHost host(Sandbox("parameters-only"));
    guitarfx::PluginController controller(host);
    controller.Initialize();

    // No nodeId: what an older client sends, and all an older entry holds.
    Send(controller, "saveEffectPreset",
         {{"effectType", EffectGuids::kAmpNamOptimized}, {"name", "Hot"}, {"parameters", {{"inputGain", 5.0}}}});
    const auto saved = Saved(host, EffectGuids::kAmpNamOptimized, "Hot");
    Expect(saved && !saved->contains("resources") && !saved->contains("config"),
           "a save without a node stores parameters alone");

    auto amp = Node("amp_1", EffectGuids::kAmpNamOptimized, {{"inputGain", 0.0}});
    amp.resources = {FileRef("nam", ModelPath())};
    Load(controller, Chain("p-model", {amp}));

    Send(controller, "applyEffectPreset",
         {{"nodeId", "amp_1"},
          {"effectType", EffectGuids::kAmpNamOptimized},
          {"presetId", saved ? saved->value("id", "") : ""}});

    const auto* node = LiveNode(controller, "amp_1");
    Expect(node && node->params.at("inputGain") == 5.0, "the parameters are applied");
    Expect(node && HasFile(*node, ModelPath()), "the node keeps its own model");
}

void TestConfigIsSavedWithoutTransientKeysAndScrubbedForTheUi()
{
    TestHost host(Sandbox("config"));
    guitarfx::PluginController controller(host);
    controller.Initialize();

    auto source = Node("gain_1", EffectGuids::kGain, {{"gain", 4.0}, {"retiredKey", 1.0}});
    source.config = {{"pluginStateBase64", "c3RhdGU="}, {"showPluginEditor", "1"}, {"someSetting", "on"}};
    Load(controller, Chain("p-config", {source}));

    Send(controller, "saveEffectPreset", {{"effectType", EffectGuids::kGain}, {"nodeId", "gain_1"}, {"name", "Boost"}});
    const auto saved = Saved(host, EffectGuids::kGain, "Boost");
    Expect(saved.has_value(), "the preset is saved");

    if (!saved)
    {
        return;
    }

    const auto config = saved->value("config", nlohmann::json::object());
    Expect(!config.contains("showPluginEditor"), "a transient editor request is not saved");
    Expect(!config.contains("pluginStateBase64") && config.value("pluginStateBase64Length", "") == "8",
           "plugin state never reaches the UI, only its length");

    auto target = Node("gain_x", EffectGuids::kGain, {{"gain", 0.0}});
    target.config = {{"showPluginEditor", "0"}};
    Load(controller, Chain("p-config-target", {target}));

    Send(controller, "applyEffectPreset",
         {{"nodeId", "gain_x"}, {"effectType", EffectGuids::kGain}, {"presetId", saved->value("id", "")}});

    const auto* node = LiveNode(controller, "gain_x");
    Expect(node && node->params.at("gain") == 4.0, "the gain is applied");
    Expect(node && !node->params.contains("retiredKey"), "a key the effect does not declare is left out");
    Expect(node && node->config.contains("someSetting") && node->config.at("someSetting") == "on",
           "the node's config is restored");
    Expect(node && node->config.contains("pluginStateBase64") && node->config.at("pluginStateBase64") == "c3RhdGU=",
           "the stored state is restored in full, not the UI's scrubbed copy");
    Expect(node && node->config.contains("showPluginEditor") && node->config.at("showPluginEditor") == "0",
           "the target keeps its own transient keys");
}

void TestAPresetOnlyLoadsIntoItsOwnEffect()
{
    TestHost host(Sandbox("wrong-type"));
    guitarfx::PluginController controller(host);
    controller.Initialize();

    auto amp = Node("amp_1", EffectGuids::kAmpNamOptimized, {{"inputGain", 2.0}});
    amp.resources = {FileRef("nam", ModelPath())};
    Load(controller, Chain("p-types", {amp, Node("fx_1", EffectGuids::kFxNam, {{"inputGain", 0.0}})}));

    Send(controller, "saveEffectPreset",
         {{"effectType", EffectGuids::kAmpNamOptimized}, {"nodeId", "amp_1"}, {"name", "Amp"}});
    const auto saved = Saved(host, EffectGuids::kAmpNamOptimized, "Amp");
    Send(controller, "applyEffectPreset",
         {{"nodeId", "fx_1"},
          {"effectType", EffectGuids::kAmpNamOptimized},
          {"presetId", saved ? saved->value("id", "") : ""}});

    const auto* fx = LiveNode(controller, "fx_1");
    Expect(fx && fx->resources.empty() && fx->params.at("inputGain") == 0.0,
           "a Neural Amp preset is not applied to a Neural FX node");
}
} // namespace

int main()
{
    TestModelAndIrComeBackInAnotherChain();
    TestOlderEntriesLeaveTheModelAlone();
    TestConfigIsSavedWithoutTransientKeysAndScrubbedForTheUi();
    TestAPresetOnlyLoadsIntoItsOwnEffect();

    if (gFailures > 0)
    {
        std::cerr << gFailures << " check(s) failed" << std::endl;
        return 1;
    }

    std::cout << "EffectPresetWorkflowTests passed" << std::endl;
    return 0;
}
