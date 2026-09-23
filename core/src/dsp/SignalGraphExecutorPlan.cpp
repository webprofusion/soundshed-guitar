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
    mTempoAwareProcessors.clear();
    mInputTrimNode = mGraph.FindNode("__input__");
    mOutputTrimNode = mGraph.FindNode("__output__");

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
        planned.mayProduceStereo = NodeMayProduceStereo(state.type, state.category);
        planned.isNam = IsNamNodeType(state.type);

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
                mTempoAwareProcessors.push_back(state.processor.get());
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
}

void SignalGraphExecutor::ProcessPlannedNode(PlannedNode& planned, int numSamples, bool diagnosticsEnabled,
                                             bool collectLevels)
{
    thread_local std::vector<float> tempLeft;
    thread_local std::vector<float> tempRight;

    if (static_cast<int>(tempLeft.size()) < numSamples)
    {
        tempLeft.resize(static_cast<size_t>(numSamples), 0.0f);
        tempRight.resize(static_cast<size_t>(numSamples), 0.0f);
    }

    NodeState* state = planned.state;

    if (planned.isInput)
    {
        return;
    }

    const bool isMixer = planned.isMixer;
    const bool shouldAccumulate = planned.accumulateInputs;
    bool incomingStereoSignal = false;
    bool mixerHasNonCenterPan = false;

    MixerEffect* mixerEffect = planned.mixer;

    {
        for (const PlannedEdge& edge : planned.incoming)
        {
            NodeState* sourceState = edge.source;

            if (sourceState && sourceState->hasInput)
            {
                const float edgeGain = edge.gain;
                const int inputPort = edge.toPort;
                incomingStereoSignal = incomingStereoSignal || sourceState->hasStereoSignal;

                if (isMixer && mixerEffect)
                {
                    if (mixerEffect->IsInputMuted(inputPort))
                    {
                        state->hasInput = true;
                        continue;
                    }

                    const float panL = mixerEffect->GetInputPanL(inputPort);
                    const float panR = mixerEffect->GetInputPanR(inputPort);
                    const float level = mixerEffect->GetInputLevel(inputPort);
                    const float gainL = edgeGain * level * panL;
                    const float gainR = edgeGain * level * panR;

                    // Track whether any active input is panned off-centre; if so the
                    // mixer output is genuinely stereo even when the input is mono.
                    if (std::abs(gainL - gainR) > 1.0e-5f)
                    {
                        mixerHasNonCenterPan = true;
                    }

                    // Use ProcessInput so per-input delay is applied alongside level/pan.
                    mixerEffect->ProcessInput(inputPort, sourceState->bufferLeft.data(),
                                              sourceState->bufferRight.data(), state->bufferLeft.data(),
                                              state->bufferRight.data(), numSamples, edgeGain);
                    state->hasInput = true;
                }
                else if (shouldAccumulate)
                {
                    for (int i = 0; i < numSamples; ++i)
                    {
                        state->bufferLeft[static_cast<size_t>(i)] +=
                            sourceState->bufferLeft[static_cast<size_t>(i)] * edgeGain;
                        state->bufferRight[static_cast<size_t>(i)] +=
                            sourceState->bufferRight[static_cast<size_t>(i)] * edgeGain;
                    }
                }
                else
                {
                    for (int i = 0; i < numSamples; ++i)
                    {
                        state->bufferLeft[static_cast<size_t>(i)] =
                            sourceState->bufferLeft[static_cast<size_t>(i)] * edgeGain;
                        state->bufferRight[static_cast<size_t>(i)] =
                            sourceState->bufferRight[static_cast<size_t>(i)] * edgeGain;
                    }
                }

                state->hasInput = true;
            }
        }
    }

    if (state->hasInput)
    {
        state->hasStereoSignal = incomingStereoSignal;

        // An EQ display shows what arrives at the node, so the tap takes the input before the
        // node processes it -- enabled or not, since a bypassed EQ still has a source to show.
        if (auto* tap = state->spectrumTap.load(std::memory_order_acquire))
        {
            tap->Push(state->bufferLeft.data(), incomingStereoSignal ? state->bufferRight.data() : nullptr, numSamples);
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
            }

            state->notesThisBlock = true;
        }

        if (planned.acceptsNotes)
        {
            HandNotesTo(planned);
        }
    };

    if (state->processor && state->hasInput)
    {
        const bool forceNamMonoByInputMode = mNamInputModeMono && planned.isNam;
        const bool nodeCanMono = state->processor->SupportsMonoProcessing() && !planned.mayProduceStereo &&
                                 !state->processor->ProducesStereoOutput() &&
                                 (!incomingStereoSignal || forceNamMonoByInputMode);

        if (planned.isSplitter || planned.isOutput)
        {
            if (planned.isOutput && !state->processor->IsEnabled())
            {
                std::fill(state->bufferLeft.begin(), state->bufferLeft.begin() + numSamples, 0.0f);
                std::fill(state->bufferRight.begin(), state->bufferRight.begin() + numSamples, 0.0f);
                state->hasStereoSignal = false;
            }
        }
        else if (planned.isMixer)
        {
            if (state->processor->IsEnabled())
            {
                float* inPtrs[2] = {state->bufferLeft.data(), state->bufferRight.data()};
                float* outPtrs[2] = {tempLeft.data(), tempRight.data()};
                processTimed([&]() { state->processor->Process(inPtrs, outPtrs, numSamples); });
                std::copy(tempLeft.begin(), tempLeft.begin() + numSamples, state->bufferLeft.begin());
                std::copy(tempRight.begin(), tempRight.begin() + numSamples, state->bufferRight.begin());

                if (!incomingStereoSignal && !mixerHasNonCenterPan && !planned.mayProduceStereo)
                {
                    std::copy(state->bufferLeft.begin(), state->bufferLeft.begin() + numSamples,
                              state->bufferRight.begin());
                    state->hasStereoSignal = false;
                }
                else
                {
                    state->hasStereoSignal = incomingStereoSignal || mixerHasNonCenterPan || planned.mayProduceStereo;
                }
            }
        }
        else if (state->processor->IsEnabled() && nodeCanMono)
        {
            routeNotes();
            processTimed(
                [&]() { state->processor->ProcessMono(state->bufferLeft.data(), tempLeft.data(), numSamples); });
            std::copy(tempLeft.begin(), tempLeft.begin() + numSamples, state->bufferLeft.begin());
            std::copy(state->bufferLeft.begin(), state->bufferLeft.begin() + numSamples, state->bufferRight.begin());
            state->hasStereoSignal = false;
        }
        else if (state->processor->IsEnabled())
        {
            float* inPtrs[2] = {state->bufferLeft.data(), state->bufferRight.data()};
            float* outPtrs[2] = {tempLeft.data(), tempRight.data()};
            routeNotes();
            processTimed([&]() { state->processor->Process(inPtrs, outPtrs, numSamples); });
            std::copy(tempLeft.begin(), tempLeft.begin() + numSamples, state->bufferLeft.begin());
            std::copy(tempRight.begin(), tempRight.begin() + numSamples, state->bufferRight.begin());
            state->hasStereoSignal =
                incomingStereoSignal || planned.mayProduceStereo || state->processor->ProducesStereoOutput();
        }
        else
        {
            state->hasStereoSignal = incomingStereoSignal;
        }
    }

    if (collectLevels && state->hasInput)
    {
        const auto levelStats = ComputeLevelStats(state->bufferLeft.data(), state->bufferRight.data(), numSamples);
        state->peak.store(levelStats.peak, std::memory_order_relaxed);
        state->rms.store(levelStats.rms, std::memory_order_relaxed);
        state->clipCount.store(levelStats.clipCount, std::memory_order_relaxed);
    }
}
} // namespace guitarfx
