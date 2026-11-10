/**
 * @file AutomationSlotThreadTests.cpp
 * @brief The automation slots are replaced on the message thread while the audio thread and the
 * host read them.
 *
 * A restore, a shared-sync reload and startup rebuild the whole slot list from JSON; adding or
 * removing a custom slot changes it one slot at a time. Meanwhile the audio thread walks the
 * slots to apply MIDI (under mDSPMutex, taken with try_lock), and a host reads its parameters on
 * any thread, the audio thread included. So a rebuild is built off the lock and swapped in under
 * it, and a host reads each parameter from a cell of its own that mirrors its slot's value, never
 * from the slots themselves.
 *
 * This test's main thread is the message thread. Its DAW parameters are bound the way the plugin
 * adapter binds them, including a reserved placeholder no slot will ever have.
 *  - The parameters follow their slots: restores, MIDI, the UI, the host's own automation, a slot
 *    added and removed, and a parameter whose slot has gone reads 0. A shared-sync reload of
 *    another instance's mappings keeps every value.
 *  - Under churn, one thread reads every parameter in a loop while the audio thread applies MIDI
 *    to a mapped slot and processes audio, and the message thread restores host state over and
 *    over (with custom slots coming and going, so the list is rebuilt at different sizes). Every
 *    value read is one a slot really had: a restore never shows a value in between.
 *  - The same again with custom slots added and removed one at a time, and another instance's
 *    mappings reloaded through shared sync.
 */

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(_WIN32) && defined(_DEBUG)
    #include <crtdbg.h>
#endif

#include "MessageThreadTestHost.h"
#include "PluginController.h"
#include "automation/AutomationTypes.h"
#include "dsp/FiniteCheck.h"
#include "dsp/effects/BuiltinEffects.h"

namespace fs = std::filesystem;
using namespace guitarfx;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 128;
constexpr int kMidiController = 20;
constexpr int kMidiEventsPerBlock = 64;

// The DAW parameters, in the order bound. The adapter binds the defaults, the custom slots there
// are at startup, placeholders for the rest, then the defaults added since.
constexpr int kInputLevel = 0;
constexpr int kMidiSlot = 1;
constexpr int kUiSlot = 2;
constexpr int kPlaceholder = 3;
constexpr int kSceneSlot = 4;
const std::vector<std::string> kBoundSlotIds = {"default.inputLevel", "custom.midi", "custom.ui", "custom._reserved_0",
                                                "default.scene4"};

// The input level each host state restores; nothing else sets it during the churn.
constexpr float kInputLevelA = 0.25f;
constexpr float kInputLevelB = 0.75f;

bool Check(bool condition, const std::string& what)
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << what << "\n";
    return condition;
}

// ── Host state ───────────────────────────────────────────────────────────────

nlohmann::json CustomSlot(const std::string& slotId, bool mappedToMidi)
{
    nlohmann::json slot = {{"slotId", slotId}, {"label", slotId}, {"address", ""}};

    if (mappedToMidi)
    {
        // Absolute CC on any channel; no address, so MIDI only moves the value.
        slot["midiMap"] = {{"eventType", 0}, {"channel", -1}, {"controller", kMidiController}, {"mode", 0}};
    }

    return slot;
}

/// Host state carrying automation only. A has the MIDI-mapped slot and a crowd of others; B has
/// neither, so each restore rebuilds the list at a different size and B's leaves the MIDI slot's
/// parameter with no slot.
std::string HostState(bool a)
{
    nlohmann::json customSlots = nlohmann::json::array();
    nlohmann::json values = nlohmann::json::object();

    if (a)
    {
        customSlots.push_back(CustomSlot("custom.midi", true));
        values["custom.midi"] = 0.5;

        for (int i = 0; i < 12; ++i)
        {
            customSlots.push_back(CustomSlot("custom.a" + std::to_string(i), false));
        }
    }
    else
    {
        for (int i = 0; i < 3; ++i)
        {
            customSlots.push_back(CustomSlot("custom.b" + std::to_string(i), false));
        }
    }

    values["default.inputLevel"] = a ? kInputLevelA : kInputLevelB;

    const nlohmann::json automation = {
        {"schemaVersion", 1}, {"defaultSlotOverrides", nlohmann::json::object()}, {"customSlots", customSlots}};
    return nlohmann::json{{"version", 1}, {"automation", automation}, {"automationValues", values}}.dump();
}

void Send(PluginController& controller, const nlohmann::json& message)
{
    controller.HandleUIMessage(message.dump());
}

