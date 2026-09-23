/**
 * AutomationNodeRealtimeTests.cpp — applying a node.* automation slot must allocate nothing on the
 * audio thread, and must drive the node the name-based lookups always chose.
 *
 * MIDI and DAW automation apply slots on the audio thread, under the DSP lock
 * (PluginController::ProcessQueuedMidi, the JUCE adapter's DAW drain). For a node.* address that
 * path used to parse the address, resolve the effect type, look its parameter up, list every node
 * of the type by id and queue a notification behind a mutex: 44 allocations for one CC. Now the
 * address is resolved when the slot is set (AutomationSlotTable::BindNodeAddress), the node is found
 * by walking a list each executor keeps (MultiPresetMixer::FindAutomationTarget), and what changed
 * is posted to a NodeChangeQueue.
 *
 * The CCs are sent from a thread standing in for the audio thread, which counts what it allocates:
 * a parameter by alias and by UUID, the Input node's gain, a bypass, a DAW write, a long sweep, and
 * the same again after a preset switch and after an effect type is re-registered. The node each one
 * reached is checked against MultiPresetMixer::FindFirstEnabledNodeOfType, which still resolves by
 * name. The queue is also run on its own, full and under a concurrent taker.
 */

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "automation/AutomationSlotTable.h"
#include "automation/NodeChangeQueue.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/MultiPresetMixer.h"
#include "dsp/effects/BuiltinEffects.h"
#include "helpers/AudioThreadAllocations.h"
#include "presets/PresetTypes.h"
#include "resources/ResourceLibrary.h"

using namespace audio_thread_allocations;
using namespace guitarfx;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 64;
// A slot holds its value as a float, so a mapped value is only float-accurate.
constexpr double kTolerance = 1e-4;
/// What ParamOf reads for a node that is not there. Not NaN: Release builds with fast math.
constexpr double kNoNode = -1.0e30;

constexpr int kGainCc = 20;
constexpr int kUuidGainCc = 21;
constexpr int kInputGainCc = 22;
constexpr int kDepthCc = 23;
constexpr int kBypassCc = 24;

constexpr const char* kDepthType = "automation_realtime_test_depth";

bool Expect(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << std::endl;
    }

    return condition;
}

bool ExpectNoAllocations(const Allocations& allocations, const std::string& what)
{
    std::cout << what << ": " << Describe(allocations) << std::endl;
    return Expect(allocations.count == 0, what + " allocated on the audio thread (" + Describe(allocations) + ")");
}

/// An effect type the test registers itself, so it can change the range its parameter declares.
class DepthProcessor final : public PassthroughProcessor
{
  public:
    void SetParam(const std::string& key, double value) override
    {
        if (key == "depth")
        {
            mDepth = value;
        }
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        return key == "depth" ? mDepth : 0.0;
    }

  private:
    double mDepth = 0.0;
};

void RegisterDepthEffect(double maxValue)
{
    EffectTypeInfo info;
    info.type = kDepthType;
    info.displayName = "Depth";
    info.category = "utility";
    info.parameters = {{"depth", "Depth", 0.0, 0.0, maxValue, ""}};
    EffectRegistry::Instance().Register(info.type, info, [] { return std::make_unique<DepthProcessor>(); });
}

/// Input, two gains, a depth node, Output. The first gain is stored under the alias and the
/// second under the UUID, and neither holds gainDb, so the executor's copy of the graph has no
/// key for it: the executor must not insert one on the audio thread.
Preset MakePreset(const std::string& presetId, const std::string& firstGainId)
{
    Preset preset;
    preset.id = presetId;
    preset.name = presetId;

    GraphNode in{"__input__", kNodeTypeInput, "", "Input", true};
    GraphNode first{firstGainId, "gain", "utility", "Gain", true};
    GraphNode second{"g2", EffectGuids::kGain, "utility", "Gain", true};
    GraphNode depth{"d1", kDepthType, "utility", "Depth", true};
    GraphNode out{"__output__", kNodeTypeOutput, "", "Output", true};

    preset.graph.nodes = {in, first, second, depth, out};
    preset.graph.edges = {GraphEdge{"__input__", firstGainId, 0, 0, 1.0}, GraphEdge{firstGainId, "g2", 0, 0, 1.0},
                          GraphEdge{"g2", "d1", 0, 0, 1.0}, GraphEdge{"d1", "__output__", 0, 0, 1.0}};
    return preset;
}

