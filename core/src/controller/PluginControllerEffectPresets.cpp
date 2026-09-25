/**
 * PluginControllerEffectPresets.cpp - User-saved presets for a single effect
 * type.
 *
 * Factory presets live in the effect registry (EffectTypeInfo::presets); these
 * are the custom half of that same vocabulary, kept in UI storage so they are
 * shared across presets rather than baked into any one of them.
 *
 * A factory preset is only parameter values. A user preset is everything the
 * node was set to — its parameters, its resources (the NAM model, the IR, the
 * hosted plugin) and its config (a blend, an oversampling override, a hosted
 * plugin's state) — so loading it into another chain gives the same sound.
 * Entries saved before resources and config were captured carry parameters
 * only, and loading one leaves the node's resources and config alone.
 */

#include "PluginController.h"

#include "controller/internal/ControllerUtils.h"
#include "controller/internal/HostedPluginSupport.h"
#include "controller/internal/NamResourceMetadata.h"
#include "dsp/EffectRegistry.h"
#include "presets/PresetStorage.h"

#include <set>

using namespace guitarfx::controller_detail;

namespace guitarfx
{
namespace
{
constexpr const char* kEffectPresetsFile = "effect-presets.json";

nlohmann::json NormalizeEffectPresetsDocument(nlohmann::json document)
{
    if (!document.is_object())
    {
        document = nlohmann::json::object();
    }

    if (!document.contains("byEffectType") || !document["byEffectType"].is_object())
    {
        document["byEffectType"] = nlohmann::json::object();
    }

    return document;
}

/// Config a node carries about its moment rather than its settings: a request to open the
/// plugin editor, the last load error, and the length that stands in for a scrubbed plugin
/// state. None of it is saved, and loading a preset leaves the node's own copy in place.
bool IsTransientNodeConfigKey(const std::string& key)
{
    return key == "showPluginEditor" || key == "openPluginEditor" || key == kHostedPluginLastErrorCodeConfigKey ||
           key == kHostedPluginStateLengthConfigKey;
}

/// The node's settings as an effect preset entry, without its id or name.
nlohmann::json CaptureNodeSettings(const GraphNode& node)
{
    nlohmann::json entry;
    entry["parameters"] = nlohmann::json::object();

    for (const auto& [key, value] : node.params)
    {
        // Injected from the interface calibration each time a chain is built, never the node's own.
        if (key == "calibrationInputLevel" || key == "calibrationInputLevelEnabled" || key == "calibrationOutputLevel")
        {
            continue;
        }

        entry["parameters"][key] = value;
    }

    entry["resources"] = nlohmann::json::array();

    for (const auto& resource : node.resources)
    {
        entry["resources"].push_back(SerializeResourceRef(resource));
    }

    entry["config"] = nlohmann::json::object();

    for (const auto& [key, value] : node.config)
    {
        if (!IsTransientNodeConfigKey(key))
        {
            entry["config"][key] = value;
        }
    }

    return entry;
}

/// Compared as saved: a node's refs also carry library metadata that is never stored, and
/// would otherwise make every load look like a new model.
bool SameResources(const std::vector<ResourceRef>& a, const std::vector<ResourceRef>& b)
{
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const ResourceRef& x, const ResourceRef& y) {
        return SerializeResourceRef(x) == SerializeResourceRef(y);
    });
}

/// The store as the UI sees it: hosted plugin state chunks can be megabytes and the UI never
/// needs them, the same rule ScrubHostedPluginStateForUi applies to graphs.
nlohmann::json ScrubEffectPresetsForUi(nlohmann::json byEffectType)
{
    for (auto& [effectType, presets] : byEffectType.items())
    {
        if (!presets.is_array())
        {
            continue;
        }

        for (auto& entry : presets)
        {
            if (!entry.is_object() || !entry.contains("config") || !entry["config"].is_object())
            {
                continue;
            }

            auto& config = entry["config"];

            if (config.contains(kHostedPluginStateConfigKey) && config[kHostedPluginStateConfigKey].is_string())
            {
                config[kHostedPluginStateLengthConfigKey] =
                    std::to_string(config[kHostedPluginStateConfigKey].get_ref<const std::string&>().size());
                config.erase(kHostedPluginStateConfigKey);
            }
        }
    }

    return byEffectType;
}
} // namespace

void PluginController::BroadcastEffectPresets()
{
    const auto document =
        NormalizeEffectPresetsDocument(LoadUiStorageJson(kEffectPresetsFile, nlohmann::json::object()));
    nlohmann::json msg;
    msg["type"] = "effectPresets";
    msg["byEffectType"] = ScrubEffectPresetsForUi(document["byEffectType"]);
    SendMessageToUI(msg.dump());
}

void PluginController::HandleGetEffectPresetsRequest()
{
    BroadcastEffectPresets();
}

