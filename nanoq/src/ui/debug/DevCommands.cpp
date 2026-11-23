#include "ui/debug/DevCommands.h"

#include "Log.h"
#include "uiclient/ClientState.h"
#include "uiclient/UiCommands.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <vector>

namespace nanoq::ui
{
namespace
{
namespace uic = guitarfx::uiclient;

/// `i` modulo `n`, never negative; n must be > 0.
std::size_t Wrap(long long i, std::size_t n)
{
    const auto m = static_cast<long long>(n);
    return static_cast<std::size_t>(((i % m) + m) % m);
}

/// The effect types a script may add: not a hosted plugin or WASM host, whose add asks for a
/// file in a native dialog that would stop the run.
std::vector<const uic::EffectTypeInfo*> AddableTypes(const uic::ClientState& state)
{
    std::vector<const uic::EffectTypeInfo*> types;

    for (const auto& info : state.catalog)
    {
        if (info.type.find("plugin") == std::string::npos && info.type.find("host") == std::string::npos)
        {
            types.push_back(&info);
        }
    }

    return types;
}
} // namespace

bool RunDevCommand(const uic::ClientState& state, uic::UiCommands& commands, const std::string& line)
{
    std::istringstream words(line);
    std::string verb;
    words >> verb;

    if (verb == "setting")
    {
        std::string key, json;
        words >> key;
        std::getline(words >> std::ws, json);
        const auto value = nlohmann::json::parse(json, nullptr, false);

        if (key.empty() || value.is_discarded())
        {
            return false;
        }

        commands.SetSetting(key, value);
        return true;
    }

    if (verb == "theme")
    {
        // As the theme flyout picks one (the state's theme and the setting): cmd theme studio-1973
        std::string id;
        words >> id;

        if (id.empty())
        {
            return false;
        }

        commands.SetTheme(id);
        return true;
    }

    if (verb == "device")
    {
        // An audioDevice request, as Settings sends it: cmd device setDeviceType {"deviceType":"ASIO"}
        std::string action, json;
        words >> action;
        std::getline(words >> std::ws, json);
        auto payload = json.empty() ? nlohmann::json::object() : nlohmann::json::parse(json, nullptr, false);

        if (action.empty() || !payload.is_object())
        {
            return false;
        }

        Log("[drive] audioDevice " + action + " " + payload.dump());
        commands.AudioDevice(action, std::move(payload));
        return true;
    }

    if (verb == "preset")
    {
        int delta = 1;
        words >> delta;
        commands.StepPreset(delta);
        return true;
    }

    if (verb == "load")
    {
        long long i = 0;

        if (!(words >> i) || state.presetList.empty())
        {
            return false;
        }

        commands.LoadPreset(state.presetList[Wrap(i, state.presetList.size())].id);
        return true;
    }

    if (!state.activePreset)
    {
        return false;
    }

    const auto& preset = *state.activePreset;
    const auto& nodes = preset.graph.nodes;

    if (verb == "scene" || verb == "removescene")
    {
        long long i = 0;

        if (!(words >> i) || preset.scenes.empty())
        {
            return false;
        }

        const auto& id = preset.scenes[Wrap(i, preset.scenes.size())].id;
        verb == "scene" ? commands.SelectScene(id) : commands.RemoveScene(id);
        return true;
    }

    if (verb == "addscene")
    {
        commands.AddScene();
        return true;
    }

    const auto types = AddableTypes(state);

    if (verb == "add")
    {
        long long type = 0, after = -1;

        if (!(words >> type >> after) || types.empty() || nodes.empty())
        {
            return false;
        }

        // The engine needs a node to insert after: "at the start" is after the first (the input).
        const auto& afterId = nodes[after < 0 ? 0 : Wrap(after, nodes.size())].id;
        const auto& effectType = types[Wrap(type, types.size())]->type;
        Log("[drive] add " + effectType + " after " + afterId);
        commands.AddNode(effectType, afterId);
        return true;
    }

    long long nodeIndex = 0;

    if (!(words >> nodeIndex) || nodes.empty())
    {
        return false;
    }

    const auto& node = nodes[Wrap(nodeIndex, nodes.size())];
    Log("[drive] " + verb + " " + node.id + " (" + node.type + ")");

    if (verb == "remove")
    {
        commands.RemoveNode(node.id);
    }
    else if (verb == "bypass")
    {
        commands.SetNodeBypassed(node.id, node.enabled);
    }
    else if (verb == "move")
    {
        long long after = 0;
        words >> after;
        commands.MoveNode(node.id, nodes[Wrap(after, nodes.size())].id);
    }
    else if (verb == "replace")
    {
        long long type = 0;

        if (!(words >> type) || types.empty())
        {
            return false;
        }

        commands.ReplaceNode(node.id, types[Wrap(type, types.size())]->type);
    }
    else if (verb == "param")
    {
        long long param = 0;
        double fraction = 0.5;
        words >> param >> fraction;
        const auto it = std::find_if(state.catalog.begin(), state.catalog.end(),
                                     [&node](const uic::EffectTypeInfo& info) { return info.type == node.type; });

        if (it == state.catalog.end() || it->parameters.empty())
        {
            return false;
        }

        const auto& info = it->parameters[Wrap(param, it->parameters.size())];
        fraction = std::clamp(fraction, 0.0, 1.0);
        auto value = info.minValue + fraction * (info.maxValue - info.minValue);

        if (info.step > 0.0)
        {
            value = info.minValue + std::round((value - info.minValue) / info.step) * info.step;
        }

        commands.SetNodeParam(node.id, info.key, value);
    }
    else if (verb == "branches")
    {
        int count = 2;
        words >> count;
        commands.SetSplitBranchCount(node.id, std::clamp(count, 2, 4));
    }
    else if (verb == "collapse")
    {
        commands.CollapseSplit(node.id);
    }
    else
    {
        return false;
    }

    return true;
}
} // namespace nanoq::ui