MidiControlMap CcMap(int controller, MidiControlMap::Mode mode = MidiControlMap::Mode::Absolute)
{
    MidiControlMap map;
    map.eventType = MidiControlMap::EventType::CC;
    map.channel = 0;
    map.controller = controller;
    map.mode = mode;
    return map;
}

bool BindSlot(AutomationSlotTable& table, const std::string& slotId, const std::string& address,
              const MidiControlMap& map)
{
    return Expect(table.SetCustomSlot(slotId, std::optional<std::string>(slotId), std::optional<std::string>(address),
                                      std::nullopt, std::optional<MidiControlMap>(map), std::nullopt),
                  "Failed to bind " + address);
}

void SendCc(AutomationSlotTable& table, int controller, int value)
{
    table.HandleMidi(MidiEvent{0xB0, static_cast<std::uint8_t>(controller), static_cast<std::uint8_t>(value), 0});
}

double ParamOf(const MultiPresetMixer& mixer, const std::string& presetId, const std::string& nodeId,
               const std::string& key)
{
    const auto* processor = mixer.GetNodeProcessor(presetId, nodeId);
    return processor ? processor->GetParam(key) : kNoNode;
}

/// Where a CC value lands on a declared linear range.
double MapCc(int value, double minValue, double maxValue)
{
    return minValue + (static_cast<double>(value) / 127.0) * (maxValue - minValue);
}

/// What the message thread would take: the value of each change, keyed "nodeId/paramId", or
/// "bypass/effectType" for a bypass.
std::map<std::string, double> TakeChanges(AutomationSlotTable& table)
{
    std::map<std::string, double> changes;
    std::size_t taken = 0;
    table.TakeNodeChanges([&](NodeChangeQueue::Change&& change) {
        ++taken;
        const auto key =
            change.nodeId ? *change.nodeId + "/" + change.binding->paramId : "bypass/" + change.binding->effectType;
        changes[key] = change.value;
    });

    // Two changes to one target in a single take would mean the queue failed to coalesce.
    return taken == changes.size() ? changes : std::map<std::string, double>{};
}

/// Whether FindFirstEnabledNodeOfType, which resolves by name, names the node `nodeId` of
/// `presetId` for the gain type in either of its forms.
bool NameLookupAgrees(const MultiPresetMixer& mixer, const std::string& presetId, const std::string& nodeId)
{
    const auto byAlias = mixer.FindFirstEnabledNodeOfType("gain");
    const auto byUuid = mixer.FindFirstEnabledNodeOfType(EffectGuids::kGain);
    const auto expected = std::make_pair(presetId, nodeId);
    return byAlias == expected && byUuid == expected;
}

bool TestQueueCapacity()
{
    bool passed = true;
    NodeChangeQueue queue;
    const auto nodeId = std::make_shared<const std::string>("node");
    std::vector<std::shared_ptr<const NodeAddressBinding>> bindings;

    for (std::size_t i = 0; i <= NodeChangeQueue::kCapacity; ++i)
    {
        auto binding = std::make_shared<NodeAddressBinding>();
        binding->effectType = "type";
        binding->paramId = "p" + std::to_string(i);
        bindings.push_back(std::move(binding));
    }

    std::size_t posted = 0;
    bool repostLanded = false;
    const auto allocations = CountOnAudioThread([&] {
        for (const auto& binding : bindings)
        {
            posted += queue.Post(binding, &nodeId, 1.0) ? 1 : 0;
        }

        // A target already waiting takes the new value in its own cell, so it lands even now.
        repostLanded = queue.Post(bindings.front(), &nodeId, 2.0);
    });

    passed &= ExpectNoAllocations(allocations, "Filling the queue");
    passed &= Expect(posted == NodeChangeQueue::kCapacity, "The queue should hold exactly its capacity");
    passed &= Expect(queue.TakeDroppedCount() == 1, "The change with no cell should be counted as dropped");
    passed &= Expect(queue.TakeDroppedCount() == 0, "Taking the dropped count should reset it");
    passed &= Expect(repostLanded, "A change to a target already waiting should land in a full queue");

    std::size_t taken = 0;
    double firstValue = 0.0;
    queue.Take([&](NodeChangeQueue::Change&& change) {
        ++taken;

        if (change.binding->paramId == "p0")
        {
            firstValue = change.value;
        }
    });

    passed &= Expect(taken == NodeChangeQueue::kCapacity && firstValue == 2.0,
                     "Take should hand over every change once, with its latest value");
    passed &= Expect(queue.Post(bindings.back(), &nodeId, 3.0), "Taken cells should be free for new changes");

    // Once taken, the queue holds the only other references, and gives them up.
    queue.Take([](NodeChangeQueue::Change&&) {});
    passed &= Expect(nodeId.use_count() == 1 && bindings.front().use_count() == 1,
                     "Taken changes should release what they named");
    return passed;
}

