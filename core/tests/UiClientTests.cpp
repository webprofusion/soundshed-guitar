/**
 * UiClientTests.cpp - Soundshed Nano Q's client library, against a real engine.
 *
 * The client talks to a headless PluginController over the same JSON messages the WebView UI
 * uses, in process (the controller's outbound messages are queued into the client, the
 * client's requests go to HandleUIMessage), so these exercise the protocol end to end with no
 * JUCE involved. The presentation logic the views lean on - chain layout, preset filtering,
 * value formatting, tuner smoothing, tap tempo, the effect presentation table - is checked
 * against the web UI's behaviour it ports.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "IPluginHost.h"
#include "PluginController.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/GraphicEQEffect.h"
#include "presets/PresetStorage.h"
#include "net/Http.h"
#include "uiclient/Looper.h"
#include "uiclient/Riffs.h"
#include "uiclient/Analysis.h"
#include "uiclient/Automation.h"
#include "uiclient/DspStatus.h"
#include "uiclient/Metronome.h"
#include "uiclient/UpdateCheck.h"
#include "uiclient/SignalFlow.h"
#include "uiclient/ChainDropRules.h"
#include "uiclient/ChainHistory.h"
#include "uiclient/ChainLayout.h"
#include "uiclient/EffectPresentation.h"
#include "uiclient/FeatureFlags.h"
#include "uiclient/FxMarks.h"
#include "uiclient/InputLevelling.h"
#include "uiclient/Instruments.h"
#include "uiclient/LayoutMode.h"
#include "uiclient/NodeLabels.h"
#include "uiclient/ParamFormat.h"
#include "uiclient/PresetBrowse.h"
#include "uiclient/PresetTrail.h"
#include "uiclient/ResourceIndex.h"
#include "uiclient/Tone3000.h"
#include "uiclient/ToneSharing.h"
#include "uiclient/UiClient.h"
#include "uiclient/UiClientParsing.h"
#include "uiclient/UiCommands.h"
#include "util/PathEncoding.h"
#include "util/Wav.h"

namespace fs = std::filesystem;
using namespace guitarfx;
using namespace guitarfx::uiclient;

namespace
{
int gFailures = 0;

void Expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << what << std::endl;
        ++gFailures;
    }
}

class LoopbackHost final : public IPluginHost
{
  public:
    explicit LoopbackHost(fs::path root) : mRoot(std::move(root))
    {
    }

    void SendMessageToUI(const std::string& json) override
    {
        if (client != nullptr)
        {
            client->Enqueue(json);
        }
    }

    void BrowseFileAsync(BrowseFileType, const std::string&, std::function<void(const BrowseFileResult&)> cb) override
    {
        cb(BrowseFileResult{});
    }

    void SaveFileAsync(BrowseFileType, const std::string&, const std::string&,
                       std::function<void(const BrowseFileResult&)> cb) override
    {
        cb(BrowseFileResult{});
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

    UiClient* client = nullptr;

  private:
    fs::path mRoot;
};

fs::path Sandbox(const std::string& name)
{
    static const auto run = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 100000000);
    const auto root = fs::temp_directory_path() / ("guitarfx-uiclient-tests-" + run) / name;
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

GraphNode Node(const std::string& id, const std::string& type, bool enabled = true)
{
    GraphNode node;
    node.id = id;
    node.type = type;
    node.enabled = enabled;
    return node;
}

Preset BuildPreset(const std::string& id, const std::string& name, int scenes = 1)
{
    Preset preset;
    preset.id = id;
    preset.name = name;
    preset.category = "Test";
    preset.graph.nodes = {Node("__input__", kNodeTypeInput), Node("gain_1", EffectGuids::kGain),
                          Node("__output__", kNodeTypeOutput)};
    preset.graph.nodes[1].params["gain"] = 2.0;
    preset.graph.edges = {GraphEdge{"__input__", "gain_1", 0, 0, 1.0}, GraphEdge{"gain_1", "__output__", 0, 0, 1.0}};

    for (int i = 0; i < scenes; ++i)
    {
        preset.scenes.push_back({"scene-" + std::to_string(i + 1), "Scene " + std::to_string(i + 1), preset.graph});
    }

    return preset;
}

/// Runs the engine's idle pass and applies whatever it sent, as the editor's tick does.
void Pump(PluginController& controller, UiClient& client)
{
    for (int i = 0; i < 3; ++i)
    {
        controller.OnIdle();
        client.DrainPending();
    }
}

void TestClientMirrorsTheEngine()
{
    LoopbackHost host(Sandbox("mirror"));
    PluginController controller(host);
    controller.Initialize();

    UiClient client([&controller](const std::string& json) { controller.HandleUIMessage(json); });
    host.client = &client;
    UiCommands commands(client);

    for (const auto& preset : {BuildPreset("pa", "Alpha", 2), BuildPreset("pb", "Beta")})
    {
        client.Send("savePreset", {{"saveMode", "overwrite"},
                                   {"presetId", preset.id},
                                   {"name", preset.name},
                                   {"preset", nlohmann::json::parse(PresetStorage::SerializeToJson(preset))}});
    }

    int sessionCalls = 0;
    auto subscription = client.Subscribe(Topic::Session, [&sessionCalls] { ++sessionCalls; });

    client.Start();
    Pump(controller, client);
    const auto& state = client.State();

    Expect(state.haveState, "Start() brings the full state");
    Expect(sessionCalls == 1, "a topic's listeners run once per drain, however many messages touched it");
    Expect(!state.catalog.empty() && state.FindEffectType(EffectGuids::kGain) != nullptr, "the effect catalog arrives");
    Expect(state.FindPresetSummary("pa") && state.FindPresetSummary("pb"), "the preset list arrives");

    commands.LoadPreset("pa");
    Pump(controller, client);
    Expect(state.activePresetId == "pa" && state.activePreset && state.activePreset->name == "Alpha",
           "LoadPreset loads by id and the client mirrors it");

    commands.SelectScene("scene-2");
    Pump(controller, client);
    Expect(state.activeSceneId == "scene-2", "SelectScene switches scene");

    commands.SetNodeParam("gain_1", "gain", 4.5);
    Expect(state.activePreset->graph.FindNode("gain_1")->params.at("gain") == 4.5,
           "a param change is mirrored at once");
    Expect(controller.GetActivePreset()->graph.FindNode("gain_1")->params.at("gain") == 4.5, "and reaches the engine");

    // Stepping follows the engine's list order and wraps, as the web UI's previous/next.
    const auto& list = state.presetList;
    const auto index = [&list](const std::string& id) {
        return static_cast<int>(
            std::find_if(list.begin(), list.end(), [&id](const PresetSummary& p) { return p.id == id; }) -
            list.begin());
    };
    commands.StepPreset(+1);
    Pump(controller, client);
    const auto expectedNext = list[static_cast<std::size_t>((index("pa") + 1) % static_cast<int>(list.size()))].id;
    Expect(state.activePresetId == expectedNext, "StepPreset(+1) loads the next preset in the engine's order");

    subscription.Reset();
    client.Send("requestState");
    Pump(controller, client);
    Expect(sessionCalls == 1, "an unsubscribed listener is not called");
}

/// A factory archive's effect presets can choose its models, IRs and blends, which only the
/// engine holds, so the client lists them and leaves applying them to the engine.
void TestFactoryPackEffectPresetsApplyInTheEngine()
{
    LoopbackHost host(Sandbox("factory-pack-presets"));
    PluginController controller(host);
    controller.Initialize();

    std::vector<std::string> sentTypes;
    UiClient client([&controller, &sentTypes](const std::string& json) {
        sentTypes.push_back(nlohmann::json::parse(json).value("type", ""));
        controller.HandleUIMessage(json);
    });
    host.client = &client;
    UiCommands commands(client);

    const auto preset = BuildPreset("pa", "Alpha");
    client.Send("savePreset", {{"saveMode", "overwrite"},
                               {"presetId", preset.id},
                               {"name", preset.name},
                               {"preset", nlohmann::json::parse(PresetStorage::SerializeToJson(preset))}});
    client.Start();
    Pump(controller, client);
    commands.LoadPreset("pa");
    Pump(controller, client);

    // As the engine sends them when a factory archive ships effect presets.
    const nlohmann::json pack =
        nlohmann::json::array({{{"id", "bundle__boost"}, {"name", "Pack Boost"}, {"parameters", {{"gain", 6.0}}}}});
    client.Enqueue(nlohmann::json{{"type", "effectPresets"},
                                  {"byEffectType", nlohmann::json::object()},
                                  {"factoryByEffectType", {{EffectGuids::kGain, pack}}}}
                       .dump());
    client.DrainPending();

    const auto& listed = client.State().factoryPackEffectPresets;
    const auto entries = listed.find(EffectGuids::kGain);
    Expect(entries != listed.end() && entries->second.size() == 1 && entries->second[0].source == "factoryPack",
           "a factory archive's effect presets are listed apart from the user's");

    if (entries == listed.end() || entries->second.empty())
    {
        return;
    }

    sentTypes.clear();
    commands.ApplyEffectPreset("gain_1", entries->second[0]);
    Expect(sentTypes == std::vector<std::string>{"applyEffectPreset"},
           "one is applied by the engine, not as parameter changes from the client");
}

/// The keys a registry factory preset is applied in: its parameterOrder first, then the rest,
/// as the web UI's applyEffectPresetParams (core/ui/ts/signalPath/effectPresets.ts) sends them.
std::vector<std::string> FactoryPresetApplyOrder(const EffectPresetDefinition& preset)
{
    std::vector<std::string> keys;

    for (const auto& key : preset.parameterOrder)
    {
        if (preset.parameters.count(key) != 0 && std::find(keys.begin(), keys.end(), key) == keys.end())
        {
            keys.push_back(key);
        }
    }

    for (const auto& [key, value] : preset.parameters)
    {
        (void)value;

        if (std::find(keys.begin(), keys.end(), key) == keys.end())
        {
            keys.push_back(key);
        }
    }

    return keys;
}

/// Every Graphic EQ profile, applied from the catalog the way a native UI's presets menu does, leaves
/// the node - the engine's graph, the live DSP and the client's mirror - holding exactly that
/// profile's band count and band values, whichever layout it switched from.
void TestGraphicEqFactoryProfilesApplyExactly()
{
    LoopbackHost host(Sandbox("graphic-eq-profiles"));
    PluginController controller(host);
    controller.Initialize();

    std::vector<std::string> sentParamKeys;
    UiClient client([&controller, &sentParamKeys](const std::string& json) {
        const auto message = nlohmann::json::parse(json);

        if (message.value("type", "") == "updateSignalPathNodeParam")
        {
            sentParamKeys.push_back(message.value("paramKey", ""));
        }

        controller.HandleUIMessage(json);
    });
    host.client = &client;
    UiCommands commands(client);

    // Start from an edited 10-band curve, so a profile has band count and values to undo.
    auto preset = BuildPreset("pa", "Alpha");
    auto eq = Node("eq_1", EffectGuids::kEqGraphic);
    eq.params["bandCount"] = 10.0;

    for (int band = 1; band <= GraphicEQEffect::kMaxBands; ++band)
    {
        const std::string prefix = "band" + std::to_string(band);
        eq.params[prefix + "Enabled"] = 1.0;
        eq.params[prefix + "Gain"] = band % 2 == 0 ? 6.0 : -4.5;
        eq.params[prefix + "Freq"] = 100.0 * band;
        eq.params[prefix + "Q"] = 3.0;
    }

    preset.graph.nodes.insert(preset.graph.nodes.begin() + 2, eq);
    preset.graph.edges = {GraphEdge{"__input__", "gain_1", 0, 0, 1.0}, GraphEdge{"gain_1", "eq_1", 0, 0, 1.0},
                          GraphEdge{"eq_1", "__output__", 0, 0, 1.0}};
    preset.scenes[0].graph = preset.graph;
    client.Send("savePreset", {{"saveMode", "overwrite"},
                               {"presetId", preset.id},
                               {"name", preset.name},
                               {"preset", nlohmann::json::parse(PresetStorage::SerializeToJson(preset))}});
    client.Start();
    Pump(controller, client);
    commands.LoadPreset("pa");
    Pump(controller, client);

    const auto registered = EffectRegistry::Instance().GetTypeInfo(EffectGuids::kEqGraphic);
    const auto* catalogued = client.State().FindEffectType(EffectGuids::kEqGraphic);
    Expect(registered.has_value() && registered->presets.size() == 6, "the Graphic EQ registers six profiles");
    Expect(catalogued != nullptr && catalogued->presets.size() == 6, "and the client's catalog lists all six");

    if (!registered || catalogued == nullptr || !controller.GetActivePreset())
    {
        return;
    }

    // Registry order alternates 5 and 10 bands, so each step changes the layout.
    for (const auto& definition : registered->presets)
    {
        const auto entry = std::find_if(catalogued->presets.begin(), catalogued->presets.end(),
                                        [&definition](const EffectPresetInfo& p) { return p.id == definition.id; });
        Expect(entry != catalogued->presets.end() && entry->source == "factory",
               definition.id + " is in the client's catalog as a factory preset");

        if (entry == catalogued->presets.end())
        {
            continue;
        }

        sentParamKeys.clear();
        commands.ApplyEffectPreset("eq_1", *entry);
        Pump(controller, client);

        const auto* engineNode = controller.GetActivePreset()->graph.FindNode("eq_1");
        const auto* liveEq = controller.GetMixer().GetNodeProcessor("pa", "eq_1");
        const auto* mirrored =
            client.State().activePreset ? client.State().activePreset->graph.FindNode("eq_1") : nullptr;
        Expect(engineNode != nullptr && liveEq != nullptr && mirrored != nullptr,
               definition.id + ": the node is found");

        if (engineNode == nullptr || liveEq == nullptr || mirrored == nullptr)
        {
            continue;
        }

        std::vector<std::string> wrong;

        for (const auto& [key, value] : definition.parameters)
        {
            const auto stored = engineNode->params.find(key);
            const auto shown = mirrored->params.find(key);

            if (stored == engineNode->params.end() || stored->second != value || liveEq->GetParam(key) != value ||
                shown == mirrored->params.end() || shown->second != value)
            {
                wrong.push_back(key);
            }
        }

        Expect(wrong.empty(),
               definition.id + ": the engine, live DSP and client all hold the profile" +
                   (wrong.empty() ? std::string{}
                                  : " (wrong: " + wrong.front() + ", " + std::to_string(wrong.size()) + " keys)"));
        Expect(liveEq->GetParam("bandCount") == definition.parameters.at("bandCount"),
               definition.id + ": the live EQ runs the profile's band count");
        Expect(sentParamKeys == FactoryPresetApplyOrder(definition),
               definition.id + ": keys are sent in the preset's parameterOrder, then the rest");
    }
}

/// A factory preset's parameterOrder is read from the catalog and leads the apply; keys it
/// does not name follow, and names it lists that the preset does not carry are skipped.
void TestFactoryPresetParameterOrder()
{
    const auto parsed = parse::EffectPreset(nlohmann::json::parse(R"({
        "id": "p", "name": "P", "source": "factory",
        "parameters": {"alpha": 1, "bandCount": 10, "band1Gain": 3, "zeta": 2},
        "parameterOrder": ["bandCount", "missing", "zeta", 7, "bandCount"]})"),
                                            "factory");
    Expect(parsed.parameterOrder == std::vector<std::string>{"bandCount", "missing", "zeta", "bandCount"},
           "parameterOrder is read from the catalog, skipping non-strings");
    Expect(parse::EffectPreset(nlohmann::json::parse(R"({"id": "q", "parameters": {"a": 1}})"), "factory")
               .parameterOrder.empty(),
           "a preset without parameterOrder parses with none");

    std::vector<std::string> sentParamKeys;
    UiClient client([&sentParamKeys](const std::string& json) {
        sentParamKeys.push_back(nlohmann::json::parse(json).value("paramKey", ""));
    });
    UiCommands commands(client);

    commands.ApplyEffectPreset("gain_1", parsed);
    Expect(sentParamKeys == std::vector<std::string>{"bandCount", "zeta", "alpha", "band1Gain"},
           "ordered keys go first, once each, then the rest in key order");
}

/// The tone sharing service as the web UI reads it (core/ui/ts/toneSharingPanel/).
// The chain's undo/redo and A/B (ChainHistory), as the web UI's historyModel.ts.
void TestChainHistory()
{
    SignalGraph graph;
    graph.nodes.push_back(GraphNode{"in", kNodeTypeInput});
    GraphNode gate{"gate", "noise_gate"};
    gate.params["threshold"] = -60.0;
    graph.nodes.push_back(gate);
    graph.nodes.push_back(GraphNode{"out", kNodeTypeOutput});
    graph.edges = {GraphEdge{"in", "gate"}, GraphEdge{"gate", "out"}};

    // A turned knob and a bypass are node messages; a node added is a reload.
    auto turned = graph;
    turned.FindNode("gate")->params["threshold"] = -40.0;
    turned.FindNode("gate")->enabled = false;
    const auto diff = DiffNodeStates(graph, turned);
    Expect(diff && diff->size() == 1 && diff->front().params.size() == 1 && diff->front().bypassed == true,
           "history: a knob and a bypass replay as node messages");
    auto grown = graph;
    grown.nodes.insert(grown.nodes.begin() + 2, GraphNode{"comp", "compressor"});
    Expect(!DiffNodeStates(graph, grown), "history: an added node needs the whole chain");
    Expect(DescribeGraphChange(&graph, grown, {}) == "Add compressor", "history: an addition is described");

    // Key order and engine-owned config do not count as edits.
    auto blob = graph;
    blob.FindNode("gate")->config["pluginStateBase64"] = "xyz";
    Expect(GraphSignature(blob) == GraphSignature(graph), "history: a plugin's own state is not an edit");

    ChainHistory history;
    Expect(!history.IsSeeded() && !history.CanUndo(), "history: nothing before it is seeded");
    history.Reset(MakeSnapshot(graph, "p", "", "Loaded"));
    history.Push(MakeSnapshot(graph, "p", "", "same")); // unchanged: no step
    Expect(!history.CanUndo(), "history: an unchanged graph records nothing");
    history.Push(MakeSnapshot(turned, "p", "", "Noise gate off"));
    Expect(history.CanUndo() && history.UndoLabel() == "Noise gate off", "history: an edit is a step");
    const auto* back = history.Undo();
    Expect(back != nullptr && back->signature == GraphSignature(graph) && history.CanRedo(),
           "history: undo returns the state before");

    // A branch from a past position drops the redo; B keeps its own stack.
    history.Push(MakeSnapshot(grown, "p", "", "Add compressor"));
    Expect(!history.CanRedo(), "history: a new edit after undo drops the redo");
    const auto* b = history.SetActiveSlot(ChainHistory::Slot::B);
    Expect(b != nullptr && b->signature == GraphSignature(graph) && !history.CanUndo(), "history: B starts as loaded");
    history.SetActiveSlot(ChainHistory::Slot::A);
    Expect(history.CopyActiveToOther() && history.EditCount(ChainHistory::Slot::B) == 1,
           "history: copying A over B is a step in B");
}

// Tone3000 through Soundshed's relay (Tone3000.h), and the HTTP workers' cancelling (net/Http.h).
void TestTone3000AndHttp()
{
    namespace t3 = tone3000;
    Expect(
        t3::Url(t3::TonesSearchPath("Super Reverb", "full-rig", "newest", "2", 3)) ==
            "https://api-guitar.soundshed.com/v1/resourcesearch/tones/search?page=3&page_size=20&query=Super%20Reverb&gear=full-rig&sort=newest&architecture=2",
        "tone3000: a search path");
    Expect(t3::ModelsPath("87866", 1, "2") == "/models?tone_id=87866&page=1&page_size=100&architecture=2",
           "tone3000: a models path");
    Expect(t3::ThroughRelay("https://www.tone3000.com/api/v1/models/742072/download/x.nam") ==
               "https://api-guitar.soundshed.com/v1/resourcesearch/models/742072/download/x.nam",
           "tone3000: a model download goes through the relay");
    Expect(t3::ThroughRelay("https://cdn.example.com/x.nam") == "https://cdn.example.com/x.nam",
           "tone3000: other URLs stay");

    const auto tones = t3::ParseTones(nlohmann::json::parse(
        R"({"data":[{"id":87866,"title":"Mesa","gear":"amp-cab","models_count":2,"images":["https://img/x.png"],
            "user":{"id":"u1","username":"mad","display_name":"Mad Hatter"}}],"page":2,"page_size":20,"total":41,"total_pages":3})"));
    Expect(tones.items.size() == 1 && tones.items[0].id == "87866" && tones.items[0].creatorName == "Mad Hatter" &&
               tones.items[0].imageUrl == "https://img/x.png" && tones.page == 2 && tones.totalPages == 3,
           "tone3000: tones parse, ids as text");
    const auto models = t3::ParseModels(nlohmann::json::parse(
        R"({"data":[{"id":742072,"name":"Red Modern","model_url":"https://www.tone3000.com/api/v1/models/742072/download/a.nam","tone_id":87866,"architecture_version":"2"}],"total_pages":0})"));
    Expect(models.items.size() == 1 && models.items[0].architecture == "2" && models.totalPages == 1,
           "tone3000: models parse");

    const auto payload = t3::ImportPayload(tones.items[0], models.items[0], "nam", "", "AAAA");
    Expect(payload.value("resourceId", "") == "tone3000:742072" &&
               payload["metadata"].value("modelId", "") == "742072" &&
               payload["metadata"].value("captureScope", "") == "amp+cab" && payload.value("category", "") == "amp-cab",
           "tone3000: an import is named as the web UI names it");

    ClientState state;
    guitarfx::uiclient::LibraryResource imported;
    imported.id = "elsewhere";
    imported.type = "nam";
    imported.metadata = {{"provider", "tone3000"}, {"modelId", "742072"}};
    state.resources["nam"].push_back(imported);
    Expect(t3::FindImported(state, "nam", models.items[0]) != nullptr,
           "tone3000: an import is found by the model id it records");

    // A transport that blocks until cancelled: destroying the workers cuts it short at once.
    struct Blocking final : net::HttpTransport
    {
        std::atomic<bool> started{false};

        net::HttpResponse Perform(const net::HttpRequest&, net::CancelToken& cancel) override
        {
            std::atomic<bool> aborted{false};

            if (!cancel.SetAbort([&aborted] { aborted = true; }))
            {
                return {};
            }

            started = true;

            while (!aborted.load())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            cancel.ClearAbort();
            return {};
        }
    };

    auto transport = std::make_shared<Blocking>();
    std::atomic<bool> cut{false};
    const auto began = std::chrono::steady_clock::now();
    {
        net::HttpWorkers workers(transport, 2);
        workers.Run([&workers, &cut] { cut = workers.Perform(net::HttpRequest{}).cancelled; });

        while (!transport->started.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    Expect(cut.load() && std::chrono::steady_clock::now() - began < std::chrono::seconds(5),
           "http: destroying the workers cancels a request in flight");
}

void TestToneSharing()
{
    using namespace guitarfx::uiclient::tones;

    // The envelope, and a session handed out by sign-in.
    const auto signedIn = ParseResponse(
        200, R"({"ok":true,"data":{"sessionId":"s-1","user":{"id":"u1","email":"a@b.c","displayName":"Ann"}}})");
    Expect(signedIn.ok && signedIn.sessionId == "s-1", "a sign-in answer's session is found in its data");
    Expect(ParseUser(signedIn.data) && ParseUser(signedIn.data)->Label() == "Ann", "the user reads by display name");
    Expect(ParseResponse(200, "{}", "s-2").sessionId == "s-2", "the x-session-id header wins");

    const auto refused =
        ParseResponse(401, R"({"ok":false,"error":{"code":"INVALID_TOKEN","message":"Invalid code"}})");
    Expect(!refused.ok && refused.error == "Invalid code", "an error reads as the service's message");
    Expect(ParseResponse(409, "").error == "Request failed (409)", "a bodiless error reads as its status");
    Expect(!ParseResponse(200, R"({"ok":false})").ok, "ok:false is a failure whatever the status");
    Expect(ParseResponse(0, "").error.find("connection") != std::string::npos, "no answer at all says so");

    // The featured page: presets and packs mixed.
    const auto home = ParseHome(nlohmann::json::parse(R"({"rows":[
        {"id":"r1","title":"Featured","items":[
            {"id":"i1","kind":"item","title":" Crunch ","type":"preset","downloadCount":1250,
             "creatorDisplayName":"Ann","creatorProfileHandle":"ann_b","creatorAvatarUrl":"/v1/auth/avatar/a1",
             "tags":["preset","crunch"]},
            {"id":"p1","kind":"pack","title":"Pack","thumbnailUrl":"/v1/packs/p1/thumbnail"}]},
        {"id":"r2","title":"Empty","items":[]}]})"));
    Expect(home.size() == 1, "an empty row is left out");

    if (!home.empty() && home[0].tones.size() == 2)
    {
        const auto& preset = home[0].tones[0];
        Expect(preset.title == "Crunch" && !preset.IsPack(), "a preset card, title trimmed");
        Expect(preset.downloads == 1250 && FormatCount(preset.downloads) == "1.3K", "downloads, compact");
        Expect(preset.creatorHandle == "@ann_b", "the creator's handle gets its @");
        Expect(preset.creatorAvatarUrl == "https://api-guitar.soundshed.com/v1/auth/avatar/a1",
               "and the avatar a full URL");
        Expect(preset.tags == std::vector<std::string>{"crunch"}, "\"preset\" is not shown as a tag");

        const auto& pack = home[0].tones[1];
        Expect(pack.IsPack() && pack.thumbnailUrl == "https://api-guitar.soundshed.com/v1/packs/p1/thumbnail",
               "a pack card, its thumbnail without a doubled /v1");
    }
    else
    {
        Expect(false, "the featured row has its two cards");
    }

    // A pack list: a thumbnail only where the pack has one.
    const auto packs = ParsePacks(nlohmann::json::parse(
        R"({"packs":[{"id":"p2","title":"With","thumbnailAssetId":"legacy_available"},{"id":"p3","title":"Without","thumbnailAssetId":null}]})"));
    Expect(packs.size() == 2 && !packs[0].thumbnailUrl.empty() && packs[1].thumbnailUrl.empty(),
           "a pack without a picture is not asked for one");

    const auto detail = ParsePackDetail(nlohmann::json::parse(
        R"({"pack":{"id":"p1","title":"Pack"},"items":[{"itemId":"b","title":"Second","sortOrder":2},{"itemId":"a","title":"First","sortOrder":1}]})"));
    Expect(detail && detail->items.size() == 2 && detail->items[0].itemId == "a", "a pack's presets come in its order");

    Expect(ItemsPath(2, " high gain ", "high-gain") == "/items?page=2&pageSize=36&q=high%20gain&tag=high-gain",
           "the preset search path, encoded");
    Expect(ShareLink(packs[0]) == "https://api-guitar.soundshed.com/share/pack/p2", "a pack's share link");
    Expect(NormalizeHandle("@@x") == "", "a handle is at least two characters");
    Expect(FormatCount(999) == "999" && FormatCount(12'400) == "12K" && FormatCount(2'000'000) == "2M",
           "counts read as the web UI writes them");

    // What is installed: the setting both UIs keep.
    nlohmann::json settings = {
        {kInstalledSettingKey,
         {{{"id", "tone-sharing-api:item:i1"},
           {"title", "Crunch"},
           {"presetIds", {"user-1"}},
           {"resources", {{{"type", "nam"}, {"id", "r1"}}}}},
          {{"id", "tone-sharing-api:p1"}, {"packId", "p1"}, {"presetIds", {"user-2", "user-3"}}}}}};
    Expect(InstalledEntries(settings).size() == 2, "installed entries are read from the setting");
    Expect(FindInstalled(settings, EntryIdFor(home.empty() ? Tone{} : home[0].tones[0])).has_value(),
           "a preset is installed under its item entry id");
    Expect(FindInstalled(settings, PackEntryId("p1")) && FindInstalled(settings, PackEntryId("p1"))->IsPack(),
           "a pack under its pack entry id");

    // The install replies.
    UiClient client([](const std::string&) {});
    guitarfx::uiclient::UiCommands commands(client);
    std::vector<std::string> sent;
    UiClient sending([&sent](const std::string& json) { sent.push_back(json); });
    guitarfx::uiclient::UiCommands sendingCommands(sending);
    sendingCommands.InstallPresetArchives({{"id", "tone-sharing-api:item:i1"}, {"title", "Crunch"}}, {},
                                          nlohmann::json::array({{{"title", "Crunch"}, {"data", "UEsFBg=="}}}));
    const auto request = sent.empty() ? nlohmann::json() : nlohmann::json::parse(sent.back());
    Expect(request.value("type", "") == "installPresetArchives" &&
               request.value("requestId", "") == "tone-sharing-api:item:i1",
           "an install is sent with its entry id as the request id");
    Expect(sending.State().toneInstalls.count("tone-sharing-api:item:i1") == 1 &&
               sending.State().toneInstalls.at("tone-sharing-api:item:i1").status ==
                   guitarfx::uiclient::ToneInstall::Status::Installing,
           "and is shown installing until it is answered");

    client.Enqueue(R"({"type":"presetArchivesInstalled","entryId":"e1","presetIds":["user-9"]})");
    client.Enqueue(
        R"({"type":"presetArchivesInstallFailed","entryId":"e2","message":"Install failed","detail":"Needs Tone3000 models"})");
    client.DrainPending();
    const auto& installs = client.State().toneInstalls;
    Expect(installs.count("e1") && installs.at("e1").status == guitarfx::uiclient::ToneInstall::Status::Installed &&
               installs.at("e1").presetIds == std::vector<std::string>{"user-9"},
           "an install that succeeded knows its presets");
    Expect(installs.count("e2") && installs.at("e2").detail == "Needs Tone3000 models", "one that failed knows why");

    client.Enqueue(R"({"type":"installedPresetArchiveDeleted","id":"e1","presetIds":["user-9"]})");
    client.DrainPending();
    Expect(client.State().toneInstalls.count("e1") == 0, "a removed install is forgotten");
    (void)commands;
}

void TestTelemetryDecoding()
{
    UiClient client([](const std::string&) {});
    client.Enqueue(
        R"({"type":"sldRoster","seq":7,"nodes":[["preset","p1","amp","x",0],["post","","global_eq","y",0]]})");
    client.Enqueue(R"({"type":"sld","seq":7,"r":[-12.5,-20,0,0],"i":[-10,-18,0,0],"o":[-0.1,-6,2,1],)"
                   R"("d":[-3,-9,0,0,2,-40,-50,0,0,1]})");
    client.DrainPending();
    const auto& telemetry = client.State().telemetry;
    Expect(telemetry.rawInput.peakDb == -12.5, "raw input level decoded");
    Expect(telemetry.output.clipped && telemetry.output.clipCount == 2, "output clip decoded");
    Expect(telemetry.nodes.count("p1::amp") == 1 && telemetry.nodes.at("p1::amp").channelCount == 2,
           "a preset node is keyed <presetId>::<nodeId>");
    Expect(telemetry.nodes.count("post::global_eq") == 1 && telemetry.nodes.at("post::global_eq").peakDb == -40,
           "a global node is keyed <scope>::<nodeId>");
}

void TestChainLayoutWalksParallelLanes()
{
    SignalGraph graph;
    graph.nodes = {Node("__input__", kNodeTypeInput),     Node("drive", EffectGuids::kOverdrive),
                   Node("split", EffectGuids::kSplitter), Node("a", EffectGuids::kDelayDigital),
                   Node("b1", EffectGuids::kChorus),      Node("b2", EffectGuids::kPhaser),
                   Node("mix", EffectGuids::kMixer),      Node("rev", EffectGuids::kReverbRoom),
                   Node("__output__", kNodeTypeOutput)};
    graph.edges = {{"__input__", "drive", 0, 0, 1.0}, {"drive", "split", 0, 0, 1.0}, {"split", "b1", 1, 0, 1.0},
                   {"split", "a", 0, 0, 1.0},         {"a", "mix", 0, 0, 1.0},       {"b1", "b2", 0, 0, 1.0},
                   {"b2", "mix", 0, 1, 1.0},          {"mix", "rev", 0, 0, 1.0},     {"rev", "__output__", 0, 0, 1.0}};

    const auto layout = BuildChainLayout(graph);
    Expect(layout.items.size() == 4, "drive, the parallel block, its join and the reverb");
    Expect(layout.items[1].kind == ChainItem::Kind::Parallel && layout.items[1].collapsible,
           "a splitter/mixer pair is a collapsible parallel block");
    Expect(layout.items[1].branches.size() == 2 &&
               layout.items[1].branches[0].nodeIds == std::vector<std::string>{"a"} &&
               layout.items[1].branches[1].nodeIds == std::vector<std::string>{"b1", "b2"},
           "branches in port order, each walked to the join");
    Expect(layout.items[2].nodeId == "mix" && layout.items[3].nodeId == "rev",
           "the join is drawn, then the chain goes on");
    Expect(layout.NodeBefore("b2") == "b1" && layout.NodeBefore("a") == "split" &&
               layout.NodeBefore("drive") == kInputNodeId,
           "NodeBefore follows the branch a node is on");
}

void TestChainLayoutWalksNestedSplits()
{
    // drive → split → [ a → inner → [ c | (empty) ] → innerMix → d | b ] → mix → output
    SignalGraph graph;
    graph.nodes = {Node("__input__", kNodeTypeInput),     Node("drive", EffectGuids::kOverdrive),
                   Node("split", EffectGuids::kSplitter), Node("a", EffectGuids::kDelayDigital),
                   Node("inner", EffectGuids::kSplitter), Node("c", EffectGuids::kChorus),
                   Node("innerMix", EffectGuids::kMixer), Node("d", EffectGuids::kPhaser),
                   Node("b", EffectGuids::kReverbRoom),   Node("mix", EffectGuids::kMixer),
                   Node("__output__", kNodeTypeOutput)};
    graph.edges = {{"__input__", "drive", 0, 0, 1.0}, {"drive", "split", 0, 0, 1.0}, {"split", "a", 0, 0, 1.0},
                   {"split", "b", 1, 0, 1.0},         {"a", "inner", 0, 0, 1.0},     {"inner", "c", 0, 0, 1.0},
                   {"inner", "innerMix", 1, 1, 1.0},  {"c", "innerMix", 0, 0, 1.0},  {"innerMix", "d", 0, 0, 1.0},
                   {"d", "mix", 0, 0, 1.0},           {"b", "mix", 0, 1, 1.0},       {"mix", "__output__", 0, 0, 1.0}};

    const auto layout = BuildChainLayout(graph);
    Expect(layout.items.size() == 3 && layout.items[1].joinNodeId == "mix", "the outer split joins at its own mixer");
    const auto& first = layout.items[1].branches[0];
    Expect(first.items.size() == 4 && first.items[0].nodeId == "a" &&
               first.items[1].kind == ChainItem::Kind::Parallel && first.items[1].splitNodeId == "inner" &&
               first.items[1].joinNodeId == "innerMix" && first.items[2].nodeId == "innerMix" &&
               first.items[3].nodeId == "d",
           "the inner split is a parallel block in the branch, and the branch goes on past its mixer");
    Expect(first.items[1].collapsible && first.items[1].branches.size() == 2 &&
               first.items[1].branches[0].nodeIds == std::vector<std::string>{"c"} &&
               first.items[1].branches[1].nodeIds.empty(),
           "the inner split's own branches");
    Expect(first.nodeIds == std::vector<std::string>{"a", "inner", "c", "innerMix", "d"},
           "the branch's nodes, the inner split's included, in drawing order");
    Expect(layout.FindParallel("innerMix") == &first.items[1] && layout.FindParallel("inner") == &first.items[1] &&
               layout.FindParallel("mix") == &layout.items[1] && layout.FindParallel("c") == nullptr,
           "FindParallel finds a block by its split or join at any depth");
    Expect(layout.NodeBefore("c") == "inner" && layout.NodeBefore("d") == "innerMix" &&
               layout.NodeBefore("a") == "split",
           "NodeBefore follows nested branches");
}

void TestNodeArchitectureBadge()
{
    ClientState state;
    const auto model = [](const std::string& id, nlohmann::json metadata) {
        uiclient::LibraryResource resource;
        resource.id = id;
        resource.type = "nam";
        resource.metadata = std::move(metadata);
        return resource;
    };
    state.resources["nam"] = {
        model("slimmable", {{"architecture", "SlimmableContainer"}, {"architectureVersion", "A2"}}),
        model("unwrapped-a2", {{"architecture", "WaveNet"}, {"architectureVersion", "A2"}}),
        model("a1", {{"architecture", "WaveNet"}, {"architectureVersion", "A1"}}),
        model("tone3000", {{"architecture", "WaveNet"}, {"architectureVersion", "2"}}),
        model("unread", {{"architecture", "WaveNet"}}),
    };
    const auto badge = [&](const std::string& id) {
        GraphNode node = Node("amp", EffectGuids::kAmpNam);
        ResourceRef ref;
        ref.resourceType = "nam";
        ref.resourceId = id;
        node.resources = {ref};
        return NodeArchitectureBadge(state, node);
    };

    Expect(badge("slimmable") == "A2", "a SlimmableContainer shows A2");
    Expect(badge("unwrapped-a2") == "A2", "an unwrapped A2 WaveNet shows A2, not its file's WaveNet");
    Expect(badge("a1") == "A1", "an A1 WaveNet shows A1");
    Expect(badge("tone3000") == "A2", "the Tone3000 API's 2 shows A2");
    Expect(badge("unread").empty(), "WaveNet alone says nothing about A1 or A2");
}

void TestPresetBrowse()
{
    ClientState state;
    state.presetList = {{"t10", "Tone 10", "Lead", "user"},
                        {"t2", "tone 2", "Clean", "user"},
                        {"f1", "Factory Crunch", "Crunch", "factory"},
                        {"x", "", "Misc", "user"}};
    state.presetFavorites = {"t10", "x"};
    state.presetRecents = {"f1", "t10"};
    state.presetFolders = {{"live", "Live", {{"sub", "Sub", {}, {"t2"}}}, {"t10"}}};

    const auto ids = [](const std::vector<PresetSummary>& list) {
        std::vector<std::string> out;

        for (const auto& p : list)
        {
            out.push_back(p.id);
        }

        return out;
    };

    Expect(ids(FilterPresets(state, {})) == std::vector<std::string>{"f1", "t2", "t10", "x"},
           "names compare case-insensitively with numbers as numbers; an unnamed preset sorts by id");
    Expect(ids(FilterPresets(state, {kPresetFolderFavorites, ""})) == std::vector<std::string>{"t10", "x"},
           "favourites");
    Expect(ids(FilterPresets(state, {kPresetFolderRecents, ""})) == std::vector<std::string>{"f1", "t10"},
           "recents keep their order");
    Expect(ids(FilterPresets(state, {"live", ""})) == std::vector<std::string>{"t2", "t10"},
           "a folder includes its subfolders");
    Expect(ids(FilterPresets(state, {kPresetFolderAll, "CRUNCH"})) == std::vector<std::string>{"f1"},
           "search matches name or category, ignoring case");
}

void TestParamFormatting()
{
    EffectParamInfo enumParam{"model", "Model", "enum", "", 0, 2, 0, 1, false, false, {"TS", "Rat", "Muff"}};
    EffectParamInfo pan{"pan", "Pan", "pan", "", -1, 1, 0};
    EffectParamInfo freq{"freq", "Freq", "Hz", "", 20, 20000, 1000, 0, false, true};
    EffectParamInfo db{"gain", "Gain", "dB", "", -24, 24, 0};
    EffectParamInfo amount{"mix", "Mix", "amount", "", 0, 1, 0.5};
    EffectParamInfo toggle{"on", "On", "toggle", "", 0, 1, 0};

    Expect(FormatParamValue(enumParam, 1.2) == "Rat", "an enum shows its label");
    // Labels run from the minimum: the arp's Steps (2 to 8) used to show "6" at 4, and its menu
    // committed a label's index, so "2", "3" and "4" all chose 2.
    EffectParamInfo steps{
        "numSteps", "Steps", "enum", "", 2, 8, 4, 1, false, false, {"2", "3", "4", "5", "6", "7", "8"}};
    Expect(FormatParamValue(steps, 4) == "4" && FormatParamValue(steps, 2) == "2" && FormatParamValue(steps, 8) == "8",
           "an enum's labels are counted from its minimum");
    Expect(EnumLabelValue(steps, 0) == 2 && EnumLabelValue(steps, 6) == 8 && EnumLabelIndex(steps, 5) == 3 &&
               EnumLabelIndex(steps, 1) == -1 && EnumLabelIndex(enumParam, 2) == 2,
           "a label stands for the minimum plus its index");
    Expect(FormatParamValue(pan, 0.004) == "C" && FormatParamValue(pan, -0.25) == "L25" &&
               FormatParamValue(pan, 0.5) == "R50",
           "pan shows L/C/R");
    Expect(FormatParamValue(freq, 999.6) == "1.00kHz" && FormatParamValue(freq, 12500) == "12.5kHz" &&
               FormatParamValue(freq, 440) == "440Hz" && FormatParamValue(freq, 55.5) == "55.5Hz",
           "a log-taper value shows about three figures, kHz from 1 kHz");
    Expect(FormatParamValue(db, -3.456) == "-3.46dB", "otherwise two decimals and the unit");
    Expect(FormatParamValue(amount, 0.5) == "0.50", "an amount has no unit");
    Expect(FormatParamValue(db, -0.001) == "0.00dB", "never negative zero");
    Expect(FormatParamValue(toggle, 1) == "On", "a toggle is On or Off");
    // Guitar to MIDI's MIDI Channel showed "1.00", and its Transpose "-12.00st".
    EffectParamInfo channel{"channel", "MIDI Channel", "", "", 1, 16, 1, 1};
    EffectParamInfo transpose{"transpose", "Transpose", "st", "", -24, 24, 0, 1};
    EffectParamInfo halves{"h", "H", "", "", 0, 10, 0, 2.5};
    Expect(FormatParamValue(channel, 1) == "1" && FormatParamValue(transpose, -12) == "-12st" &&
               FormatParamValue(transpose, -1.0e-9) == "0st",
           "a parameter in whole steps shows a whole number");
    Expect(FormatParamValue(halves, 5) == "5.00" && FormatParamValue(transpose, 0.5) == "0.50st",
           "but not one with a fractional step, or a value between steps");

    Expect(std::abs(ParamPositionToValue(freq, 0.5) - std::sqrt(20.0 * 20000.0)) < 1e-6,
           "a log taper's middle is the geometric mean");
    Expect(std::abs(ParamValueToPosition(db, 12) - 0.75) < 1e-9, "a linear taper is linear");
    EffectParamInfo stepped{"s", "S", "", "", 0, 10, 0, 2.5};
    Expect(SnapParamValue(stepped, 6.4) == 7.5 && SnapParamValue(stepped, 42) == 10,
           "values snap to the step and clamp");
}

/// A parameter shown only under some values of another (Verbal's machine controls), and the
/// "picker" an enum asks for, as the engine describes them.
void TestParamVisibility()
{
    const auto json =
        nlohmann::json::parse(R"({"key": "diffusion", "name": "Diffusion", "unit": "amount", "min": 0, "max": 1,
        "default": 0.6, "control": "picker", "visibleWhen": {"key": "machine", "values": [0, 6, 9]}})");
    const auto param = parse::EffectParam(json);
    Expect(param.control == "picker", "an enum's control is read");
    Expect(param.visibleWhenKey == "machine" && param.visibleWhenValues == std::vector<int>{0, 6, 9},
           "visibleWhen is read");

    uiclient::EffectTypeInfo type;
    type.parameters = {parse::EffectParam(nlohmann::json::parse(
                           R"({"key": "machine", "unit": "enum", "min": 0, "max": 11, "default": 1})")),
                       param};

    GraphNode node = Node("verbal_1", "verbal");
    Expect(!IsParamVisible(param, node, &type),
           "without a value the controlling parameter's default decides (1: hidden)");
    node.params["machine"] = 6.2;
    Expect(IsParamVisible(param, node, &type), "a value it lists shows it, rounded as the web UI rounds it");
    node.params["machine"] = 2.0;
    Expect(!IsParamVisible(param, node, &type), "any other value hides it");
    Expect(IsParamVisible(type.parameters[0], node, &type), "a parameter with no condition always shows");

    const auto empty =
        parse::EffectParam(nlohmann::json::parse(R"({"key": "x", "visibleWhen": {"key": "machine", "values": []}})"));
    Expect(empty.visibleWhenKey.empty(), "a condition with no values is no condition");
}

/// The chain edits Nano Q's chain strip makes: replace, rename, and the parallel block's
/// branch count and per-branch insert, each answered by the engine's rebuilt chain.
void TestChainEditCommands()
{
    LoopbackHost host(Sandbox("chain-edits"));
    PluginController controller(host);
    controller.Initialize();

    UiClient client([&controller](const std::string& json) { controller.HandleUIMessage(json); });
    host.client = &client;
    UiCommands commands(client);

    const auto preset = BuildPreset("pe", "Edits");
    client.Send("savePreset", {{"saveMode", "overwrite"},
                               {"presetId", preset.id},
                               {"name", preset.name},
                               {"preset", nlohmann::json::parse(PresetStorage::SerializeToJson(preset))}});
    client.Start();
    Pump(controller, client);
    commands.LoadPreset("pe");
    Pump(controller, client);

    const auto& state = client.State();
    auto graph = [&state]() -> const SignalGraph& { return state.activePreset->graph; };
    auto firstOfType = [&graph](const std::string& type) -> const GraphNode* {
        for (const auto& node : graph().nodes)
        {
            if (EffectRegistry::Instance().Resolve(node.type) == type)
            {
                return &node;
            }
        }

        return nullptr;
    };

    commands.ReplaceNode("gain_1", EffectGuids::kOverdrive);
    Pump(controller, client);
    const auto* drive = firstOfType(EffectGuids::kOverdrive);
    // The engine replaces in place: the node keeps its id, and so its place in the chain.
    Expect(drive != nullptr && drive->id == "gain_1" && firstOfType(EffectGuids::kGain) == nullptr,
           "ReplaceNode puts the new effect in the node's place");
    const std::string driveId = drive != nullptr ? drive->id : std::string();

    commands.RenameNode(driveId, "Lead boost");
    Pump(controller, client);
    Expect(graph().FindNode(driveId) && graph().FindNode(driveId)->title == "Lead boost", "RenameNode names the node");
    Expect(NodeDisplayName(state, *graph().FindNode(driveId)) == "Lead boost", "and the name is what the chain shows");

    commands.AddNode(EffectGuids::kSplitter, driveId);
    Pump(controller, client);
    const auto* splitter = firstOfType(EffectGuids::kSplitter);
    Expect(splitter != nullptr, "a splitter is added");
    const std::string splitterId = splitter != nullptr ? splitter->id : std::string();

    auto branchEdges = [&graph, &splitterId] {
        std::vector<GraphEdge> out;

        for (const auto& edge : graph().edges)
        {
            if (edge.from == splitterId)
            {
                out.push_back(edge);
            }
        }

        return out;
    };

    commands.SetSplitBranchCount(splitterId, 3);
    Pump(controller, client);
    Expect(branchEdges().size() == 3, "SetSplitBranchCount gives the split three branches");

    // The second branch is empty: its one edge runs from the splitter straight to the mixer.
    std::optional<GraphEdge> second;

    for (const auto& edge : branchEdges())
    {
        if (edge.fromPort == 1)
        {
            second = edge;
        }
    }

    Expect(second.has_value(), "the split has a branch on port 1");

    if (second)
    {
        commands.AddNodeOnEdge(EffectGuids::kGain, *second);
        Pump(controller, client);
        const auto* added = firstOfType(EffectGuids::kGain);
        bool onBranch = false;

        for (const auto& edge : branchEdges())
        {
            onBranch = onBranch || (added != nullptr && edge.fromPort == 1 && edge.to == added->id);
        }

        Expect(onBranch, "AddNodeOnEdge inserts into that branch, which an add after a node cannot name");
    }
}

void TestInstruments()
{
    TunerModel tuner;
    TunerModel::Reading reading;

    for (double cents : {10.0, 10.0, 10.0, 10.0, 10.0, 10.0, -8.0})
    {
        reading = tuner.Push(true, "C#/Db", 3, cents, 138.6);
    }

    Expect(std::abs(reading.cents - 7.0) < 1e-9, "the tuner averages the last six readings");
    Expect(reading.note == "C#" && reading.noteLeft == "C" && reading.noteRight == "D", "notes and neighbours");
    Expect(!reading.inTune && reading.needle > 0.5, "7 cents sharp is not in tune");
    Expect(tuner.Push(false, "", 0, 0, 0).detected == false, "no signal clears the reading");
    Expect(std::abs(tuner.Push(true, "A", 4, 2.0, 440).cents - 2.0) < 1e-9 && tuner.Push(true, "A", 4, 2.0, 440).inTune,
           "and the history, so a new note does not average with the last");

    TapTempo tap;
    Expect(!tap.Tap(0.0) && !tap.Tap(0.5), "two taps are not a tempo");
    Expect(tap.Tap(1.0).value_or(0) == 120.0, "three taps half a second apart are 120 BPM");
    Expect(!tap.Tap(4.0), "a pause over 2.5 s starts again");
    Expect(ClampBpm(12.345) == 30.0 && ClampBpm(99.96) == 100.0, "tempo clamps to 30..300 at 0.1 BPM");
}

void TestEffectPresentationTable()
{
    const auto table =
        EffectPresentation::Load(fs::path(GUITARFX_DEMO_AUDIO_DIR) / ".." / "data" / "effect-presentation.json");
    Expect(table.IsLoaded(), "core/ui/data/effect-presentation.json loads");
    Expect(table.IconFor(EffectGuids::kDynamicsGate, "dynamics") == "door", "an effect's own icon");
    Expect(table.IconFor("unknown-type", "delay") == "delay", "else its category's");
    Expect(table.IconFor("unknown-type", "nope") == "gear", "else the default");
    Expect(table.CategoryColour("amp") == 0xffe07848 && table.CategoryColour("pedal") == 0xffe07848,
           "category colours, with pedal folded into amp");
    Expect(table.NodeClass("channel") == "amp" && table.NodeClass("drive") == "utility",
           "node classes as the web UI's");
    Expect(table.EquipmentImage(EffectGuids::kDelayTape, "delay") == "images/equipment/fx/studio-rack-delay.png" &&
               table.EquipmentImage("fx_nam", "amp") == "images/equipment/pedals/colourful-pedal2.png",
           "stock artwork by effect, legacy id and category");
    Expect(ParseCssColour("rgba(255, 0, 0, 0.5)").value_or(0) == 0x80ff0000 &&
               ParseCssColour("#abc").value_or(0) == 0xffaabbcc,
           "css colours parse");
}

void TestLayoutDecisions()
{
    Expect(DecideLayout(800, 480, true).shell == ShellLayout::Rail, "landscape gets the rail");
    Expect(DecideLayout(360, 640, true).shell == ShellLayout::Stack && DecideLayout(360, 640, true).sliderList,
           "portrait stacks and uses sliders");
    Expect(DecideLayout(640, 360, false).splitEffectView && !DecideLayout(1024, 600, false).splitEffectView,
           "a narrow landscape splits the effect view");
}
} // namespace

// Where a dragged chain card lands, as the web UI's signalPathDropTargets.test.ts has it.
void TestChainDropRules()
{
    const auto edge = [](const char* from, const char* to) { return GraphEdge{from, to, 0, 0, 1.0}; };
    const auto node = [](const char* id, bool left, std::vector<GraphEdge> incoming) {
        NodeDropTarget t;
        t.kind = NodeDropTarget::Kind::Node;
        t.nodeId = id;
        t.isLeftOfDragged = left;
        t.incoming = std::move(incoming);
        return t;
    };
    const auto onEdge = [](GraphEdge e) {
        NodeDropTarget t;
        t.kind = NodeDropTarget::Kind::Edge;
        t.edge = std::move(e);
        return t;
    };
    using K = NodeDropAction::Kind;

    auto a = ResolveNodeDrop("drive", node("delay", false, {edge("amp", "delay")}), 0, 0);
    Expect(a.kind == K::ReorderAfterNode && a.targetNodeId == "delay", "dragged rightwards: after the target");
    a = ResolveNodeDrop("delay", node("drive", true, {edge("__input__", "drive")}), 0, 0);
    Expect(a.kind == K::ReorderToEdge && a.edge.from == "__input__" && a.edge.to == "drive",
           "dragged leftwards: the target's slot");
    a = ResolveNodeDrop("delay", node("mix", true, {edge("branchA", "mix"), edge("branchB", "mix")}), 0, 0);
    Expect(a.kind == K::ReorderAfterNode && a.targetNodeId == "mix", "a leftward join target: after it");
    a = ResolveNodeDrop("drive", onEdge(edge("amp", "delay")), 0, 0);
    Expect(a.kind == K::ReorderToEdge && a.edge.to == "delay", "onto a cable");
    Expect(ResolveNodeDrop("drive", onEdge(edge("drive", "amp")), 0, 0).kind == K::None, "onto its own cable: nothing");
    Expect(ResolveNodeDrop("drive", node("drive", false, {}), 0, 0).kind == K::None, "onto itself: nothing");
    Expect(ResolveNodeDrop("drive", {}, 4, 50).kind == K::ToggleBypass, "a vertical flick on nothing toggles bypass");
    Expect(ResolveNodeDrop("drive", {}, 4, 50, true).kind == K::None, "not when it lands on another line");
    Expect(ResolveNodeDrop("drive", {}, 80, 10).kind == K::None, "released away from a target, no flick: nothing");
    Expect(!IsBypassFlick(0, 20) && !IsBypassFlick(40, 40) && IsBypassFlick(0, -40),
           "enough, mostly vertical, either way");
}

// The shared feature switches and the effect chooser's lists, as the web UI reads them.
void TestFeatureFlagsAndFxMarks()
{
    ClientState state;
    Expect(IsFeatureEnabled(state, Feature::ToneSharing) && !IsFeatureEnabled(state, Feature::PickupMatch),
           "unset, a feature reads as the web UI's default");
    state.appSettings["features.toneSharing.enabled"] = false;
    state.appSettings["features.pickupMatch.enabled"] = true;
    Expect(!IsFeatureEnabled(state, Feature::ToneSharing) && IsFeatureEnabled(state, Feature::PickupMatch),
           "set, it reads as set");

    Expect(FxItemKey("amp_nam") == "amp_nam||", "a plain effect's key, as fxItemKey makes it");
    state.uiSettings["fxFavorites"] = nlohmann::json::array({"a||", 3, "a||", "", "b||"});
    Expect(FxFavoriteKeys(state) == std::vector<std::string>{"a||", "b||"}, "strings only, each once, in order");
    nlohmann::json recents = nlohmann::json::array();

    for (int i = 0; i < 20; ++i)
    {
        recents.push_back("r" + std::to_string(i) + "||");
    }

    state.uiSettings["fxRecents"] = recents;
    Expect(FxRecentKeys(state).size() == kFxRecentsLimit && FxRecentKeys(state).front() == "r0||",
           "recents keep the newest 12");
    Expect(IsFxFavorite(state, "b||") && !IsFxFavorite(state, "r0||"), "IsFxFavorite reads the favourites");
}

// The preset bar's back and forward, as presets/history.ts keeps it.
void TestPresetTrail()
{
    PresetTrail trail;
    const auto all = [](const std::string&) { return true; };
    trail.Note("a");
    trail.Note("b");
    trail.Note("b");
    trail.Note("c");
    Expect(trail.CanGoBack() && !trail.CanGoForward(), "three loaded: back, no forward");
    Expect(trail.Step(-1, all) == std::optional<std::string>("b"), "back to b");
    trail.Note("b"); // the load of the stepped-to preset adds nothing
    Expect(trail.CanGoForward(), "c is still forward");
    trail.Note("d");
    Expect(!trail.CanGoForward(), "a new load drops what was forward");
    Expect(!trail.Step(-1, [](const std::string& id) { return id != "b"; }).has_value(),
           "a deleted preset is not stepped onto");
    Expect(trail.Step(-1, all) == std::optional<std::string>("a"), "it was dropped: back goes on to a");

    for (int i = 0; i < 20; ++i)
    {
        trail.Note("p" + std::to_string(i));
    }

    int back = 0;

    while (trail.Step(-1, all))
    {
        ++back;
    }

    Expect(back == static_cast<int>(kPresetTrailLimit) - 1, "at most ten are kept");
}

// The resource chooser's list, as resourceDedup.ts, renderLibraryList and libraryPacks.ts make it.
void TestResourceIndex()
{
    const auto res = [](const char* id, const char* name, const char* hash,
                        nlohmann::json meta = nlohmann::json::object()) {
        uiclient::LibraryResource r;
        r.id = id;
        r.type = "nam";
        r.name = name;
        r.hash = hash;
        r.category = "Amps";
        r.filePath = std::string("C:/m/") + id + ".nam";
        r.metadata = std::move(meta);
        return r;
    };
    ClientState state;
    auto& list = state.resources["nam"];
    list.push_back(res("a1", "Crunch", "H1", {{"architectureVersion", "1"}}));
    list.push_back(res("a2", "Crunch", "h1", {{"architectureVersion", "2"}}));
    list.push_back(res("b", "Clean Bass", "H2",
                       {{"creatorName", "Ann"}, {"tags", "clean; vintage"}, {"toneId", "7"}, {"toneTitle", "Tweed"}}));
    list.push_back(res("c", "Tweed Deluxe", "H3", {{"toneId", "7"}, {"toneTitle", "Tweed"}}));
    list.push_back(res("z", "Zed", "H4"));

    const auto dedup = DeduplicateResources(list);
    Expect(dedup.deduped.size() == 4 && dedup.aliasById.at("a1") == "a2",
           "one entry per hash, ignoring case; A2 over A1 of the same name");
    Expect(DeduplicateResources(list, {"a1"}).aliasById.at("a2") == "a1", "a preferred id is kept over its duplicate");
    Expect(ResourceTags(list[2]) == std::vector<std::string>{"clean", "vintage"} && ResourceCreator(list[2]) == "Ann",
           "tags from metadata, and the creator");
    Expect(FormatGearDescription("Diezel", "Diezel Herbert") == "Diezel Herbert" &&
               FormatGearDescription("Mesa", "Rectifier") == "Mesa Rectifier" && FormatGearDescription("", "") == "",
           "the gear, its make said once");
    auto meta = res("m", "Lead", "H9",
                    {{"creatorName", "csobrinos"},
                     {"architectureVersion", "2"},
                     {"gearMake", "Diezel"},
                     {"gearModel", "Diezel Herbert"},
                     {"toneType", "hi_gain"}});
    Expect(ResourceMetaLine(meta, "nam", true) ==
               "Amps \xC2\xB7 by csobrinos \xC2\xB7 A2 \xC2\xB7 Diezel Herbert \xC2\xB7 hi gain",
           "a row's line: category, creator, architecture, gear, tone type");
    meta.category.clear();
    Expect(ResourceMetaLine(meta, "ir", false) == "by csobrinos \xC2\xB7 Diezel Herbert \xC2\xB7 hi gain",
           "no category while filtered to one, and no architecture for an IR");

    ResourceFilter filter;
    filter.tags = {"CLEAN"};
    Expect(FilterLibrary(state, "nam", filter).size() == 1, "a tag filter ignores case");
    filter = {};
    filter.architecture = "A2";
    Expect(FilterLibrary(state, "nam", filter).size() == 1, "the architecture filter");
    filter = {};
    filter.query = "ann";
    Expect(FilterLibrary(state, "nam", filter).size() == 1, "the search covers the creator");

    state.appSettings["resources.favorites"] = nlohmann::json::array({"z"});
    const auto ordered = FilterLibrary(state, "nam", ResourceFilter{});
    Expect(!ordered.empty() && ordered.front()->id == "z", "favourites first, then by name");

    const auto groups = GroupLibraryByPack(state, ordered);
    Expect(groups.size() == 3 && groups[0].items.front()->id == "z" &&
               std::count_if(groups.begin(), groups.end(),
                             [](const LibraryEntry& g) { return g.pack && g.items.size() == 2; }) == 1,
           "a pack groups its tone's models; a favourite's entry first");
    Expect(NameWithinPack("Tweed Deluxe", *LibraryPackOf(list[3])) == "Deluxe",
           "the pack's title comes off the model's name");
    state.appSettings["resources.library.groupPacks"] = false;
    Expect(NavigationOrder(state, ordered).size() == ordered.size() && NavigationOrder(state, ordered)[1] == ordered[1],
           "ungrouped, previous/next follows the list as it is");
}

// The editors' analysis: EQ curves from the engine, coalesced per plot, and the live feeds.
void TestAnalysis()
{
    {
        LoopbackHost host(Sandbox("analysis"));
        PluginController controller(host);
        controller.Initialize();
        UiClient client([&controller](const std::string& json) { controller.HandleUIMessage(json); });
        host.client = &client;

        std::optional<ResponseCurve> curve;
        int answers = 0;
        client.GetAnalysis().RequestResponse("eq", "eq_parametric",
                                             {{"lowGain", 9.0}, {"lowFreq", 100.0}, {"highGain", -6.0}}, true,
                                             [&](const std::optional<ResponseCurve>& c) {
                                                 curve = c;
                                                 ++answers;
                                             });
        Pump(controller, client);
        Expect(answers == 1 && curve &&
                   curve->frequencies.size() == static_cast<std::size_t>(Analysis::kResponsePoints),
               "an EQ's curve comes from the engine");
        Expect(curve && curve->bandsDb.size() == 4, "with each band's own curve");
        Expect(curve && std::abs(ResponseDbAt(*curve, 25.0) - 9.0) < 0.5 &&
                   std::abs(ResponseDbAt(*curve, 19000.0) + 6.0) < 0.5,
               "the low shelf lifts the lows and the high shelf cuts the highs");

        std::optional<ResponseCurve> unsupported = ResponseCurve{};
        client.GetAnalysis().RequestResponse("x", "reverb_hall", nlohmann::json::object(), false,
                                             [&](const std::optional<ResponseCurve>& c) { unsupported = c; });
        Pump(controller, client);
        Expect(!unsupported, "an effect with no fixed response has no curve");
    }

    {
        std::vector<nlohmann::json> sent;
        UiClient client([&sent](const std::string& json) { sent.push_back(nlohmann::json::parse(json)); });
        auto& analysis = client.GetAnalysis();
        const auto sentOf = [&sent](const char* type) {
            std::vector<nlohmann::json> out;

            for (const auto& m : sent)
            {
                if (m.value("type", "") == type)
                {
                    out.push_back(m);
                }
            }

            return out;
        };
        std::vector<std::string> answered;
        const auto answer = [&](const nlohmann::json& request, bool supported) {
            client.Enqueue(nlohmann::json{
                {"type", "effectResponse"},
                {"requestId", request["requestId"]},
                {"supported", supported},
                {"frequencies", {20, 20000}},
                {"magnitudesDb",
                 {1, 2}}}.dump());
            client.DrainPending();
        };

        analysis.RequestResponse("a", "eq_parametric", {{"lowGain", 1}}, false,
                                 [&](auto&) { answered.push_back("a1"); });
        analysis.RequestResponse("a", "eq_parametric", {{"lowGain", 2}}, false,
                                 [&](auto&) { answered.push_back("a2"); });
        analysis.RequestResponse("a", "eq_parametric", {{"lowGain", 3}}, false,
                                 [&](auto&) { answered.push_back("a3"); });
        analysis.RequestResponse("b", "eq_graphic", {{"bandCount", 5}}, true, [&](auto&) { answered.push_back("b1"); });
        auto requests = sentOf("getEffectResponse");
        Expect(requests.size() == 2 && requests[1].value("bands", false),
               "one request in flight per lane, bands asked for");

        answer(requests[0], true);
        requests = sentOf("getEffectResponse");
        Expect(requests.size() == 3 && requests[2]["params"]["lowGain"] == 3,
               "the newest parameters follow, the middle ones folded away");
        answer(requests[1], true);
        answer(requests[2], false);
        Expect((answered == std::vector<std::string>{"a1", "b1", "a3"}),
               "each lane answers its own, a replaced request never");

        analysis.RequestResponse("c", "eq_parametric", {}, false, [&](auto&) { answered.push_back("c1"); });
        analysis.RequestResponse("c", "eq_parametric", {{"lowGain", 4}}, false,
                                 [&](auto&) { answered.push_back("c2"); });
        client.Tick(10.0);
        Expect(sentOf("getEffectResponse").size() == 5, "an unanswered request is given up on, so the next one goes");

        analysis.WatchSpectrum({"preset", "eq1", "p1"});
        analysis.WatchSpectrum({"preset", "eq1", "p1"});
        client.Tick(13.0);
        Expect(sentOf("setSpectrumWatch").size() == 2 && sentOf("setSpectrumWatch")[1]["nodeId"] == "eq1",
               "a spectrum watch is sent once and renewed on the tick");
        analysis.StopSpectrum();
        client.Tick(16.0);
        Expect(sentOf("setSpectrumWatch").size() == 3 && sentOf("setSpectrumWatch")[2].size() == 1,
               "stopping sends {} and renews nothing");
        analysis.WatchOutputLoudness(true);
        Expect(sentOf("setLoudnessWatch").back()["slot"] == 1, "the output's loudness is slot 1");
    }

    UiClient client([](const std::string&) {});
    client.Enqueue(
        R"({"type":"sldS","scope":"preset","presetId":"p1","id":"eq1","r":[20,20000,-96,0],"s":[-50,-40,-30]})");
    client.Enqueue(R"({"type":"sldL","scope":"post","id":"global_eq","l":[-18.2,-19,null,0]})");
    client.Enqueue(R"({"type":"sldL","slot":1,"scope":"output","l":[-14,-15,-16,12]})");
    client.Enqueue(
        R"({"type":"sldRoster","seq":3,"nodes":[],"spectrogramRange":[-90,-6,30,18000],"barkRange":[-80,0,40,16000]})");
    client.Enqueue(
        R"({"type":"sldA","seq":3,"id":"an1","t":5,"l":[50,20,-10,-12,0.2,-20,-21,null,2,1,1],"s":[-40,-50],"b":[-30]})");
    client.Enqueue(
        R"({"type":"spatialPosition","nodes":[{"scope":"preset","presetId":"p1","nodeId":"sp","azimuth":30,"moving":true}]})");
    client.Enqueue(
        R"({"type":"gateLearnStatus","scope":"global","nodeId":"global_gate","state":"done","noiseFloorDb":-70,"thresholdDb":-64})");
    client.DrainPending();
    const auto& a = client.State().analysis;
    Expect(a.spectra.count("p1::eq1") && a.spectra.at("p1::eq1").binsDb.size() == 3,
           "a node's spectrum is kept by its feed key");
    Expect(a.loudness.count("post::global_eq") && a.loudness.at("post::global_eq").momentaryLufs == -18.2 &&
               !a.loudness.at("post::global_eq").integratedLufs,
           "a node's loudness, an unfilled window empty");
    Expect(a.outputLoudness.integratedLufs == -16.0 && a.outputLoudness.integratedSeconds == 12.0,
           "the output's loudness");
    Expect(a.analyzers.count("an1") && a.analyzers.at("an1").stereo &&
               a.analyzers.at("an1").spectrogramDb.size() == 2 && a.analyzerRanges.spectrogram[3] == 18000.0,
           "an analyzer frame, drawn against the roster's ranges");
    Expect(a.spatial.count("p1::sp") && a.spatial.at("p1::sp").azimuth == 30.0 && a.spatial.at("p1::sp").moving,
           "a spatial position");
    Expect(a.gateLearn.count("global::global_gate") && a.gateLearn.at("global::global_gate").thresholdDb == -64.0,
           "the global gate's Learn result");

    // A Multi-band Gate's listen: one entry per band, its noise floors kept through a listen
    // that heard playing.
    client.Enqueue(
        R"({"type":"gateLearnStatus","scope":"preset","presetId":"p1","nodeId":"mbg","state":"done","bands":[)"
        R"({"noiseFloorDb":-90,"thresholdDb":-84},{"noiseFloorDb":-80,"thresholdDb":-74},{},)"
        R"({"thresholdDb":-70},{"thresholdDb":-70},{"thresholdDb":-70}]})");
    client.DrainPending();
    const auto& bands = client.State().analysis.gateLearn.at("p1::mbg").bands;
    Expect(bands.size() == 6 && bands[1].thresholdDb == -74.0 && !bands[2].thresholdDb && !bands[0].levelDb,
           "a Multi-band Gate's Learn result, band by band");
    client.Enqueue(
        R"({"type":"gateLearnStatus","scope":"preset","presetId":"p1","nodeId":"mbg","state":"listening","progress":0.5,)"
        R"("bands":[{"levelDb":-60},{"levelDb":-61},{"levelDb":-62},{"levelDb":-63},{"levelDb":-64},{"levelDb":-65}]})");
    client.Enqueue(
        R"({"type":"gateLearnStatus","scope":"preset","presetId":"p1","nodeId":"mbg","state":"failed","reason":"playing",)"
        R"("bands":[{"noiseFloorDb":-20},{"noiseFloorDb":-20},{},{},{},{}]})");
    client.DrainPending();
    const auto& kept = client.State().analysis.gateLearn.at("p1::mbg").bands;
    Expect(kept.size() == 6 && kept[0].noiseFloorDb == -90.0 && kept[1].noiseFloorDb == -80.0 && !kept[0].levelDb &&
               !kept[0].thresholdDb,
           "a band's learned noise floor stays drawn; one that heard playing is not taken as noise");
    Expect(DecodeFloat32Base64("AACAPwAAAMA=") == std::vector<float>{1.0f, -2.0f}, "float32 samples from base64");
}

// The IR Cabinet's Alignment section: where IR B lines up, and the two summed, from a real engine;
// then the summed response's coalescing on its own.
void TestIrAlignmentResponse()
{
    {
        const auto dir = Sandbox("ir-alignment");
        // A decaying cabinet-ish IR, and the same one 20 samples later as IR B.
        const auto writeIr = [&dir](const char* name, int onset) {
            std::vector<float> samples(2400, 0.0f);

            for (std::size_t n = static_cast<std::size_t>(onset); n < samples.size(); ++n)
            {
                const double t = static_cast<double>(n - static_cast<std::size_t>(onset)) / 48000.0;
                samples[n] = static_cast<float>(0.5 * std::exp(-t / 0.004) *
                                                (std::sin(2.0 * 3.14159265358979 * 900.0 * t) +
                                                 0.5 * std::sin(2.0 * 3.14159265358979 * 2700.0 * t)));
            }

            const std::vector<const std::vector<float>*> channels{&samples};
            const auto bytes = util::Encode16BitWav(channels, 48000);
            const auto path = dir / name;
            std::ofstream(path, std::ios::binary)
                .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            return nlohmann::json{{"resourceType", "ir"}, {"filePath", util::PathToUtf8(path)}};
        };
        const auto irA = writeIr("a.wav", 10);
        const auto irB = writeIr("b.wav", 30);

        LoopbackHost host(dir);
        PluginController controller(host);
        controller.Initialize();
        UiClient client([&controller](const std::string& json) { controller.HandleUIMessage(json); });
        host.client = &client;
        auto& analysis = client.GetAnalysis();

        std::optional<IrAlignment> alignment;
        analysis.AnalyzeIrAlignment(irA, irB,
                                    [&](const std::optional<IrAlignment>& a, const std::string&) { alignment = a; });
        Pump(controller, client);
        Expect(alignment && std::abs(alignment->alignedOffsetMs + 20.0 / 48.0) < 0.01 && !alignment->invertB,
               "B 20 samples late lines up 0.42 ms earlier, not inverted");

        std::optional<IrAlignmentResponse> lined;
        analysis.RequestIrAlignmentResponse(
            irA, irB, {{"irBlend", 0.5}}, alignment ? alignment->alignedOffsetMs : 0.0,
            [&](const std::optional<IrAlignmentResponse>& r, const std::string&) { lined = r; });
        Pump(controller, client);
        Expect(lined && lined->slots == "both" &&
                   lined->frequencies.size() == static_cast<std::size_t>(Analysis::kResponsePoints) &&
                   lined->unrelatedDb.size() == lined->frequencies.size(),
               "a pair's summed response comes with the curve to compare it with");
        // Lined up, one IR with itself sums 3 dB over the two added without interfering.
        const auto gainAt = [](const IrAlignmentResponse& r, double hz) {
            return ResponseDbAt(r.frequencies, r.summedDb, hz) - ResponseDbAt(r.frequencies, r.unrelatedDb, hz);
        };
        Expect(lined && std::abs(gainAt(*lined, 900.0) - 3.0) < 0.3 && std::abs(gainAt(*lined, 2700.0) - 3.0) < 0.3,
               "lined up, the pair reinforces");

        std::optional<IrAlignmentResponse> combed;
        analysis.RequestIrAlignmentResponse(
            irA, irB, {{"irBlend", 0.5}, {"slotBOffset", 0.0}}, 0.0,
            [&](const std::optional<IrAlignmentResponse>& r, const std::string&) { combed = r; });
        Pump(controller, client);
        // 20 samples apart, they cancel at 1.2 kHz (half a period of 20 samples).
        Expect(combed && gainAt(*combed, 1200.0) < -10.0, "left 0.42 ms apart, they comb-filter");

        std::optional<IrAlignment> lone;
        analysis.AnalyzeIrAlignment(irA, nullptr,
                                    [&](const std::optional<IrAlignment>& a, const std::string&) { lone = a; });
        Pump(controller, client);
        Expect(lone && !lone->waveformA.empty() && lone->waveformB.empty() && lone->matchByOffset.size() == 1,
               "one IR is analysed alone: its waveform, nothing to align");

        std::optional<IrAlignmentResponse> single;
        analysis.RequestIrAlignmentResponse(
            irA, nullptr, {}, 0.0,
            [&](const std::optional<IrAlignmentResponse>& r, const std::string&) { single = r; });
        Pump(controller, client);
        Expect(single && single->slots == "a" && single->unrelatedDb.empty(), "one IR alone has nothing to compare");

        std::string error;
        analysis.RequestIrAlignmentResponse(
            irA, nlohmann::json{{"resourceType", "ir"}, {"filePath", util::PathToUtf8(dir / "none.wav")}}, {}, 0.0,
            [&](const std::optional<IrAlignmentResponse>&, const std::string& e) { error = e; });
        Pump(controller, client);
        Expect(error == "IR B could not be read", "a missing IR is reported");
    }

    std::vector<nlohmann::json> sent;
    UiClient client([&sent](const std::string& json) { sent.push_back(nlohmann::json::parse(json)); });
    auto& analysis = client.GetAnalysis();
    const auto requests = [&sent] {
        std::vector<nlohmann::json> out;

        for (const auto& m : sent)
        {
            if (m.value("type", "") == "getIrAlignmentResponse")
            {
                out.push_back(m);
            }
        }

        return out;
    };
    std::vector<std::string> answered;
    const nlohmann::json ir = {{"resourceId", "a"}};
    analysis.RequestIrAlignmentResponse(ir, ir, {}, 0.1, [&](auto&, auto&) { answered.push_back("first"); });
    analysis.RequestIrAlignmentResponse(ir, ir, {}, 0.2, [&](auto&, auto&) { answered.push_back("skipped"); });
    analysis.RequestIrAlignmentResponse(ir, ir, {}, 0.3, [&](auto&, auto&) { answered.push_back("newest"); });
    Expect(requests().size() == 1 && requests()[0]["offsetMs"] == 0.1 &&
               requests()[0]["points"] == Analysis::kResponsePoints,
           "one response request out at a time");
    client.Enqueue(nlohmann::json{
        {"type", "irAlignmentResponse"},
        {"requestId", requests()[0]["requestId"]},
        {"slots", "both"},
        {"frequencies", {20, 20000}},
        {"summedDb", {1, 2}},
        {"unrelatedDb",
         {0, 1}}}.dump());
    client.DrainPending();
    Expect(requests().size() == 2 && requests()[1]["offsetMs"] == 0.3,
           "the newest follows, the middle one folded away");
    client.Tick(10.0);
    Expect((answered == std::vector<std::string>{"first", "newest"}),
           "an unanswered request is given up on, its callback told");
    Expect(!ParseIrAlignmentResponse({{"frequencies", {20, 20000}}, {"summedDb", {1, 2}}}),
           "a pair's reply without the curve to compare it with is malformed");
}

// What the effect editors' live sections print: Gate Learn's line, the Signal Analyzer's
// readouts, Pickup Match's custom design, and the DSP status tiles.
void TestAnalysisPresentation()
{
    Expect(GateLearnStatusText(nullptr, -80.0) == "Mute the strings, then Learn.", "Learn before any listen");
    GateLearnStatus done{"done"};
    done.noiseFloorDb = -92.0;
    done.thresholdDb = -76.0;
    Expect(GateLearnStatusText(&done, -80.0) == "Noise under \xE2\x88\x92"
                                                "80 dB \xC2\xB7 threshold set to \xE2\x88\x92"
                                                "76 dB",
           "noise below the meter reads as under its floor");
    GateLearnStatus failed{"failed"};
    failed.reason = "playing";
    Expect(GateLearnStatusText(&failed, -80.0) == "Heard playing. Mute the strings and try again.",
           "a refused listen says why");
    GateLearnStatus bands{"done"};
    bands.bands.resize(6);
    Expect(GateLearnStatusText(&bands, -96.0) == "Thresholds set", "a Multi-band Gate's listen with no floors");
    bands.bands[1].noiseFloorDb = -88.4;
    bands.bands[4].noiseFloorDb = -71.6;
    Expect(GateLearnStatusText(&bands, -96.0) == "Noise up to \xE2\x88\x92"
                                                 "72 dB \xC2\xB7 each band's threshold set just above it",
           "a Multi-band Gate's listen reads as its loudest band");
    bands.bands[1].noiseFloorDb = -99.0;
    bands.bands[4].noiseFloorDb = -97.0;
    Expect(GateLearnStatusText(&bands, -96.0) == "Noise under \xE2\x88\x92"
                                                 "96 dB \xC2\xB7 each band's threshold set just above it",
           "a Multi-band Gate's quiet bands read as under the meter");

    AnalyzerFrame frame;
    frame.peakPercent = 50.0;
    frame.rmsPercent = 10.0;
    frame.momentaryLufs = -18.25;
    frame.loudnessValid = true;
    frame.activeChannelCount = 1;
    auto readouts = AnalyzerReadouts(&frame, 2);
    Expect(readouts.size() == 11 && readouts[0].value == "-6.0 dBFS" && readouts[2].value == "50.0 %",
           "peak in dBFS and %FS");
    Expect(readouts[7].value == "-18.2 LUFS" || readouts[7].value == "-18.3 LUFS", "momentary loudness");
    Expect(readouts[8].value == "\xE2\x80\x94", "a window not yet filled reads as a dash");
    Expect(readouts[10].value == "Stereo" && readouts[10].hint == "one side silent",
           "the layout, with what is measured as a hint");
    Expect(AnalyzerReadouts(nullptr, 0)[0].value == "\xE2\x80\x94", "no frame: dashes");

    const std::map<std::string, double> summary = {{"targetWireLengthM", 1191.4}, {"targetInductanceH", 2.66},
                                                   {"targetResistanceKohm", 6.5}, {"targetCapacitancePf", 118.0},
                                                   {"targetPeakHz", 3400.0},      {"targetQ", 1.42},
                                                   {"targetLevelDb", -0.4}};
    Expect(
        PickupDesignSummary(summary, "target") ==
            "1,191 m of wire \xC2\xB7 2.66 H \xC2\xB7 6.50 k\xCE\xA9 \xC2\xB7 118 pF \xE2\x86\x92 peak 3.40 kHz, Q 1.42, output \xE2\x88\x92"
            "0.4 dB",
        "a custom design's build, as the web UI words it");
    Expect(PickupDesignSummary(summary, "source").empty(), "a side with no design figures says nothing");

    ClientState state;
    Expect(ShownDspStatusItems(state, EffectDspStatus()).size() == 4, "an effect's readout starts with its defaults");
    state.appSettings["ui.dspStatus.items"] = nlohmann::json::array({"latency", "dspLoad", "momentary", "latency"});
    const auto shown = ShownDspStatusItems(state, EffectDspStatus());
    Expect((shown == std::vector<DspStatusItem>{DspStatusItem::Latency, DspStatusItem::Momentary}) &&
               ShowsLoudness(shown),
           "the stored choice, kept to what this readout can show, each once");
    Expect(DspStatusSettingValue(shown) == nlohmann::json::array({"latency", "momentary"}),
           "the choice as the setting stores it");

    state.telemetry.nodes["p1::amp"] = LevelReading{-6.0, -12.0, 0, false, 2};
    state.telemetry.nodeProcessingUs["p1::amp"] = 25.0;
    state.telemetry.totalProcessingUs = 100.0;
    state.telemetry.nodeLatencySamples["p1::amp"] = 48.0;
    state.telemetry.sampleRate = 48000.0;
    state.telemetry.frameCounter = 1;
    DspStatusMeter meter("p1::amp");
    meter.Update(state, 0.0);
    Expect(meter.Value(DspStatusItem::Peak) == "-6.0 dBFS" && meter.Value(DspStatusItem::Headroom) == "6.0 dB",
           "a node's levels");
    Expect(meter.Value(DspStatusItem::Processing) == "25.0 \xCE\xBCs (25.0%)", "its time and share of all processing");
    Expect(meter.Value(DspStatusItem::Latency) == "48 smp (1.00 ms)", "its latency");
    Expect(meter.TakeRenderTurn(0.0) && !meter.TakeRenderTurn(0.2) && meter.TakeRenderTurn(0.6),
           "redrawn twice a second");

    state.telemetry.peakDspLoadPercent = 40.0;
    DspStatusMeter output;
    output.Update(state, 0.0);
    state.telemetry.peakDspLoadPercent = 10.0;
    state.telemetry.frameCounter = 2;
    output.Update(state, 5.0);
    Expect(output.Value(DspStatusItem::DspPeak) == "40.0%", "the load peak holds the worst block of the window");
    output.Update(state, 11.0);
    Expect(output.Value(DspStatusItem::DspPeak) == "10.0%", "and lets it go after ten seconds");
    Expect(output.Value(DspStatusItem::Latency) == "0 smp (0.00 ms)", "a whole chain's zero latency is a figure");
}

// Which cables light with the signal (signalFlow.ts): a node without its own reading hears what
// feeds it, and a node stays lit a moment through quiet.
void TestSignalFlow()
{
    ClientState state;
    Expect(!IsSignalFlowAnimationEnabled(state), "off unless turned on");
    state.appSettings[kSignalFlowAnimationSetting] = true;
    Expect(IsSignalFlowAnimationEnabled(state), "the setting turns it on");

    Preset preset;
    preset.id = "p1";
    preset.graph.nodes = {Node("__input__", kNodeTypeInput), Node("split", "splitter"), Node("amp", EffectGuids::kGain),
                          Node("__output__", kNodeTypeOutput)};
    preset.graph.edges = {GraphEdge{"__input__", "split", 0, 0, 1.0}, GraphEdge{"split", "amp", 0, 0, 1.0},
                          GraphEdge{"amp", "__output__", 0, 0, 1.0}};
    state.activePreset = preset;
    state.activePresetId = "p1";
    state.telemetry.input.rmsDb = -30.0;
    state.telemetry.output.rmsDb = -80.0;
    state.telemetry.nodes["p1::amp"] = LevelReading{-20.0, -70.0, 0, false, 1};

    SignalFlow flow;
    Expect(flow.Update(state, 0.0), "the first frame changes what is lit");
    Expect(flow.IsLive("__input__") && flow.IsLive("split") && !flow.IsLive("amp") && !flow.IsLive("__output__"),
           "a node with no reading hears its source; a quiet one is dark");
    state.telemetry.input.rmsDb = -120.0;
    flow.Update(state, 0.2);
    Expect(flow.IsLive("__input__"), "held through a short quiet");
    flow.Update(state, 0.5);
    Expect(!flow.AnyLive(), "and let go after it");
}

// MIDI and automation against a real engine: the slots and registry, a control's MIDI Learn menu
// (learn, capture, clear, per preset), keyboard mappings run by the engine (key learn, firing,
// chords left alone), and the MIDI Log; then midiMapping.ts's pure helpers.
void TestAutomation()
{
    LoopbackHost host(Sandbox("automation"));
    PluginController controller(host);
    controller.Initialize();
    UiClient client([&controller](const std::string& json) { controller.HandleUIMessage(json); });
    host.client = &client;
    UiCommands commands(client);
    client.Start();
    client.Send("getEffectCatalog");
    Pump(controller, client);

    const auto& state = client.State();
    const auto& automation = state.automation;
    Expect(automation.loaded && automation.FindRegistryEntry("global.inputTrim") != nullptr &&
               FindSlotForAddress(automation.slots, "global.inputTrim") != nullptr,
           "the slots and registry come with Start");

    // ── MIDI Learn from a control's menu ──
    const auto target = NodeParamLearnTarget(state.FindEffectType(EffectGuids::kGain), "gainDb");
    Expect(target && target->address == std::string("node.") + EffectGuids::kGain + ".gainDb" &&
               target->label == "Gain: Gain",
           "an effect parameter drives node.<type>.<key>");
    Expect(!NodeParamLearnTarget(state.FindEffectType(EffectGuids::kGain), "noSuchParam"),
           "an undeclared parameter drives nothing");

    if (!target)
    {
        return;
    }

    auto items = BuildMidiLearnMenuItems(automation, *target, state.activePresetId);
    Expect(!items.empty() && items[0].action == MidiLearnMenuAction::Learn && !items[0].disabled &&
               (state.activePresetId.empty() ||
                (items.size() == 2 && items[1].action == MidiLearnMenuAction::LearnPreset)),
           "a control nothing maps offers MIDI Learn, and for this preset");

    Expect(RunMidiLearnMenuAction(commands, state, MidiLearnMenuAction::Learn, *target).empty(), "a learn starts");
    Pump(controller, client);
    const auto* learning = FindSlotForAddress(automation.slots, target->address);
    Expect(learning != nullptr && learning->slotId == "custom.1" && automation.midiLearnSlotId == "custom.1",
           "a custom slot is made for it and armed");
    items = BuildMidiLearnMenuItems(automation, *target, state.activePresetId);
    Expect(!items.empty() && items[0].action == MidiLearnMenuAction::Cancel, "while it listens the menu offers Cancel");

    controller.EnqueueMidi(MidiEvent{0xB0, 21, 64, 0});
    controller.ProcessQueuedMidi();
    Pump(controller, client);
    const auto* mapped = automation.FindSlot("custom.1");
    Expect(automation.capture && automation.capture->slotId == "custom.1" && automation.midiLearnSlotId.empty(),
           "the capture is reported and the learn ends");
    Expect(mapped != nullptr && mapped->midiMap && DescribeMidiMap(*mapped->midiMap) == "CC 21 ch1 Abs",
           "the slot is mapped to CC 21");

    const auto cleared = RunMidiLearnMenuAction(commands, state, MidiLearnMenuAction::Clear, *target);
    Pump(controller, client);
    Expect(cleared.rfind("Cleared MIDI mapping for ", 0) == 0 && automation.FindSlot("custom.1") != nullptr &&
               !automation.FindSlot("custom.1")->midiMap,
           "Clear drops the mapping and keeps the slot");

    if (!state.activePresetId.empty())
    {
        RunMidiLearnMenuAction(commands, state, MidiLearnMenuAction::LearnPreset, *target);
        Pump(controller, client);
        const auto* own = FindPresetSlotForAddress(automation.slots, state.activePresetId, target->address);
        Expect(own != nullptr && own->slotId == "preset.1" && automation.midiLearnSlotId == "preset.1",
               "MIDI Learn for this preset learns into the preset's own slot");
        RunMidiLearnMenuAction(commands, state, MidiLearnMenuAction::Cancel, *target);
        Expect(automation.midiLearnSlotId.empty(), "Cancel stops listening");
    }

    // ── Keyboard mappings, run by the engine ──
    const auto* input = FindSlotForAddress(automation.slots, "global.inputTrim");
    const std::string inputSlot = input != nullptr ? input->slotId : std::string();
    commands.ArmKeyLearn(inputSlot);
    Pump(controller, client);
    Expect(automation.keyLearnSlotId == inputSlot, "the engine says which slot its key learn waits on");
    commands.AutomationKey("Shift", false);
    commands.AutomationKey(" ", false);
    commands.AutomationKey("W", false);
    Pump(controller, client);
    input = automation.FindSlot(inputSlot);
    Expect(automation.keyLearnSlotId.empty() && input != nullptr && input->keyMap.size() == 1 &&
               input->keyMap[0].key == "w" && input->keyMap[0].mode == 1 &&
               std::abs(input->keyMap[0].value - 0.5) < 1e-6,
           "a modifier and Space are waited through; the next key is stored in lower case, setting the level half way");
    Expect(KeyIsMapped(automation, "w") && KeyIsMapped(automation, "W") && !KeyIsMapped(automation, " ") &&
               !KeyIsMapped(automation, "x"),
           "a UI can tell which keys to hold back");

    commands.AutomationKey("w", false, true);
    commands.GetAutomation();
    Pump(controller, client);
    input = automation.FindSlot(inputSlot);
    Expect(input != nullptr && std::abs(input->value - 0.5) > 1e-3,
           "a chord leaves the mapping alone (Ctrl+W is a shortcut)");
    commands.AutomationKey("W", false);
    commands.GetAutomation();
    Pump(controller, client);
    input = automation.FindSlot(inputSlot);
    Expect(input != nullptr && std::abs(input->value - 0.5) < 1e-3, "the bare key fires the slot");

    commands.ArmKeyLearn(inputSlot);
    commands.AutomationKey("Escape", false);
    Pump(controller, client);
    Expect(automation.keyLearnSlotId.empty() && automation.FindSlot(inputSlot)->keyMap.size() == 1,
           "Escape backs out of a key learn");

    // ── The MIDI Log ──
    controller.EnqueueMidi(MidiEvent{0xB0, 7, 99, 0});
    controller.ProcessQueuedMidi();
    Pump(controller, client);
    Expect(automation.midiLog.empty(), "nothing is logged until the log is on");
    commands.SetMidiLogEnabled(true);
    Pump(controller, client);
    controller.EnqueueMidi(MidiEvent{0x91, 7, 100, 0});
    controller.ProcessQueuedMidi();
    Pump(controller, client);
    Expect(!automation.midiLog.empty() && automation.midiLog.back().channel == 1 &&
               automation.midiLog.back().data1 == 7 && automation.midiLog.back().data2 == 100,
           "an incoming event is logged");
    commands.SetMidiLogEnabled(false);

    // ── The pure helpers ──
    std::vector<AutomationSlotInfo> slots(2);
    slots[0].slotId = "custom.1";
    slots[1].slotId = "custom.4";
    Expect(NextCustomSlotId(slots) == "custom.5" && NextPresetSlotId(slots) == "preset.1", "the next free slot ids");
    Expect(DescribeMidiMap(1, -1, 5, std::nullopt) == "PC 5 chany" && FormatMidiChannel(15) == "16",
           "mappings read as musicians count");
    Expect(IsBypassAddress("node.x.bypassed") && !IsBypassAddress("node.x.mix") &&
               !AllowsPresetMapping("setlist.bankUp") && AllowsPresetMapping("global.outputTrim"),
           "bypass addresses, and which controls take per-preset mappings");
    const auto options = BuildTargetOptions(state);
    Expect(
        std::any_of(options.begin(), options.end(), [](const auto& o) { return o.address == "node.input.gainDb"; }) &&
            WithCurrentTargetOption(options, "node.unknown.x", "X").front().group == "Current",
        "the target picker offers the chain's gains, and a slot's own address");
}

// The metronome's meter picker and where a bar's groups start (metronomeMeters.ts).
void TestInputLevelling()
{
    LoopbackHost host(Sandbox("input-levelling"));
    PluginController controller(host);
    controller.Initialize();
    controller.Prepare(48000.0, 256);
    UiClient client([&controller](const std::string& json) { controller.HandleUIMessage(json); });
    host.client = &client;
    UiCommands commands(client);
    client.Start();
    commands.GetInputLevelling();
    Pump(controller, client);

    const auto& levelling = client.State().inputLevelling;
    Expect(levelling.known && levelling.profiles.empty() && levelling.activeId.empty() && !levelling.trainingActive,
           "a new install has no levelling profiles");
    Expect(std::abs(levelling.targetPeakDbfs + 12.0) < 1e-9 && std::abs(levelling.maxTrimDb - 24.0) < 1e-9,
           "the engine names the target and the trim limit");

    // The raw input, as the audio thread hands it over.
    std::vector<float> left(256), right(256), outL(256), outR(256);
    const auto play = [&](float amplitude, int blocks) {
        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < 256; ++i)
            {
                left[static_cast<std::size_t>(i)] = amplitude * static_cast<float>(std::sin(0.05 * (b * 256 + i)));
                right[static_cast<std::size_t>(i)] = left[static_cast<std::size_t>(i)];
            }

            float* ins[2] = {left.data(), right.data()};
            float* outs[2] = {outL.data(), outR.data()};
            controller.ProcessAudio(ins, outs, 256);
        }

        // The training status goes at ~15 Hz; let the next one be due.
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    };
    const auto closeTo = [](std::optional<double> value, double expected, double tolerance) {
        return value && std::abs(*value - expected) < tolerance;
    };

    // ── Training follows the raw input's peak ──
    commands.StartInputLevellingTraining();
    Pump(controller, client);
    Expect(levelling.trainingActive && levelling.training.active && !levelling.training.capturedPeakDbfs,
           "training starts from silence");
    Expect(InputLevellingTrainingStatus(levelling).find("Play your loudest") == 0, "and asks for a peak");

    play(0.1f, 20); // -20 dBFS
    Pump(controller, client);
    Expect(closeTo(levelling.training.capturedPeakDbfs, -20.0, 0.1), "the captured peak is the raw input's");
    Expect(closeTo(levelling.training.recommendedTrimDb, 8.0, 0.1), "the trim is target - captured");
    Expect(!levelling.training.clipped && CanSaveInputLevellingTraining(levelling), "an unclipped peak can be saved");
    Expect(InputLevellingTrainingStatus(levelling) ==
               "Captured -20.0 dBFS. Saving this profile will apply +8.0 dB toward a -12.0 dBFS target.",
           "the status says what saving applies");

    play(0.02f, 10); // quieter playing does not lower the captured peak
    Pump(controller, client);
    Expect(closeTo(levelling.training.capturedPeakDbfs, -20.0, 0.1), "the captured peak holds the loudest");

    // ── Saving ──
    commands.SaveInputLevellingProfile("  ", "");
    Pump(controller, client);
    Expect(levelling.saveSerial == 1 && !levelling.saveOk && levelling.profiles.empty(), "a profile needs a name");

    commands.SaveInputLevellingProfile("Strat", "Bridge pickup");
    Pump(controller, client);
    Expect(levelling.saveSerial == 2 && levelling.saveOk && levelling.profiles.size() == 1, "a named profile saves");
    const auto* saved = levelling.Active();
    Expect(saved && saved->name == "Strat" && saved->description == "Bridge pickup" &&
               std::abs(saved->gainDb - 8.0) < 0.1 && closeTo(saved->capturedPeakDbfs, -20.0, 0.1) &&
               !saved->createdAt.empty(),
           "and becomes the active profile");
    Expect(!levelling.trainingActive && !levelling.training.active, "saving ends the training");
    Expect(std::abs(controller.GetMixer().GetUserInputCalibrationGainDb() - 8.0) < 0.1,
           "the new trim reaches the mixer");
    const auto& settings = client.State().appSettings;
    Expect(settings.contains("audio.userInputCalibration.profiles") &&
               settings["audio.userInputCalibration.profiles"].size() == 1 &&
               settings.value("audio.userInputCalibration.activeProfileId", std::string{}) == (saved ? saved->id : "x"),
           "the settings keep the older keys");
    const std::string stratId = saved ? saved->id : std::string{};

    // ── A clip blocks the save; Reset starts the capture again ──
    commands.StartInputLevellingTraining();
    Pump(controller, client);
    Expect(std::abs(controller.GetMixer().GetUserInputCalibrationGainDb()) < 1e-9, "training bypasses the active trim");
    play(1.5f, 4);
    Pump(controller, client);
    Expect(levelling.training.clipped && !CanSaveInputLevellingTraining(levelling) &&
               InputLevellingTrainingStatus(levelling).find("Input clipped") == 0,
           "a clipped input is reported");
    commands.SaveInputLevellingProfile("Hot", "");
    Pump(controller, client);
    Expect(!levelling.saveOk && levelling.profiles.size() == 1, "a clipped training is not saved");
    commands.ResetInputLevellingTraining();
    Pump(controller, client);
    Expect(!levelling.training.clipped && !levelling.training.capturedPeakDbfs, "Reset forgets the peak and the clip");
    commands.StopInputLevellingTraining();
    Pump(controller, client);
    Expect(!levelling.trainingActive && std::abs(controller.GetMixer().GetUserInputCalibrationGainDb() - 8.0) < 0.1,
           "stopping restores the active trim");

    // ── Rename, select, delete ──
    commands.RenameInputLevellingProfile(stratId, "Strat bridge", "Neck off");
    Pump(controller, client);
    Expect(levelling.Active() && levelling.Active()->name == "Strat bridge" &&
               levelling.Active()->description == "Neck off",
           "a profile renames");

    commands.SelectInputLevellingProfile("");
    Pump(controller, client);
    Expect(levelling.activeId.empty() && std::abs(controller.GetMixer().GetUserInputCalibrationGainDb()) < 1e-9,
           "No levelling clears the trim");
    commands.SelectInputLevellingProfile(stratId);
    Pump(controller, client);
    Expect(levelling.activeId == stratId && std::abs(controller.GetMixer().GetUserInputCalibrationGainDb() - 8.0) < 0.1,
           "selecting a profile applies it");

    commands.DeleteInputLevellingProfile(stratId);
    Pump(controller, client);
    Expect(
        levelling.profiles.empty() && levelling.activeId.empty() &&
            std::abs(controller.GetMixer().GetUserInputCalibrationGainDb()) < 1e-9 &&
            controller.GetAppSettings().value("audio.userInputCalibration.activeProfileId", nlohmann::json()).is_null(),
        "deleting the active profile leaves no levelling");

    // ── Profiles saved by an older build carry over ──
    client.Send(
        "setSetting",
        {{"key", "audio.userInputCalibration.profiles"},
         {"value", nlohmann::json::array(
                       {{{"id", "old"}, {"name", " Tele "}, {"capturedPeakDbfs", -18.0}, {"targetPeakDbfs", -12.0}},
                        {{"id", "loud"}, {"name", "Loud"}, {"gainDb", 40.0}},
                        {{"id", ""}, {"name", "No id"}}})}});
    client.Send("setSetting", {{"key", "audio.userInputCalibration.activeProfileId"}, {"value", "old"}});
    Pump(controller, client);
    Expect(levelling.profiles.size() == 2 && levelling.Active() && levelling.Active()->name == "Tele" &&
               std::abs(levelling.Active()->gainDb - 6.0) < 1e-9,
           "an older profile's trim is target - captured");
    Expect(levelling.Find("loud") && std::abs(levelling.Find("loud")->gainDb - 24.0) < 1e-9,
           "and a trim is held to 24 dB");
    Expect(std::abs(controller.GetMixer().GetUserInputCalibrationGainDb() - 6.0) < 1e-9,
           "the older active profile applies");

    // The older training switch still bypasses.
    client.Send("setUserInputCalibrationTrainingActive", {{"active", true}});
    Pump(controller, client);
    Expect(levelling.trainingActive && std::abs(controller.GetMixer().GetUserInputCalibrationGainDb()) < 1e-9,
           "setUserInputCalibrationTrainingActive still starts training");
    client.Send("setUserInputCalibrationTrainingActive", {{"active", false}});
    Pump(controller, client);
    Expect(!levelling.trainingActive && std::abs(controller.GetMixer().GetUserInputCalibrationGainDb() - 6.0) < 1e-9,
           "and stops it");
}

void TestMetronomeMeters()
{
    const auto& meters = TimeSignatures();
    Expect(meters.size() == 21 && meters.front().id == "2/4" && meters.back().id == "7/8:2+2+3" &&
               meters.back().label == "7/8 (2+2+3)",
           "the web UI's meters, in its order");
    Expect((ParseGrouping("2+2+3", 7) == std::vector<int>{2, 2, 3}), "a grouping that adds up");
    Expect(ParseGrouping("2+2", 7).empty() && ParseGrouping("2+x+3", 7).empty(),
           "one that does not, or is malformed, is dropped");
    Expect((GroupHeads(7, 8, "2+2+3") == std::vector<bool>{true, false, true, false, true, false, false}),
           "odd meter group heads");
    Expect((GroupHeads(6, 8, "") == std::vector<bool>{true, false, false, true, false, false}),
           "a compound meter falls into threes");
    Expect((GroupHeads(4, 4, "") == std::vector<bool>{true, false, false, false}), "a simple meter has one head");
}

// The update check's version comparison (updateCheck.ts toNumericSemver / isVersionNewer).
void TestUpdateCheckVersions()
{
    Expect((ParseVersion("v1.7.2-beta") == std::array<int, 3>{1, 7, 2}), "a prefix and a pre-release suffix");
    Expect((ParseVersion("2") == std::array<int, 3>{2, 0, 0}) && (ParseVersion("1.8") == std::array<int, 3>{1, 8, 0}),
           "missing parts are 0");
    Expect(!ParseVersion("") && !ParseVersion("latest") && !ParseVersion("1.2.3 extra"), "text that is not a version");
    Expect(IsVersionNewer("1.10.0", "1.9.9") && !IsVersionNewer("1.6.0", "1.6.0") && !IsVersionNewer("1.5.9", "1.6.0"),
           "compared part by part, numerically");
    Expect(!IsVersionNewer("garbage", "1.0.0") && !IsVersionNewer("2.0.0", ""), "an unreadable side is never newer");
}

// The engine applies a global chain edit without echoing it, so the client mirrors it: a switch
// that read the old state back would send the same state again, and a reset would not show.
void TestGlobalChainEditsMirror()
{
    LoopbackHost host(Sandbox("global-chain-mirror"));
    PluginController controller(host);
    controller.Initialize();
    UiClient client([&controller](const std::string& json) { controller.HandleUIMessage(json); });
    host.client = &client;
    UiCommands commands(client);
    client.Start();
    Pump(controller, client);

    int notified = 0;
    auto subscription = client.Subscribe(Topic::GlobalChain, [&notified] { ++notified; });
    const auto& chain = client.State().globalChain;
    const auto* gate = chain.preChainGraph.FindNode("global_gate");
    const auto* doubler = chain.postChainGraph.FindNode("global_doubler");
    Expect(gate != nullptr && doubler != nullptr, "the global chain arrives with its gate and doubler");

    if (gate == nullptr || doubler == nullptr)
    {
        return;
    }

    const bool gateWas = gate->enabled;
    commands.SetGlobalChainToggle("gate.enabled", !gateWas);
    commands.SetGlobalChainParam("gate.threshold", -44.0);
    commands.SetGlobalChainToggle("doubler.enabled", true);
    commands.SetGlobalChainParam("doubler.delay", 12.0);
    commands.SetGlobalChainParam("output.gain", -3.0);
    Expect(notified == 5, "each edit tells the controls");
    Expect(gate->enabled == !gateWas && gate->params.at("threshold") == -44.0,
           "the gate's switch and threshold are mirrored");
    Expect(doubler->enabled && doubler->params.at("time") == 12.0, "the doubler's delay is its node's time");
    Expect(chain.outputGain == -3.0, "the output level");

    // What the engine now holds agrees with the mirror.
    client.Send("getGlobalChain");
    Pump(controller, client);
    const auto* engineGate = client.State().globalChain.preChainGraph.FindNode("global_gate");
    const auto* engineDoubler = client.State().globalChain.postChainGraph.FindNode("global_doubler");
    Expect(engineGate != nullptr && engineGate->enabled == !gateWas && engineGate->params.at("threshold") == -44.0,
           "the engine's gate matches");
    Expect(engineDoubler != nullptr && engineDoubler->enabled && engineDoubler->params.at("time") == 12.0,
           "the engine's doubler matches");
    Expect(client.State().globalChain.outputGain == -3.0, "the engine's output level matches");
}

// Library export goes to the engine (exportLibraryArchive), and its answer comes back as a
// notification; this host's save dialog is always cancelled.
void TestLibraryExportAnswer()
{
    LoopbackHost host(Sandbox("library-export"));
    PluginController controller(host);
    controller.Initialize();
    UiClient client([&controller](const std::string& json) { controller.HandleUIMessage(json); });
    host.client = &client;
    UiCommands commands(client);
    client.Start();
    Pump(controller, client);
    (void)client.TakeNotifications();

    commands.ExportLibraryArchive(true);
    Pump(controller, client);
    const auto notes = client.TakeNotifications();
    const bool failed = std::any_of(notes.begin(), notes.end(), [](const Notification& n) {
        return n.kind == Notification::Kind::Error && n.title == "Library export failed" && !n.detail.empty();
    });
    Expect(failed, "a cancelled or refused library export is told as a notification");
}

// The riff library and a capture, as the engine reports them (PluginControllerRiffs.cpp).
void TestRiffMessages()
{
    std::vector<nlohmann::json> sent;
    UiClient client([&sent](const std::string& json) { sent.push_back(nlohmann::json::parse(json)); });
    UiCommands commands(client);
    const auto deliver = [&client](const nlohmann::json& m) {
        client.Enqueue(m.dump());
        client.DrainPending();
    };
    const auto& riffs = client.State().riffs;

    deliver({{"type", "riffLibraryState"},
             {"library",
              {{"path", "C:/riffs"},
               {"riffs",
                {{{"id", "r1"},
                  {"title", "Chug"},
                  {"favorite", true},
                  {"tags", {"metal", "drop d"}},
                  {"preferredTakeId", "t2"},
                  {"takes",
                   {{{"id", "t1"},
                     {"bars", 2},
                     {"tempoBpm", 120.0},
                     {"timeSigNum", 4},
                     {"timeSigDen", 4},
                     {"durationSec", 4.0}},
                    {{"id", "t2"},
                     {"bars", 4},
                     {"tempoBpm", 120.0},
                     {"timeSigNum", 4},
                     {"timeSigDen", 4},
                     {"durationSec", 8.0}}}}},
                 {{"id", "r2"},
                  {"title", "Lead"},
                  {"used", true},
                  {"usedSongTitle", "Song"},
                  {"takes", nlohmann::json::array()}}}}}}});
    Expect(riffs.known && riffs.path == "C:/riffs" && riffs.riffs.size() == 2, "the riff library is read");
    const auto* chug = riffs.Find("r1");
    Expect(chug != nullptr && chug->PreferredTake() != nullptr && chug->PreferredTake()->id == "t2",
           "a riff plays its preferred take");
    Expect(chug != nullptr &&
               RiffTakeSummary(*chug->PreferredTake()) == "4 bars \xC2\xB7 120 BPM \xC2\xB7 4/4 \xC2\xB7 0:08",
           "a take in one line");
    Expect(chug != nullptr && RiffMeta(*chug) == "Tags: metal, drop d \xC2\xB7 Unused \xC2\xB7 Takes: 2",
           "a riff's tags, use and takes");
    Expect(riffs.Find("r2") != nullptr && riffs.Find("r2")->PreferredTake() == nullptr &&
               RiffMeta(*riffs.Find("r2")).find("Used (Song)") != std::string::npos,
           "a riff with no takes, used in a song");
    Expect(client.State().demo.riffClips.size() == 1 && client.State().demo.riffClips[0].takeId == "t2",
           "favourites are still demo clips");

    // Armed with 64 bars, as the web UI arms, so the live waveform's scale matches.
    commands.ArmRiffCapture(120.0, 4, 4, 1, true);
    Expect(!sent.empty() && sent.back().value("type", "") == "armRiffCapture" && sent.back().value("bars", 0) == 64,
           "a capture is armed for 64 bars");

    deliver({{"type", "riffCaptureArmed"},
             {"takeId", "k1"},
             {"tempoBpm", 120.0},
             {"timeSigNum", 4},
             {"timeSigDen", 4},
             {"bars", 64}});
    Expect(riffs.capture.phase == RiffCapture::Phase::Armed &&
               RiffCaptureStatus(riffs.capture, 48000.0).rfind("Armed", 0) == 0,
           "armed");

    deliver({{"type", "riffCaptureStarted"},
             {"takeId", "k1"},
             {"bars", 64},
             {"tempoBpm", 120.0},
             {"timeSigNum", 4},
             {"timeSigDen", 4},
             {"countInBars", 0},
             {"barAlignOffsetSamples", 4800}});
    Expect(riffs.capture.phase == RiffCapture::Phase::Recording && riffs.capture.barAlignOffsetSamples == 4800,
           "recording from the first note");

    // A bar at 120 BPM in 4/4 is 96000 samples at 48 kHz; the buffer is 64 of them.
    deliver({{"type", "riffCaptureProgress"},
             {"capturedSamples", 614400},
             {"waveformPeaks", std::vector<double>(256, 0.25)}});
    Expect(riffs.capture.peaks.size() == 256 && std::abs(RiffCaptureFill(riffs.capture, 48000.0) - 0.1) < 1e-9,
           "the live trace stops a tenth of the way through the buffer");
    Expect(RiffCaptureStatus(riffs.capture, 48000.0) == "Recording \xC2\xB7 ~7 bars @ 120 BPM",
           "and says how many bars so far");

    deliver({{"type", "riffCaptureStopped"},
             {"takeId", "k1"},
             {"bars", 4},
             {"capturedSamples", 384000},
             {"sampleRate", 48000.0},
             {"hasAudio", true},
             {"waveformPeaks", std::vector<double>(256, 0.5)}});
    Expect(riffs.capture.phase == RiffCapture::Phase::Complete && riffs.capture.serial == 1 &&
               riffs.capture.barAlignOffsetSamples == 4800,
           "a captured take keeps where its first bar starts");
    Expect(RiffCaptureStatus(riffs.capture, 0.0) == "Captured \xC2\xB7 8.0 s, 4 bars", "and says how long it is");

    deliver({{"type", "riffCaptureStopped"},
             {"takeId", "k1"},
             {"bars", 2},
             {"capturedSamples", 192000},
             {"sampleRate", 48000.0},
             {"hasAudio", true},
             {"source", "trim"},
             {"tempoBpm", 120.0},
             {"timeSigNum", 4},
             {"timeSigDen", 4}});
    Expect(riffs.capture.serial == 2 && riffs.capture.barAlignOffsetSamples == 0 && riffs.capture.source == "trim",
           "a cropped take starts where it was cut");

    deliver({{"type", "riffSaved"}, {"riffId", "r3"}, {"takeId", "k1"}, {"path", "C:/riffs/takes/r3/x.wav"}});
    Expect(riffs.capture.phase == RiffCapture::Phase::Idle && riffs.savedSerial == 1 && riffs.lastSavedRiffId == "r3",
           "saving ends the take");

    // A refused capture never starts: the dialog must not wait on it.
    deliver(
        {{"type", "riffCaptureArmed"}, {"takeId", "k2"}, {"tempoBpm", 100.0}, {"timeSigNum", 3}, {"timeSigDen", 4}});
    deliver({{"type", "error"}, {"message", "Riff Capture"}, {"detail", "No audio device"}});
    Expect(riffs.capture.phase == RiffCapture::Phase::Idle, "a refused capture is dropped");

    deliver({{"type", "previewStarted"}, {"id", "t2"}, {"title", "Chug"}});
    Expect(client.State().demo.previewId == "t2", "what is playing is known by its id");
    deliver({{"type", "previewStopped"}});
    Expect(client.State().demo.previewId.empty(), "and forgotten when it stops");
    Expect(SplitRiffList(" a, b ,,c ") == std::vector<std::string>{"a", "b", "c"}, "the save form's lists");
}

// The looper's state, layers and waveforms (LooperService*.cpp), and the feature switch the
// engine's looper follows.
void TestLooperMessages()
{
    std::vector<nlohmann::json> sent;
    UiClient client([&sent](const std::string& json) { sent.push_back(nlohmann::json::parse(json)); });
    const auto deliver = [&client](const nlohmann::json& m) {
        client.Enqueue(m.dump());
        client.DrainPending();
    };
    const auto setLoopers = [&sent] {
        return std::count_if(sent.begin(), sent.end(),
                             [](const nlohmann::json& m) { return m.value("type", "") == "setLooper"; });
    };
    const auto& looper = client.State().looper;
    client.MutableState().appSettings["features.looper.enabled"] = true;

    const nlohmann::json playing = {{"type", "looperState"},
                                    {"available", true},
                                    {"state", "playing"},
                                    {"playing", true},
                                    {"positionSec", 1.0},
                                    {"lengthSec", 4.0},
                                    {"bars", 4},
                                    {"barIndex", 2},
                                    {"layerCount", 2},
                                    {"settings", {{"enabled", false}, {"length", "bars"}, {"bars", 4}}}};
    deliver(playing);
    Expect(looper.known && looper.state == "playing" && looper.settings.length == "bars", "the looper's state is read");
    Expect(setLoopers() == 1, "the engine's looper is turned on to follow the switch");
    deliver(playing);
    Expect(setLoopers() == 1, "once, until the engine agrees");
    Expect(LooperMainButtonLabel(looper.state) == "Dub" && LooperStatusLabel(looper.state) == "Playing",
           "the state button overdubs next");
    Expect(LooperBarText(looper) == "bar 2 of 4 \xC2\xB7 2 layers", "the bar readout");
    Expect(LooperTimeText(looper, 1.0) == "0:01.0 / 0:04.0", "the time readout");
    Expect(std::abs(LooperPositionNow(looper, looper.positionAt + 3.5) - 0.5) < 1e-9,
           "the playhead runs on and wraps by itself");

    deliver({{"type", "looperLayers"},
             {"layers",
              {{{"id", "L2"}, {"title", "Layer 2"}, {"order", 2}, {"muted", true}, {"level", 1.0}},
               {{"id", "L1"},
                {"title", "Layer 1"},
                {"order", 1},
                {"level", 0.5},
                {"tone", {{"presetName", "Clean"}}},
                {"retone", {{"progress", 0.25}}}}}}});
    Expect(looper.layers.size() == 2 && looper.layers[1].presetName == "Clean" &&
               looper.layers[1].retoneProgress == 0.25,
           "the layers are read");
    deliver({{"type", "looperWaveforms"},
             {"layers",
              {{"L1", {{"peaksL", std::vector<double>(256, 0.8)}, {"peaksR", std::vector<double>(256, 0.2)}}},
               {"L2", {{"peaksL", std::vector<double>(256, 1.0)}, {"peaksR", std::vector<double>(256, 1.0)}}}}}});
    const auto mix = LooperMixPeaks(looper);
    Expect(mix.size() == 256 && std::abs(mix[0] - 0.4f) < 1e-6f,
           "the waveform sums the audible layers (the muted one left out) at their level");
    deliver({{"type", "looperLayers"}, {"layers", {{{"id", "L1"}, {"order", 1}, {"level", 0.5}}}}});
    Expect(looper.layers.size() == 1 && looper.layers[0].peaksL.size() == 256,
           "a layer keeps its peaks when the list changes");

    auto withNotice = playing;
    withNotice["settings"]["enabled"] = true;
    withNotice["notice"] = "tooShort";
    (void)client.TakeNotifications();
    deliver(withNotice);
    const auto notes = client.TakeNotifications();
    Expect(notes.size() == 1 && notes[0].title == "Too short to loop.", "a notice is shown once, in words");

    client.MutableState().appSettings["features.looper.enabled"] = false;
    client.SyncLooperEnabled();
    Expect(setLoopers() == 2 && sent.back().value("enabled", true) == false,
           "switching the feature off turns the engine's looper off");
}

int main()
{
    TestClientMirrorsTheEngine();
    TestFactoryPackEffectPresetsApplyInTheEngine();
    TestGraphicEqFactoryProfilesApplyExactly();
    TestFactoryPresetParameterOrder();
    TestTelemetryDecoding();
    TestChainLayoutWalksParallelLanes();
    TestChainLayoutWalksNestedSplits();
    TestChainDropRules();
    TestFeatureFlagsAndFxMarks();
    TestPresetTrail();
    TestResourceIndex();
    TestAnalysis();
    TestAnalysisPresentation();
    TestSignalFlow();
    TestMetronomeMeters();
    TestUpdateCheckVersions();
    TestGlobalChainEditsMirror();
    TestLibraryExportAnswer();
    TestRiffMessages();
    TestLooperMessages();
    TestIrAlignmentResponse();
    TestAutomation();
    TestInputLevelling();
    TestNodeArchitectureBadge();
    TestPresetBrowse();
    TestParamFormatting();
    TestParamVisibility();
    TestChainEditCommands();
    TestInstruments();
    TestEffectPresentationTable();
    TestLayoutDecisions();
    TestToneSharing();
    TestChainHistory();
    TestTone3000AndHttp();

    if (gFailures > 0)
    {
        std::cerr << gFailures << " check(s) failed" << std::endl;
        return 1;
    }

    std::cout << "UiClientTests passed" << std::endl;
    return 0;
}
