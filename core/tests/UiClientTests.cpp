/**
 * UiClientTests.cpp - Soundshed Guitar Nano's client library, against a real engine.
 *
 * The client talks to a headless PluginController over the same JSON messages the WebView UI
 * uses, in process (the controller's outbound messages are queued into the client, the
 * client's requests go to HandleUIMessage), so these exercise the protocol end to end with no
 * JUCE involved. The presentation logic the views lean on - chain layout, preset filtering,
 * value formatting, tuner smoothing, tap tempo, the effect presentation table - is checked
 * against the web UI's behaviour it ports.
 */

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "IPluginHost.h"
#include "PluginController.h"
#include "presets/PresetStorage.h"
#include "uiclient/ChainLayout.h"
#include "uiclient/EffectPresentation.h"
#include "uiclient/Instruments.h"
#include "uiclient/LayoutMode.h"
#include "uiclient/NodeLabels.h"
#include "uiclient/ParamFormat.h"
#include "uiclient/PresetBrowse.h"
#include "uiclient/UiClient.h"
#include "uiclient/UiCommands.h"

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
        client.Send("savePreset", {{"saveMode", "overwrite"}, {"presetId", preset.id}, {"name", preset.name},
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
    Expect(state.activePreset->graph.FindNode("gain_1")->params.at("gain") == 4.5, "a param change is mirrored at once");
    Expect(controller.GetActivePreset()->graph.FindNode("gain_1")->params.at("gain") == 4.5, "and reaches the engine");

    // Stepping follows the engine's list order and wraps, as the web UI's previous/next.
    const auto& list = state.presetList;
    const auto index = [&list](const std::string& id) {
        return static_cast<int>(std::find_if(list.begin(), list.end(), [&id](const PresetSummary& p) { return p.id == id; }) -
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

void TestTelemetryDecoding()
{
    UiClient client([](const std::string&) {});
    client.Enqueue(R"({"type":"sldRoster","seq":7,"nodes":[["preset","p1","amp","x",0],["post","","global_eq","y",0]]})");
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
    graph.nodes = {Node("__input__", kNodeTypeInput), Node("drive", EffectGuids::kOverdrive),
                   Node("split", EffectGuids::kSplitter), Node("a", EffectGuids::kDelayDigital),
                   Node("b1", EffectGuids::kChorus),      Node("b2", EffectGuids::kPhaser),
                   Node("mix", EffectGuids::kMixer),      Node("rev", EffectGuids::kReverbRoom),
                   Node("__output__", kNodeTypeOutput)};
    graph.edges = {{"__input__", "drive", 0, 0, 1.0}, {"drive", "split", 0, 0, 1.0}, {"split", "b1", 1, 0, 1.0},
                   {"split", "a", 0, 0, 1.0},         {"a", "mix", 0, 0, 1.0},      {"b1", "b2", 0, 0, 1.0},
                   {"b2", "mix", 0, 1, 1.0},          {"mix", "rev", 0, 0, 1.0},    {"rev", "__output__", 0, 0, 1.0}};

    const auto layout = BuildChainLayout(graph);
    Expect(layout.items.size() == 4, "drive, the parallel block, its join and the reverb");
    Expect(layout.items[1].kind == ChainItem::Kind::Parallel && layout.items[1].collapsible,
           "a splitter/mixer pair is a collapsible parallel block");
    Expect(layout.items[1].branches.size() == 2 && layout.items[1].branches[0].nodeIds == std::vector<std::string>{"a"} &&
               layout.items[1].branches[1].nodeIds == std::vector<std::string>{"b1", "b2"},
           "branches in port order, each walked to the join");
    Expect(layout.items[2].nodeId == "mix" && layout.items[3].nodeId == "rev", "the join is drawn, then the chain goes on");
    Expect(layout.NodeBefore("b2") == "b1" && layout.NodeBefore("a") == "split" && layout.NodeBefore("drive") == kInputNodeId,
           "NodeBefore follows the branch a node is on");
}

void TestPresetBrowse()
{
    ClientState state;
    state.presetList = {{"t10", "Tone 10", "Lead", "user"}, {"t2", "tone 2", "Clean", "user"},
                        {"f1", "Factory Crunch", "Crunch", "factory"}, {"x", "", "Misc", "user"}};
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
    Expect(ids(FilterPresets(state, {kPresetFolderFavorites, ""})) == std::vector<std::string>{"t10", "x"}, "favourites");
    Expect(ids(FilterPresets(state, {kPresetFolderRecents, ""})) == std::vector<std::string>{"f1", "t10"},
           "recents keep their order");
    Expect(ids(FilterPresets(state, {"live", ""})) == std::vector<std::string>{"t2", "t10"}, "a folder includes its subfolders");
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
    Expect(FormatParamValue(pan, 0.004) == "C" && FormatParamValue(pan, -0.25) == "L25" && FormatParamValue(pan, 0.5) == "R50",
           "pan shows L/C/R");
    Expect(FormatParamValue(freq, 999.6) == "1.00kHz" && FormatParamValue(freq, 12500) == "12.5kHz" &&
               FormatParamValue(freq, 440) == "440Hz" && FormatParamValue(freq, 55.5) == "55.5Hz",
           "a log-taper value shows about three figures, kHz from 1 kHz");
    Expect(FormatParamValue(db, -3.456) == "-3.46dB", "otherwise two decimals and the unit");
    Expect(FormatParamValue(amount, 0.5) == "0.50", "an amount has no unit");
    Expect(FormatParamValue(db, -0.001) == "0.00dB", "never negative zero");
    Expect(FormatParamValue(toggle, 1) == "On", "a toggle is On or Off");

    Expect(std::abs(ParamPositionToValue(freq, 0.5) - std::sqrt(20.0 * 20000.0)) < 1e-6, "a log taper's middle is the geometric mean");
    Expect(std::abs(ParamValueToPosition(db, 12) - 0.75) < 1e-9, "a linear taper is linear");
    EffectParamInfo stepped{"s", "S", "", "", 0, 10, 0, 2.5};
    Expect(SnapParamValue(stepped, 6.4) == 7.5 && SnapParamValue(stepped, 42) == 10, "values snap to the step and clamp");
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
    const auto table = EffectPresentation::Load(fs::path(GUITARFX_DEMO_AUDIO_DIR) / ".." / "data" / "effect-presentation.json");
    Expect(table.IsLoaded(), "core/ui/data/effect-presentation.json loads");
    Expect(table.IconFor(EffectGuids::kDynamicsGate, "dynamics") == "door", "an effect's own icon");
    Expect(table.IconFor("unknown-type", "delay") == "delay", "else its category's");
    Expect(table.IconFor("unknown-type", "nope") == "gear", "else the default");
    Expect(table.CategoryColour("amp") == 0xffe07848 && table.CategoryColour("pedal") == 0xffe07848,
           "category colours, with pedal folded into amp");
    Expect(table.NodeClass("channel") == "amp" && table.NodeClass("drive") == "utility", "node classes as the web UI's");
    Expect(table.EquipmentImage(EffectGuids::kDelayTape, "delay") == "images/equipment/fx/studio-rack-delay.png" &&
               table.EquipmentImage("fx_nam", "amp") == "images/equipment/pedals/colourful-pedal2.png",
           "stock artwork by effect, legacy id and category");
    Expect(ParseCssColour("rgba(255, 0, 0, 0.5)").value_or(0) == 0x80ff0000 && ParseCssColour("#abc").value_or(0) == 0xffaabbcc,
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

int main()
{
    TestClientMirrorsTheEngine();
    TestTelemetryDecoding();
    TestChainLayoutWalksParallelLanes();
    TestPresetBrowse();
    TestParamFormatting();
    TestInstruments();
    TestEffectPresentationTable();
    TestLayoutDecisions();

    if (gFailures > 0)
    {
        std::cerr << gFailures << " check(s) failed" << std::endl;
        return 1;
    }

    std::cout << "UiClientTests passed" << std::endl;
    return 0;
}
