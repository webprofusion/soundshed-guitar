#include "dsp/SignalGraphExecutor.h"
#include "dsp/SignalGraphExecutorInternal.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/effects/MixerEffect.h"
#include "dsp/effects/InputAnalyzerEffect.h"
#include "dsp/effects/CompositeEffectProcessor.h"
#include "dsp/RealtimeParallel.h"
#include "resources/ResourceLibrary.h"

#include <algorithm>
#include <cmath>
#include <future>
#include <chrono>
#include <tuple>

namespace guitarfx
{
using namespace guitarfx::executor_detail;

namespace
{
/// A graph's deferred rebuilds (SignalGraphExecutor::TakeDeferredRebuilds), one per node that has
/// one, built together.
class GraphRebuild final : public DeferredRebuild
{
  public:
    struct Node
    {
        std::string nodeId;
        EffectProcessor* processor = nullptr;
        std::unique_ptr<DeferredRebuild> work;
    };

    void Build() override
    {
        for (auto& node : nodes)
        {
            node.work->Build();
        }
    }

    std::vector<Node> nodes;
};

ResourceRef HydrateResolvedResourceRef(const ResourceRef& ref, const ResourceLibrary* resourceLibrary)
{
    ResourceRef hydrated = ref;

    if (!resourceLibrary || !ref.IsLibraryRef())
    {
        return hydrated;
    }

    auto resource = resourceLibrary->LookupResource(ref.resourceType, ref.resourceId);

    if (!resource)
    {
        return hydrated;
    }

    hydrated.metadata = resource->metadata;

    if (!resource->hash.empty() && !hydrated.metadata.count("resourceHash"))
    {
        hydrated.metadata["resourceHash"] = resource->hash;
    }

    return hydrated;
}

std::optional<std::filesystem::path> ResolveResourcePath(const ResourceRef& ref, const ResourceLibrary* resourceLibrary)
{
    if (resourceLibrary)
    {
        if (auto path = resourceLibrary->ResolveResource(ref))
        {
            return path;
        }
    }

    if (ref.IsFilePath())
    {
        return ref.filePath;
    }

    return std::nullopt;
}

bool ShouldUseParallelLevel(int levelCount, int levelScore, int numSamples, bool executorParallelEnabled,
                            bool workersAvailable)
{
    if (!executorParallelEnabled || !workersAvailable)
    {
        return false;
    }

    if (levelCount < 2)
    {
        return false;
    }

    // Keep level parallelization for blocks/levels with enough expected CPU work.
    const int totalWorkUnits = levelScore * numSamples;
    return totalWorkUnits >= kMinLevelParallelWorkUnits;
}
} // namespace

SignalGraphExecutor::SignalGraphExecutor() = default;

SignalGraphExecutor::~SignalGraphExecutor()
{
    mWorkerPool.Stop();
}

void SignalGraphExecutor::SetGraph(const SignalGraph& graph)
{
    mGraph = graph;
    mPrepared = false;
    mNodeStates.clear();

    // CreateProcessors applies node params in map order, so a node carrying one parameter
    // under two spellings would let the key that sorts later decide the value. Fold them
    // here rather than trusting every producer of a graph to have done it.
    for (auto& node : mGraph.nodes)
    {
        CanonicalizeNodeParams(node);
    }

    // Add implicit input/output nodes if they're referenced in edges but not in nodes
    bool hasInputNode = false;
    bool hasOutputNode = false;

    for (const auto& node : mGraph.nodes)
    {
        if (node.id == "__input__" || node.type == kNodeTypeInput)
        {
            hasInputNode = true;
        }

        if (node.id == "__output__" || node.type == kNodeTypeOutput)
        {
            hasOutputNode = true;
        }
    }

    // Check if edges reference __input__ or __output__
    bool edgesReferenceInput = false;
    bool edgesReferenceOutput = false;

    for (const auto& edge : mGraph.edges)
    {
        if (edge.from == "__input__")
        {
            edgesReferenceInput = true;
        }

        if (edge.to == "__output__")
        {
            edgesReferenceOutput = true;
        }
    }

    // Add implicit nodes if needed
    if (edgesReferenceInput && !hasInputNode)
    {
        GraphNode inputNode;
        inputNode.id = "__input__";
        inputNode.type = kNodeTypeInput;
        inputNode.enabled = true;
        mGraph.nodes.insert(mGraph.nodes.begin(), inputNode);
    }

    if (edgesReferenceOutput && !hasOutputNode)
    {
        GraphNode outputNode;
        outputNode.id = "__output__";
        outputNode.type = kNodeTypeOutput;
        outputNode.enabled = true;
        mGraph.nodes.push_back(outputNode);
    }

    // Process() reads the Input and Output nodes' gain from here, and automation can only write
    // it without allocating to a key already held (SetAutomationTargetParam). 0 dB is what a
    // missing key reads as anyway.
    for (auto& node : mGraph.nodes)
    {
        if (node.type == kNodeTypeInput || node.type == kNodeTypeOutput)
        {
            node.params.try_emplace(kBoundaryGainParam, 0.0);
        }
    }

    // Each node's incoming edges, for the plan and the latency walk.
    mIncomingEdgesByNode.clear();

    for (const auto& node : mGraph.nodes)
    {
        mIncomingEdgesByNode[node.id] = {};
    }

    for (std::size_t i = 0; i < mGraph.edges.size(); ++i)
    {
        mIncomingEdgesByNode[mGraph.edges[i].to].push_back(i);
    }

    BuildExecutionLevels();
    CreateProcessors();
    // Must follow CreateProcessors(): the plan holds pointers into mNodeStates and to the
    // processors it creates. Processors are only ever built there, and mNodeStates is only
    // repopulated here, so this is the single place the plan can go stale.
    BuildExecutionPlan();
    // After the plan, whose layout pass has told every node its input layout.
    LoadPendingResources();
}

void SignalGraphExecutor::BuildExecutionLevels()
{
    // Kahn's algorithm, peeling the graph a level at a time: a level is every node whose
    // sources have all run, so its nodes are independent of each other and may run in
    // parallel, and the levels in order are the execution order. A node left over has a
    // source that never runs -- a cycle -- and an edge naming a node the graph does not have
    // is as broken; either way the graph is invalid and Process() outputs silence.
    mExecutionOrder.clear();
    mExecutionLevels.clear();
    mExecutionLevelScores.clear();
    mIsValid = false;

    std::map<std::string, int> inDegree;
    std::map<std::string, std::vector<std::string>> adjacency;

    for (const auto& node : mGraph.nodes)
    {
        inDegree[node.id] = 0;
        adjacency[node.id];
    }

    for (const auto& edge : mGraph.edges)
    {
        if (inDegree.count(edge.from) == 0 || inDegree.count(edge.to) == 0)
        {
            return;
        }

        adjacency[edge.from].push_back(edge.to);
        ++inDegree[edge.to];
    }

    std::vector<std::string> frontier;

    for (const auto& [id, degree] : inDegree)
    {
        if (degree == 0)
        {
            frontier.push_back(id);
        }
    }

    std::size_t processed = 0;

    while (!frontier.empty())
    {
        int levelScore = 0;
        std::vector<std::string> next;

        for (const auto& id : frontier)
        {
            if (const auto* node = mGraph.FindNode(id))
            {
                levelScore += ScoreNodeTypeForParallelWork(node->type);
            }

            for (const auto& neighbor : adjacency[id])
            {
                if (--inDegree[neighbor] == 0)
                {
                    next.push_back(neighbor);
                }
            }
        }

        processed += frontier.size();
        mExecutionOrder.insert(mExecutionOrder.end(), frontier.begin(), frontier.end());
        mExecutionLevelScores.push_back(levelScore);
        mExecutionLevels.push_back(std::move(frontier));
        frontier = std::move(next);
    }

    mIsValid = processed == mGraph.nodes.size();

    if (!mIsValid)
    {
        mExecutionOrder.clear();
        mExecutionLevels.clear();
        mExecutionLevelScores.clear();
    }
}

void SignalGraphExecutor::CreateProcessors()
{
    auto& registry = EffectRegistry::Instance();
    auto& resourceWorkItems = mPendingResourceLoads;
    resourceWorkItems.clear();

    // Create processors, apply params/config, and resolve resource paths. The loads themselves
    // wait for LoadPendingResources(), after the plan has resolved each node's input layout.
    // Resource path resolution is fast (library lookups only); actual loading is deferred.
    for (const auto& node : mGraph.nodes)
    {
        auto [it, inserted] =
            mNodeStates.emplace(std::piecewise_construct, std::forward_as_tuple(node.id), std::forward_as_tuple());
        NodeState& state = it->second;
        state.id = node.id;
        state.sharedId = std::make_shared<const std::string>(node.id);
        state.type = node.type;
        state.category = node.category;
        state.monoFold = node.channelMode == kChannelModeMono        ? NodeState::MonoFold::Sum
                         : node.channelMode == kChannelModeMonoLeft  ? NodeState::MonoFold::Left
                         : node.channelMode == kChannelModeMonoRight ? NodeState::MonoFold::Right
                                                                     : NodeState::MonoFold::None;

        // Create processor based on type
        if (node.type == kNodeTypeInput || node.type == kNodeTypeOutput)
        {
            // Input/output are handled specially, use passthrough
            state.processor = std::make_unique<PassthroughProcessor>();
        }
        else if (node.type == kNodeTypeSplitter)
        {
            // Splitter is handled specially with passthrough
            state.processor = std::make_unique<PassthroughProcessor>();
        }
        else if (node.type == kNodeTypeMixer)
        {
            // Mixer uses MixerEffect for per-input control (level, pan, delay)
            state.processor = registry.Create(node.type);

            if (!state.processor)
            {
                state.processor = std::make_unique<PassthroughProcessor>();
            }
        }
        else
        {
            // Create from registry
            state.processor = registry.Create(node.type);
        }

        if (state.processor)
        {
            ConfigureNodeProcessor(*state.processor, node, &resourceWorkItems);
            state.processor->SetDualMono(mDualMono);

            // Dual mono runs a coupled node twice, once per side (see the class comment).
            if (mDualMono && NeedsDualMonoTwin(state))
            {
                state.twin = CreateDualMonoTwin(node, &resourceWorkItems);
            }
        }
    }
}

void SignalGraphExecutor::ConfigureNodeProcessor(EffectProcessor& processor, const GraphNode& node,
                                                 std::vector<PendingResourceLoad>* loads)
{
    // A composite needs the resource library, and the per-instance type defaults (NAM quality),
    // for the nodes inside it.
    if (auto* composite = dynamic_cast<CompositeEffectProcessor*>(&processor))
    {
        if (mResourceLibrary)
        {
            composite->SetResourceLibrary(mResourceLibrary);
        }

        composite->SeedInnerNodeTypeConfigDefaults(mNodeTypeConfigDefaults);
    }

    processor.SetEnabled(node.enabled);

    for (const auto& [key, value] : node.params)
    {
        processor.SetParam(key, value);
    }

    // Per-instance type defaults first, so a node's own config still wins.
    if (const auto typeDefaults = mNodeTypeConfigDefaults.find(node.type);
        typeDefaults != mNodeTypeConfigDefaults.end())
    {
        for (const auto& [key, value] : typeDefaults->second)
        {
            processor.SetConfig(key, value);
        }
    }

    for (const auto& [key, value] : node.config)
    {
        processor.SetConfig(key, value);
    }

    if (node.resources.empty())
    {
        return;
    }

    // Resolve resource paths (fast — just library lookups).
    PendingResourceLoad load{&processor, {}, {}};
    load.refs.reserve(node.resources.size());
    load.paths.reserve(node.resources.size());

    for (std::size_t resourceIndex = 0; resourceIndex < node.resources.size(); ++resourceIndex)
    {
        const auto& res = node.resources[resourceIndex];

        if (!res.IsValid())
        {
            continue;
        }

        ResourceRef hydratedRef = HydrateResolvedResourceRef(res, mResourceLibrary);
        hydratedRef.metadata["resourceSlotIndex"] = std::to_string(resourceIndex);
        auto path = ResolveResourcePath(hydratedRef, mResourceLibrary);

        if (path)
        {
            load.refs.push_back(hydratedRef);
            load.paths.push_back(*path);
        }
    }

    // All defined resource slots cleared: an empty load lets the processor unload what it had
    // rather than keep stale state.
    if (load.paths.empty() && !processor.HasResource())
    {
        return;
    }

    if (loads)
    {
        loads->push_back(std::move(load));
    }
    else
    {
        processor.LoadResources(load.refs, load.paths);
    }
}

bool SignalGraphExecutor::NeedsDualMonoTwin(const NodeState& state)
{
    // A hosted plugin cannot be kept in step with its own editor, so it runs shared.
    return state.processor && !state.processor->KeepsChannelsSeparate() && !state.processor->RequiresMainThreadLoad();
}

std::unique_ptr<EffectProcessor> SignalGraphExecutor::CreateDualMonoTwin(const GraphNode& node,
                                                                         std::vector<PendingResourceLoad>* loads)
{
    auto twin = EffectRegistry::Instance().Create(node.type);

    if (!twin)
    {
        return nullptr;
    }

    ConfigureNodeProcessor(*twin, node, loads);
    // It hears one side, on both channels.
    twin->SetInputLayout(ChannelLayout::Mono);
    return twin;
}

void SignalGraphExecutor::SetDualMono(bool dualMono)
{
    mDualMono = dualMono;

    for (auto& [id, state] : mNodeStates)
    {
        if (!state.processor)
        {
            continue;
        }

        if (dualMono && state.pendingTwin && !state.twin)
        {
            state.twin = std::move(state.pendingTwin);
        }

        // Not running yet (a graph being built, or a composite's inside while its parent is):
        // build what is missing here, off the lock.
        if (dualMono && !mPrepared && !state.twin && NeedsDualMonoTwin(state))
        {
            if (const GraphNode* node = mGraph.FindNode(id))
            {
                state.twin = CreateDualMonoTwin(*node, nullptr);
            }
        }

        state.processor->SetDualMono(dualMono);
    }

    if (!mPlan.empty())
    {
        ResolveChannelLayout(false);
    }
}

void SignalGraphExecutor::StageDualMonoTwins()
{
    for (auto& [id, state] : mNodeStates)
    {
        if (auto* composite = dynamic_cast<CompositeEffectProcessor*>(state.processor.get()))
        {
            composite->StageInnerDualMonoTwins();
            continue;
        }

        if (state.twin || state.pendingTwin || !NeedsDualMonoTwin(state))
        {
            continue;
        }

        const GraphNode* node = mGraph.FindNode(id);

        if (!node)
        {
            continue;
        }

        auto twin = CreateDualMonoTwin(*node, nullptr);

        if (!twin)
        {
            continue;
        }

        if (mAppliedTempoBpm > 0.0)
        {
            twin->SetParam("bpm", mAppliedTempoBpm);
        }

        if (mPrepared)
        {
            twin->Prepare(mSampleRate, mMaxBlockSize);
            const auto size = static_cast<std::size_t>(mMaxBlockSize);
            state.twinScratchLeft.assign(size, 0.0f);
            state.twinScratchRight.assign(size, 0.0f);
        }

        state.pendingTwin = std::move(twin);
    }
}

std::vector<std::string> SignalGraphExecutor::DualMonoSharedNodes() const
{
    std::vector<std::string> shared;

    if (!mDualMono)
    {
        return shared;
    }

    for (const auto& [id, state] : mNodeStates)
    {
        if (state.processor && !state.processor->KeepsChannelsSeparate() && !state.twin)
        {
            shared.push_back(id);
        }
    }

    return shared;
}

void SignalGraphExecutor::LoadPendingResources()
{
    auto& resourceWorkItems = mPendingResourceLoads;

    // Effects that require main-thread execution (e.g. plugin hosts using JUCE's
    // MessageManager) must run on the calling thread to avoid deadlocking when
    // MessageManager::callSync is used from within a std::async worker.
    // All other effects (NAM models, IR files) are safe to load concurrently.
    std::vector<PendingResourceLoad*> mainThreadWork;
    std::vector<PendingResourceLoad*> parallelWork;

    for (auto& work : resourceWorkItems)
    {
        if (work.processor && work.processor->RequiresMainThreadLoad())
        {
            mainThreadWork.push_back(&work);
        }
        else
        {
            parallelWork.push_back(&work);
        }
    }

    // Run main-thread-required loads first, serially on the calling thread.
    for (auto* work : mainThreadWork)
    {
        work->processor->LoadResources(work->refs, work->paths);
    }

    // Run remaining loads in parallel.
    if (parallelWork.size() > 1)
    {
        std::vector<std::future<void>> futures;
        futures.reserve(parallelWork.size());

        for (auto* work : parallelWork)
        {
            futures.push_back(
                std::async(std::launch::async, [work]() { work->processor->LoadResources(work->refs, work->paths); }));
        }

        for (auto& f : futures)
        {
            f.get();
        }
    }
    else if (parallelWork.size() == 1)
    {
        auto* work = parallelWork[0];
        work->processor->LoadResources(work->refs, work->paths);
    }

    resourceWorkItems.clear();
}

bool SignalGraphExecutor::AnyNodeRequiresMainThreadLoad() const
{
    for (const auto& entry : mNodeStates)
    {
        if (entry.second.processor && entry.second.processor->RequiresMainThreadLoad())
        {
            return true;
        }
    }

    return false;
}

std::vector<std::string> SignalGraphExecutor::GetNodeTypes() const
{
    std::vector<std::string> types;
    types.reserve(mNodeStates.size());

    for (const auto& entry : mNodeStates)
    {
        types.push_back(entry.second.type);
    }

    return types;
}

std::vector<std::string> SignalGraphExecutor::GetNodeTypesDeep() const
{
    std::vector<std::string> types;
    types.reserve(mNodeStates.size());

    for (const auto& entry : mNodeStates)
    {
        types.push_back(entry.second.type);

        if (const auto* composite = dynamic_cast<const CompositeEffectProcessor*>(entry.second.processor.get()))
        {
            const auto inner = composite->GetInnerExecutor().GetNodeTypesDeep();
            types.insert(types.end(), inner.begin(), inner.end());
        }
    }

    return types;
}

const SignalGraphExecutor::NodeState* SignalGraphExecutor::FindNodeState(const std::string& id) const
{
    auto it = mNodeStates.find(id);
    return it != mNodeStates.end() ? &it->second : nullptr;
}

void SignalGraphExecutor::Prepare(double sampleRate, int maxBlockSize)
{
    mSampleRate = sampleRate;
    mMaxBlockSize = maxBlockSize;
    mPrepared = true;

    AllocateBuffers(maxBlockSize);

    // Node Prepare() is a large share of preset-switch latency — NAM prewarm and IR
    // partition building both land here — and nodes are independent at this point, so it
    // is dispatched the same way resource loading is in CreateProcessors(). Effects that
    // require the main thread (plugin hosts marshalling through JUCE's MessageManager)
    // stay on the calling thread; everything else runs concurrently.
    std::vector<EffectProcessor*> mainThreadPrepare;
    std::vector<EffectProcessor*> parallelPrepare;

    for (auto& [id, state] : mNodeStates)
    {
        for (EffectProcessor* processor : {state.processor.get(), state.twin.get(), state.pendingTwin.get()})
        {
            if (!processor)
            {
                continue;
            }

            if (processor->RequiresMainThreadLoad())
            {
                mainThreadPrepare.push_back(processor);
            }
            else
            {
                parallelPrepare.push_back(processor);
            }
        }
    }

    for (auto* processor : mainThreadPrepare)
    {
        processor->Prepare(sampleRate, maxBlockSize);
    }

    if (parallelPrepare.size() > 1)
    {
        std::vector<std::future<void>> futures;
        futures.reserve(parallelPrepare.size());

        for (auto* processor : parallelPrepare)
        {
            futures.push_back(std::async(std::launch::async, [processor, sampleRate, maxBlockSize]() {
                processor->Prepare(sampleRate, maxBlockSize);
            }));
        }

        for (auto& f : futures)
        {
            f.get();
        }
    }
    else if (parallelPrepare.size() == 1)
    {
        parallelPrepare[0]->Prepare(sampleRate, maxBlockSize);
    }

    std::size_t maxLevelWidth = 0;
    int maxLevelScore = 0;

    for (std::size_t i = 0; i < mExecutionLevels.size(); ++i)
    {
        maxLevelWidth = std::max(maxLevelWidth, mExecutionLevels[i].size());

        if (i < mExecutionLevelScores.size())
        {
            maxLevelScore = std::max(maxLevelScore, mExecutionLevelScores[i]);
        }
    }

    const unsigned int hw = std::thread::hardware_concurrency();
    const int hardwareWorkerBudget = rtparallel::kParallelDspSupported ? static_cast<int>(hw > 1 ? hw - 1 : 0) : 0;
    const int graphWorkerLimit = std::max(0, static_cast<int>(maxLevelWidth) - 1);
    const int workerCount = std::min({hardwareWorkerBudget, graphWorkerLimit, kMaxParallelWorkers});
    const bool graphHasMeaningfulParallelLevel = (maxLevelScore * maxBlockSize) >= kMinLevelParallelWorkUnits;
    mUseParallelLevels = maxLevelWidth > 1 && workerCount > 0 && graphHasMeaningfulParallelLevel;

    mWorkerPool.Start(mUseParallelLevels ? workerCount : 0);
}

void SignalGraphExecutor::Reset()
{
    for (auto& [id, state] : mNodeStates)
    {
        if (state.processor)
        {
            state.processor->Reset();
        }

        if (state.twin)
        {
            state.twin->Reset();
        }
    }
}

void SignalGraphExecutor::AllocateBuffers(int maxBlockSize)
{
    const auto size = static_cast<size_t>(maxBlockSize);

    for (auto& [id, state] : mNodeStates)
    {
        state.bufferLeft.assign(size, 0.0f);
        state.bufferRight.assign(size, 0.0f);
        state.scratchLeft.assign(size, 0.0f);
        state.scratchRight.assign(size, 0.0f);

        if (state.twin || state.pendingTwin)
        {
            state.twinScratchLeft.assign(size, 0.0f);
            state.twinScratchRight.assign(size, 0.0f);
        }
    }
}

void SignalGraphExecutor::Process(float** inputs, float** outputs, int numSamples)
{
    const bool diagnosticsEnabled = mSignalDiagnosticsEnabled.load(std::memory_order_acquire);

    // Only clock the block when something is going to read the result. The flag follows
    // editor visibility (see the "uiVisibility" handler), and TelemetryPublisher only
    // sends the performance feed while the UI is visible, so with it off these totals
    // have no consumer -- and two clock reads per block per graph is not free.
    std::chrono::high_resolution_clock::time_point totalStart;

    if (diagnosticsEnabled)
    {
        totalStart = std::chrono::high_resolution_clock::now();
    }

    // Clamp to allocated buffer size to prevent out-of-bounds writes
    numSamples = std::min(numSamples, mMaxBlockSize);

    if (!mIsValid || !mPrepared || !inputs || !outputs)
    {
        // Output silence if not ready
        if (outputs)
        {
            if (outputs[0])
            {
                std::fill(outputs[0], outputs[0] + numSamples, 0.0f);
            }

            if (outputs[1])
            {
                std::fill(outputs[1], outputs[1] + numSamples, 0.0f);
            }
        }

        return;
    }

    // Refresh the level meters only on the blocks something will actually read. The UI
    // pulls them at kSignalDiagnosticsRateHz; at a 64-sample block that is one read per
    // ~37 blocks, so metering every block was scanning every node's buffers dozens of
    // times over to publish numbers each of which was overwritten before anyone saw it.
    bool collectLevels = false;

    if (diagnosticsEnabled)
    {
        mMeteringCountdownSamples -= numSamples;

        if (mMeteringCountdownSamples <= 0)
        {
            collectLevels = true;
            constexpr double kMeteringRefreshHz = 30.0; // comfortably ahead of the 20 Hz feed
            mMeteringCountdownSamples = std::max(1, static_cast<int>(mSampleRate / kMeteringRefreshHz));
        }
    }

    // Clear all buffers and reset input flags
    for (auto& planned : mPlan)
    {
        NodeState& state = *planned.state;
        std::fill(state.bufferLeft.begin(), state.bufferLeft.begin() + numSamples, 0.0f);
        std::fill(state.bufferRight.begin(), state.bufferRight.begin() + numSamples, 0.0f);
        state.hasInput = false;
        state.channelCount.store(0, std::memory_order_relaxed);
        state.notesLastBlock = state.notesThisBlock;
        state.notesThisBlock = false;

        if (diagnosticsEnabled)
        {
            // "Did not run this block" has to be reset every block, even when the level
            // meters are not being refreshed.
            state.processingTimeUs.store(kNodeDidNotRunUs, std::memory_order_relaxed);
        }

        if (collectLevels)
        {
            state.peak.store(0.0, std::memory_order_relaxed);
            state.rms.store(0.0, std::memory_order_relaxed);
            state.clipCount.store(0, std::memory_order_relaxed);
        }
    }

    // Apply input trim (global + input node gain)
    const float inputGain = mInputGainCache.Get(mInputTrim + (mInputNodeGainDb ? *mInputNodeGainDb : 0.0));

    if (mInputPlanNode)
    {
        NodeState& state = *mInputPlanNode->state;
        const bool inputEnabled = !state.processor || state.processor->IsEnabled();

        if (inputEnabled && inputs[0])
        {
            for (int i = 0; i < numSamples; ++i)
            {
                state.bufferLeft[static_cast<size_t>(i)] = inputs[0][i] * inputGain;
            }
        }

        // The layout is the caller's word, not something read off the samples: a mono input is
        // the left channel on both sides, whatever the right pointer holds, and a stereo input
        // keeps its right channel even while it is silent. Only a stereo input with no right
        // channel at all falls back to the left.
        if (inputEnabled && state.outputStereo && inputs[1])
        {
            for (int i = 0; i < numSamples; ++i)
            {
                state.bufferRight[static_cast<size_t>(i)] = inputs[1][i] * inputGain;
            }
        }
        else if (inputEnabled)
        {
            std::copy_n(state.bufferLeft.data(), numSamples, state.bufferRight.data());
        }

        state.hasInput = true;
        state.channelCount.store(state.outputStereo ? 2 : 1, std::memory_order_relaxed);

        if (collectLevels)
        {
            const auto stats = ComputeLevelStats(state.bufferLeft.data(), state.bufferRight.data(), numSamples);
            state.peak.store(stats.peak, std::memory_order_relaxed);
            state.rms.store(stats.rms, std::memory_order_relaxed);
            state.clipCount.store(stats.clipCount, std::memory_order_relaxed);
        }
    }

    for (std::size_t levelIndex = 0; levelIndex < mExecutionLevelPlan.size(); ++levelIndex)
    {
        const auto& level = mExecutionLevelPlan[levelIndex];
        const int levelCount = static_cast<int>(level.size());
        const int levelScore = (levelIndex < mExecutionLevelScores.size()) ? mExecutionLevelScores[levelIndex] : 0;
        const bool useParallelLevel = ShouldUseParallelLevel(
            levelCount, levelScore, numSamples,
            mUseParallelLevels && mParallelLevelsEnabled.load(std::memory_order_acquire), mWorkerPool.HasWorkers());

        if (useParallelLevel)
        {
            // This thread runs nodes alongside the workers; the pool returns once the whole
            // level has run, so the next level reads finished buffers.
            auto processNode = [&](int index) {
                ProcessPlannedNode(mPlan[static_cast<size_t>(level[static_cast<size_t>(index)])], numSamples,
                                   diagnosticsEnabled, collectLevels);
            };

            mWorkerPool.Run(levelCount, processNode);
        }
        else
        {
            for (const int planIndex : level)
            {
                ProcessPlannedNode(mPlan[static_cast<size_t>(planIndex)], numSamples, diagnosticsEnabled,
                                   collectLevels);
            }
        }
    }

    // Copy the output node's buffers out, applying trim.
    const float outputGain = mOutputGainCache.Get(mOutputTrim + (mOutputNodeGainDb ? *mOutputNodeGainDb : 0.0));

    for (const PlannedNode* plannedOutput : mOutputPlanNodes)
    {
        const NodeState& state = *plannedOutput->state;

        if (state.hasInput)
        {
            const bool outputEnabled = !state.processor || state.processor->IsEnabled();

            // Both channels as they are: a mono connection already holds the same samples on
            // each, so nothing is copied over anything.
            if (outputs[0])
            {
                for (int i = 0; i < numSamples; ++i)
                {
                    outputs[0][i] = outputEnabled ? (state.bufferLeft[static_cast<size_t>(i)] * outputGain) : 0.0f;
                }
            }

            if (outputs[1])
            {
                for (int i = 0; i < numSamples; ++i)
                {
                    outputs[1][i] = outputEnabled ? (state.bufferRight[static_cast<size_t>(i)] * outputGain) : 0.0f;
                }
            }

            break;
        }
    }

    if (diagnosticsEnabled)
    {
        const auto totalEnd = std::chrono::high_resolution_clock::now();
        const std::chrono::duration<double, std::micro> totalDuration(totalEnd - totalStart);
        const double totalProcessingTimeUs = totalDuration.count();
        const double realTimeUs = (static_cast<double>(numSamples) / mSampleRate) * 1e6;

        mLastTotalProcessingTimeUs.store(totalProcessingTimeUs, std::memory_order_relaxed);
        mLastRealTimeUs.store(realTimeUs, std::memory_order_relaxed);
        mLastDspLoadPercent.store((realTimeUs > 0.0) ? (totalProcessingTimeUs / realTimeUs) * 100.0 : 0.0,
                                  std::memory_order_relaxed);
    }
}

SignalGraphExecutor::DSPPerformanceStats SignalGraphExecutor::GetPerformanceStats() const
{
    DSPPerformanceStats stats;
    // With diagnostics off the audio thread stops timing entirely -- both the block totals
    // and the per-node times. Whatever was last published is from whenever it was last on,
    // so report nothing rather than stale numbers. Nothing consumes these while it is off:
    // the flag follows editor visibility, and the performance feed is only sent to a
    // visible UI. Node latency below is read live from the processors either way.
    const bool diagnosticsEnabled = mSignalDiagnosticsEnabled.load(std::memory_order_acquire);

    if (diagnosticsEnabled)
    {
        stats.totalProcessingTimeUs = mLastTotalProcessingTimeUs.load(std::memory_order_relaxed);
        stats.realTimeUs = mLastRealTimeUs.load(std::memory_order_relaxed);
        stats.dspLoadPercent = mLastDspLoadPercent.load(std::memory_order_relaxed);
    }

    for (const auto& [nodeId, state] : mNodeStates)
    {
        const int latencySamples =
            (state.processor && state.processor->IsEnabled()) ? state.processor->GetLatencySamples() : 0;
        stats.nodeLatencySamples[nodeId] = latencySamples;

        if (!diagnosticsEnabled)
        {
            continue;
        }

        // A node that did not run in the last block (bypassed, or no input) is left out, so
        // the UI renders a blank rather than a zero. Callers scope these ids -- see
        // MultiPresetMixer::mergeStats.
        const double nodeTimeUs = state.processingTimeUs.load(std::memory_order_relaxed);

        if (nodeTimeUs >= 0.0)
        {
            stats.nodeProcessingTimesUs[nodeId] = nodeTimeUs;
        }
    }

    return stats;
}

int SignalGraphExecutor::GetTotalLatencySamples() const
{
    std::map<std::string, int> cumulativeLatencyByNode;

    for (const auto& nodeId : mExecutionOrder)
    {
        int maxIncomingLatency = 0;

        if (auto incomingIt = mIncomingEdgesByNode.find(nodeId); incomingIt != mIncomingEdgesByNode.end())
        {
            for (const auto edgeIndex : incomingIt->second)
            {
                if (edgeIndex >= mGraph.edges.size())
                {
                    continue;
                }

                const auto& edge = mGraph.edges[edgeIndex];
                auto sourceLatencyIt = cumulativeLatencyByNode.find(edge.from);

                if (sourceLatencyIt != cumulativeLatencyByNode.end())
                {
                    maxIncomingLatency = std::max(maxIncomingLatency, sourceLatencyIt->second);
                }
            }
        }

        int ownLatency = 0;
        auto it = mNodeStates.find(nodeId);

        if (it != mNodeStates.end() && it->second.processor && it->second.processor->IsEnabled())
        {
            ownLatency = it->second.processor->GetLatencySamples();
        }

        cumulativeLatencyByNode[nodeId] = maxIncomingLatency + ownLatency;
    }

    if (auto outputIt = cumulativeLatencyByNode.find("__output__"); outputIt != cumulativeLatencyByNode.end())
    {
        return outputIt->second;
    }

    int maxLatency = 0;

    for (const auto& [_, latency] : cumulativeLatencyByNode)
    {
        maxLatency = std::max(maxLatency, latency);
    }

    return maxLatency;
}

std::vector<SignalGraphExecutor::NodeSignalLevel> SignalGraphExecutor::GetNodeSignalLevels() const
{
    std::vector<NodeSignalLevel> result;
    result.reserve(mNodeStates.size());

    for (const auto& [id, state] : mNodeStates)
    {
        NodeSignalLevel entry;
        entry.nodeId = state.id;
        entry.nodeType = state.type;
        entry.peak = state.peak.load(std::memory_order_relaxed);
        entry.rms = state.rms.load(std::memory_order_relaxed);
        entry.clipCount = state.clipCount.load(std::memory_order_relaxed);
        entry.channelCount = state.channelCount.load(std::memory_order_relaxed);
        const auto* analyzerEffect =
            state.processor ? dynamic_cast<const InputAnalyzerEffect*>(state.processor.get()) : nullptr;

        if (analyzerEffect)
        {
            const auto snapshot = analyzerEffect->GetTelemetrySnapshot();
            NodeSignalLevel::AnalyzerTelemetry analyzer;
            analyzer.peakPercent = snapshot.peakPercent;
            analyzer.rmsPercent = snapshot.rmsPercent;
            analyzer.rmsDbu = snapshot.rmsDbu;
            analyzer.rmsDbv = snapshot.rmsDbv;
            analyzer.rmsVolts = snapshot.rmsVolts;
            analyzer.loudnessValid = snapshot.loudnessValid;
            analyzer.momentaryLufs = snapshot.momentaryLufs;
            analyzer.shortTermLufs = snapshot.shortTermLufs;
            analyzer.integratedLufs = snapshot.integratedLufs;
            analyzer.stereo = snapshot.stereo;
            analyzer.activeChannelCount = snapshot.activeChannelCount;
            analyzer.spectrogramBinsDb.reserve(InputAnalyzerEffect::kSpectrogramBins);

            for (int i = 0; i < InputAnalyzerEffect::kSpectrogramBins; ++i)
            {
                analyzer.spectrogramBinsDb.push_back(snapshot.spectrogramBinsDb[static_cast<std::size_t>(i)]);
            }

            analyzer.spectrogramMinDbfs = InputAnalyzerEffect::kSpectrogramMinDbfs;
            analyzer.spectrogramMaxDbfs = InputAnalyzerEffect::kSpectrogramMaxDbfs;
            analyzer.spectrogramMinFrequencyHz = InputAnalyzerEffect::kSpectrogramMinFrequencyHz;
            analyzer.spectrogramMaxFrequencyHz = InputAnalyzerEffect::kSpectrogramMaxFrequencyHz;
            analyzer.barkBandsDb.reserve(InputAnalyzerEffect::kBarkBands);

            for (int i = 0; i < InputAnalyzerEffect::kBarkBands; ++i)
            {
                analyzer.barkBandsDb.push_back(snapshot.barkBandsDb[static_cast<std::size_t>(i)]);
            }

            analyzer.barkMinDbfs = InputAnalyzerEffect::kBarkMinDbfs;
            analyzer.barkMaxDbfs = InputAnalyzerEffect::kBarkMaxDbfs;
            analyzer.barkMinFrequencyHz = InputAnalyzerEffect::kBarkMinFrequencyHz;
            analyzer.barkMaxFrequencyHz = InputAnalyzerEffect::kBarkMaxFrequencyHz;
            analyzer.generatedAtMs = snapshot.generatedAtMs;

            if (snapshot.valid)
            {
                entry.analyzer = std::move(analyzer);
            }
        }

        result.push_back(std::move(entry));
    }

    return result;
}

bool SignalGraphExecutor::WatchNodeSpectrum(const std::string& nodeId)
{
    const NodeState* state = FindNodeState(nodeId);

    if (!state)
    {
        ClearSpectrumWatch();
        return false;
    }

    if (nodeId == mSpectrumWatchNodeId && state->spectrumTap.load(std::memory_order_acquire) == mSpectrumTap.get())
    {
        return true;
    }

    if (!mSpectrumTap)
    {
        mSpectrumTap = std::make_unique<SpectrumTap>();
    }

    const bool movedNode = nodeId != mSpectrumWatchNodeId;
    mSpectrumWatchNodeId = nodeId;
    ApplySpectrumWatch();

    // Only once the old node has let go, so none of its samples land after the restart. A
    // node merely rebuilt under the same id keeps its history.
    if (movedNode)
    {
        mSpectrumTap->Restart();
    }

    return true;
}

void SignalGraphExecutor::ClearSpectrumWatch()
{
    if (mSpectrumWatchNodeId.empty())
    {
        return;
    }

    mSpectrumWatchNodeId.clear();
    ApplySpectrumWatch();
}

bool SignalGraphExecutor::ReadWatchedSpectrum(SpectrumTap::Bins& out, std::chrono::steady_clock::time_point now)
{
    if (!mSpectrumTap || mSpectrumWatchNodeId.empty())
    {
        return false;
    }

    mSpectrumTap->Analyze(mSampleRate, out, now);
    return true;
}

void SignalGraphExecutor::ApplySpectrumWatch()
{
    for (auto& [id, state] : mNodeStates)
    {
        const bool watched = !mSpectrumWatchNodeId.empty() && id == mSpectrumWatchNodeId;
        state.spectrumTap.store(watched ? mSpectrumTap.get() : nullptr, std::memory_order_release);
    }
}

void SignalGraphExecutor::SetNodeEnabled(const std::string& nodeId, bool enabled)
{
    if (auto* node = mGraph.FindNode(nodeId))
    {
        node->enabled = enabled;
    }

    if (auto* state = FindNodeState(nodeId))
    {
        for (EffectProcessor* processor : {state->processor.get(), state->twin.get(), state->pendingTwin.get()})
        {
            if (processor)
            {
                processor->SetEnabled(enabled);
            }
        }
    }
}

void SignalGraphExecutor::SetNodeParam(const std::string& nodeId, const std::string& key, double value)
{
    if (auto* node = mGraph.FindNode(nodeId))
    {
        node->params[key] = value;
    }

    if (auto* state = FindNodeState(nodeId))
    {
        for (EffectProcessor* processor : {state->processor.get(), state->twin.get(), state->pendingTwin.get()})
        {
            if (processor)
            {
                processor->SetParam(key, value);
            }
        }
    }
}

// The host callback pushes the tempo before every block, so this runs at block rate on
// the audio thread. Which nodes want it is a property of the graph, resolved in
// BuildExecutionPlan(); what is left here is a compare and, on the rare block where the
// tempo actually moved, one SetParam per tempo-aware node.
void SignalGraphExecutor::SetTempo(double bpm)
{
    if (bpm == mAppliedTempoBpm)
    {
        return;
    }

    mAppliedTempoBpm = bpm;
    ApplyTempoToProcessors();
}

void SignalGraphExecutor::ApplyTempoToProcessors()
{
    for (NodeState* state : mTempoAwareStates)
    {
        state->processor->SetParam("bpm", mAppliedTempoBpm);

        if (state->twin)
        {
            state->twin->SetParam("bpm", mAppliedTempoBpm);
        }
    }
}

void SignalGraphExecutor::SetNodeConfig(const std::string& nodeId, const std::string& key, const std::string& value)
{
    if (auto* processor = RecordNodeConfig(nodeId, key, value))
    {
        processor->SetConfig(key, value);

        if (auto* state = FindNodeState(nodeId))
        {
            for (EffectProcessor* twin : {state->twin.get(), state->pendingTwin.get()})
            {
                if (twin)
                {
                    twin->SetConfig(key, value);
                }
            }
        }
    }
}

EffectProcessor* SignalGraphExecutor::RecordNodeConfig(const std::string& nodeId, const std::string& key,
                                                       const std::string& value)
{
    const bool transientCommand = key == "showPluginEditor" || key == "openPluginEditor";

    if (!transientCommand)
    {
        if (auto* node = mGraph.FindNode(nodeId))
        {
            node->config[key] = value;
        }
    }

    return GetNodeProcessor(nodeId);
}

std::string SignalGraphExecutor::GetNodeConfig(const std::string& nodeId, const std::string& key) const
{
    const auto* state = FindNodeState(nodeId);

    if (state && state->processor)
    {
        return state->processor->GetConfig(key);
    }

    if (const auto* node = mGraph.FindNode(nodeId))
    {
        const auto it = node->config.find(key);

        if (it != node->config.end())
        {
            return it->second;
        }
    }

    return {};
}

EffectProcessor* SignalGraphExecutor::GetNodeProcessor(const std::string& nodeId)
{
    auto* state = FindNodeState(nodeId);
    return state ? state->processor.get() : nullptr;
}

const EffectProcessor* SignalGraphExecutor::GetNodeProcessor(const std::string& nodeId) const
{
    const auto* state = FindNodeState(nodeId);
    return state ? state->processor.get() : nullptr;
}

void SignalGraphExecutor::SetNodeConfigForType(const std::string& type, const std::string& key,
                                               const std::string& value)
{
    for (auto& [id, state] : mNodeStates)
    {
        if (!state.processor)
        {
            continue;
        }

        if (state.type == type)
        {
            for (EffectProcessor* processor : {state.processor.get(), state.twin.get(), state.pendingTwin.get()})
            {
                if (processor)
                {
                    processor->SetConfig(key, value);
                }
            }
        }
        else if (auto* composite = dynamic_cast<CompositeEffectProcessor*>(state.processor.get()))
        {
            // A composite wraps its own graph, so nodes of `type` can live inside it.
            // CompositeEffectProcessor::SetConfig does not forward, so reach the inner
            // executor directly.
            composite->SetInnerNodeTypeConfigDefault(type, key, value);
        }
    }
}

void SignalGraphExecutor::SetNodeTypeConfigDefault(const std::string& type, const std::string& key,
                                                   const std::string& value)
{
    mNodeTypeConfigDefaults[type][key] = value;
    SetNodeConfigForType(type, key, value);
}

void SignalGraphExecutor::SeedNodeTypeConfigDefaults(
    const std::map<std::string, std::map<std::string, std::string>>& defaults)
{
    // Records for future nodes *and* applies to any that already exist. A composite
    // builds its inner graph in its constructor, so by the time the parent seeds it the
    // inner nodes are already there and would otherwise never see these values.
    for (const auto& [type, entries] : defaults)
    {
        for (const auto& [key, value] : entries)
        {
            mNodeTypeConfigDefaults[type][key] = value;
            SetNodeConfigForType(type, key, value);
        }
    }
}

bool SignalGraphExecutor::LoadNodeResource(const std::string& nodeId, const ResourceRef& ref)
{
    auto* state = FindNodeState(nodeId);

    if (!state || !state->processor)
    {
        return false;
    }

    const ResourceRef hydratedRef = HydrateResolvedResourceRef(ref, mResourceLibrary);

    if (auto path = ResolveResourcePath(hydratedRef, mResourceLibrary))
    {
        for (EffectProcessor* twin : {state->twin.get(), state->pendingTwin.get()})
        {
            if (twin)
            {
                twin->LoadResources({hydratedRef}, {*path});
            }
        }

        return state->processor->LoadResources({hydratedRef}, {*path});
    }

    return false;
}

std::unique_ptr<DeferredRebuild> SignalGraphExecutor::TakeDeferredRebuilds()
{
    auto graphWork = std::make_unique<GraphRebuild>();

    for (auto& [nodeId, state] : mNodeStates)
    {
        if (!state.processor)
        {
            continue;
        }

        for (EffectProcessor* processor : {state.processor.get(), state.twin.get()})
        {
            if (!processor)
            {
                continue;
            }

            if (auto work = processor->TakeDeferredRebuild())
            {
                graphWork->nodes.push_back({nodeId, processor, std::move(work)});
            }
        }
    }

    if (graphWork->nodes.empty())
    {
        return nullptr;
    }

    return graphWork;
}

void SignalGraphExecutor::CommitDeferredRebuilds(DeferredRebuild& work)
{
    auto* graphWork = dynamic_cast<GraphRebuild*>(&work);

    if (!graphWork)
    {
        return;
    }

    for (auto& node : graphWork->nodes)
    {
        // A graph rebuilt in between has new processors, which asked for nothing.
        const auto* state = FindNodeState(node.nodeId);

        if (state && (state->processor.get() == node.processor || state->twin.get() == node.processor))
        {
            node.processor->CommitDeferredRebuild(*node.work);
        }
    }
}

std::string SignalGraphExecutor::FindFirstNodeOfType(const std::string& type) const
{
    const auto nodeIds = FindNodesOfType(type, true);
    return nodeIds.empty() ? std::string{} : nodeIds.front();
}

std::vector<std::string> SignalGraphExecutor::FindNodesOfType(const std::string& type, bool includeDisabled) const
{
    // Resolve the requested type and each node's stored type to canonical IDs so
    // that alias/UUID mismatches (e.g. address uses "gain" while the graph node
    // stores the canonical UUID, or vice versa) still match. Without this, by-type
    // automation (bypass and params) silently no-ops when the forms differ.
    auto& registry = EffectRegistry::Instance();
    const auto resolvedType = registry.Resolve(type);

    std::vector<std::string> result;

    for (const auto& nodeId : mExecutionOrder)
    {
        const auto stateIt = mNodeStates.find(nodeId);

        if (stateIt == mNodeStates.end() || registry.Resolve(stateIt->second.type) != resolvedType)
        {
            continue;
        }

        bool enabled = true;

        if (stateIt->second.processor)
        {
            enabled = stateIt->second.processor->IsEnabled();
        }
        else if (const auto* node = mGraph.FindNode(nodeId))
        {
            enabled = node->enabled;
        }

        if (includeDisabled || enabled)
        {
            result.push_back(nodeId);
        }
    }

    return result;
}

SignalGraphExecutor::AutomationTarget SignalGraphExecutor::FindAutomationTarget(const std::string& canonicalType)
{
    for (const auto& [state, node] : mAutomationNodes)
    {
        if (state->canonicalType != canonicalType)
        {
            continue;
        }

        // FindNodesOfType's test of whether a node is enabled.
        const bool enabled = state->processor ? state->processor->IsEnabled() : (node == nullptr || node->enabled);

        if (enabled)
        {
            return {state->processor.get(), state->twin.get(), node, &state->sharedId};
        }
    }

    return {};
}

void SignalGraphExecutor::SetAutomationTargetParam(const AutomationTarget& target, const std::string& key, double value)
{
    if (target.node)
    {
        if (const auto recorded = target.node->params.find(key); recorded != target.node->params.end())
        {
            recorded->second = value;
        }
    }

    if (target.processor)
    {
        target.processor->SetParam(key, value);
    }

    if (target.twin)
    {
        target.twin->SetParam(key, value);
    }
}

bool SignalGraphExecutor::SetAutomatedNodesEnabled(const std::string& canonicalType, bool enabled)
{
    bool found = false;

    for (const auto& [state, node] : mAutomationNodes)
    {
        if (state->canonicalType != canonicalType)
        {
            continue;
        }

        if (node)
        {
            node->enabled = enabled;
        }

        if (state->processor)
        {
            state->processor->SetEnabled(enabled);
        }

        if (state->twin)
        {
            state->twin->SetEnabled(enabled);
        }

        found = true;
    }

    return found;
}

std::string SignalGraphExecutor::FindFirstNodeOfTypes(const std::vector<std::string>& types) const
{
    for (const auto& nodeId : mExecutionOrder)
    {
        auto it = mNodeStates.find(nodeId);

        if (it != mNodeStates.end())
        {
            const auto& nodeType = it->second.type;

            if (std::find(types.begin(), types.end(), nodeType) != types.end())
            {
                return nodeId;
            }
        }
    }

    return {};
}

SignalGraphExecutor::NodeState* SignalGraphExecutor::FindNodeState(const std::string& id)
{
    auto it = mNodeStates.find(id);

    if (it != mNodeStates.end())
    {
        return &it->second;
    }

    return nullptr;
}
} // namespace guitarfx
