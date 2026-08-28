/**
 * PluginControllerSignalPath.cpp - Editing the node graph of the preset being
 * edited.
 *
 * Every mutation here runs against ResolveEditTarget() rather than the active
 * preset directly, so the same handlers serve normal editing and composite
 * edit mode. A graph is validated acyclic before it is handed to the DSP -
 * an invalid graph must never reach Process().
 */

#include "PluginController.h"

#include "controller/internal/ControllerUtils.h"
#include "controller/internal/HostedPluginSupport.h"
#include "controller/internal/NamResourceMetadata.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#if defined(GUITARFX_ENABLE_WASM_EFFECTS)
#include "dsp/effects/WasmEffect.h"
#endif
#include "presets/PresetStorage.h"
#include "resources/ResourceLibrary.h"
#include "util/PathEncoding.h"

#include <algorithm>

using namespace guitarfx::controller_detail;

namespace guitarfx
{

void PluginController::HandleUpdateSignalPathNodeParamRequest(const nlohmann::json& payload)
{
    // Updates a single DSP parameter on a graph node by nodeId/paramKey
    std::string nodeId = payload.value("nodeId", "");
    std::string paramKey = payload.value("paramKey", "");
    double value = payload.value("value", 0.0);
    // Prefer an explicit presetId from the payload; fall back to the active preset ID so
    // that effects whose instance is keyed by a real UUID (not "p1") still receive the update.
    std::string presetId = payload.value("presetId", std::string{});
    if (presetId.empty())
        presetId = mActivePresetId.empty() ? "p1" : mActivePresetId;

    auto* graph = ResolveEditTarget();
    if (!graph) return;

    auto* node = graph->FindNode(nodeId);
    if (!node) return;

    node->params[paramKey] = value;
    SyncActivePresetSceneGraph();
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        mPresetMixer.SetNodeParam(presetId, nodeId, paramKey, value);
    }
    // Some parameters (e.g. the convolution low-latency toggle) change a node's
    // processing latency. Re-report total plugin latency so the host updates PDC.
    if (paramKey == "lowLatency")
        UpdateHostLatency();
    mActivePresetJson = mActivePreset ? PresetStorage::SerializeToJson(*mActivePreset) : "{}";
}

void PluginController::HandleUpdateSignalPathNodeBypassRequest(const nlohmann::json& payload)
{
    std::string nodeId = payload.value("nodeId", "");
    bool enabled = payload.value("enabled", true);
    if (payload.contains("bypassed"))
        enabled = !payload.value("bypassed", false);
    const std::string fallbackId = mActivePresetId.empty() ? "p1" : mActivePresetId;
    std::string presetId = payload.value("presetId", fallbackId);

    auto* graph = ResolveEditTarget();
    if (!graph) return;

    auto* node = graph->FindNode(nodeId);
    if (!node) return;

    node->enabled = enabled;
    SyncActivePresetSceneGraph();
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        mPresetMixer.SetNodeEnabled(presetId, nodeId, enabled);
    }
    mActivePresetJson = mActivePreset ? PresetStorage::SerializeToJson(*mActivePreset) : "{}";
    mPendingStateBroadcast = true;
}

void PluginController::HandleUpdateSignalPathNodeConfigRequest(const nlohmann::json& payload)
{
    const std::string nodeId = payload.value("nodeId", "");
    std::string key = payload.value("key", payload.value("configKey", ""));
    std::string value = payload.value("value", "");
    const bool persist = payload.value("persist", true);
    const bool capture = payload.value("capture", false) || value == "__capture_plugin_state__";
    const std::string fallbackId = mActivePresetId.empty() ? "p1" : mActivePresetId;
    const std::string presetId = payload.value("presetId", fallbackId);
    bool notifyStateChanged = false;

    if (nodeId.empty() || key.empty())
        return;

    if (capture)
    {
        AppendSessionLog("Hosted plugin capture requested presetId=" + presetId + ", nodeId=" + nodeId);
        key = kHostedPluginStateConfigKey;
        value = mPresetMixer.GetNodeConfig(presetId, nodeId, key);
        if (value.empty() && !mActivePresetId.empty() && mActivePresetId != presetId)
        {
            // The UI can address the focused preset by an id the mixer slot does not answer
            // to yet. Prefer an answer over a spurious "capture failed".
            value = mPresetMixer.GetNodeConfig(mActivePresetId, nodeId, key);
        }
        if (value.empty())
        {
            AppendSessionLog("Hosted plugin capture failed presetId=" + presetId + ", nodeId=" + nodeId + ": empty runtime state");
            ReportErrorToUI("Plugin state capture failed", "No hosted plugin state was available for node " + nodeId);
            return;
        }

        AppendSessionLog("Hosted plugin capture succeeded presetId=" + presetId + ", nodeId=" + nodeId
            + ", stateLength=" + std::to_string(value.size())
            + ", stateHash=" + HashStringForLog(value));
    }

    mPresetMixer.SetNodeConfig(presetId, nodeId, key, value);

    if (persist)
    {
        auto* graph = ResolveEditTarget();
        auto* node = graph ? graph->FindNode(nodeId) : nullptr;
        if (!node)
            return;

        node->config[key] = value;
        RefreshWasmNodeDescriptor(*node);

        if (IsCompositeEditMode())
        {
            BroadcastCompositeEditState();
        }
        else if (mActivePreset)
        {
            SyncActivePresetSceneGraph();
            mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);
            mMixerPresetJsonCache[presetId] = mActivePresetJson;
            mPendingStateBroadcast = true;
            notifyStateChanged = true;
        }
    }

    if (capture)
    {
        SendMessageToUI(nlohmann::json{
            {"type", "signalPathNodeConfigUpdated"},
            {"presetId", presetId},
            {"nodeId", nodeId},
            {"key", key},
            {"captured", true},
            {"valueLength", value.size()},
            {"persist", persist},
            {"dirty", persist}
        }.dump());
    }

    if (notifyStateChanged)
        mHost.NotifyStateChanged();
}

