#include "MixerInstanceLockSupport.h"

#include "PluginController.h"
#include "dsp/DeferredRebuild.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "presets/PresetStorage.h"
#include "presets/PresetTypes.h"
#include "util/Base64.h"
#include "util/Wav.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

using namespace std::chrono_literals;

namespace mixer_instance_lock_test
{
namespace
{
constexpr const char* kRebuildProbeType = "test_deferred_rebuild_probe";
constexpr const char* kRebuildPresetId = "mixer-lock-deferred-rebuild";
constexpr const char* kRebuildNodeId = "rebuildProbe";

const std::atomic<int>* gRebuildAudioBlocks = nullptr;
std::atomic<int> gBuilds{0};
std::atomic<int> gBuildsWithAudioStopped{0};
std::atomic<int> gCommits{0};
std::atomic<int> gCommitsWithAudioRunning{0};

/// True if the audio thread gets through `blocks` more blocks within `timeout`: it cannot while
/// the DSP lock is held.
bool AudioAdvances(int blocks, std::chrono::milliseconds timeout)
{
    return WaitForAdvance(*gRebuildAudioBlocks, gRebuildAudioBlocks->load(std::memory_order_acquire), blocks, timeout);
}

class ProbeRebuild final : public guitarfx::DeferredRebuild
{
  public:
    void Build() override
    {
        gBuilds.fetch_add(1);

        if (!AudioAdvances(2, 5s))
        {
            gBuildsWithAudioStopped.fetch_add(1);
        }
    }
};

/// Takes a "rebuild" param the way the IR cab takes Normalize and Low Latency: SetParam records it
/// and the message thread builds it.
class RebuildProbeEffect final : public guitarfx::EffectProcessor
{
  public:
    RebuildProbeEffect();
    ~RebuildProbeEffect() override;

    void RequestRebuild()
    {
        mPending.store(true);
        guitarfx::DeferredRebuild::NoteRequested();
    }

    void SetParam(const std::string& key, double) override
    {
        if (key == "rebuild")
        {
            RequestRebuild();
        }
    }

    [[nodiscard]] std::unique_ptr<guitarfx::DeferredRebuild> TakeDeferredRebuild() override
    {
        return mPending.exchange(false) ? std::make_unique<ProbeRebuild>() : nullptr;
    }

    void CommitDeferredRebuild(guitarfx::DeferredRebuild&) override
    {
        // A window expecting the lock: unlocked, audio would run many blocks in it.
        gCommits.fetch_add(1);

        if (AudioAdvances(1, 20ms))
        {
            gCommitsWithAudioRunning.fetch_add(1);
        }
    }

    void Prepare(double, int) override
    {
    }

    void Reset() override
    {
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        CopyStereoInputToOutput(inputs, outputs, numSamples);
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string&) const override
    {
        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return kRebuildProbeType;
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "utility";
    }

  private:
    std::atomic<bool> mPending{false};
};

std::atomic<RebuildProbeEffect*> gLatestRebuildProbe{nullptr};

RebuildProbeEffect::RebuildProbeEffect()
{
    gLatestRebuildProbe.store(this);
}

RebuildProbeEffect::~RebuildProbeEffect()
{
    RebuildProbeEffect* self = this;
    gLatestRebuildProbe.compare_exchange_strong(self, nullptr);
}
} // namespace

bool Check(bool condition, const std::string& what)
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << what << "\n";
    return condition;
}

bool WaitUntil(const std::function<bool()>& condition, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition())
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(100us);
    }
    return true;
}

bool WaitForAdvance(const std::atomic<int>& counter, int from, int by, std::chrono::milliseconds timeout)
{
    return WaitUntil([&] { return counter.load(std::memory_order_acquire) - from >= by; }, timeout);
}