void PluginController::HandleSaveEffectPresetRequest(const nlohmann::json& payload)
{
    const std::string effectType = payload.value("effectType", "");
    const std::string name = payload.value("name", "");
    const std::string nodeId = payload.value("nodeId", "");

    if (effectType.empty() || name.empty())
    {
        return;
    }

    // With a nodeId the engine's own copy of the node is the snapshot: the UI's draft has
    // no hosted plugin state, and the live plugin may have moved on from the stored one.
    // Without one (an older client), the parameters it sent are all there is to save.
    nlohmann::json settings;
    auto* graph = nodeId.empty() ? nullptr : ResolveEditTarget();
    const auto* node = graph ? graph->FindNode(nodeId) : nullptr;

    if (node)
    {
        settings = CaptureNodeSettings(*node);

        if (IsHostedPluginNode(*node) && !IsCompositeEditMode() && !mActivePresetId.empty())
        {
            auto liveState = ReadLiveNodeConfig(mActivePresetId, nodeId, kHostedPluginStateConfigKey);

            if (!liveState.empty())
            {
                settings["config"][kHostedPluginStateConfigKey] = std::move(liveState);
            }
        }
    }
    else
    {
        const auto parameters = payload.value("parameters", nlohmann::json::object());

        if (!parameters.is_object())
        {
            return;
        }

        settings["parameters"] = parameters;
    }

    auto document = NormalizeEffectPresetsDocument(LoadUiStorageJson(kEffectPresetsFile, nlohmann::json::object()));

    auto& presets = document["byEffectType"][effectType];

    if (!presets.is_array())
    {
        presets = nlohmann::json::array();
    }

    // Saving under an existing name overwrites that entry in place, keeping its id
    // so any UI selection pointing at it stays valid.
    auto existing = std::find_if(presets.begin(), presets.end(),
                                 [&name](const nlohmann::json& entry) { return entry.value("name", "") == name; });

    if (existing == presets.end())
    {
        nlohmann::json entry;
        entry["id"] = "efp-" + GenerateGuidV4String();
        entry["name"] = name;
        presets.push_back(std::move(entry));
        existing = std::prev(presets.end());
    }

    // Replaced whole, so an entry saved from a node without resources does not keep an older
    // save's model.
    existing->erase("resources");
    existing->erase("config");

    for (auto it = settings.begin(); it != settings.end(); ++it)
    {
        (*existing)[it.key()] = std::move(it.value());
    }

    SaveUiStorageJson(kEffectPresetsFile, document);
    BroadcastEffectPresets();
}