void PluginController::HandleUpdateNodeResourceRequest(const nlohmann::json& payload)
{
    std::string nodeId = payload.value("nodeId", "");
    if (nodeId.empty()) return;
    const bool openPluginEditorAfterLoad = payload.value("openPluginEditorAfterLoad", false);

    // Swapping any node's resource rebuilds the whole chain, so unrelated hosted plugins
    // in it are torn down and restored from config.
    CaptureLiveHostedPluginStateIntoActivePreset();

    int resourceIndex = payload.value("resourceIndex", -1);
    const std::string exposedResourceId = payload.value("exposedResourceId", "");

    ResourceRef ref;
    if (payload.contains("resourceType"))
        ref.resourceType = payload["resourceType"].get<std::string>();
    if (payload.contains("resourceId"))
        ref.resourceId = payload["resourceId"].get<std::string>();
    if (payload.contains("filePath"))
        ref.filePath = payload["filePath"].get<std::string>();
    if (payload.contains("embeddedId"))
        ref.embeddedId = payload["embeddedId"].get<std::string>();
    if (payload.contains("parameterId"))
        ref.parameterId = payload["parameterId"].get<std::string>();
    if (payload.contains("parameterValue") && payload["parameterValue"].is_number())
        ref.parameterValue = payload["parameterValue"].get<double>();

    if (!exposedResourceId.empty())
    {
        auto* targetGraph = ResolveEditTarget();
        auto* targetNode = targetGraph ? targetGraph->FindNode(nodeId) : nullptr;
        if (targetNode && targetNode->type.rfind("composite:", 0) == 0)
        {
            const std::string definitionId = targetNode->type.substr(std::string("composite:").size());
            if (const auto* definition = mCompositeLibrary.GetDefinition(definitionId))
            {
                const auto exposedIt = std::find_if(
                    definition->exposedResources.begin(),
                    definition->exposedResources.end(),
                    [&](const ExposedResource& exposed)
                    {
                        return exposed.resourceId == exposedResourceId;
                    });

                if (exposedIt != definition->exposedResources.end())
                {
                    if (ref.resourceType.empty())
                        ref.resourceType = exposedIt->resourceType;
                    if (ref.parameterId.empty() && !exposedIt->parameterId.empty())
                        ref.parameterId = exposedIt->parameterId;
                    if (!ref.parameterValue.has_value() && exposedIt->parameterValue.has_value())
                        ref.parameterValue = *exposedIt->parameterValue;
                }
            }
        }
    }

    if (!ref.parameterId.empty() && ref.parameterValue.has_value())
        ref.parameters[ref.parameterId] = *ref.parameterValue;

    const auto requestHostedPluginEditorOpen = [this, openPluginEditorAfterLoad](const std::string& targetNodeId,
                                                                                  const ResourceRef& selectedRef)
    {
        if (!openPluginEditorAfterLoad || selectedRef.resourceType != "plugin")
            return;

        HandleUpdateSignalPathNodeConfigRequest(nlohmann::json{
            {"nodeId", targetNodeId},
            {"key", "showPluginEditor"},
            {"value", "1"},
            {"persist", false}
        });
    };

    if (resourceIndex >= 0)
    {
        if (!IsCompositeEditMode())
            EnsureBasicGraph();

        auto* targetGraph = ResolveEditTarget();
        if (!targetGraph) return;

        auto* target = targetGraph->FindNode(nodeId);
        if (!target) return;

        // Noted before the slot is touched so a plugin swap can be detected below.
        const std::string previousPluginResourceKey = HostedPluginResourceKey(*target);

        if (static_cast<size_t>(resourceIndex) >= target->resources.size())
            target->resources.resize(static_cast<size_t>(resourceIndex) + 1);

        ResourceRef& slot = target->resources[static_cast<size_t>(resourceIndex)];
        if (!ref.resourceType.empty())
            slot.resourceType = ref.resourceType;

        // A clear is signalled by empty resourceId + empty filePath with no parameterValue update.
        // (Contrast with a blend-value-only update which also sends empty IDs but includes parameterValue.)
        const bool isClearOperation = ref.resourceId.empty() && ref.filePath.empty() && !ref.parameterValue.has_value();
        if (isClearOperation)
        {
            slot.resourceId.clear();
            slot.filePath.clear();
            slot.embeddedId.clear();
            slot.parameters.clear();
            slot.parameterValue.reset();
        }
        else
        {
            if (!ref.resourceId.empty())
            {
                slot.resourceId = ref.resourceId;
                slot.filePath.clear();
            }
            if (!ref.filePath.empty())
            {
                slot.filePath = ref.filePath;
                slot.resourceId.clear();
            }
            if (!ref.embeddedId.empty())
                slot.embeddedId = ref.embeddedId;
            if (!ref.parameterId.empty())
                slot.parameterId = ref.parameterId;
            if (ref.parameterValue.has_value())
                slot.parameterValue = ref.parameterValue;
            if (!ref.parameters.empty())
                slot.parameters = ref.parameters;
        }
        const ResourceRef selectedRef = slot;

        if (ClearStaleHostedPluginState(*target, previousPluginResourceKey))
        {
            AppendSessionLog("Hosted plugin resource changed, dropped stale plugin state nodeId=" + nodeId);
        }

        RefreshWasmNodeDescriptor(*target);
        bool appliedPreset = false;

        if (IsCompositeEditMode())
        {
            BroadcastCompositeEditState();
        }
        else if (mActivePreset)
        {
            SyncActivePresetSceneGraph();
            ApplyPreset(*mActivePreset);
            mPendingStateBroadcast = true;
            appliedPreset = true;
        }

        if (!IsCompositeEditMode()
            && IsNamEffectType(target->type)
            && !target->resources.empty()
            && target->resources.front().IsValid())
        {
            ResetNamNodeLevelState(nodeId);
        }
        if (appliedPreset && ReportHostedPluginResourceLoadFailure(nodeId, selectedRef, resourceIndex))
            DiscardFailedHostedPluginResourceSelection(nodeId, selectedRef, resourceIndex);
        else if (appliedPreset)
        {
            NotifyHostedPluginResourceLoadCompleted(nodeId, selectedRef, resourceIndex);
            requestHostedPluginEditorOpen(nodeId, selectedRef);
        }
        return;
    }

    if (!ref.filePath.empty())
    {
        auto* fpGraph = ResolveEditTarget();
        auto* node = fpGraph ? fpGraph->FindNode(nodeId) : nullptr;
        if (node && !node->resources.empty())
        {
            const std::string previousPluginResourceKey = HostedPluginResourceKey(*node);

            node->resources.clear();
            node->resources.push_back(ref);
            const ResourceRef selectedRef = ref;

            if (ClearStaleHostedPluginState(*node, previousPluginResourceKey))
            {
                AppendSessionLog("Hosted plugin resource changed, dropped stale plugin state nodeId=" + nodeId);
            }

            RefreshWasmNodeDescriptor(*node);
            bool appliedPreset = false;

            if (IsCompositeEditMode())
            {
                BroadcastCompositeEditState();
            }
            else if (mActivePreset)
            {
                SyncActivePresetSceneGraph();
                ApplyPreset(*mActivePreset);
                mPendingStateBroadcast = true;
                appliedPreset = true;
            }

            if (!IsCompositeEditMode()
                && IsNamEffectType(node->type)
                && !node->resources.empty()
                && node->resources.front().IsValid())
            {
                ResetNamNodeLevelState(node->id);
            }
            if (appliedPreset && ReportHostedPluginResourceLoadFailure(nodeId, selectedRef))
                DiscardFailedHostedPluginResourceSelection(nodeId, selectedRef);
            else if (appliedPreset)
            {
                NotifyHostedPluginResourceLoadCompleted(nodeId, selectedRef);
                requestHostedPluginEditorOpen(nodeId, selectedRef);
            }
            return;
        }
    }

    const bool updatedResource = UpdateResourceForNodeId(nodeId, ref);

    if (auto* targetGraph = ResolveEditTarget())
    {
        if (auto* node = targetGraph->FindNode(nodeId))
            RefreshWasmNodeDescriptor(*node);
    }

    if (mActivePreset)
    {
        auto* node = mActivePreset->graph.FindNode(nodeId);
        if (node && IsNamEffectType(node->type)
            && !node->resources.empty() && node->resources.front().IsValid())
        {
            ResetNamNodeLevelState(nodeId);
        }
    }
    if (updatedResource && ReportHostedPluginResourceLoadFailure(nodeId, ref))
        DiscardFailedHostedPluginResourceSelection(nodeId, ref);
    else if (updatedResource)
    {
        NotifyHostedPluginResourceLoadCompleted(nodeId, ref);
        requestHostedPluginEditorOpen(nodeId, ref);
    }
}

