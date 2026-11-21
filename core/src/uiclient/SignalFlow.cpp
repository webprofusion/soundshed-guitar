/**
 * SignalFlow.cpp - which nodes are passing signal (see SignalFlow.h).
 */

#include "uiclient/SignalFlow.h"

#include <functional>

namespace guitarfx::uiclient
{
bool SignalFlow::Update(const ClientState& state, double nowSeconds)
{
    std::set<std::string> live;

    if (state.activePreset)
    {
        const auto& graph = state.activePreset->graph;
        const auto& telemetry = state.telemetry;
        const auto hears = [](const LevelReading& reading) { return reading.rmsDb > kLiveThresholdDb; };

        // What each node reports; the boundaries read the chain's input and output.
        std::map<std::string, bool> reported;
        const std::string scope = state.activePresetId + "::";

        for (const auto& node : graph.nodes)
        {
            if (node.type == kNodeTypeInput)
            {
                reported[node.id] = hears(telemetry.input);
            }
            else if (node.type == kNodeTypeOutput)
            {
                reported[node.id] = hears(telemetry.output);
            }
            else if (const auto it = telemetry.nodes.find(scope + node.id); it != telemetry.nodes.end())
            {
                reported[node.id] = hears(it->second);
            }
        }

        // A node with no reading of its own hears what any node feeding it hears.
        std::map<std::string, bool> resolved;
        std::set<std::string> resolving;
        std::function<bool(const std::string&)> hearsAt = [&](const std::string& id) {
            if (const auto it = reported.find(id); it != reported.end())
            {
                return it->second;
            }

            if (const auto it = resolved.find(id); it != resolved.end())
            {
                return it->second;
            }

            if (!resolving.insert(id).second)
            {
                return false;
            }

            bool hearing = false;

            for (const auto& edge : graph.edges)
            {
                if (edge.to == id && hearsAt(edge.from))
                {
                    hearing = true;
                    break;
                }
            }

            resolving.erase(id);
            resolved[id] = hearing;
            return hearing;
        };

        for (const auto& node : graph.nodes)
        {
            if (hearsAt(node.id))
            {
                mLastHeard[node.id] = nowSeconds;
            }

            if (const auto it = mLastHeard.find(node.id);
                it != mLastHeard.end() && nowSeconds - it->second < kReleaseSeconds)
            {
                live.insert(node.id);
            }
        }
    }

    const bool changed = live != mLive;
    mLive = std::move(live);
    return changed;
}
} // namespace guitarfx::uiclient
