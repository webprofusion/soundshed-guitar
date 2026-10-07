/**
 * @file SignalGraphExecutorPlan.cpp
 * @brief Building the resolved execution plan, and running one planned node.
 *
 * Part of SignalGraphExecutor -- see SignalGraphExecutor.h for the class and
 * SignalGraphExecutorInternal.h for the helpers this shares with the main TU.
 * Split out because the per-block hot path and the plan that feeds it are the two
 * halves of the file that get read together, and the file was well past the
 * repository's size budget.
 */

#include "dsp/SignalGraphExecutor.h"
#include "dsp/SignalGraphExecutorInternal.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/MixerEffect.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <vector>

namespace guitarfx
{
using namespace guitarfx::executor_detail;

float SignalGraphExecutor::DbToLinear::Get(double db)
{
    if (db != mDb)
    {
        mDb = db;
        mLinear = static_cast<float>(std::pow(10.0, db / 20.0));
    }

    return mLinear;
}

void SignalGraphExecutor::BuildExecutionPlan()
{
    mPlan.clear();
    mExecutionLevelPlan.clear();
    mInputPlanNode = nullptr;
    mOutputPlanNodes.clear();
    mTempoAwareStates.clear();

    const auto boundaryGainDb = [this](const char* nodeId) -> const double* {
        const GraphNode* node = mGraph.FindNode(nodeId);

        if (!node)
        {
            return nullptr;
        }

        const auto it = node->params.find(kBoundaryGainParam);
        return it != node->params.end() ? &it->second : nullptr;
    };

    mInputNodeGainDb = boundaryGainDb("__input__");
    mOutputNodeGainDb = boundaryGainDb("__output__");

    // mPlan must not reallocate once the level lists and the input/output pointers refer
    // into it, so it is sized up front.
    mPlan.reserve(mNodeStates.size());

    // Index of each node's entry in mPlan, so edges can be resolved without a second
    // pass of string lookups.
    std::map<std::string, int> planIndexById;

    auto& registry = EffectRegistry::Instance();

    for (auto& [id, state] : mNodeStates)
    {
        state.canonicalType = registry.Resolve(state.type);

        const auto planIndex = static_cast<int>(mPlan.size());
        planIndexById[id] = planIndex;

        PlannedNode planned;
        planned.state = &state;
        planned.isInput = (state.type == kNodeTypeInput);
        planned.isOutput = (state.type == kNodeTypeOutput) || (id == "__output__");
        planned.isSplitter = (state.type == kNodeTypeSplitter);
        planned.isMixer = (state.type == kNodeTypeMixer);

        if (planned.isMixer && state.processor)
        {
            planned.mixer = dynamic_cast<MixerEffect*>(state.processor.get());
        }

        if (state.processor)
        {
            planned.noteOutput = state.processor->GetNoteOutput();
            planned.acceptsNotes = state.processor->AcceptsNoteInput();
        }

        // Resolved here rather than in SetTempo(), which the host callback drives once
        // per block: GetTypeInfo() returns EffectTypeInfo by value, and that is a deep
        // copy of every parameter and preset definition the type declares.
        if (state.processor)
        {
            const auto typeInfo = registry.GetTypeInfo(state.canonicalType);

            if (typeInfo && typeInfo->requiresTempo)
            {
                mTempoAwareStates.push_back(&state);
            }
        }

        mPlan.push_back(std::move(planned));
    }

    // Resolve incoming edges to source states. Same ordering as the old
    // mIncomingEdgesByNode walk, so accumulation order across edges is unchanged.
    for (auto& [id, edgeIndices] : mIncomingEdgesByNode)
    {
        const auto planIt = planIndexById.find(id);

        if (planIt == planIndexById.end())
        {
            continue;
        }

        auto& planned = mPlan[static_cast<std::size_t>(planIt->second)];
        planned.incoming.reserve(edgeIndices.size());

        for (const std::size_t edgeIndex : edgeIndices)
        {
            if (edgeIndex >= mGraph.edges.size())
            {
                continue;
            }

            const auto& edge = mGraph.edges[edgeIndex];
            PlannedEdge plannedEdge;
            plannedEdge.source = FindNodeState(edge.from);
            plannedEdge.gain = static_cast<float>(edge.gain);
            plannedEdge.toPort = edge.toPort;
            planned.incoming.push_back(plannedEdge);
        }

        planned.accumulateInputs = planned.isMixer || planned.incoming.size() > 1;
    }

    ResolveNoteRouting();

    // The input node Process() copies into is the first by id order that is input-typed
    // or literally named "__input__" — matching what the old scan settled on.
    for (auto& [id, state] : mNodeStates)
    {
        if (state.type == kNodeTypeInput || id == "__input__")
        {
            mInputPlanNode = &mPlan[static_cast<std::size_t>(planIndexById[id])];
            break;
        }
    }

    for (auto& [id, state] : mNodeStates)
    {
        if (state.type == kNodeTypeOutput || id == "__output__")
        {
            mOutputPlanNodes.push_back(&mPlan[static_cast<std::size_t>(planIndexById[id])]);
        }
    }

    mExecutionLevelPlan.reserve(mExecutionLevels.size());

    for (const auto& level : mExecutionLevels)
    {
        std::vector<int> levelPlan;
        levelPlan.reserve(level.size());

        for (const auto& nodeId : level)
        {
            if (const auto it = planIndexById.find(nodeId); it != planIndexById.end())
            {
                levelPlan.push_back(it->second);
            }
        }

        mExecutionLevelPlan.push_back(std::move(levelPlan));
    }

    mAutomationNodes.clear();
    mAutomationNodes.reserve(mExecutionOrder.size());

    for (const auto& nodeId : mExecutionOrder)
    {
        if (auto* state = FindNodeState(nodeId))
        {
            mAutomationNodes.push_back({state, mGraph.FindNode(nodeId)});
        }
    }

    ResolveChannelLayout(true);

    // A node added to a running graph gets a freshly constructed processor that has never
    // seen a tempo, and SetTempo() only pushes on a change — so seed the new set here.
    // Zero means nothing has pushed a tempo yet, and there is nothing to seed with.
    if (mAppliedTempoBpm > 0.0)
    {
        ApplyTempoToProcessors();
    }

    // Likewise a rebuilt node starts out untapped, which would silently end an EQ display.
    ApplySpectrumWatch();
}

void SignalGraphExecutor::SetInputLayout(ChannelLayout layout)
{
    if (layout == mInputLayout)
    {
        return;
    }

    mInputLayout = layout;

    // Before SetGraph() there is no plan yet, and SetGraph() resolves with this layout.
    if (!mPlan.empty())
    {
        ResolveChannelLayout(false);
    }
}

void SignalGraphExecutor::ResolveChannelLayout(bool notifyAll)
{
    const bool stereoInput = (mInputLayout == ChannelLayout::Stereo);

    // Execution order, so every source is resolved before the nodes it feeds.
    for (const auto& level : mExecutionLevelPlan)
    {
        for (const int planIndex : level)
        {
            PlannedNode& planned = mPlan[static_cast<std::size_t>(planIndex)];
            NodeState& state = *planned.state;
            EffectProcessor* processor = state.processor.get();

            bool inputStereo = false;

            if (&planned == mInputPlanNode)
            {
                inputStereo = stereoInput;
            }
            else
            {
                for (const PlannedEdge& edge : planned.incoming)
                {
                    inputStereo = inputStereo || (edge.source && edge.source->outputStereo);
                }
            }

            // Input, output and splitter nodes run a passthrough, which cannot widen; a null
            // processor passes its input on as well. A node set to a mono channel mode folds a
            // stereo input to one channel and puts out mono whatever it is.
            const bool isBoundary = planned.isInput || planned.isOutput || planned.isSplitter;
            // Dual mono lets nothing cross between the sides, so a mono fold, which would, is off.
            const bool monoMode = !mDualMono && !isBoundary && state.monoFold != NodeState::MonoFold::None;
            const bool canWiden = processor && processor->CanWiden();
            const bool outputStereo = !monoMode && (inputStereo || canWiden);
            const bool processesMono = !inputStereo || monoMode;
            const bool inputChanged = (inputStereo != state.inputStereo);

            state.inputStereo = inputStereo;
            state.outputStereo = outputStereo;
            // A mixer pans its inputs as it gathers them, so its gathered input is folded too.
            planned.foldInput = monoMode && (inputStereo || planned.isMixer);
            planned.foldOutput = monoMode && canWiden;
            planned.runMono = processesMono && !canWiden && processor && !isBoundary && !planned.isMixer &&
                              processor->SupportsMonoProcessing();
            planned.twin = mDualMono ? state.twin.get() : nullptr;

            if (processor && (notifyAll || inputChanged))
            {
                processor->SetInputLayout(processesMono ? ChannelLayout::Mono : ChannelLayout::Stereo);
            }
        }
    }

    // Process() takes the first output node that ran; they all see the same graph, so the first
    // one stands for what reaches the output.
    mOutputStereo = !mOutputPlanNodes.empty() && mOutputPlanNodes.front()->state->inputStereo;
}

bool SignalGraphExecutor::AnyNodeCanWiden() const
{
    for (const auto& [id, state] : mNodeStates)
    {
        if (state.processor && state.processor->CanWiden())
        {
            return true;
        }
    }

    return false;
}

void SignalGraphExecutor::ResolveNoteRouting()
{
    std::map<const NodeState*, std::size_t> planIndexByState;

    for (std::size_t index = 0; index < mPlan.size(); ++index)
    {
        planIndexByState[mPlan[index].state] = index;
    }

    for (auto& player : mPlan)
    {
        if (!player.acceptsNotes)
        {
            continue;
        }

        // Everything upstream, walking the incoming edges back to the input.
        std::vector<bool> seen(mPlan.size(), false);
        std::vector<std::size_t> pending;
        std::vector<std::size_t> sources;

        const auto visitIncoming = [&](const PlannedNode& node) {
            for (const PlannedEdge& edge : node.incoming)
            {
                const auto it = planIndexByState.find(edge.source);

                if (it != planIndexByState.end() && !seen[it->second])
                {
                    seen[it->second] = true;
                    pending.push_back(it->second);
                }
            }
        };

        visitIncoming(player);

        while (!pending.empty())
        {
            const std::size_t index = pending.back();
            pending.pop_back();

            if (mPlan[index].noteOutput)
            {
                sources.push_back(index);
            }

            visitIncoming(mPlan[index]);
        }

        std::sort(sources.begin(), sources.end());

        for (const std::size_t index : sources)
        {
            player.noteSources.push_back(&mPlan[index]);
        }

        player.liveNotes.reserve(player.noteSources.size());
    }
}

void SignalGraphExecutor::HandNotesTo(PlannedNode& planned)
{
    planned.liveNotes.clear();

    for (const PlannedNode* source : planned.noteSources)
    {
        if (source->state->notesThisBlock)
        {
            planned.liveNotes.push_back(source->noteOutput);
        }
    }

    planned.state->processor->SetNoteInput(planned.liveNotes);

    if (planned.twin)
    {
        planned.twin->SetNoteInput(planned.liveNotes);
    }
}

void SignalGraphExecutor::ProcessPlannedNode(PlannedNode& planned, int numSamples, bool diagnosticsEnabled,
                                             bool collectLevels)
{
    NodeState* state = planned.state;

    if (planned.isInput)
    {
        return;
    }

    // Gather the node's input from its sources: a plain copy for one edge, a sum for several,
    // and for a mixer each input's own level, pan and delay.
    const bool isMixer = planned.isMixer;
    const bool shouldAccumulate = planned.accumulateInputs;

    MixerEffect* mixerEffect = planned.mixer;

    for (const PlannedEdge& edge : planned.incoming)
    {
        NodeState* sourceState = edge.source;

        if (!sourceState || !sourceState->hasInput)
        {
            continue;
        }

        const float edgeGain = edge.gain;
        const int inputPort = edge.toPort;
        state->hasInput = true;

        if (isMixer && mixerEffect)
        {
            if (mixerEffect->IsInputMuted(inputPort))
            {
                continue;
            }

            // ProcessInput applies the input's delay along with its level and pan.
            mixerEffect->ProcessInput(inputPort, sourceState->bufferLeft.data(), sourceState->bufferRight.data(),
                                      state->bufferLeft.data(), state->bufferRight.data(), numSamples, edgeGain);
        }
        else if (shouldAccumulate)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                state->bufferLeft[static_cast<size_t>(i)] += sourceState->bufferLeft[static_cast<size_t>(i)] * edgeGain;
                state->bufferRight[static_cast<size_t>(i)] +=
                    sourceState->bufferRight[static_cast<size_t>(i)] * edgeGain;
            }
        }
        else
        {
            for (int i = 0; i < numSamples; ++i)
            {
                state->bufferLeft[static_cast<size_t>(i)] = sourceState->bufferLeft[static_cast<size_t>(i)] * edgeGain;
                state->bufferRight[static_cast<size_t>(i)] =
                    sourceState->bufferRight[static_cast<size_t>(i)] * edgeGain;
            }
        }
    }

    if (!state->hasInput)
    {
        return;
    }

    // An EQ display shows what arrives at the node, so the tap takes the input before the
    // node processes it -- enabled or not, since a bypassed EQ still has a source to show.
    if (auto* tap = state->spectrumTap.load(std::memory_order_acquire))
    {
        tap->Push(state->bufferLeft.data(), state->inputStereo ? state->bufferRight.data() : nullptr, numSamples);
    }

    // A mono channel mode folds a stereo input before the node runs, bypassed or not: what it
    // puts out is mono either way.
    if (planned.foldInput)
    {
        float* left = state->bufferLeft.data();
        float* right = state->bufferRight.data();

        switch (state->monoFold)
        {
        case NodeState::MonoFold::Left:
            std::copy_n(left, numSamples, right);
            break;
        case NodeState::MonoFold::Right:
            std::copy_n(right, numSamples, left);
            break;
        default:

            for (int i = 0; i < numSamples; ++i)
            {
                const float folded = 0.5f * (left[i] + right[i]);
                left[i] = folded;
                right[i] = folded;
            }

            break;
        }
    }

    // Time the node only when diagnostics are on, and publish into the node's own atomic
    // rather than a string-keyed map. The maps used to be filled here, on the audio thread,
    // from a stats object rebuilt every block -- so every record was a std::map insert:
    // a heap allocation plus a string copy, per node, per graph, per block. They are now
    // assembled in GetPerformanceStats() on the message thread, which is where node latency
    // has always been collected. The relaxed store needs no lock, so a node in a parallel
    // level no longer contends with its siblings either.
    const auto processTimed = [&](auto&& invoke) {
        if (!diagnosticsEnabled)
        {
            invoke();
            return;
        }

        const auto nodeStart = std::chrono::high_resolution_clock::now();
        invoke();
        const auto nodeEnd = std::chrono::high_resolution_clock::now();
        const std::chrono::duration<double, std::micro> nodeDuration(nodeEnd - nodeStart);
        state->processingTimeUs.store(nodeDuration.count(), std::memory_order_relaxed);
    };

    // Note routing, for a node about to run. A source that did not run last block starts afresh,
    // since what it was tracking then is long gone; a player hears the sources that ran this block.
    const auto routeNotes = [&]() {
        if (planned.noteOutput)
        {
            if (!state->notesLastBlock)
            {
                state->processor->Reset();

                if (planned.twin)
                {
                    planned.twin->Reset();
                }
            }

            state->notesThisBlock = true;
        }

        if (planned.acceptsNotes)
        {
            HandNotesTo(planned);
        }
    };

    // The node runs from its buffers into its scratch pair, which then becomes its buffers: no
    // effect is asked to process in place, and nothing is copied back.
    EffectProcessor* processor = state->processor.get();

    const auto runStereo = [&]() {
        float* inPtrs[2] = {state->bufferLeft.data(), state->bufferRight.data()};
        float* outPtrs[2] = {state->scratchLeft.data(), state->scratchRight.data()};
        processTimed([&]() { processor->Process(inPtrs, outPtrs, numSamples); });
        state->bufferLeft.swap(state->scratchLeft);
        state->bufferRight.swap(state->scratchRight);
    };

    const auto copyLeftToRight = [&]() {
        std::copy_n(state->bufferLeft.data(), numSamples, state->bufferRight.data());
    };

    if (processor)
    {
        const bool enabled = processor->IsEnabled();

        if (planned.isOutput)
        {
            if (!enabled)
            {
                std::fill_n(state->bufferLeft.data(), numSamples, 0.0f);
                std::fill_n(state->bufferRight.data(), numSamples, 0.0f);
            }
        }
        else if (planned.isSplitter)
        {
            // Fans out what it was given; there is nothing to run or to bypass.
        }
        else if (planned.isMixer)
        {
            // The inputs' level, pan and delay were applied as they were gathered, bypassed or
            // not; the master level is all Process() adds.
            if (enabled)
            {
                runStereo();
            }
        }
        else if (enabled)
        {
            // Mono or stereo was settled when the layout was resolved; nothing about this block
            // changes it.
            routeNotes();

            if (planned.twin)
            {
                // Dual mono: each side through its own instance, each on its own input. The
                // primary keeps the left of what it makes, the second instance the right.
                float* left = state->bufferLeft.data();
                float* right = state->bufferRight.data();
                float* leftIn[2] = {left, left};
                float* leftOut[2] = {state->scratchLeft.data(), state->scratchRight.data()};
                float* rightIn[2] = {right, right};
                float* rightOut[2] = {state->twinScratchLeft.data(), state->twinScratchRight.data()};
                processTimed([&]() {
                    processor->Process(leftIn, leftOut, numSamples);
                    planned.twin->Process(rightIn, rightOut, numSamples);
                });
                state->bufferLeft.swap(state->scratchLeft);
                state->bufferRight.swap(state->twinScratchRight);
            }
            else if (planned.runMono)
            {
                processTimed(
                    [&]() { processor->ProcessMono(state->bufferLeft.data(), state->scratchLeft.data(), numSamples); });
                state->bufferLeft.swap(state->scratchLeft);
                copyLeftToRight();
            }
            else
            {
                runStereo();

                if (planned.foldOutput)
                {
                    // A widening type set to mono: sum its two sides, as a mono speaker would.
                    for (int i = 0; i < numSamples; ++i)
                    {
                        const float folded = 0.5f * (state->bufferLeft[static_cast<size_t>(i)] +
                                                     state->bufferRight[static_cast<size_t>(i)]);
                        state->bufferLeft[static_cast<size_t>(i)] = folded;
                        state->bufferRight[static_cast<size_t>(i)] = folded;
                    }
                }
                else if (!state->outputStereo)
                {
                    // A mono connection holds the same samples on both sides, exactly. An effect
                    // that cannot widen keeps them within rounding, and a pitch shifter's two
                    // channels can round differently, so the left is copied over the right rather
                    // than trusted to match.
                    copyLeftToRight();
                }
            }
        }

        // A bypassed effect passes its gathered input on as it arrived.
    }

    state->channelCount.store(state->outputStereo ? 2 : 1, std::memory_order_relaxed);

    if (collectLevels)
    {
        const auto levelStats = ComputeLevelStats(state->bufferLeft.data(), state->bufferRight.data(), numSamples);
        state->peak.store(levelStats.peak, std::memory_order_relaxed);
        state->rms.store(levelStats.rms, std::memory_order_relaxed);
        state->clipCount.store(levelStats.clipCount, std::memory_order_relaxed);
    }
}
} // namespace guitarfx