void PluginController::HandleBrowseNodeResourceRequest(const nlohmann::json& payload)
{
    std::string nodeId = payload.value("nodeId", "");
    std::string resourceType = payload.value("resourceType", "nam");
    const int resourceIndex = payload.value("resourceIndex", -1);
    const std::string exposedResourceId = payload.value("exposedResourceId", "");
    const std::string category = payload.value("category", "");
    const bool openPluginEditorAfterLoad = payload.value("openPluginEditorAfterLoad", false);

    mHost.BrowseFileAsync(ResolveBrowseFileType(resourceType), "Select Resource",
        [this, nodeId, resourceType, resourceIndex, exposedResourceId, category, openPluginEditorAfterLoad](const BrowseFileResult& result)
        {
            if (result.success)
            {
                nlohmann::json payload;
                payload["filePath"] = util::PathToUtf8(result.path);
                payload["resourceType"] = resourceType;
                payload["nodeId"] = nodeId;
                payload["name"] = util::PathToUtf8(result.path.stem());
                payload["category"] = category.empty() ? std::string{"Local"} : category;
                payload["metadata"] = nlohmann::json::object({{"provider", kLocalResourceProvider}});
                if (resourceIndex >= 0)
                    payload["resourceIndex"] = resourceIndex;
                if (!exposedResourceId.empty())
                    payload["exposedResourceId"] = exposedResourceId;
                if (openPluginEditorAfterLoad)
                    payload["openPluginEditorAfterLoad"] = true;
                HandleSaveLocalLibraryResourceRequest(payload);
            }
            else
            {
                // Let the UI know the browse dialog was dismissed so it can
                // clear any pending loading indicators.
                nlohmann::json cancelMsg{
                    {"type", "nodeResourceBrowseCancelled"},
                    {"nodeId", nodeId},
                    {"resourceType", resourceType}
                };
                if (resourceIndex >= 0)
                    cancelMsg["resourceIndex"] = resourceIndex;
                if (!exposedResourceId.empty())
                    cancelMsg["exposedResourceId"] = exposedResourceId;
                SendMessageToUI(cancelMsg.dump());
            }
        });
}

