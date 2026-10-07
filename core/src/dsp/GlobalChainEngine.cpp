#include "dsp/GlobalChainEngine.h"

#include "dsp/GlobalChainEditor.h"

namespace guitarfx
{
void GlobalChainEngine::Load(SignalGraphExecutor& executor, const SignalGraph& graph, const double* inputTrimDb,
                             ChannelLayout inputLayout, const ExecutorSetup& setup)
{
    executor.SetResourceLibrary(setup.resourceLibrary);
    executor.SetInputLayout(inputLayout);
    executor.SetDualMono(setup.dualMono);

    if (setup.nodeTypeConfigDefaults != nullptr)
    {
        executor.SeedNodeTypeConfigDefaults(*setup.nodeTypeConfigDefaults);
    }

    executor.SetGraph(graph);

    if (inputTrimDb != nullptr)
    {
        executor.SetInputTrim(*inputTrimDb);
    }

    executor.SetSignalDiagnosticsEnabled(setup.signalDiagnostics);
    executor.Prepare(setup.sampleRate, setup.maxBlockSize);
}

bool GlobalChainEngine::EnsureUpToDate(const ExecutorSetup& setup)
{
    if (!setup.prepared || !mNeedsRebuild.load(std::memory_order_acquire))
    {
        return false;
    }

    Rebuild(setup);
    return true;
}

void GlobalChainEngine::Rebuild(const ExecutorSetup& setup)
{
    auto preGraph = mConfig.BuildPreChainGraph();

    if (preGraph.nodes.empty() && preGraph.edges.empty())
    {
        preGraph = GlobalSignalChainConfig::BuildDefaultPreChainGraph();
        mConfig.preChainGraph = preGraph;
    }

    Load(*mPre, GlobalChainEditor::LivePreChain(preGraph), &mConfig.inputGain, setup.inputLayout, setup);

    auto postGraph = mConfig.BuildPostChainGraph();

    if (postGraph.nodes.empty() && postGraph.edges.empty())
    {
        postGraph = GlobalSignalChainConfig::BuildDefaultPostChainGraph();
        mConfig.postChainGraph = postGraph;
    }

    Load(*mPost, postGraph, nullptr, ChannelLayout::Stereo, setup);

    mNeedsRebuild.store(false, std::memory_order_release);
}

bool GlobalChainEngine::Adopt(GlobalSignalChainConfig normalized)
{
    // Rebuilding tears down and recreates both executors — construction, resource loading
    // and allocation. Skip it entirely when the graphs are unchanged, which is the common
    // case: global settings are per-instance state and do not come from presets, so most
    // preset loads pass through a config identical to the one already running.
    const bool graphsChanged =
        normalized.preChainGraph != mConfig.preChainGraph || normalized.postChainGraph != mConfig.postChainGraph;

    mConfig = std::move(normalized);

    if (graphsChanged)
    {
        MarkNeedsRebuild();
    }

    return graphsChanged;
}

bool GlobalChainEngine::PrepareSwap(GlobalSignalChainConfig normalized, const ExecutorSetup& setup)
{
    const bool graphsChanged =
        normalized.preChainGraph != mConfig.preChainGraph || normalized.postChainGraph != mConfig.postChainGraph;
    const bool rebuildNeeded = setup.prepared && (graphsChanged || mNeedsRebuild.load(std::memory_order_acquire));

    mPendingPre.reset();
    mPendingPost.reset();
    mPendingConfig = std::move(normalized);

    if (!rebuildNeeded)
    {
        return false;
    }

    // Expensive part: runs on the caller's thread with no DSP lock held.
    mPendingPre = std::make_unique<SignalGraphExecutor>();
    mPendingPost = std::make_unique<SignalGraphExecutor>();
    Load(*mPendingPre, GlobalChainEditor::LivePreChain(mPendingConfig->preChainGraph), &mPendingConfig->inputGain,
         setup.inputLayout, setup);
    Load(*mPendingPost, mPendingConfig->postChainGraph, nullptr, ChannelLayout::Stereo, setup);
    return true;
}

bool GlobalChainEngine::CommitSwap()
{
    if (!mPendingConfig.has_value())
    {
        return false;
    }

    mConfig = std::move(*mPendingConfig);
    mPendingConfig.reset();

    if (mPendingPre && mPendingPost)
    {
        // The outgoing executors go to the reaper whole rather than being destroyed here: the
        // audio thread try_locks the DSP mutex and outputs silence when it cannot take it, so
        // freeing node state, or joining worker threads, under that lock is an audible dropout.
        mReaper.RetireExecutor(std::move(mPre));
        mReaper.RetireExecutor(std::move(mPost));

        mPre = std::move(mPendingPre);
        mPost = std::move(mPendingPost);
        mNeedsRebuild.store(false, std::memory_order_release);
    }

    mPendingPre.reset();
    mPendingPost.reset();
    return true;
}
} // namespace guitarfx