/// Another instance on the same store, changing the shared automation mappings as its own
/// setAutomationSlot would: the document, then the shared-sync version. The controller's next
/// getSharedSyncState then reloads them. Its own writes no longer do: it already has those.
class OtherInstance
{
  public:
    explicit OtherInstance(const fs::path& sandbox)
    {
        std::string error;
        mOpen = mStore.Open(sandbox / "Sepia" / "data" / "v1" / "soundshed.db", error);

        if (!mOpen)
        {
            std::cout << "[FAIL] could not open the store as another instance: " << error << "\n";
        }
    }

    [[nodiscard]] bool Ok() const
    {
        return mOpen;
    }

    bool WriteAutomation(const nlohmann::json& customSlots)
    {
        const nlohmann::json automation = {
            {"schemaVersion", 1}, {"defaultSlotOverrides", nlohmann::json::object()}, {"customSlots", customSlots}};

        return mStore.Transact([&] {
            std::uint64_t next = 1;

            if (const auto previous = mStore.Get(storage::ItemType::kDocument, "shared-sync-state");
                previous && previous->is_object())
            {
                next = previous->value("version", std::uint64_t{0}) + 1;
            }

            return mStore.Put(storage::ItemType::kDocument, "automation", automation) &&
                   mStore.Put(storage::ItemType::kDocument, "shared-sync-state",
                              {{"version", next}, {"domains", {"automation"}}});
        });
    }

  private:
    storage::JsonStore mStore;
    bool mOpen = false;
};

MidiEvent Cc(int value)
{
    return MidiEvent{static_cast<std::uint8_t>(0xB0), static_cast<std::uint8_t>(kMidiController),
                     static_cast<std::uint8_t>(value), 0};
}

bool Near(float actual, float expected)
{
    return std::abs(actual - expected) < 1e-4f;
}

std::string Show(float value)
{
    return std::to_string(value);
}

// ── Other threads ────────────────────────────────────────────────────────────

/// The host's audio callback: queues a burst of MIDI CCs for the mapped slot, applies queued MIDI
/// the way processBlock does, then processes a block. Each event is a walk over every slot, so the
/// burst keeps this thread inside the slots for much of the time: a rebuild that did not take the
/// DSP lock would free the list from under it.
class MidiAudioThread
{
  public:
    explicit MidiAudioThread(PluginController& controller) : mController(controller)
    {
        mThread = std::thread([this] { Run(); });
    }

    ~MidiAudioThread()
    {
        mStop.store(true, std::memory_order_release);
        mThread.join();
    }

    [[nodiscard]] int Blocks() const
    {
        return mBlocks.load(std::memory_order_acquire);
    }

  private:
    void Run()
    {
        std::vector<float> inL(kBlock, 0.05f), inR(kBlock, 0.05f), outL(kBlock), outR(kBlock);

        for (int value = 0; !mStop.load(std::memory_order_acquire);)
        {
            for (int event = 0; event < kMidiEventsPerBlock; ++event, value = (value + 1) % 128)
            {
                mController.EnqueueMidi(Cc(value));
            }

            mController.ProcessQueuedMidi();

            float* inputs[] = {inL.data(), inR.data()};
            float* outputs[] = {outL.data(), outR.data()};
            (void)mController.ProcessAudio(inputs, outputs, kBlock);
            mBlocks.fetch_add(1, std::memory_order_acq_rel);

            // Short, but long enough that the message thread's blocking lock is not starved.
            const auto resume = Clock::now() + 50us;

            while (Clock::now() < resume)
            {
                std::this_thread::yield();
            }
        }
    }

    PluginController& mController;
    std::thread mThread;
    std::atomic<bool> mStop{false};
    std::atomic<int> mBlocks{0};
};

/// A host thread reading every parameter back over and over, the way hosts poll them, and
/// counting any value no slot could have had.
class ParameterReader
{
  public:
    explicit ParameterReader(PluginController& controller) : mController(controller)
    {
        mThread = std::thread([this] { Run(); });
    }

    ~ParameterReader()
    {
        Stop();
    }

    void Stop()
    {
        mStop.store(true, std::memory_order_release);

        if (mThread.joinable())
        {
            mThread.join();
        }
    }

    std::atomic<int> reads{0};
    std::atomic<int> impossible{0};
    std::atomic<int> midiValuesSeen{0}; ///< the MIDI slot at something other than its restored 0.5
    float firstImpossible = 0.0f;
    int firstImpossibleParameter = -1;