void PluginController::HandleAddSignalPathNodeRequest(const nlohmann::json& payload)
{
    // Adding a node rebuilds every processor in the chain, hosted plugins included.
    CaptureLiveHostedPluginStateIntoActivePreset();

    const std::string effectType = payload.value("effectType", "");
    const std::string insertAfter = payload.value("insertAfter", "");
    const std::string labelOverride = payload.value("label", "");
    const std::string categoryOverride = payload.value("category", "");
    const auto configPayload = payload.value("config", nlohmann::json::object());
    const auto paramsPayload = payload.value("params", nlohmann::json::object());
    const auto resourcesPayload = payload.value("resources", nlohmann::json::array());

    std::string edgeFrom, edgeTo;
    int edgeFromPort = 0, edgeToPort = 0;
    double edgeGain = 1.0;

    const auto edgeIt = payload.find("edge");
    if (edgeIt != payload.end() && edgeIt->is_object())
    {
        edgeFrom = edgeIt->value("from", "");
        edgeTo = edgeIt->value("to", "");
        edgeFromPort = edgeIt->value("fromPort", 0);
        edgeToPort = edgeIt->value("toPort", 0);
        edgeGain = edgeIt->value("gain", 1.0);
    }

    if (effectType.empty() || (insertAfter.empty() && edgeFrom.empty()))
    {
        ReportErrorToUI("Add node failed", "Missing effectType or insertion target (insertAfter/edge)");
        return;
    }

    SignalGraph* targetGraph = ResolveEditTarget();
    if (!targetGraph)
    {
        ReportErrorToUI("Add node failed", "No active preset or composite");
        return;
    }

    auto& edges = targetGraph->edges;
    const auto originalEdges = edges;
    auto chosenEdgeIt = edges.end();

    if (!edgeFrom.empty() && !edgeTo.empty())
    {
        chosenEdgeIt = std::find_if(edges.begin(), edges.end(),
            [&](const GraphEdge& e) {
                return e.from == edgeFrom && e.to == edgeTo && e.fromPort == edgeFromPort && e.toPort == edgeToPort;
            });
    }
    else
    {
        chosenEdgeIt = std::find_if(edges.begin(), edges.end(),
            [&](const GraphEdge& e) { return e.from == insertAfter && e.fromPort == 0; });
        if (chosenEdgeIt == edges.end())
            chosenEdgeIt = std::find_if(edges.begin(), edges.end(),
                [&](const GraphEdge& e) { return e.from == insertAfter; });
    }

    if (chosenEdgeIt == edges.end())
    {
        ReportErrorToUI("Add node failed", "Could not find target edge for insertion");
        return;
    }

    const std::string resolvedEffectType = EffectRegistry::Instance().Resolve(effectType);
    if (resolvedEffectType == EffectGuids::kSplitter)
    {
        auto& graph = *targetGraph;
        const std::string splitterId = MakeUniqueNodeId(graph, "split");
        const std::string mixerId = MakeUniqueNodeId(graph, "mix");

        GraphNode splitter; splitter.id = splitterId; splitter.type = EffectGuids::kSplitter; splitter.category = "utility"; splitter.label = "Splitter"; splitter.enabled = true;
        GraphNode mixer; mixer.id = mixerId; mixer.type = EffectGuids::kMixer; mixer.category = "utility"; mixer.label = "Mixer"; mixer.enabled = true;

        const std::string nextNodeId = chosenEdgeIt->to;
        const int preservedToPort = chosenEdgeIt->toPort;
        const double preservedGain = chosenEdgeIt->gain;

        chosenEdgeIt->to = splitterId; chosenEdgeIt->toPort = 0; chosenEdgeIt->gain = 1.0;

        GraphEdge branch0; branch0.from = splitterId; branch0.to = mixerId; branch0.fromPort = 0; branch0.toPort = 0; branch0.gain = 1.0;
        GraphEdge branch1; branch1.from = splitterId; branch1.to = mixerId; branch1.fromPort = 1; branch1.toPort = 1; branch1.gain = 1.0;
        GraphEdge mixToNext; mixToNext.from = mixerId; mixToNext.to = nextNodeId; mixToNext.fromPort = 0; mixToNext.toPort = preservedToPort; mixToNext.gain = preservedGain;

        edges.push_back(branch0); edges.push_back(branch1); edges.push_back(mixToNext);
        graph.nodes.push_back(splitter); graph.nodes.push_back(mixer);

        if (IsCompositeEditMode()) BroadcastCompositeEditState();
        else if (mActivePreset) { SyncActivePresetSceneGraph(); ApplyPreset(*mActivePreset); BroadcastState(); }
        return;
    }

    // Create new node with default parameters
    GraphNode newNode;
    newNode.id = resolvedEffectType + "_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    newNode.type = resolvedEffectType;
    newNode.enabled = true;

    const auto effectInfoOpt = EffectRegistry::Instance().GetTypeInfo(resolvedEffectType);
    if (effectInfoOpt)
    {
        newNode.category = effectInfoOpt->category;
        newNode.label = effectInfoOpt->displayName;
        for (const auto& p : effectInfoOpt->parameters)
            newNode.params[p.id] = p.defaultValue;
        // Prefer the preset the effect nominates as its starting point; fall back
        // to the first factory preset for effects that don't mark one.
        auto factoryPreset = std::find_if(
            effectInfoOpt->presets.begin(),
            effectInfoOpt->presets.end(),
            [](const EffectPresetDefinition& preset) { return preset.isFactory && preset.isDefault; });
        if (factoryPreset == effectInfoOpt->presets.end())
        {
            factoryPreset = std::find_if(
                effectInfoOpt->presets.begin(),
                effectInfoOpt->presets.end(),
                [](const EffectPresetDefinition& preset) { return preset.isFactory; });
        }
        if (factoryPreset != effectInfoOpt->presets.end())
        {
            for (const auto& [key, value] : factoryPreset->parameters)
                newNode.params[key] = value;
        }
    }
    else { newNode.category = "utility"; newNode.label = effectType; }

    if (paramsPayload.is_object())
    {
        for (const auto& entry : paramsPayload.items())
        {
            if (entry.value().is_number())
                newNode.params[entry.key()] = entry.value().get<double>();
        }
    }

    if (configPayload.is_object())
        for (const auto& entry : configPayload.items())
            if (entry.value().is_string()) newNode.config[entry.key()] = entry.value().get<std::string>();

    if (resourcesPayload.is_array())
    {
        for (const auto& resourceJson : resourcesPayload)
        {
            if (!resourceJson.is_object())
                continue;
            const auto resource = DeserializeResourceRef(resourceJson);
            if (resource.IsValid())
                newNode.resources.push_back(resource);
        }
    }

    if (!labelOverride.empty()) newNode.label = labelOverride;
    if (!categoryOverride.empty()) newNode.category = categoryOverride;
    RefreshWasmNodeDescriptor(newNode);

    const std::string nextNodeId = chosenEdgeIt->to;
    const int preservedToPort = chosenEdgeIt->toPort;
    const double preservedGain = chosenEdgeIt->gain;
    (void)edgeGain;

    chosenEdgeIt->to = newNode.id; chosenEdgeIt->toPort = 0; chosenEdgeIt->gain = 1.0;

    GraphEdge newEdge; newEdge.from = newNode.id; newEdge.to = nextNodeId; newEdge.fromPort = 0; newEdge.toPort = preservedToPort; newEdge.gain = preservedGain;
    edges.push_back(newEdge);
    targetGraph->nodes.push_back(newNode);

    if (!IsGraphAcyclic(*targetGraph))
    {
        edges = originalEdges;
        ReportErrorToUI("Reorder node failed", "Operation would create a cycle");
        return;
    }

    if (IsCompositeEditMode()) BroadcastCompositeEditState();
    else if (mActivePreset) { SyncActivePresetSceneGraph(); ApplyPreset(*mActivePreset); BroadcastState(); }
}