/// One poster, one taker, both flat out: every target's values must arrive in the order they
/// were posted, and each target's last value must arrive.
bool TestQueueOrderUnderConcurrentTake()
{
    constexpr int kTargets = 4;
    constexpr int kPostsPerTarget = 50000;

    NodeChangeQueue queue;
    const auto nodeId = std::make_shared<const std::string>("node");
    std::vector<std::shared_ptr<const NodeAddressBinding>> bindings;

    for (int i = 0; i < kTargets; ++i)
    {
        auto binding = std::make_shared<NodeAddressBinding>();
        binding->effectType = "type";
        binding->paramId = "p" + std::to_string(i);
        bindings.push_back(std::move(binding));
    }

    std::atomic<bool> posting{true};
    std::map<std::string, double> lastSeen;
    bool inOrder = true;

    const auto take = [&] {
        queue.Take([&](NodeChangeQueue::Change&& change) {
            auto& last = lastSeen[change.binding->paramId];
            inOrder = inOrder && change.value > last;
            last = change.value;
        });
    };

    std::thread taker([&] {
        while (posting.load(std::memory_order_acquire))
        {
            take();
        }
    });

    bool allPosted = true;

    for (int value = 1; value <= kPostsPerTarget; ++value)
    {
        for (const auto& binding : bindings)
        {
            allPosted = queue.Post(binding, &nodeId, static_cast<double>(value)) && allPosted;
        }
    }

    posting.store(false, std::memory_order_release);
    taker.join();
    take();

    bool passed = Expect(allPosted, "No change should be dropped with only a few targets");
    passed &= Expect(inOrder, "A target's changes should arrive in the order they were posted");

    for (const auto& binding : bindings)
    {
        passed &= Expect(lastSeen[binding->paramId] == kPostsPerTarget,
                         "The last change to " + binding->paramId + " should arrive");
    }

    return passed;
}
} // namespace