  private:
    void Run()
    {
        while (!mStop.load(std::memory_order_acquire))
        {
            for (int parameter = 0; parameter < static_cast<int>(kBoundSlotIds.size()); ++parameter)
            {
                const float value = mController.GetDawParameterValue(parameter);
                reads.fetch_add(1, std::memory_order_acq_rel);

                if (!Possible(parameter, value))
                {
                    if (impossible.fetch_add(1, std::memory_order_acq_rel) == 0)
                    {
                        firstImpossible = value;
                        firstImpossibleParameter = parameter;
                    }
                }
                else if (parameter == kMidiSlot && value != 0.0f && !Near(value, 0.5f))
                {
                    midiValuesSeen.fetch_add(1, std::memory_order_acq_rel);
                }
            }
        }
    }

    [[nodiscard]] bool Possible(int parameter, float value) const
    {
        if (!IsFinite(value) || value < 0.0f || value > 1.0f)
        {
            return false;
        }

        switch (parameter)
        {
        case kInputLevel:
            // Only ever restored. A shared-sync reload carries it into the slots it rebuilds.
            return value == kInputLevelA || value == kInputLevelB;
        case kPlaceholder:
            return value == 0.0f;
        default:
            return true;
        }
    }

    PluginController& mController;
    std::thread mThread;
    std::atomic<bool> mStop{false};
};

// ── Tests ────────────────────────────────────────────────────────────────────

bool TestParametersFollowSlots(PluginController& controller, OtherInstance& other)
{
    const std::string label = "parameters: ";
    const auto value = [&controller](int parameter) { return controller.GetDawParameterValue(parameter); };

    // The input level's slot holds the trim (-40..+20 dB) from the start, not 0, which a host
    // that saves and restores its parameters (QPlug) would set back as -40 dB; and it follows
    // the trim when the trim changes.
    bool passed = Check(Near(value(kInputLevel), 40.0f / 60.0f),
                        label + "the input level starts at the trim's 0 dB (" + Show(value(kInputLevel)) + ")");
    Send(controller, {{"type", "setGlobalChainParam"}, {"path", "input.gain"}, {"value", -10.0}});
    passed = Check(Near(value(kInputLevel), 30.0f / 60.0f),
                   label + "and follows the trim to -10 dB (" + Show(value(kInputLevel)) + ")") &&
             passed;
    Send(controller, {{"type", "setGlobalChainParam"}, {"path", "input.gain"}, {"value", 0.0}});

    (void)controller.DeserializeState(HostState(true));
    passed = Check(Near(value(kInputLevel), kInputLevelA) && Near(value(kMidiSlot), 0.5f),
                   label + "a restore sets them (" + Show(value(kInputLevel)) + ", " + Show(value(kMidiSlot)) + ")") &&
             passed;

    controller.EnqueueMidi(Cc(127));
    controller.ProcessQueuedMidi();
    passed =
        Check(Near(value(kMidiSlot), 1.0f), label + "MIDI moves its slot's (" + Show(value(kMidiSlot)) + ")") && passed;

    Send(controller, {{"type", "setAutomationValue"}, {"slotId", "default.inputLevel"}, {"value", 0.6}});
    passed = Check(Near(value(kInputLevel), 0.6f), label + "the UI sets one") && passed;

    controller.ApplyAutomationFromDAW("default.inputLevel", 0.4f);
    passed = Check(Near(value(kInputLevel), 0.4f), label + "so does the host's own automation") && passed;

    // Another instance adds a mapping. This one takes the mappings, but the values are its own.
    passed = Check(other.WriteAutomation(
                       nlohmann::json::array({CustomSlot("custom.midi", true), CustomSlot("custom.elsewhere", false)})),
                   label + "another instance changes the mappings") &&
             passed;
    Send(controller, {{"type", "getSharedSyncState"}});
    passed = Check(Near(value(kInputLevel), 0.4f) && Near(value(kMidiSlot), 1.0f),
                   label + "a shared-sync reload keeps them (" + Show(value(kInputLevel)) + ", " +
                       Show(value(kMidiSlot)) + ")") &&
             passed;

    Send(controller, {{"type", "setAutomationSlot"}, {"slotId", "custom.ui"}, {"label", "UI"}, {"address", ""}});
    Send(controller, {{"type", "setAutomationValue"}, {"slotId", "custom.ui"}, {"value", 0.3}});
    passed =
        Check(Near(value(kUiSlot), 0.3f), label + "a custom slot added later is joined to its parameter") && passed;

    Send(controller, {{"type", "removeAutomationSlot"}, {"slotId", "custom.ui"}});
    passed = Check(value(kUiSlot) == 0.0f, label + "and once removed, its parameter reads 0") && passed;

    (void)controller.DeserializeState(HostState(false));
    passed =
        Check(Near(value(kInputLevel), kInputLevelB) && value(kMidiSlot) == 0.0f,
              label + "a restore without the MIDI slot leaves its parameter at 0 (" + Show(value(kMidiSlot)) + ")") &&
        passed;
    passed =
        Check(value(kPlaceholder) == 0.0f && value(kSceneSlot) == 0.0f, label + "the placeholder reads 0") && passed;
    passed = Check(value(-1) == 0.0f && value(static_cast<int>(kBoundSlotIds.size())) == 0.0f,
                   label + "a parameter that does not exist reads 0") &&
             passed;
    return passed;
}

