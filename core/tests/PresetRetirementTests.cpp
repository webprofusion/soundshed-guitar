#include "PluginController.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/BuiltinEffects.h"
#include "dsp/effects/CompositeEffectProcessor.h"

#include <array>
#include <atomic>
#include <future>
#include <iostream>
#include <thread>

using namespace guitarfx;

namespace
{
constexpr int kBlock = 64;
constexpr const char* kAffineType = "test-main-thread-effect";
bool passed = true;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        passed = false;
        std::cerr << "FAIL: " << message << '\n';
    }
}

Preset MakePreset(const std::string& id, const std::string& type = "passthrough")
{
    Preset preset;
    preset.id = id;
    preset.name = id;
    GraphNode in;
    in.id = "in";
    in.type = kNodeTypeInput;
    GraphNode fx;
    fx.id = "fx";
    fx.type = type;
    GraphNode out;
    out.id = "out";
    out.type = kNodeTypeOutput;
    preset.graph.nodes = {in, fx, out};
    preset.graph.edges = {{"in", "fx", 0, 0, 1.0}, {"fx", "out", 0, 0, 1.0}};
    return preset;
}

struct Lifetime
{
    const std::thread::id owner = std::this_thread::get_id();
    std::atomic<int> destroyed{0};
    std::atomic<int> wrongThread{0};
    std::function<void()> duringPrepare;
};

// Models the hosted processor's thread affinity without requiring a plugin binary or
// hanging the test runner on a synchronous editor callback when retirement is wrong.
class AffineEffect final : public PassthroughProcessor
{
  public:
    explicit AffineEffect(std::shared_ptr<Lifetime> lifetime) : state(std::move(lifetime))
    {
    }

    ~AffineEffect() override
    {
        if (std::this_thread::get_id() != state->owner)
        {
            ++state->wrongThread;
        }

        ++state->destroyed;
    }

    bool RequiresMainThreadLoad() const noexcept override
    {
        return true;
    }

    void Prepare(double rate, int block) override
    {
        PassthroughProcessor::Prepare(rate, block);

        if (state->duringPrepare)
        {
            state->duringPrepare();
        }
    }

  private:
    std::shared_ptr<Lifetime> state;
};

void RegisterAffine(const std::shared_ptr<Lifetime>& state)
{
    EffectTypeInfo info;
    info.type = kAffineType;
    info.category = "utility";
    EffectRegistry::Instance().Register(kAffineType, info, [state] { return std::make_unique<AffineEffect>(state); });
}

void Pump(MultiPresetMixer& mixer)
{
    std::array<float, kBlock> left{}, right{};
    float* channels[] = {left.data(), right.data()};

    for (int i = 0; i < 40; ++i)
    {
        mixer.Process(channels, channels, kBlock);
    }
}

void TestMainThreadRetirement(bool composite, bool drain)
{
    auto state = std::make_shared<Lifetime>();
    RegisterAffine(state);
    auto type = std::string(kAffineType);

    if (composite)
    {
        CompositeEffectDefinition definition;
        definition.id = "retirement-wrapper";
        definition.innerGraph = MakePreset("inner", kAffineType).graph;
        type = definition.GetEffectTypeId();
        EffectTypeInfo info;
        info.type = type;
        info.category = "utility";
        EffectRegistry::Instance().Register(
            type, info, [definition] { return std::make_unique<CompositeEffectProcessor>(definition); });
    }

    {
        ResourceLibrary library;
        MultiPresetMixer mixer;
        mixer.SetResourceLibrary(&library);
        mixer.Prepare(48000.0, kBlock);
        mixer.AddActivePreset(MakePreset("old", type), "old", "old");
        mixer.PreparePresetSwap(MakePreset("new"), "new", "new");
        mixer.CommitPresetSwap();
        std::async(std::launch::async, [&] { Pump(mixer); }).get();
        Check(mixer.GetRetiringPresetCount() == 0, "completed fade leaves the audio graph");

        if (drain)
        {
            mixer.CollectRetiredMainThread();
            Check(state->destroyed == 1, "message-thread collection destroys hosted state");
        }

        // Without a drain, shutdown must collect it on this thread without a reaper/UI join cycle.
    }
    Check(state->destroyed == 1 && state->wrongThread == 0, "hosted state is destroyed exactly once on its owner");

    if (composite)
    {
        EffectRegistry::Instance().Unregister(type);
    }

    EffectRegistry::Instance().Unregister(kAffineType);
}