int main()
{
    RegisterAllEffects();
    RegisterDepthEffect(10.0);

    const auto* gainDb = EffectRegistry::Instance().FindParameter("gain", "gainDb");

    if (!gainDb)
    {
        std::cerr << "The gain effect must declare gainDb" << std::endl;
        return 1;
    }

    ResourceLibrary library;
    MultiPresetMixer mixer;
    mixer.SetResourceLibrary(&library);
    mixer.Prepare(kSampleRate, kBlockSize);

    const auto presetA = MakePreset("presetA", "g1");

    if (!mixer.AddActivePreset(presetA, presetA.id, presetA.name))
    {
        std::cerr << "Failed to add the first preset" << std::endl;
        return 1;
    }

    AutomationSlotTable table;
    table.InitializeRegistry(
        mixer, []() { return 0.0; }, [](int) {}, [](int) {}, [](int) {}, []() { return 0; }, []() { return 0; },
        [](int) {}, []() { return 0; }, [](int) {}, []() { return -1; });
    table.SetMixer(&mixer);
    table.SetEffectRegistry(&EffectRegistry::Instance());

    // The counter sees what the audio thread allocates, so a zero below means something.
    bool passed = Expect(CountOnAudioThread([] {
                             auto text = std::make_unique<std::string>(64, 'x');
                             (void)text;
                         }).count > 0,
                         "The allocation counter should see an allocation on the audio thread");

    passed &= BindSlot(table, "custom.gain", "node.gain.gainDb", CcMap(kGainCc));
    passed &=
        BindSlot(table, "custom.uuidGain", std::string("node.") + EffectGuids::kGain + ".gainDb", CcMap(kUuidGainCc));
    passed &= BindSlot(table, "custom.input", "node.input.gainDb", CcMap(kInputGainCc));
    passed &=
        BindSlot(table, std::string("custom.depth"), std::string("node.") + kDepthType + ".depth", CcMap(kDepthCc));
    passed &= BindSlot(table, "custom.bypass", "node.gain.bypassed", CcMap(kBypassCc, MidiControlMap::Mode::Toggle));

    // ── Nothing allocates, the very first event included ────────────────────────────────────

    const std::string dawSlotId = "custom.gain";
    passed &= ExpectNoAllocations(CountOnAudioThread([&] { SendCc(table, kGainCc, 127); }), "Gain by alias");
    passed &= Expect(std::abs(ParamOf(mixer, "presetA", "g1", "gainDb") - gainDb->maxValue) < kTolerance,
                     "CC 127 by alias should reach the top of the first gain node's range");

    passed &= ExpectNoAllocations(CountOnAudioThread([&] { SendCc(table, kUuidGainCc, 0); }), "Gain by UUID");
    passed &= Expect(std::abs(ParamOf(mixer, "presetA", "g1", "gainDb") - gainDb->minValue) < kTolerance,
                     "The UUID address should drive the same node, stored under the alias");
    passed &= Expect(ParamOf(mixer, "presetA", "g2", "gainDb") == 0.0, "The second gain node should be untouched");
    passed &= Expect(NameLookupAgrees(mixer, "presetA", "g1"), "The name-based lookup should name the same node");

    passed &= ExpectNoAllocations(CountOnAudioThread([&] { SendCc(table, kInputGainCc, 96); }), "Input node gain");
    passed &= ExpectNoAllocations(CountOnAudioThread([&] { SendCc(table, kDepthCc, 127); }), "Depth");
    passed &= Expect(std::abs(ParamOf(mixer, "presetA", "d1", "depth") - 10.0) < kTolerance,
                     "CC 127 should reach the top of the depth's declared range");

    bool dawApplied = false;
    passed &= ExpectNoAllocations(
        CountOnAudioThread([&] { dawApplied = table.ApplyAutomationLocked(dawSlotId, 0.25f, AutomationSource::DAW); }),
        "DAW write");
    passed &=
        Expect(dawApplied && std::abs(ParamOf(mixer, "presetA", "g1", "gainDb") -
                                      (gainDb->minValue + 0.25 * (gainDb->maxValue - gainDb->minValue))) < kTolerance,
               "A DAW value of 0.25 should land a quarter up the range");

    // An expression pedal's worth of CCs with no message thread taking the changes.
    constexpr int kSweep = 1000;
    passed &= ExpectNoAllocations(CountOnAudioThread([&] {
                                      for (int i = 0; i < kSweep; ++i)
                                      {
                                          SendCc(table, kGainCc, i % 128);
                                      }
                                  }),
                                  "A sweep of 1000 CCs");
    const double sweepEnd = MapCc((kSweep - 1) % 128, gainDb->minValue, gainDb->maxValue);

    // The message thread takes one change per target, each its latest.
    {
        const auto changes = TakeChanges(table);
        passed &= Expect(changes.size() == 3, "Three node parameters changed, so three changes should wait (got " +
                                                  std::to_string(changes.size()) + ")");
        passed &= Expect(changes.count("g1/gainDb") && std::abs(changes.at("g1/gainDb") - sweepEnd) < kTolerance,
                         "The gain change should carry the sweep's last value, native");
        passed &= Expect(changes.count("__input__/gainDb") &&
                             std::abs(changes.at("__input__/gainDb") -
                                      MapCc(96, kBoundaryGainMinDb, kBoundaryGainMaxDb)) < kTolerance,
                         "The Input gain change should carry dB");
        passed &= Expect(changes.count("d1/depth") && changes.at("d1/depth") == 10.0,
                         "The depth change should carry its value");
    }

    // ── A bypass switches every node of the type, and the lookup follows it ─────────────────

    passed &= ExpectNoAllocations(CountOnAudioThread([&] { SendCc(table, kBypassCc, 127); }), "Bypass");
    passed &= Expect(!mixer.FindFirstEnabledNodeOfType("gain").has_value(), "Both gain nodes should be bypassed");
    passed &= Expect(!table.ApplyAutomationLocked(dawSlotId, 0.5f, AutomationSource::DAW),
                     "With every gain node bypassed there is nothing to apply to");

    passed &= ExpectNoAllocations(CountOnAudioThread([&] {
                                      SendCc(table, kBypassCc, 0);
                                      SendCc(table, kBypassCc, 127);
                                  }),
                                  "Bypass off");
    passed &= Expect(NameLookupAgrees(mixer, "presetA", "g1"), "Both gain nodes should be back");

    {
        const auto changes = TakeChanges(table);
        const std::string bypassKey = std::string("bypass/") + EffectGuids::kGain;
        passed &= Expect(changes.size() == 1 && changes.count(bypassKey) && changes.at(bypassKey) == 1.0,
                         "One bypass change should wait, by canonical type, with the latest state: enabled");
    }

    // The first gain bypassed by hand: the slot moves on to the second, as the name lookup does.
    mixer.SetNodeEnabled("presetA", "g1", false);
    passed &= Expect(NameLookupAgrees(mixer, "presetA", "g2"), "The name lookup should move to the second node");
    passed &= ExpectNoAllocations(CountOnAudioThread([&] { SendCc(table, kGainCc, 127); }), "Gain, second node");
    passed &= Expect(ParamOf(mixer, "presetA", "g2", "gainDb") == gainDb->maxValue,
                     "The slot should drive the first enabled node, stored under the UUID");
    {
        const auto changes = TakeChanges(table);
        passed &= Expect(changes.size() == 1 && changes.count("g2/gainDb"), "The change should name the second node");
    }

    mixer.SetNodeEnabled("presetA", "g1", true);

    // ── A preset switch: the outgoing preset is skipped while it rings out ──────────────────

    // The outgoing node, held from before the switch: once it is retiring, nothing finds it by id.
    const EffectProcessor* outgoingGain = mixer.GetNodeProcessor("presetA", "g1");
    const double outgoingGainDb = outgoingGain ? outgoingGain->GetParam("gainDb") : kNoNode;

    const auto presetB = MakePreset("presetB", "b1");
    mixer.PreparePresetSwap(presetB, presetB.id, presetB.name);
    mixer.CommitPresetSwap();
    passed &=
        Expect(NameLookupAgrees(mixer, "presetB", "b1"), "The name lookup should name the incoming preset's node");

    passed &= ExpectNoAllocations(CountOnAudioThread([&] { SendCc(table, kGainCc, 127); }), "Gain after a switch");
    passed &= Expect(ParamOf(mixer, "presetB", "b1", "gainDb") == gainDb->maxValue,
                     "After a switch the slot should drive the incoming preset's node");
    passed &=
        Expect(outgoingGain && outgoingGainDb != gainDb->maxValue && outgoingGain->GetParam("gainDb") == outgoingGainDb,
               "The outgoing preset's node should be left alone while it rings out");
    {
        const auto changes = TakeChanges(table);
        passed &= Expect(changes.size() == 1 && changes.count("b1/gainDb"), "The change should name the new node");
    }

    // ── An effect type registered again with another range ──────────────────────────────────

    passed &= Expect(!table.NodeBindingsStale(), "Nothing has been registered since the slots were resolved");
    RegisterDepthEffect(20.0);
    passed &= Expect(table.NodeBindingsStale(), "Registering a type should mark the slots' bindings stale");

    auto bindings = table.ResolveNodeBindings();
    table.CommitNodeBindings(bindings);
    passed &= Expect(!table.NodeBindingsStale(), "Committing fresh bindings should bring them up to date");

    passed &= ExpectNoAllocations(CountOnAudioThread([&] { SendCc(table, kDepthCc, 127); }), "Depth, new range");
    passed &= Expect(std::abs(ParamOf(mixer, "presetB", "d1", "depth") - 20.0) < kTolerance,
                     "The slot should map onto the range the type declares now");

    // ── The queue on its own ────────────────────────────────────────────────────────────────

    passed &= TestQueueCapacity();
    passed &= TestQueueOrderUnderConcurrentTake();

    std::cout << (passed ? "AutomationNodeRealtimeTests passed" : "AutomationNodeRealtimeTests FAILED") << std::endl;
    return passed ? 0 : 1;
}
