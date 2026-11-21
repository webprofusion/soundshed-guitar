/**
 * DspDiagnostics.cpp - the DSP Performance page's figures (see DspDiagnostics.h).
 */

#include "uiclient/DspDiagnostics.h"

#include "dsp/EffectGuids.h"
#include "dsp/FiniteCheck.h"
#include "uiclient/ChainLayout.h"
#include "uiclient/DspStatus.h"
#include "uiclient/NodeLabels.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace guitarfx::uiclient
{
namespace
{
const std::string kNone = "\xE2\x80\x94";
const std::string kInputKey = "__chain_input__";
const std::string kOutputKey = "__chain_output__";

std::string Fixed(double value, int digits, const char* unit = "")
{
    if (!guitarfx::IsFinite(value))
    {
        return kNone;
    }

    char text[48];
    std::snprintf(text, sizeof(text), "%.*f%s", digits, value, unit);
    return text;
}

/// nodeId -> position in signal order (Kahn's algorithm, as dspNodeTable.ts).
std::map<std::string, int> SignalOrder(const SignalGraph& graph)
{
    std::map<std::string, int> indegree;
    std::map<std::string, std::vector<std::string>> outgoing;

    for (const auto& node : graph.nodes)
    {
        indegree[node.id] = 0;
    }

    for (const auto& edge : graph.edges)
    {
        ++indegree[edge.to];
        outgoing[edge.from].push_back(edge.to);
    }

    std::deque<std::string> queue;

    for (const auto& node : graph.nodes)
    {
        if (indegree[node.id] == 0)
        {
            queue.push_back(node.id);
        }
    }

    std::map<std::string, int> order;
    int index = 0;

    while (!queue.empty())
    {
        const auto id = queue.front();
        queue.pop_front();
        order[id] = index++;

        for (const auto& to : outgoing[id])
        {
            if (--indegree[to] == 0)
            {
                queue.push_back(to);
            }
        }
    }

    return order;
}

/// " (Split ch 2)" for a node down a splitter's branch, so two branches' like effects differ.
std::string SplitterBranchSuffix(const ClientState& state, const SignalGraph& graph, const std::string& nodeId)
{
    if (nodeId == kInputNodeId || nodeId == kOutputNodeId)
    {
        return {};
    }

    const auto findNode = [&graph](const std::string& id) -> const GraphNode* {
        const auto it =
            std::find_if(graph.nodes.begin(), graph.nodes.end(), [&id](const GraphNode& n) { return n.id == id; });
        return it != graph.nodes.end() ? &*it : nullptr;
    };

    std::string current = nodeId;
    std::vector<std::string> visited;

    while (!current.empty() && std::find(visited.begin(), visited.end(), current) == visited.end())
    {
        visited.push_back(current);
        const GraphEdge* incoming = nullptr;
        int incomingCount = 0;

        for (const auto& edge : graph.edges)
        {
            if (edge.to == current)
            {
                incoming = &edge;
                ++incomingCount;
            }
        }

        if (incomingCount != 1)
        {
            return {};
        }

        const auto* upstream = findNode(incoming->from);

        if (upstream == nullptr)
        {
            return {};
        }

        const auto outgoing = std::count_if(graph.edges.begin(), graph.edges.end(),
                                            [&upstream](const GraphEdge& e) { return e.from == upstream->id; });

        if (upstream->type == EffectGuids::kSplitter || outgoing > 1)
        {
            return " (" + NodeDisplayName(state, *upstream) + " ch " + std::to_string(incoming->fromPort + 1) + ")";
        }

        current = upstream->id;
    }

    return {};
}

const SignalGraph* GraphOf(const ClientState& state, const std::string& scope)
{
    if (scope == "pre")
    {
        return &state.globalChain.preChainGraph;
    }

    if (scope == "post")
    {
        return &state.globalChain.postChainGraph;
    }

    if (state.activePreset && scope == state.activePresetId)
    {
        return &state.activePreset->graph;
    }

    return nullptr; // another Multi-Rig preset: its graph is not held here
}

std::string GroupTitle(const ClientState& state, const std::string& scope)
{
    if (scope == "pre")
    {
        return "Global pre chain";
    }

    if (scope == "post")
    {
        return "Global post chain";
    }

    if (const auto it = state.mixer.slots.find(scope); it != state.mixer.slots.end() && !it->second.name.empty())
    {
        return it->second.name;
    }

    if (state.activePreset && scope == state.activePresetId && !state.activePreset->name.empty())
    {
        return state.activePreset->name;
    }

    if (const auto* summary = state.FindPresetSummary(scope); summary != nullptr && !summary->name.empty())
    {
        return summary->name;
    }

    return scope;
}
} // namespace

void DspDiagnostics::HoldPeak(const std::string& key, double peakDb, double now)
{
    auto it = mHolds.find(key);

    if (it == mHolds.end() || now - it->second.since > kPeakHoldSeconds)
    {
        mHolds[key] = Hold{peakDb, now};
    }
    else if (peakDb >= it->second.peakDb)
    {
        it->second.peakDb = peakDb;
    }
}

double DspDiagnostics::Held(const std::string& key, double fallback) const
{
    const auto it = mHolds.find(key);
    return it != mHolds.end() ? it->second.peakDb : fallback;
}

void DspDiagnostics::Update(const ClientState& state, double nowSeconds)
{
    const auto& t = state.telemetry;

    if (t.performanceFrameCounter != mLastPerformanceFrame)
    {
        mLastPerformanceFrame = t.performanceFrameCounter;

        if (guitarfx::IsFinite(t.dspLoadPercent))
        {
            mLoad.push_back(t.dspLoadPercent);

            while (mLoad.size() > kHistoryPoints)
            {
                mLoad.pop_front();
            }
        }
    }

    if (t.frameCounter == mLastFrame)
    {
        return;
    }

    mLastFrame = t.frameCounter;

    if (state.activePresetId != mHoldPresetId)
    {
        mHoldPresetId = state.activePresetId;
        mHolds.clear();
    }

    HoldPeak(kInputKey, t.input.peakDb, nowSeconds);
    HoldPeak(kOutputKey, t.output.peakDb, nowSeconds);

    for (const auto& [key, reading] : t.nodes)
    {
        HoldPeak(key, reading.peakDb, nowSeconds);
    }
}

bool DspDiagnostics::TakeRenderTurn(double nowSeconds)
{
    if (nowSeconds - mLastRender < kRenderIntervalSeconds)
    {
        return false;
    }

    mLastRender = nowSeconds;
    return true;
}

std::string DspDiagnostics::Load() const
{
    return mLoad.empty() ? Fixed(0.0, 1, "%") : Fixed(mLoad.back(), 1, "%");
}

std::string DspDiagnostics::PeakLoad() const
{
    return Fixed(mLoad.empty() ? 0.0 : *std::max_element(mLoad.begin(), mLoad.end()), 1, "%");
}

std::string DspDiagnostics::Latency(const ClientState& state)
{
    return FormatDspStatusLatency(static_cast<double>(state.telemetry.totalLatencySamples), state.telemetry.sampleRate,
                                  true);
}

DspLevelSummary DspDiagnostics::Input(const ClientState& state) const
{
    const auto& r = state.telemetry.input;
    return {Fixed(Held(kInputKey, r.peakDb), 1, " dBFS"), Fixed(r.rmsDb, 1, " dBFS"),
            Fixed(std::max(0.0, -r.peakDb), 1, " dB"), r.clipped, r.clipCount};
}

DspLevelSummary DspDiagnostics::Output(const ClientState& state) const
{
    const auto& r = state.telemetry.output;
    return {Fixed(Held(kOutputKey, r.peakDb), 1, " dBFS"), Fixed(r.rmsDb, 1, " dBFS"),
            Fixed(std::max(0.0, -r.peakDb), 1, " dB"), r.clipped, r.clipCount};
}

std::vector<DspDiagnosticsGroup> DspDiagnostics::Table(const ClientState& state) const
{
    const auto& t = state.telemetry;
    std::vector<DspDiagnosticsGroup> groups;

    // The chain's input first, on its own.
    DspDiagnosticsRow input;
    input.label = "Chain input";
    input.channels = input.time = input.share = input.latency = kNone;
    input.peak = Fixed(Held(kInputKey, t.input.peakDb), 1);
    input.clipped = t.input.clipped;
    input.clipCount = t.input.clipCount;
    groups.push_back({"", {input}});

    // The chains in the roster, pre first, then the active preset, the others, and post.
    std::vector<std::string> scopes;

    for (const auto& node : t.roster)
    {
        if (std::find(scopes.begin(), scopes.end(), node.scope) == scopes.end())
        {
            scopes.push_back(node.scope);
        }
    }

    const auto rank = [&state](const std::string& scope) {
        return scope == "pre" ? 0 : scope == state.activePresetId ? 1 : scope == "post" ? 3 : 2;
    };
    std::stable_sort(scopes.begin(), scopes.end(),
                     [&rank](const std::string& a, const std::string& b) { return rank(a) < rank(b); });

    for (const auto& scope : scopes)
    {
        const auto* graph = GraphOf(state, scope);
        const auto order = graph != nullptr ? SignalOrder(*graph) : std::map<std::string, int>{};
        std::vector<const TelemetryNode*> nodes;

        for (const auto& node : t.roster)
        {
            if (node.scope == scope && t.nodes.count(node.key) != 0)
            {
                nodes.push_back(&node);
            }
        }

        if (nodes.empty())
        {
            continue;
        }

        const auto position = [&order](const TelemetryNode* n) {
            const auto it = order.find(n->nodeId);
            return it != order.end() ? it->second : 9999;
        };
        std::stable_sort(nodes.begin(), nodes.end(), [&position](const TelemetryNode* a, const TelemetryNode* b) {
            return position(a) < position(b);
        });

        DspDiagnosticsGroup group{GroupTitle(state, scope), {}};

        for (const auto* node : nodes)
        {
            const auto& reading = t.nodes.at(node->key);
            DspDiagnosticsRow row;

            const GraphNode* graphNode = nullptr;

            if (graph != nullptr)
            {
                const auto it = std::find_if(graph->nodes.begin(), graph->nodes.end(),
                                             [node](const GraphNode& g) { return g.id == node->nodeId; });
                graphNode = it != graph->nodes.end() ? &*it : nullptr;
            }

            if (graphNode != nullptr)
            {
                row.label = NodeDisplayName(state, *graphNode) + SplitterBranchSuffix(state, *graph, node->nodeId);
            }
            else if (const auto* type = state.FindEffectType(node->nodeType); type != nullptr && !type->name.empty())
            {
                row.label = type->name;
            }
            else
            {
                row.label = node->nodeId == kInputNodeId    ? "Input"
                            : node->nodeId == kOutputNodeId ? "Output"
                                                            : node->nodeId;
            }

            row.stereo = reading.channelCount > 1;
            row.channels = reading.channelCount > 0 ? std::to_string(reading.channelCount) : kNone;

            const auto time = t.nodeProcessingUs.find(node->key);
            const bool timed = time != t.nodeProcessingUs.end() && guitarfx::IsFinite(time->second);
            row.time = timed ? Fixed(time->second, 1) : kNone;
            row.share =
                timed && t.totalProcessingUs > 0.0 ? Fixed(time->second / t.totalProcessingUs * 100.0, 1) : kNone;

            const auto latency = t.nodeLatencySamples.find(node->key);
            const bool delays =
                latency != t.nodeLatencySamples.end() && guitarfx::IsFinite(latency->second) && latency->second > 0.0;
            row.latency = !delays              ? kNone
                          : t.sampleRate > 0.0 ? Fixed(latency->second / t.sampleRate * 1000.0, 2, " ms")
                                               : std::to_string(std::lround(latency->second)) + " smp";

            row.peak = Fixed(Held(node->key, reading.peakDb), 1);
            row.clipped = reading.clipped;
            row.clipCount = reading.clipCount;
            group.rows.push_back(std::move(row));
        }

        groups.push_back(std::move(group));
    }

    return groups;
}

void DspDiagnostics::Reset()
{
    *this = DspDiagnostics();
}
} // namespace guitarfx::uiclient