void PluginController::HandleSplitSignalPathEdgeRequest(const nlohmann::json& payload)
{
    CaptureLiveHostedPluginStateIntoActivePreset();

    SignalGraph* targetGraph = ResolveEditTarget();
    if (!targetGraph) { ReportErrorToUI("Split failed", "No active preset or composite"); return; }

    const auto edgeIt = payload.find("edge");
    if (edgeIt == payload.end() || !edgeIt->is_object()) { ReportErrorToUI("Split failed", "Missing edge payload"); return; }

    const std::string from = edgeIt->value("from", "");
    const std::string to = edgeIt->value("to", "");
    const int fromPort = edgeIt->value("fromPort", 0);
    const int toPort = edgeIt->value("toPort", 0);
    if (from.empty() || to.empty()) { ReportErrorToUI("Split failed", "Edge is missing from/to"); return; }

    auto& edges = targetGraph->edges;
    auto targetEdgeIt = std::find_if(edges.begin(), edges.end(),
        [&](const GraphEdge& e) { return e.from == from && e.to == to && e.fromPort == fromPort && e.toPort == toPort; });
    if (targetEdgeIt == edges.end()) { ReportErrorToUI("Split failed", "Target edge not found"); return; }

    const std::string splitterId = MakeUniqueNodeId(*targetGraph, "split");
    const std::string mixerId = MakeUniqueNodeId(*targetGraph, "mix");

    GraphNode splitter; splitter.id = splitterId; splitter.type = EffectGuids::kSplitter; splitter.category = "utility"; splitter.label = "Splitter"; splitter.enabled = true;
    GraphNode mixer; mixer.id = mixerId; mixer.type = EffectGuids::kMixer; mixer.category = "utility"; mixer.label = "Mixer"; mixer.enabled = true;

    const std::string nextNodeId = targetEdgeIt->to;
    const int preservedToPort = targetEdgeIt->toPort;
    const double preservedGain = targetEdgeIt->gain;

    targetEdgeIt->to = splitterId; targetEdgeIt->toPort = 0; targetEdgeIt->gain = 1.0;

    GraphEdge b0; b0.from = splitterId; b0.to = mixerId; b0.fromPort = 0; b0.toPort = 0; b0.gain = 1.0;
    GraphEdge b1; b1.from = splitterId; b1.to = mixerId; b1.fromPort = 1; b1.toPort = 1; b1.gain = 1.0;
    GraphEdge mtn; mtn.from = mixerId; mtn.to = nextNodeId; mtn.fromPort = 0; mtn.toPort = preservedToPort; mtn.gain = preservedGain;

    edges.push_back(b0); edges.push_back(b1); edges.push_back(mtn);
    targetGraph->nodes.push_back(splitter); targetGraph->nodes.push_back(mixer);

    if (IsCompositeEditMode()) BroadcastCompositeEditState();
    else if (mActivePreset) { SyncActivePresetSceneGraph(); ApplyPreset(*mActivePreset); BroadcastState(); }
}

void PluginController::HandleCollapseSignalPathSplitRequest(const nlohmann::json& payload)
{
    CaptureLiveHostedPluginStateIntoActivePreset();

    SignalGraph* targetGraph = ResolveEditTarget();
    if (!targetGraph) { ReportErrorToUI("Collapse split failed", "No active preset or composite"); return; }

    const std::string splitterId = payload.value("splitterId", "");
    const std::string mixerId = payload.value("mixerId", "");
    if (splitterId.empty() || mixerId.empty()) { ReportErrorToUI("Collapse split failed", "Missing splitterId/mixerId"); return; }

    auto& edges = targetGraph->edges;
    std::vector<GraphEdge*> splitterOut;
    GraphEdge* mixerOut = nullptr;
    GraphEdge* splitterIn = nullptr;

    for (auto& e : edges)
    {
        if (e.from == splitterId) splitterOut.push_back(&e);
        if (e.from == mixerId) mixerOut = &e;
        if (e.to == splitterId) splitterIn = &e;
    }

    if (!splitterIn || !mixerOut) { ReportErrorToUI("Collapse split failed", "Split is not connected correctly"); return; }

    const bool branchesEmpty = !splitterOut.empty() && std::all_of(splitterOut.begin(), splitterOut.end(),
        [&](const GraphEdge* e) { return e && e->to == mixerId; });
    if (!branchesEmpty) { ReportErrorToUI("Collapse split failed", "Can only collapse an empty split (remove branch effects first)"); return; }

    splitterIn->to = mixerOut->to;
    splitterIn->toPort = mixerOut->toPort;
    splitterIn->gain = mixerOut->gain;

    edges.erase(std::remove_if(edges.begin(), edges.end(),
        [&](const GraphEdge& e) { return e.from == splitterId || e.from == mixerId || e.to == mixerId; }), edges.end());

    targetGraph->nodes.erase(std::remove_if(targetGraph->nodes.begin(), targetGraph->nodes.end(),
        [&](const GraphNode& n) { return n.id == splitterId || n.id == mixerId; }), targetGraph->nodes.end());

    if (IsCompositeEditMode()) BroadcastCompositeEditState();
    else if (mActivePreset) { SyncActivePresetSceneGraph(); ApplyPreset(*mActivePreset); BroadcastState(); }
}