bool TestRiffTrimKeepsAudioRunning(guitarfx::PluginController& controller, const std::atomic<int>& audioBlocks,
                                   const std::atomic<int>& audioLockMisses, double sampleRate)
{
    constexpr std::size_t kFrames = 2'000'000;
    {
        const std::vector<float> left(kFrames, 0.1f);
        const std::vector<float> right(kFrames, -0.1f);
        const auto wavBytes = guitarfx::util::EncodeStereo16BitWav(left, right, static_cast<int>(sampleRate));
        controller.HandleUIMessage(nlohmann::json{{"type", "importRiffWav"},
                                                {"data", guitarfx::util::EncodeBase64(wavBytes)},
                                                {"tempoBpm", 120.0},
                                                {"timeSigNum", 4},
                                                {"timeSigDen", 4}}
                                       .dump());
    }

    const int blocksBefore = audioBlocks.load(std::memory_order_acquire);
    const int missesBefore = audioLockMisses.load(std::memory_order_acquire);
    controller.HandleUIMessage(nlohmann::json{{"type", "trimCapturedRiff"}, {"startRatio", 0.05}, {"endRatio", 0.95}}
                                   .dump());
    const int blocksDuringTrim = audioBlocks.load(std::memory_order_acquire) - blocksBefore;
    const int missesDuringTrim = audioLockMisses.load(std::memory_order_acquire) - missesBefore;

    bool passed = Check(blocksDuringTrim > 0,
                        "riff trim: audio processed while the large buffers were copied and scanned (" +
                            std::to_string(blocksDuringTrim) + " blocks)");
    passed = Check(missesDuringTrim <= 2,
                   "riff trim: only brief snapshot swaps contended with audio (" +
                       std::to_string(missesDuringTrim) + " missed blocks)") &&
             passed;

    controller.HandleUIMessage(nlohmann::json{{"type", "stopRiffCapture"}, {"canceled", true}}.dump());
    return passed;
}

bool TestDeferredRebuildBuiltOffLock(guitarfx::PluginController& controller, const std::atomic<int>& audioBlocks)
{
    using guitarfx::GraphNode;
    gRebuildAudioBlocks = &audioBlocks;

    guitarfx::EffectTypeInfo info;
    info.type = kRebuildProbeType;
    info.displayName = "Deferred rebuild probe";
    info.category = "utility";
    guitarfx::EffectRegistry::Instance().Register(info.type, info,
                                                  [] { return std::make_unique<RebuildProbeEffect>(); });

    guitarfx::Preset preset;
    preset.id = kRebuildPresetId;
    preset.name = kRebuildPresetId;
    preset.version = 2;
    preset.graph.nodes = {GraphNode{"__input__", guitarfx::kNodeTypeInput, "", "", true},
                          GraphNode{kRebuildNodeId, kRebuildProbeType, "utility", "", true},
                          GraphNode{"__output__", guitarfx::kNodeTypeOutput, "", "", true}};
    preset.graph.edges = {{"__input__", kRebuildNodeId, 0, 0, 1.0}, {kRebuildNodeId, "__output__", 0, 0, 1.0}};
    const nlohmann::json load{{"type", "loadPreset"},
                              {"presetId", preset.id},
                              {"preset", nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(preset))}};
    controller.HandleUIMessage(load.dump());

    RebuildProbeEffect* probe = gLatestRebuildProbe.load();

    if (!Check(probe != nullptr, "deferred rebuild: probe node built"))
    {
        return false;
    }

    bool passed = true;
    const auto check = [&](const std::string& what, const std::function<void()>& action) {
        const int builds = gBuilds.load();
        const int commits = gCommits.load();
        const int stopped = gBuildsWithAudioStopped.load();
        const int running = gCommitsWithAudioRunning.load();
        action();
        passed = Check(gBuilds.load() - builds == 1 && gCommits.load() - commits == 1,
                       what + ": built and installed once") &&
                 passed;
        passed = Check(gBuildsWithAudioStopped.load() == stopped, what + ": audio ran while it was built") && passed;
        passed = Check(gCommitsWithAudioRunning.load() == running, what + ": installed under the DSP lock") && passed;
    };

    // The node panel's change is built before its handler returns.
    check("deferred rebuild from the UI", [&] {
        controller.HandleUIMessage(nlohmann::json{{"type", "updateSignalPathNodeParam"},
                                                  {"presetId", kRebuildPresetId},
                                                  {"nodeId", kRebuildNodeId},
                                                  {"paramKey", "rebuild"},
                                                  {"value", 1.0}}
                                       .dump());
    });

    // Automation's, recorded on the audio thread, is found by the idle loop.
    check("deferred rebuild from automation", [&] {
        probe->RequestRebuild();
        controller.OnIdle();
    });

    return passed;
}
} // namespace mixer_instance_lock_test