/// Restores back and forth, and with `sharedSync` also custom slots added and removed and another
/// instance's mappings reloaded, while a host thread reads every parameter and the audio thread
/// applies MIDI.
bool TestReadersUnderChurn(PluginController& controller, test::PumpedTestHost& host, OtherInstance& other,
                           bool sharedSync)
{
    const std::string label = sharedSync ? "churn with shared sync: " : "churn: ";
    constexpr int kRounds = 200;

    (void)controller.DeserializeState(HostState(true));
    host.Pump();

    MidiAudioThread audio(controller);
    ParameterReader reader(controller);

    for (int round = 0; round < kRounds; ++round)
    {
        (void)controller.DeserializeState(HostState(round % 2 == 1));

        if (sharedSync)
        {
            Send(controller, {{"type", "setAutomationSlot"},
                              {"slotId", "custom.ui"},
                              {"label", "UI " + std::to_string(round)},
                              {"address", ""}});

            if (round % 4 == 0)
            {
                (void)other.WriteAutomation(nlohmann::json::array(
                    {CustomSlot("custom.midi", true), CustomSlot("custom.other" + std::to_string(round), false)}));
                Send(controller, {{"type", "getSharedSyncState"}});
            }

            Send(controller, {{"type", "removeAutomationSlot"}, {"slotId", "custom.ui"}});
        }

        controller.OnIdle();
        host.Pump();
        std::this_thread::sleep_for(2ms);
    }

    reader.Stop();
    const int blocks = audio.Blocks();

    bool passed = Check(reader.reads.load() > kRounds * 10,
                        label + std::to_string(reader.reads.load()) + " parameter reads over " +
                            std::to_string(kRounds) + " rebuilds and " + std::to_string(blocks) + " MIDI blocks");
    const int impossible = reader.impossible.load();
    passed =
        Check(impossible == 0, label + "every value read was one a slot had" +
                                   (impossible == 0 ? std::string{}
                                                    : " (" + std::to_string(impossible) + " not, the first " +
                                                          Show(reader.firstImpossible) + " on parameter " +
                                                          std::to_string(reader.firstImpossibleParameter) + ")")) &&
        passed;
    passed = Check(reader.midiValuesSeen.load() > 0, label + "MIDI from the audio thread reached the reader") && passed;
    passed = Check(blocks > kRounds, label + "the audio thread kept going") && passed;
    return passed;
}

bool Run()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-automation-slot-thread-tests";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    test::SetSettingsEnvRoot(sandbox);
    bool passed = true;

    {
        test::PumpedTestHost host(sandbox, std::this_thread::get_id(), kSampleRate, kBlock);
        PluginController controller(host);
        controller.Initialize();
        controller.Prepare(kSampleRate, kBlock);
        controller.BindDawParameters(kBoundSlotIds);
        host.Pump();

        OtherInstance other(sandbox);
        passed = other.Ok() && passed;
        passed = TestParametersFollowSlots(controller, other) && passed;
        passed = TestReadersUnderChurn(controller, host, other, false) && passed;
        passed = TestReadersUnderChurn(controller, host, other, true) && passed;
        host.Pump();
    }

    fs::remove_all(sandbox, ec);
    return passed;
}
} // namespace

int main()
{
    // A race this test exists to catch can abort the process; show how far it got, and send a
    // Debug CRT assertion to stderr rather than to a dialog nobody will click.
    std::cout << std::unitbuf;
#if defined(_WIN32) && defined(_DEBUG)

    for (const int report : {_CRT_ERROR, _CRT_ASSERT})
    {
        _CrtSetReportMode(report, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(report, _CRTDBG_FILE_STDERR);
    }

#endif

    std::atomic<bool> finished{false};
    std::thread watchdog([&finished] {
        const auto deadline = Clock::now() + 180s;

        while (!finished.load())
        {
            if (Clock::now() >= deadline)
            {
                std::cout << "[FAIL] watchdog: still running after 180 s, most likely deadlocked\n";
                std::_Exit(3);
            }

            std::this_thread::sleep_for(100ms);
        }
    });

    RegisterAllEffects();

    const bool passed = Run();
    finished.store(true);
    watchdog.join();
    std::cout << (passed ? "AutomationSlotThreadTests PASSED\n" : "AutomationSlotThreadTests FAILED\n");
    return passed ? 0 : 1;
}