void PluginController::HandleReplaceSignalPathNodeRequest(const nlohmann::json& payload)
{
    CaptureLiveHostedPluginStateIntoActivePreset();

    const std::string nodeId = payload.value("nodeId", "");
    const std::string newEffectType = payload.value("newEffectType", "");
    const std::string labelOverride = payload.value("label", "");
    const std::string categoryOverride = payload.value("category", "");
    const auto configPayload = payload.value("config", nlohmann::json::object());
    const auto paramsPayload = payload.value("params", nlohmann::json::object());
    const auto resourcesPayload = payload.value("resources", nlohmann::json::array());

    if (nodeId.empty() || newEffectType.empty()) { ReportErrorToUI("Replace node failed", "Missing nodeId or newEffectType parameter"); return; }

    SignalGraph* targetGraph = ResolveEditTarget();
    if (!targetGraph) { ReportErrorToUI("Replace node failed", "No active preset or composite"); return; }

    GraphNode* node = targetGraph->FindNode(nodeId);
    if (!node) { ReportErrorToUI("Replace node failed", "Node not found: " + nodeId); return; }

    const auto oldEffectInfoOpt = EffectRegistry::Instance().GetTypeInfo(node->type);
    const std::string resolvedNewEffectType = EffectRegistry::Instance().Resolve(newEffectType);
    const auto newEffectInfoOpt = EffectRegistry::Instance().GetTypeInfo(resolvedNewEffectType);
    if (!newEffectInfoOpt) { ReportErrorToUI("Replace node failed", "Unknown effect type: " + newEffectType); return; }

    const std::string oldCategory = oldEffectInfoOpt ? oldEffectInfoOpt->category : node->category;
    const std::string requestedCategory = !categoryOverride.empty() ? categoryOverride : newEffectInfoOpt->category;
    if (!oldCategory.empty() && !requestedCategory.empty() && oldCategory != requestedCategory)
    { ReportErrorToUI("Replace node failed", "Cannot replace effect with different category"); return; }

    node->type = resolvedNewEffectType;
    node->label = newEffectInfoOpt->displayName;
    node->category = newEffectInfoOpt->category;
    node->params.clear();
    node->resources.clear();
    node->config.clear();

    for (const auto& p : newEffectInfoOpt->parameters)
        node->params[p.id] = p.defaultValue;

    if (paramsPayload.is_object())
    {
        for (const auto& entry : paramsPayload.items())
        {
            if (entry.value().is_number())
                node->params[entry.key()] = entry.value().get<double>();
        }
    }

    if (configPayload.is_object())
        for (const auto& entry : configPayload.items())
            if (entry.value().is_string()) node->config[entry.key()] = entry.value().get<std::string>();

    if (resourcesPayload.is_array())
    {
        for (const auto& resourceJson : resourcesPayload)
        {
            if (!resourceJson.is_object())
                continue;
            const auto resource = DeserializeResourceRef(resourceJson);
            if (resource.IsValid())
                node->resources.push_back(resource);
        }
    }

    if (!labelOverride.empty()) node->label = labelOverride;
    if (!categoryOverride.empty()) node->category = categoryOverride;
    RefreshWasmNodeDescriptor(*node);

    if (IsCompositeEditMode()) BroadcastCompositeEditState();
    else if (mActivePreset) { SyncActivePresetSceneGraph(); ApplyPreset(*mActivePreset); BroadcastState(); }
}

void PluginController::HandleReorderSignalPathNodeRequest(const nlohmann::json& payload)
{
    CaptureLiveHostedPluginStateIntoActivePreset();

    const std::string nodeId = payload.value("nodeId", "");
    const std::string targetNodeId = payload.value("targetNodeId", "");

    std::string edgeFrom, edgeTo;
    int edgeFromPort = 0, edgeToPort = 0;

    const auto edgeIt = payload.find("edge");
    if (edgeIt != payload.end() && edgeIt->is_object())
    {
        edgeFrom = edgeIt->value("from", "");
        edgeTo = edgeIt->value("to", "");
        edgeFromPort = edgeIt->value("fromPort", 0);
        edgeToPort = edgeIt->value("toPort", 0);
    }

    if (nodeId.empty() || (targetNodeId.empty() && edgeFrom.empty())) return;

    SignalGraph* targetGraph = ResolveEditTarget();
    if (!targetGraph) { ReportErrorToUI("Reorder node failed", "No active preset or composite"); return; }

    const GraphNode* node = targetGraph->FindNode(nodeId);
    if (!node) { ReportErrorToUI("Reorder node failed", "Node not found"); return; }
    if (node->type == "splitter" || node->type == "mixer" || node->type == EffectGuids::kSplitter || node->type == EffectGuids::kMixer) { ReportErrorToUI("Reorder node failed", "Cannot move splitter/mixer nodes"); return; }

    // Everything is validated and resolved before the graph is touched: an early
    // return after a partial splice would leave the node disconnected, which then
    // makes every later reorder of that node fail with a missing-edge error.
    const auto& currentEdges = targetGraph->edges;
    const std::size_t edgeCount = currentEdges.size();

    std::size_t incomingIndex = edgeCount;
    std::size_t outgoingIndex = edgeCount;
    std::size_t incomingCount = 0;
    std::size_t outgoingCount = 0;
    for (std::size_t i = 0; i < edgeCount; ++i)
    {
        if (currentEdges[i].to == nodeId && incomingCount++ == 0) incomingIndex = i;
        if (currentEdges[i].from == nodeId && outgoingCount++ == 0) outgoingIndex = i;
    }

    if (incomingCount == 0 || outgoingCount == 0)
    { ReportErrorToUI("Reorder node failed", "Node is not fully connected (missing input or output connection)"); return; }
    if (incomingCount > 1 || outgoingCount > 1)
    { ReportErrorToUI("Reorder node failed", "Node has multiple connections. Remove branch effects first."); return; }

    std::size_t targetEdgeIndex = edgeCount;
    if (!edgeFrom.empty() && !edgeTo.empty())
    {
        // Dropping a node back onto one of its own connections is a no-op, not an error.
        if (edgeFrom == nodeId || edgeTo == nodeId) return;

        for (std::size_t i = 0; i < edgeCount; ++i)
        {
            const auto& e = currentEdges[i];
            if (e.from == edgeFrom && e.to == edgeTo && e.fromPort == edgeFromPort && e.toPort == edgeToPort)
            { targetEdgeIndex = i; break; }
        }
        if (targetEdgeIndex == edgeCount) { ReportErrorToUI("Reorder node failed", "Cannot find target edge"); return; }
    }
    else
    {
        if (targetNodeId == nodeId) return;
        if (!targetGraph->FindNode(targetNodeId)) { ReportErrorToUI("Reorder node failed", "Target node not found"); return; }

        for (std::size_t i = 0; i < edgeCount; ++i)
        {
            if (currentEdges[i].from == targetNodeId) { targetEdgeIndex = i; break; }
        }
        if (targetEdgeIndex == edgeCount) { ReportErrorToUI("Reorder node failed", "Cannot find target position"); return; }

        // The node already sits directly after the target node.
        if (currentEdges[targetEdgeIndex].to == nodeId) return;
    }

    // Commit-on-success: mutate a copy so a failure can never corrupt the graph.
    auto edges = currentEdges;

    const std::string bypassNextId = edges[outgoingIndex].to;
    const int bypassToPort = edges[outgoingIndex].toPort;
    const double bypassGain = edges[outgoingIndex].gain;

    const std::string reinsertNextId = edges[targetEdgeIndex].to;
    const int reinsertToPort = edges[targetEdgeIndex].toPort;
    const double reinsertGain = edges[targetEdgeIndex].gain;

    edges[incomingIndex].to = bypassNextId;
    edges[incomingIndex].toPort = bypassToPort;
    edges[incomingIndex].gain = bypassGain;

    edges[targetEdgeIndex].to = nodeId;
    edges[targetEdgeIndex].toPort = 0;
    edges[targetEdgeIndex].gain = 1.0;

    edges.erase(edges.begin() + static_cast<std::ptrdiff_t>(outgoingIndex));

    GraphEdge reinserted;
    reinserted.from = nodeId;
    reinserted.to = reinsertNextId;
    reinserted.fromPort = 0;
    reinserted.toPort = reinsertToPort;
    reinserted.gain = reinsertGain;
    edges.push_back(reinserted);

    targetGraph->edges = std::move(edges);

    if (IsCompositeEditMode()) BroadcastCompositeEditState();
    else if (mActivePreset) { SyncActivePresetSceneGraph(); ApplyPreset(*mActivePreset); BroadcastState(); }
}

