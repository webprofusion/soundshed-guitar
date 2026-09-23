/**
 * @file MixerInstanceLockTests.cpp
 * @brief Walks of the mixer's slots must not race the audio thread erasing one.
 *
 * The audio thread erases a preset's outgoing instance from MultiPresetMixer::mInstances once
 * its fade finishes, at the end of Process(): just after any preset switch. The controller's
 * one-off walks (latency, adding a slot, a failed plugin load's report, project save, state
 * broadcast, mixer levels) take the DSP lock for it. The periodic telemetry reads cannot take
 * it 20-30 times a second without silencing a block whenever one lands on a callback, so they
 * hold the erase off instead. Nothing tells the host, builds a slot, builds a node's deferred
 * rebuild or asks a hosted plugin with the lock held.
 *
 * An audio thread calls PluginController::ProcessAudio throughout, as a host callback does.
 * Probe effects, a stand-in hosted plugin and the host's NotifyLatencyChanged() hold a window
 * open inside each walk and record whether audio ran in it, so the result is not left to luck.
 * A stress pass then runs every walk against slot churn.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(_WIN32) && defined(_DEBUG)
    #include <crtdbg.h>
#endif

#include "IPluginHost.h"
#include "MixerInstanceLockSupport.h"
#include "PluginController.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetStorage.h"
#include "presets/PresetTypes.h"

namespace fs = std::filesystem;
using namespace guitarfx;
using namespace mixer_instance_lock_test;
using namespace std::chrono_literals;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 128;
constexpr const char* kPresetId = "mixer-lock-preset";
constexpr const char* kSlotB = "mixer-lock-slot-b";
constexpr const char* kPluginPresetId = "mixer-lock-plugin-preset";
constexpr const char* kProbeType = "test_mixer_lock_probe";
constexpr const char* kLastErrorKey = "lastError";
constexpr const char* kLastErrorCodeKey = "lastErrorCode";

// How long a window that expects the DSP lock stays open. An unlocked audio thread runs a
// block every fraction of a millisecond here, so one landing inside it is certain.
constexpr auto kLockedWindow = 20ms;
// Upper bound on any wait for the audio thread; a hang becomes a failure, not a stuck ctest.
constexpr auto kAudioTimeout = 5s;
// A swap's declick fade is 1024 samples, eight blocks here. The telemetry window outlasts it.
constexpr int kBlocksPastFade = 16;

// ── Probe state ──────────────────────────────────────────────────────────────

std::atomic<int> gAudioBlocks{0};      // blocks the audio thread has processed
std::atomic<int> gAudioLockMisses{0};  // callbacks that lost the controller's try-lock
std::atomic<bool> gAudioPaused{false}; // the audio thread stops calling ProcessAudio
std::atomic<bool> gAudioParked{false}; // ... and has acknowledged it, between blocks
std::thread::id gMessageThread;        // written before any probe is armed
thread_local bool tIsAudioThread = false;
const MultiPresetMixer* gMixer = nullptr;

/// One kind of probed call: how often it ran, and how often audio did the wrong thing in it.
struct Window
{
    std::atomic<int> calls{0};
    std::atomic<int> bad{0};
};

/// A window's counts as they stood when this was taken, to check what it recorded since.
struct Mark
{
    const Window& window;
    int calls = window.calls.load();
    int bad = window.bad.load();
};

enum class Expect
{
    Locked,   ///< no block may run while the window is open: the DSP lock is held
    Unlocked, ///< blocks have to run: the DSP lock is not held
};

void Measure(Window& window, Expect expect)
{
    const int before = gAudioBlocks.load(std::memory_order_acquire);
    const bool bad = expect == Expect::Locked ? WaitForAdvance(gAudioBlocks, before, 1, kLockedWindow)
                                              : !WaitForAdvance(gAudioBlocks, before, 2, kAudioTimeout);
    window.calls.fetch_add(1, std::memory_order_acq_rel);

    if (bad)
    {
        window.bad.fetch_add(1, std::memory_order_acq_rel);
    }
}

struct Probes
{
    std::atomic<bool> latencyArmed{false};
    std::atomic<bool> telemetryArmed{false};
    std::atomic<bool> buildArmed{false};
    std::atomic<bool> errorArmed{false};
    std::atomic<int> latency{0};

    Window latencyReads;     // UpdateHostLatency's read: under the lock
    Window notifies;         // the host told: outside it
    Window prepares;         // a slot being built: outside it
    Window attaches;         // its runtime callbacks attached: under it
    Window plainErrorReads;  // a failed load's error, from a plain processor: under it
    Window hostedErrorReads; // ... from a hosted plugin: outside it
    Window hostedLookups;    // telling a hosted plugin apart: under it

    std::atomic<int> telemetryWindows{0};
    std::atomic<int> telemetryBlocked{0};
    std::atomic<int> retiringAtStart{-1};
    std::atomic<int> retiringAtEnd{-1};
};

Probes gProbes;

bool OnMessageThread()
{
    return std::this_thread::get_id() == gMessageThread;
}

/// The telemetry read in progress lets the audio thread run on past the end of the outgoing
/// slot's fade, and notes whether that slot was erased from under it meanwhile.
void OpenTelemetryWindow()
{
    gProbes.retiringAtStart.store(static_cast<int>(gMixer->GetRetiringPresetCount()));
    const int before = gAudioBlocks.load(std::memory_order_acquire);
    gAudioPaused.store(false, std::memory_order_release);
    const bool ran = WaitForAdvance(gAudioBlocks, before, kBlocksPastFade, kAudioTimeout);
    gProbes.retiringAtEnd.store(static_cast<int>(gMixer->GetRetiringPresetCount()));
    gProbes.telemetryWindows.fetch_add(1);

    if (!ran)
    {
        gProbes.telemetryBlocked.fetch_add(1);
    }
}

/// What both probes share: a pass-through that ignores its params and config.
class PassThroughEffect : public EffectProcessor
{
  public:
    void Prepare(double, int) override
    {
    }

    void Reset() override
    {
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            if (outputs[ch] && inputs[ch] && outputs[ch] != inputs[ch])
            {
                std::copy(inputs[ch], inputs[ch] + numSamples, outputs[ch]);
            }
        }
    }

    void SetParam(const std::string&, double) override
    {
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string&) const override
    {
        return 0.0;
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "utility";
    }
};

class ProbeEffect final : public PassThroughEffect
{
  public:
    /// Any thread but the audio thread: a slot's nodes are prepared on worker threads while
    /// the message thread waits for them, so a build under the DSP lock stalls them too.
    void Prepare(double, int) override
    {
        if (gProbes.buildArmed.load() && !tIsAudioThread)
        {
            Measure(gProbes.prepares, Expect::Unlocked);
        }
    }

    [[nodiscard]] std::string GetConfig(const std::string& key) const override
    {
        if (!gProbes.errorArmed.load() || (key != kLastErrorKey && key != kLastErrorCodeKey))
        {
            return {};
        }

        if (key == kLastErrorKey && OnMessageThread())
        {
            Measure(gProbes.plainErrorReads, Expect::Locked);
        }

        return key == kLastErrorKey ? "probe failed to load" : "E_PROBE";
    }

    void SetRuntimeConfigChangedCallback(RuntimeConfigChangedCallback) override
    {
        if (gProbes.buildArmed.load() && OnMessageThread())
        {
            Measure(gProbes.attaches, Expect::Locked);
        }
    }

    [[nodiscard]] int GetLatencySamples() const override
    {
        if (OnMessageThread())
        {
            if (gProbes.telemetryArmed.exchange(false))
            {
                OpenTelemetryWindow();
            }
            else if (gProbes.latencyArmed.load())
            {
                Measure(gProbes.latencyReads, Expect::Locked);
            }
        }

        return gProbes.latency.load();
    }

    [[nodiscard]] std::string GetType() const override
    {
        return kProbeType;
    }
};

class HostedPluginStandIn final : public PassThroughEffect
{
  public:
    [[nodiscard]] std::string GetConfig(const std::string& key) const override
    {
        if (!gProbes.errorArmed.load() || (key != kLastErrorKey && key != kLastErrorCodeKey))
        {
            return {};
        }

        if (key == kLastErrorKey && OnMessageThread())
        {
            Measure(gProbes.hostedErrorReads, Expect::Unlocked);
        }

        return key == kLastErrorKey ? "stand-in failed to load" : "E_STAND_IN";
    }

    /// How the controller tells a hosted plugin from anything else, under the DSP lock.
    [[nodiscard]] std::string GetType() const override
    {
        if (gProbes.errorArmed.load() && OnMessageThread())
        {
            Measure(gProbes.hostedLookups, Expect::Locked);
        }

        return EffectGuids::kPluginHost;
    }
};

void RegisterProbes()
{
    EffectTypeInfo probe;
    probe.type = kProbeType;
    probe.displayName = "Mixer lock probe";
    probe.category = "utility";
    EffectRegistry::Instance().Register(probe.type, probe, [] { return std::make_unique<ProbeEffect>(); });

    // The core tests build without JUCE, so the plugin-host type is free to stand in for.
    EffectTypeInfo hosted;
    hosted.type = EffectGuids::kPluginHost;
    hosted.aliases = {"plugin_host"};
    hosted.displayName = "Hosted plugin stand-in";
    hosted.category = "utility";
    EffectRegistry::Instance().Register(hosted.type, hosted, [] { return std::make_unique<HostedPluginStandIn>(); });
}

// ── Host and presets ─────────────────────────────────────────────────────────

class TestHost final : public IPluginHost
{
  public:
    explicit TestHost(fs::path userDataPath) : mUserDataPath(std::move(userDataPath))
    {
    }

    void SendMessageToUI(const std::string& message) override
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mMessages.push_back(message);
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

    /// A host can answer this by asking for state, which takes the DSP lock: it must arrive
    /// with the lock released.
    void NotifyLatencyChanged(int) override
    {
        if (gProbes.latencyArmed.load() && OnMessageThread())
        {
            Measure(gProbes.notifies, Expect::Unlocked);
        }
    }

    /// True if a message of `type` was sent whose fields all match `fields`.
    [[nodiscard]] bool SawMessage(const std::string& type, const nlohmann::json& fields) const
    {
        std::lock_guard<std::mutex> lock(mMutex);

        return std::any_of(mMessages.begin(), mMessages.end(), [&](const std::string& text) {
            const auto message = nlohmann::json::parse(text, nullptr, false);

            if (message.is_discarded() || message.value("type", "") != type)
            {
                return false;
            }

            return std::all_of(fields.items().begin(), fields.items().end(), [&](const auto& field) {
                return message.contains(field.key()) && message[field.key()] == field.value();
            });
        });
    }

  private:
    fs::path mUserDataPath;
    mutable std::mutex mMutex;
    std::vector<std::string> mMessages;
};

void SetSettingsEnvRoot(const fs::path& root)
{
#ifdef _WIN32
    _putenv_s("APPDATA", root.string().c_str());
#else
    setenv("HOME", root.string().c_str(), 1);
#endif
}

/// input -> probe [-> hosted stand-in] -> output. Without the stand-in nothing in the graph
/// can ring on, so a switch away from it is the plain declick fade rather than a tail.
Preset BuildPreset(const std::string& id, int variant, bool withPlugin)
{
    Preset preset;
    preset.id = id;
    preset.name = id;
    preset.version = 2;
    preset.category = "Test";

    auto& graph = preset.graph;
    graph.nodes = {{"__input__", kNodeTypeInput, "", "", true},
                   {"__output__", kNodeTypeOutput, "", "", true},
                   {"probe", kProbeType, "utility", "", true, {{"variant", variant}}}};
    const std::string last = withPlugin ? "plugin" : "probe";
    graph.edges = {{"__input__", "probe", 0, 0, 1.0}, {last, "__output__", 0, 0, 1.0}};

    if (withPlugin)
    {
        graph.nodes.push_back({"plugin", EffectGuids::kPluginHost, "utility", "", true});
        graph.edges.push_back({"probe", "plugin", 0, 0, 1.0});
    }

    return preset;
}

nlohmann::json PresetJson(const Preset& preset)
{
    return nlohmann::json::parse(PresetStorage::SerializeToJson(preset));
}

void Send(PluginController& controller, const nlohmann::json& message)
{
    controller.HandleUIMessage(message.dump());
}

void LoadPreset(PluginController& controller, const Preset& preset)
{
    Send(controller, {{"type", "loadPreset"}, {"presetId", preset.id}, {"preset", PresetJson(preset)}});
}

// ── The audio thread ─────────────────────────────────────────────────────────

/// Calls ProcessAudio back to back on its own thread, the way a host's callback does, with a
/// short gap so the message thread's blocking lock is not starved by the try_lock. Parks
/// between blocks while gAudioPaused is set.
class AudioThread
{
  public:
    explicit AudioThread(PluginController& controller) : mController(controller)
    {
        mThread = std::thread([this] { Run(); });
    }

    ~AudioThread()
    {
        gAudioPaused.store(false);
        mStop.store(true, std::memory_order_release);
        mThread.join();
    }

    [[nodiscard]] bool Pause() const
    {
        gAudioPaused.store(true, std::memory_order_release);
        return WaitUntil([] { return gAudioParked.load(std::memory_order_acquire); }, kAudioTimeout);
    }

    /// Blocks until the chain has processed `blocks` more blocks; false on timeout.
    [[nodiscard]] bool WaitForBlocks(int blocks) const
    {
        return WaitForAdvance(gAudioBlocks, gAudioBlocks.load(std::memory_order_acquire), blocks, kAudioTimeout);
    }

  private:
    void Run()
    {
        tIsAudioThread = true;
        std::vector<float> inL(kBlock, 0.05f), inR(kBlock, 0.05f), outL(kBlock), outR(kBlock);

        while (!mStop.load(std::memory_order_acquire))
        {
            if (gAudioPaused.load(std::memory_order_acquire))
            {
                gAudioParked.store(true, std::memory_order_release);
                std::this_thread::yield();
                continue;
            }

            gAudioParked.store(false, std::memory_order_release);
            float* inputs[] = {inL.data(), inR.data()};
            float* outputs[] = {outL.data(), outR.data()};

            if (mController.ProcessAudio(inputs, outputs, kBlock))
            {
                gAudioBlocks.fetch_add(1, std::memory_order_acq_rel);
            }
            else
            {
                gAudioLockMisses.fetch_add(1, std::memory_order_acq_rel);
            }

            const auto resume = std::chrono::steady_clock::now() + 200us;

            while (std::chrono::steady_clock::now() < resume)
            {
                std::this_thread::yield();
            }
        }
    }

    PluginController& mController;
    std::thread mThread;
    std::atomic<bool> mStop{false};
};

// ── Tests ────────────────────────────────────────────────────────────────────

void RunArmed(std::atomic<bool>& armed, const std::function<void()>& action)
{
    armed.store(true);
    action();
    armed.store(false);
}

/// The window was probed since `mark`, and audio never did the wrong thing in it.
bool CheckWindow(const std::string& what, const Mark& mark, const std::string& badMeaning)
{
    const int calls = mark.window.calls.load() - mark.calls;
    const int bad = mark.window.bad.load() - mark.bad;
    bool passed = Check(calls >= 1, what + ": probed (" + std::to_string(calls) + " calls)");
    return Check(bad == 0, what + ": " + badMeaning + " (" + std::to_string(bad) + " times)") && passed;
}

/// getPerformanceStats walks every slot for its latency. Audio is paused across the switch so
/// the outgoing slot is still fading when that walk starts; the walk then lets audio run on
/// well past the end of the fade.
bool TestTelemetryHoldsEraseOff(PluginController& controller, AudioThread& audio)
{
    constexpr int kRounds = 3;
    bool passed = true;
    const auto noneRetiring = [] { return gMixer->GetRetiringPresetCount() == 0; };

    for (int round = 1; round <= kRounds; ++round)
    {
        const std::string label = "telemetry round " + std::to_string(round) + ": ";
        const auto expect = [&](bool condition, const std::string& what) {
            passed = Check(condition, label + what) && passed;
        };

        // The feed sends at most every 200 ms, and the previous round's slot has to be gone.
        std::this_thread::sleep_for(250ms);
        expect(WaitUntil(noneRetiring, kAudioTimeout), "starts with no slot retiring");

        if (!Check(audio.Pause(), label + "audio paused"))
        {
            return false;
        }

        LoadPreset(controller, BuildPreset(kPresetId, round, false));
        const int windowsBefore = gProbes.telemetryWindows.load();
        RunArmed(gProbes.telemetryArmed, [&] { Send(controller, {{"type", "getPerformanceStats"}}); });
        gAudioPaused.store(false);

        expect(gProbes.telemetryWindows.load() == windowsBefore + 1, "the read reached the probe");
        expect(gProbes.telemetryBlocked.load() == 0, "audio ran during the read (no DSP lock)");
        expect(gProbes.retiringAtStart.load() == 1, "the outgoing slot was still fading as it began");
        expect(gProbes.retiringAtEnd.load() == 1, "and was not erased from under it (" +
                                                      std::to_string(gProbes.retiringAtEnd.load()) +
                                                      " retiring after the fade)");
        expect(WaitUntil(noneRetiring, kAudioTimeout), "it is erased once the read is over");
    }

    return passed;
}

/// UpdateHostLatency walks every slot under the DSP lock, and tells the host after it.
bool TestLatencyReadLockedNotifiedUnlocked(PluginController& controller)
{
    bool passed = true;

    const auto trigger = [&](const std::string& what, const std::function<void()>& action) {
        gProbes.latency.fetch_add(8); // a change, so the host is told
        const Mark reads{gProbes.latencyReads};
        const Mark notifies{gProbes.notifies};
        RunArmed(gProbes.latencyArmed, action);
        passed = CheckWindow(what + " latency read", reads, "audio ran inside it") && passed;
        passed = CheckWindow(what + " host notification", notifies, "arrived under the DSP lock") && passed;
    };

    trigger("lowLatency param", [&] {
        Send(controller, {{"type", "updateSignalPathNodeParam"},
                          {"presetId", kPresetId},
                          {"nodeId", "probe"},
                          {"paramKey", "lowLatency"},
                          {"value", 1.0}});
    });
    trigger("offline render on", [&] { controller.SetOfflineRendering(true); });
    trigger("offline render off", [&] { controller.SetOfflineRendering(false); });
    return passed;
}

/// A slot is built with audio running and installed, callbacks and all, under the lock:
/// added from the UI, and restored from host state.
bool TestSlotsBuiltOffLockAttachedUnderIt(PluginController& controller)
{
    bool passed = true;

    const auto checkBuild = [&](const std::string& what, const std::function<void()>& action) {
        const Mark prepares{gProbes.prepares};
        const Mark attaches{gProbes.attaches};
        RunArmed(gProbes.buildArmed, action);
        passed = CheckWindow(what + " build", prepares, "ran under the DSP lock") && passed;
        passed = CheckWindow(what + " callback attach", attaches, "ran without the DSP lock") && passed;
        passed = Check(gMixer->GetPresetCount() == 2,
                       what + ": two slots running (" + std::to_string(gMixer->GetPresetCount()) + ")") &&
                 passed;
    };

    checkBuild("addActivePreset", [&] {
        Send(
            controller,
            {{"type", "addActivePreset"}, {"presetId", kSlotB}, {"preset", PresetJson(BuildPreset(kSlotB, 0, false))}});
    });

    const auto state = controller.SerializeState();
    checkBuild("host state restore", [&] { controller.DeserializeState(state); });
    return passed;
}

/// A failed plugin load is reported from the running processor: found under the lock, and
/// asked under it too unless it is a hosted plugin, which is asked after.
bool TestLoadFailureReport(PluginController& controller, const TestHost& host, const fs::path& sandbox)
{
    LoadPreset(controller, BuildPreset(kPluginPresetId, 0, true));
    bool passed = true;

    const auto sendPluginResource = [&](const std::string& nodeId) {
        Send(controller, {{"type", "updateNodeResource"},
                          {"nodeId", nodeId},
                          {"resourceIndex", 0},
                          {"resourceType", "plugin"},
                          {"filePath", (sandbox / ("missing-" + nodeId + ".vst3")).generic_string()}});
    };

    const Mark plainReads{gProbes.plainErrorReads};
    RunArmed(gProbes.errorArmed, [&] { sendPluginResource("probe"); });
    passed = CheckWindow("load failure, plain processor: error read", plainReads, "audio ran inside it") && passed;
    passed =
        Check(host.SawMessage("hostedPluginResourceLoadFailed",
                              {{"nodeId", "probe"}, {"message", "probe failed to load"}, {"errorCode", "E_PROBE"}}),
              "load failure, plain processor: reported to the UI") &&
        passed;

    const Mark lookups{gProbes.hostedLookups};
    const Mark hostedReads{gProbes.hostedErrorReads};
    RunArmed(gProbes.errorArmed, [&] { sendPluginResource("plugin"); });
    passed = CheckWindow("load failure, hosted plugin: lookup", lookups, "audio ran inside it") && passed;
    passed = CheckWindow("load failure, hosted plugin: error read", hostedReads, "made under the DSP lock") && passed;
    passed = Check(host.SawMessage(
                       "hostedPluginResourceLoadFailed",
                       {{"nodeId", "plugin"}, {"message", "stand-in failed to load"}, {"errorCode", "E_STAND_IN"}}),
                   "load failure, hosted plugin: reported to the UI") &&
             passed;
    return passed;
}

/// Every walk this file is about, against slot churn and finishing fades. A race shows up
/// here as a crash or a Debug iterator assertion rather than a clean failure.
bool TestWalksUnderChurn(PluginController& controller, AudioThread& audio)
{
    constexpr int kRounds = 12;
    bool audioKeptUp = true;

    for (int round = 0; round < kRounds; ++round)
    {
        Send(controller, {{"type", "removeActivePreset"}, {"presetId", kSlotB}});
        LoadPreset(controller, BuildPreset(kPresetId, 100 + round, false));
        Send(controller, {{"type", "addActivePreset"},
                          {"presetId", kSlotB},
                          {"preset", PresetJson(BuildPreset(kSlotB, round, false))}});
        Send(controller, {{"type", "setPresetMix"}, {"presetId", kSlotB}, {"value", 0.5}});
        Send(controller, {{"type", "setPresetPan"}, {"presetId", kSlotB}, {"value", -0.25}});
        Send(controller, {{"type", "setPresetMute"}, {"presetId", kSlotB}, {"value", false}});
        Send(controller, {{"type", "setPresetSolo"}, {"presetId", kSlotB}, {"value", false}});
        Send(controller, {{"type", "uiVisibility"}, {"visible", round % 2 == 0}});
        Send(controller, {{"type", "getPerformanceStats"}});
        Send(controller, {{"type", "getSignalDiagnostics"}});
        Send(controller, {{"type", "requestState"}});
        Send(controller, {{"type", "saveCompositePreset"}, {"name", "Mixer lock mix"}});
        (void)controller.SerializeState();
        controller.OnIdle();
        audioKeptUp = audio.WaitForBlocks(1) && audioKeptUp;
    }

    bool passed = Check(audioKeptUp, "churn: audio resumed after every round");
    passed = Check(gMixer->GetPresetCount() == 2, "churn: both slots running at the end") && passed;
    return passed;
}

bool Run()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-mixer-instance-lock-tests";
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
        LoadPreset(controller, BuildPreset(kPresetId, 0, false));

        gMixer = &controller.GetMixer();
        gMessageThread = std::this_thread::get_id();
        AudioThread audio(controller); // stopped before the controller goes

        if (!Check(audio.WaitForBlocks(20), "audio thread is processing the mixer"))
        {
            passed = false;
        }
        else
        {
            passed = TestTelemetryHoldsEraseOff(controller, audio) && passed;
            passed = TestLatencyReadLockedNotifiedUnlocked(controller) && passed;
            passed = TestSlotsBuiltOffLockAttachedUnderIt(controller) && passed;
            passed = TestWalksUnderChurn(controller, audio) && passed;
            passed = TestLoadFailureReport(controller, host, sandbox) && passed;
            passed = TestRiffTrimKeepsAudioRunning(controller, gAudioBlocks, gAudioLockMisses, kSampleRate) && passed;
            passed = TestDeferredRebuildBuiltOffLock(controller, gAudioBlocks) && passed;
        }
    }

    fs::remove_all(sandbox, ec);
    return passed;
}
} // namespace

int main()
{
    // A race this test exists to catch can abort the process; show how far it got, and send
    // a Debug CRT assertion to stderr rather than to a dialog nobody will click.
    std::cout << std::unitbuf;
#if defined(_WIN32) && defined(_DEBUG)
    for (const int report : {_CRT_ERROR, _CRT_ASSERT})
    {
        _CrtSetReportMode(report, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(report, _CRTDBG_FILE_STDERR);
    }
#endif

    // A deadlock on the DSP lock would otherwise hang ctest until its own timeout.
    std::atomic<bool> finished{false};
    std::thread watchdog([&finished] {
        const auto deadline = std::chrono::steady_clock::now() + 180s;

        while (!finished.load())
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                std::cout << "[FAIL] watchdog: still running after 180 s, most likely deadlocked\n";
                std::_Exit(3);
            }

            std::this_thread::sleep_for(100ms);
        }
    });

    RegisterAllEffects();
    RegisterProbes();

    const bool passed = Run();
    finished.store(true);
    watchdog.join();
    std::cout << (passed ? "MixerInstanceLockTests PASSED\n" : "MixerInstanceLockTests FAILED\n");
    return passed ? 0 : 1;
}