void PluginController::HandleApplyEffectPresetRequest(const nlohmann::json& payload)
{
    const std::string nodeId = payload.value("nodeId", "");
    const std::string effectType = payload.value("effectType", "");
    const std::string presetId = payload.value("presetId", "");

    if (nodeId.empty() || effectType.empty() || presetId.empty())
    {
        return;
    }

    const auto document =
        NormalizeEffectPresetsDocument(LoadUiStorageJson(kEffectPresetsFile, nlohmann::json::object()));
    const auto presetsIt = document["byEffectType"].find(effectType);

    if (presetsIt == document["byEffectType"].end() || !presetsIt->is_array())
    {
        return;
    }

    const auto entryIt = std::find_if(presetsIt->begin(), presetsIt->end(), [&presetId](const nlohmann::json& entry) {
        return entry.is_object() && entry.value("id", "") == presetId;
    });

    if (entryIt == presetsIt->end())
    {
        ReportErrorToUI("Effect preset not found", "No saved preset with id " + presetId + " for this effect");
        return;
    }

    const auto& entry = *entryIt;
    auto& registry = EffectRegistry::Instance();

    auto* graph = ResolveEditTarget();
    auto* node = graph ? graph->FindNode(nodeId) : nullptr;

    if (!node)
    {
        return;
    }

    if (registry.Resolve(node->type) != registry.Resolve(effectType))
    {
        AppendSessionLog("Effect preset " + presetId + " is for " + effectType + ", not node " + nodeId + " (" +
                         node->type + ")");
        return;
    }

    // Parameters in the order the effect declares them, which is the order the ones that
    // bound others (Graphic EQ's band count) are declared in, then anything the effect does
    // not declare but the node already has: WASM and composite nodes carry their own.
    // A key neither knows is a parameter since renamed, and is left out.
    std::vector<std::pair<std::string, double>> params;
    const auto savedParams = entry.value("parameters", nlohmann::json::object());

    if (savedParams.is_object())
    {
        const auto typeInfo = registry.GetTypeInfo(node->type);
        std::set<std::string> declared;

        if (typeInfo)
        {
            for (const auto& def : typeInfo->parameters)
            {
                declared.insert(def.id);

                if (const auto it = savedParams.find(def.id); it != savedParams.end() && it->is_number())
                {
                    params.emplace_back(def.id, it->get<double>());
                }
            }
        }

        const bool declaresNone = !typeInfo || typeInfo->parameters.empty();

        for (const auto& [key, value] : savedParams.items())
        {
            if (value.is_number() && !declared.contains(key) && (declaresNone || node->params.contains(key)))
            {
                params.emplace_back(key, value.get<double>());
            }
        }
    }

    // Resources and config replace the node's whole, when the entry carries them.
    std::optional<std::vector<ResourceRef>> resources;

    if (entry.contains("resources") && entry["resources"].is_array())
    {
        resources.emplace();

        for (const auto& resource : entry["resources"])
        {
            if (resource.is_object())
            {
                resources->push_back(DeserializeResourceRef(resource));
            }
        }
    }

    std::optional<std::map<std::string, std::string>> config;

    if (entry.contains("config") && entry["config"].is_object())
    {
        config.emplace();

        for (const auto& [key, value] : node->config)
        {
            if (IsTransientNodeConfigKey(key))
            {
                (*config)[key] = value;
            }
        }

        for (const auto& [key, value] : entry["config"].items())
        {
            if (value.is_string() && !IsTransientNodeConfigKey(key))
            {
                (*config)[key] = value.get<std::string>();
            }
        }
    }

    const bool resourcesChange = resources && !SameResources(*resources, node->resources);
    const bool configChanges = config && *config != node->config;

    for (const auto& [key, value] : params)
    {
        node->params[key] = value;
    }

    if (!resourcesChange && !configChanges)
    {
        // Parameters alone: set on the running chain, as a knob turn would, with no rebuild.
        const std::string slotId = mActivePresetId.empty() ? "p1" : mActivePresetId;
        SyncActivePresetSceneGraph();
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);

            for (const auto& [key, value] : params)
            {
                mPresetMixer.SetNodeParam(slotId, nodeId, key, value);
            }
        }

        ApplyDeferredNodeRebuilds();

        if (std::any_of(params.begin(), params.end(), [](const auto& param) { return param.first == "lowLatency"; }))
        {
            UpdateHostLatency();
        }

        if (IsCompositeEditMode())
        {
            BroadcastCompositeEditState();
        }
        else
        {
            mActivePresetJson = mActivePreset ? PresetStorage::SerializeToJson(*mActivePreset) : "{}";
            mPendingStateBroadcast = true;
        }

        return;
    }

    // A new model, IR, plugin or config means rebuilding the chain, as choosing one does.
    // Other hosted plugins in it are torn down and restored from config, so their live
    // state is folded in first.
    CaptureLiveHostedPluginStateIntoActivePreset();
    graph = ResolveEditTarget();
    node = graph ? graph->FindNode(nodeId) : nullptr;

    if (!node)
    {
        return;
    }

    if (resourcesChange)
    {
        node->resources = std::move(*resources);
    }

    if (configChanges)
    {
        node->config = std::move(*config);
    }

    RefreshWasmNodeDescriptor(*node);

    // ApplyPreset replaces the working copy, so nothing may read `node` after it.
    const bool resetNamLevels =
        IsNamEffectType(node->type) && !node->resources.empty() && node->resources.front().IsValid();
    node = nullptr;
    graph = nullptr;

    if (IsCompositeEditMode())
    {
        BroadcastCompositeEditState();
        return;
    }

    if (!mActivePreset)
    {
        return;
    }

    SyncActivePresetSceneGraph();
    ApplyPreset(*mActivePreset);
    mPendingStateBroadcast = true;

    if (resetNamLevels)
    {
        ResetNamNodeLevelState(nodeId);
    }
}

void PluginController::HandleDeleteEffectPresetRequest(const nlohmann::json& payload)
{
    const std::string effectType = payload.value("effectType", "");
    const std::string presetId = payload.value("presetId", "");

    if (effectType.empty() || presetId.empty())
    {
        return;
    }

    auto document = NormalizeEffectPresetsDocument(LoadUiStorageJson(kEffectPresetsFile, nlohmann::json::object()));

    if (!document["byEffectType"].contains(effectType))
    {
        return;
    }

    auto& presets = document["byEffectType"][effectType];

    if (!presets.is_array())
    {
        return;
    }

    nlohmann::json remaining = nlohmann::json::array();

    for (const auto& entry : presets)
    {
        if (entry.value("id", "") != presetId)
        {
            remaining.push_back(entry);
        }
    }

    if (remaining.size() == presets.size())
    {
        return; // nothing matched — leave the file untouched
    }

    if (remaining.empty())
    {
        document["byEffectType"].erase(effectType);
    }
    else
    {
        presets = std::move(remaining);
    }

    SaveUiStorageJson(kEffectPresetsFile, document);
    BroadcastEffectPresets();
}
} // namespace guitarfx