void PluginController::HandleDeleteSignalPathNodeRequest(const nlohmann::json& payload)
{
    CaptureLiveHostedPluginStateIntoActivePreset();

    const std::string nodeId = payload.value("nodeId", "");
    if (nodeId.empty()) return;

    SignalGraph* targetGraph = ResolveEditTarget();
    if (!targetGraph) { ReportErrorToUI("Delete node failed", "No active preset or composite"); return; }

    const GraphNode* node = targetGraph->FindNode(nodeId);
    if (!node) { ReportErrorToUI("Delete node failed", "Node not found: " + nodeId); return; }

    auto& edges = targetGraph->edges;
    auto& nodes = targetGraph->nodes;

    std::vector<GraphEdge*> incomingEdges;
    std::vector<GraphEdge*> outgoingEdges;
    for (auto& edge : edges)
    {
        if (edge.to == nodeId)
            incomingEdges.push_back(&edge);
        if (edge.from == nodeId)
            outgoingEdges.push_back(&edge);
    }

    if (incomingEdges.size() != 1 || outgoingEdges.size() != 1)
    {
        ReportErrorToUI("Delete node failed", "Node has multiple connections. Remove branch effects first.");
        return;
    }

    GraphEdge* inEdge = incomingEdges.front();
    GraphEdge* outEdge = outgoingEdges.front();
    inEdge->to = outEdge->to;
    inEdge->toPort = outEdge->toPort;
    inEdge->gain = outEdge->gain;
    edges.erase(std::remove_if(edges.begin(), edges.end(), [&](const GraphEdge& e) { return e.from == nodeId; }), edges.end());
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [&](const GraphNode& n) { return n.id == nodeId; }), nodes.end());

    if (IsCompositeEditMode()) BroadcastCompositeEditState();
    else if (mActivePreset) { SyncActivePresetSceneGraph(); ApplyPreset(*mActivePreset); BroadcastState(); }
}

bool PluginController::IsCompositeEditMode() const
{
    return mEditingComposite.has_value();
}

SignalGraph* PluginController::ResolveEditTarget()
{
    if (mEditingComposite)
        return &mEditingComposite->innerGraph;
    if (mActivePreset)
    {
        NormalizePresetScenes(*mActivePreset);
        if (auto* scene = FindPresetScene(*mActivePreset, GetResolvedActiveSceneId()))
            return &scene->graph;
        return &mActivePreset->graph;
    }
    return nullptr;
}

std::string PluginController::GetResolvedActiveSceneId() const
{
    if (mActivePreset)
    {
        if (FindPresetScene(*mActivePreset, mActiveSceneId))
            return mActiveSceneId;
        return GetDefaultPresetSceneId(*mActivePreset);
    }
    return mActiveSceneId;
}

void PluginController::SyncActivePresetSceneGraph()
{
    if (!mActivePreset)
        return;

    NormalizePresetScenes(*mActivePreset);
    const std::string resolvedSceneId = GetResolvedActiveSceneId();
    if (auto* scene = FindPresetScene(*mActivePreset, resolvedSceneId))
    {
        EnsurePresetBoundaryGainNodes(scene->graph);
        mActivePreset->graph = scene->graph;
        EnsurePresetBoundaryGainNodes(mActivePreset->graph);
        mActiveSceneId = resolvedSceneId;
        return;
    }

    (void)SetPresetActiveScene(*mActivePreset, resolvedSceneId, &mActiveSceneId);
}