void TestGlobalChainRetirement()
{
    auto state = std::make_shared<Lifetime>();
    RegisterAffine(state);
    {
        MultiPresetMixer mixer;
        mixer.Prepare(48000.0, kBlock);
        const auto original = mixer.GetGlobalChainConfig();
        auto config = original;
        auto node = MakePreset("global", kAffineType).graph.nodes[1];
        config.postChainGraph.nodes.push_back(node);
        auto edge = config.postChainGraph.edges.front();
        config.postChainGraph.edges.front().to = node.id;
        edge.from = node.id;
        config.postChainGraph.edges.push_back(edge);
        mixer.PrepareGlobalChainSwap(config);
        mixer.CommitGlobalChainSwap();
        mixer.PrepareGlobalChainSwap(original);
        mixer.CommitGlobalChainSwap();
        mixer.CollectRetiredMainThread();
        Check(state->destroyed == 1 && state->wrongThread == 0, "global hosted chain retires on the message thread");
    }
    EffectRegistry::Instance().Unregister(kAffineType);
}

class TestHost final : public IPluginHost
{
  public:
    void SendMessageToUI(const std::string&) override
    {
    }

    void BrowseFileAsync(BrowseFileType, const std::string&, std::function<void(const BrowseFileResult&)>) override
    {
    }

    void SaveFileAsync(BrowseFileType, const std::string&, const std::string&,
                       std::function<void(const BrowseFileResult&)>) override
    {
    }

    void RunOnMainThread(std::function<void()> fn) override
    {
        fn();
    }

    std::filesystem::path GetUserDataPath() const override
    {
        return {};
    }

    std::filesystem::path GetBundledAssetsPath() const override
    {
        return {};
    }

    double GetSampleRate() const override
    {
        return 48000.0;
    }

    int GetBlockSize() const override
    {
        return kBlock;
    }
};

void TestControllerReplacementDoesNotBlockAudio()
{
    TestHost host;
    PluginController controller(host);
    controller.Prepare(48000.0, kBlock);
    auto& mixer = controller.GetMixer();
    mixer.AddActivePreset(MakePreset("slot"), "slot", "slot");
    mixer.AddActivePreset(MakePreset("other"), "other", "other");
    auto state = std::make_shared<Lifetime>();
    RegisterAffine(state);
    int prepareCalls = 0;
    state->duringPrepare = [&] {
        ++prepareCalls;
        auto audioSucceeded = std::async(std::launch::async, [&] {
                                  std::array<float, kBlock> left{}, right{};
                                  float* channels[] = {left.data(), right.data()};
                                  return controller.ProcessAudio(channels, channels, kBlock);
                              }).get();
        Check(audioSucceeded, "audio can process while replacement Prepare is running");
        // Controls changed while preparing must survive the commit.
        mixer.SetPresetMix("slot", 0.3);
        mixer.SetPresetPan("slot", -0.4);
    };
    Check(controller.ReplaceActiveMixerPresetInPlace(MakePreset("slot", kAffineType), "slot", "replacement"),
          "controller replaces the requested slot");
    Check(prepareCalls == 1, "replacement Prepare ran once");
    const auto config = mixer.GetPresetConfig("slot");
    Check(config && config->mix == 0.3 && config->pan == -0.4 && config->name == "replacement",
          "commit preserves current mixer controls and updates the name");
    Check(mixer.GetPresetCount() == 2 && mixer.GetPresetConfig("other").has_value(), "other slot stays live");
    state->duringPrepare = {};
    EffectRegistry::Instance().Unregister(kAffineType);
}
} // namespace

int main()
{
    RegisterAllEffects();

    for (bool composite : {false, true})
    {
        for (bool drain : {false, true})
        {
            TestMainThreadRetirement(composite, drain);
        }
    }

    TestGlobalChainRetirement();
    TestControllerReplacementDoesNotBlockAudio();
    return passed ? 0 : 1;
}