bool PluginController::UpdateResourceForNodeType(const std::string& nodeType,
                                                 const std::string& resourceType,
                                                 const std::filesystem::path& filePath,
                                                 bool applyPreset)
{
    if (!mActivePreset) return false;

    CaptureLiveHostedPluginStateIntoActivePreset();

    for (auto& node : mActivePreset->graph.nodes)
    {
        if (node.type == nodeType)
        {
            ResourceRef ref;
            ref.resourceType = resourceType;
            ref.filePath = filePath;

            const auto normalizePath = [](const std::filesystem::path& value) {
                std::string normalized = value.lexically_normal().generic_string();
                std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                               [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                return normalized;
            };
            const auto normalizedFilePath = normalizePath(filePath);
            for (const auto& resource : mResourceLibrary.GetResourcesByType(resourceType))
            {
                if (normalizePath(resource.filePath) == normalizedFilePath)
                {
                    ref.resourceId = resource.id;
                    ref.filePath.clear();
                    break;
                }
            }

            if (node.resources.empty())
                node.resources.push_back(ref);
            else
                node.resources[0] = ref;

            if (applyPreset)
                ApplyPreset(*mActivePreset);

            mPendingStateBroadcast = true;
            return true;
        }
    }
    return false;
}

bool PluginController::UpdateResourceForNodeId(const std::string& nodeId,
                                               const ResourceRef& ref,
                                               bool applyPreset)
{
    auto* graph = ResolveEditTarget();
    if (!graph) return false;

    auto* node = graph->FindNode(nodeId);
    if (!node) return false;

    CaptureLiveHostedPluginStateIntoActivePreset();

    // Re-resolve: folding live state above can reserialise the working copy, and on the
    // composite-edit path ResolveEditTarget() is unaffected — but taking the pointer again
    // keeps this correct regardless of what the fold touched.
    graph = ResolveEditTarget();
    node = graph ? graph->FindNode(nodeId) : nullptr;
    if (!node) return false;

    const std::string previousPluginResourceKey = HostedPluginResourceKey(*node);

    if (node->resources.empty())
        node->resources.push_back(ref);
    else
        node->resources[0] = ref;

    if (ClearStaleHostedPluginState(*node, previousPluginResourceKey))
    {
        AppendSessionLog("Hosted plugin resource changed, dropped stale plugin state nodeId=" + nodeId);
    }

    if (applyPreset && mActivePreset)
    {
        ApplyPreset(*mActivePreset);
        mPendingStateBroadcast = true;
    }

    return true;
}

void PluginController::RefreshWasmNodeDescriptor(GraphNode& node)
{
#if !defined(GUITARFX_ENABLE_WASM_EFFECTS)
    (void)node;
    return;
#else
    auto& registry = EffectRegistry::Instance();
    if (registry.Resolve(node.type) != EffectGuids::kWasmHost)
        return;

    std::optional<WasmModuleDescriptor> previousDescriptor;
    if (const auto existingDescriptorIt = node.config.find(WasmEffect::kDescriptorConfigKey);
        existingDescriptorIt != node.config.end())
    {
        std::string parseError;
        previousDescriptor = WasmEffect::ParseDescriptorConfig(existingDescriptorIt->second, &parseError);
        if (!previousDescriptor && !parseError.empty())
            AppendSessionLog("WASM descriptor cache parse failed for node " + node.id + ": " + parseError);
    }

    const auto typeInfo = registry.GetTypeInfo(EffectGuids::kWasmHost);
    const auto labelIsDefault = [&]() {
        return node.label.empty()
            || (typeInfo && node.label == typeInfo->displayName)
            || (previousDescriptor && !previousDescriptor->displayName.empty() && node.label == previousDescriptor->displayName);
    };
    const auto categoryIsDefault = [&]() {
        return node.category.empty()
            || (typeInfo && node.category == typeInfo->category)
            || (previousDescriptor && !previousDescriptor->category.empty() && node.category == previousDescriptor->category);
    };

    const auto clearDescriptorState = [&]() {
        if (previousDescriptor)
        {
            for (const auto& oldParam : previousDescriptor->parameters)
                node.params.erase(oldParam.definition.id);
        }

        node.config.erase(WasmEffect::kDescriptorConfigKey);
        if (typeInfo && labelIsDefault())
            node.label = typeInfo->displayName;
        if (typeInfo && categoryIsDefault())
            node.category = typeInfo->category;
    };

    if (node.resources.empty() || !node.resources.front().IsValid())
    {
        clearDescriptorState();
        return;
    }

    const auto modulePath = ResolveResourceRef(node.resources.front());
    if (!modulePath)
        return;

    std::string readError;
    const auto descriptor = WasmEffect::InspectModuleFile(*modulePath, &readError);
    if (!descriptor)
    {
        clearDescriptorState();
        if (!readError.empty())
            AppendSessionLog("WASM descriptor read failed for node " + node.id + ": " + readError);
        return;
    }

    if (descriptor->entries.empty())
    {
        clearDescriptorState();
        return;
    }

    node.config[WasmEffect::kDescriptorConfigKey] = WasmEffect::SerializeDescriptorConfig(descriptor->entries);
    if (labelIsDefault() && !descriptor->displayName.empty())
        node.label = descriptor->displayName;
    if (categoryIsDefault() && !descriptor->category.empty())
        node.category = descriptor->category;

    std::unordered_set<std::string> currentGuestParamIds;
    for (const auto& guestParam : descriptor->parameters)
    {
        currentGuestParamIds.insert(guestParam.definition.id);
        if (node.params.count(guestParam.definition.id) > 0)
            continue;

        double initialValue = guestParam.definition.defaultValue;
        if (!previousDescriptor.has_value())
        {
            const std::string legacyParamKey = "param" + std::to_string(guestParam.slot + 1);
            if (const auto legacyIt = node.params.find(legacyParamKey); legacyIt != node.params.end())
                initialValue = legacyIt->second;
        }

        node.params[guestParam.definition.id] = initialValue;
    }

    if (previousDescriptor)
    {
        for (const auto& oldParam : previousDescriptor->parameters)
        {
            if (currentGuestParamIds.count(oldParam.definition.id) == 0)
                node.params.erase(oldParam.definition.id);
        }
    }
#endif
}

void PluginController::EnsureBasicGraph()
{
    if (!mActivePreset) return;
    if (mActivePreset->graph.nodes.empty())
    {
        // Create a minimal input → output graph
        GraphNode input;
        input.id = "__input__";
        input.type = kNodeTypeInput;
        GraphNode output;
        output.id = "__output__";
        output.type = kNodeTypeOutput;
        mActivePreset->graph.nodes = {input, output};

        GraphEdge edge;
        edge.from = "__input__";
        edge.to = "__output__";
        mActivePreset->graph.edges = {edge};
    }

    EnsurePresetBoundaryGainNodes(mActivePreset->graph);
}

} // namespace guitarfx
