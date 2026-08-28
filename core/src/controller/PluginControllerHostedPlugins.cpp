/**
 * PluginControllerHostedPlugins.cpp - Keeping third-party plugin state alive
 * across rebuilds, scenes and sessions.
 *
 * A hosted plugin's state is an opaque chunk owned by that plugin. It is
 * captured from the running instance on save and reapplied on load, but only
 * onto a node whose plugin identity still matches - handing one plugin's chunk
 * to another is what these checks exist to prevent.
 */

#include "PluginController.h"

#include "controller/internal/ControllerUtils.h"
#include "controller/internal/HostedPluginSupport.h"
#include "presets/PresetStorage.h"
#include "resources/ResourceLibrary.h"
#include "util/PathEncoding.h"

#include <algorithm>

using namespace guitarfx::controller_detail;

namespace guitarfx
{
void PluginController::AttachRuntimeConfigCallbacks(const std::string& presetId, const Preset& preset)
{
    if (presetId.empty())
    {
        return;
    }

    for (const auto& node : preset.graph.nodes)
    {
        if (auto* processor = mPresetMixer.GetNodeProcessor(presetId, node.id))
        {
            processor->SetRuntimeConfigChangedCallback(
                [this, presetId, nodeId = node.id](const std::string& key, const std::string& value) {
                    mHost.RunOnMainThread([this, presetId, nodeId, key, value]() {
                        HandleRuntimeNodeConfigChanged(presetId, nodeId, key, value);
                    });
                });
        }
    }
}

bool PluginController::ReportHostedPluginResourceLoadFailure(const std::string& nodeId, const ResourceRef& ref,
                                                             int resourceIndex)
{
    if (nodeId.empty() || ref.resourceType != "plugin" || !mActivePreset)
    {
        return false;
    }

    const std::string presetId = !mActivePresetId.empty() ? mActivePresetId : mActivePreset->id;

    if (presetId.empty())
    {
        return false;
    }

    auto* processor = mPresetMixer.GetNodeProcessor(presetId, nodeId);

    if (!processor)
    {
        return false;
    }

    const std::string lastError = processor->GetConfig("lastError");

    if (lastError.empty())
    {
        return false;
    }

    const std::string lastErrorCode = processor->GetConfig(kHostedPluginLastErrorCodeConfigKey);

    nlohmann::json message{{"type", "hostedPluginResourceLoadFailed"},
                           {"nodeId", nodeId},
                           {"resourceType", "plugin"},
                           {"message", lastError}};

    if (!lastErrorCode.empty())
    {
        message["errorCode"] = lastErrorCode;
    }

    if (resourceIndex >= 0)
    {
        message["resourceIndex"] = resourceIndex;
    }

    if (!ref.resourceId.empty())
    {
        message["resourceId"] = ref.resourceId;
    }

    if (!ref.filePath.empty())
    {
        message["filePath"] = util::PathToUtf8(ref.filePath);
    }
    else if (auto resolvedPath = ResolveResourceRef(ref))
    {
        message["filePath"] = util::PathToUtf8(*resolvedPath);
    }

    SendMessageToUI(message.dump());
    return true;
}

void PluginController::NotifyHostedPluginResourceLoadCompleted(const std::string& nodeId, const ResourceRef& ref,
                                                               int resourceIndex)
{
    if (nodeId.empty() || ref.resourceType != "plugin")
    {
        return;
    }

    nlohmann::json message{
        {"type", "hostedPluginResourceLoadCompleted"}, {"nodeId", nodeId}, {"resourceType", "plugin"}};

    if (resourceIndex >= 0)
    {
        message["resourceIndex"] = resourceIndex;
    }

    if (!ref.resourceId.empty())
    {
        message["resourceId"] = ref.resourceId;
    }

    SendMessageToUI(message.dump());
}

void PluginController::DiscardFailedHostedPluginResourceSelection(const std::string& nodeId, const ResourceRef& ref,
                                                                  int resourceIndex)
{
    if (nodeId.empty() || ref.resourceType != "plugin")
    {
        return;
    }

    if (!ref.resourceId.empty())
    {
        if (auto resource = mResourceLibrary.LookupResource("plugin", ref.resourceId))
        {
            const auto providerIt = resource->metadata.find("provider");
            const bool isLocalResource =
                providerIt != resource->metadata.end() && providerIt->second == kLocalResourceProvider;
            mResourceLibrary.RemoveResource("plugin", ref.resourceId);

            if (isLocalResource)
            {
                ResourceLibrary::RemoveFromStore(Store(), "plugin", ref.resourceId);
            }
        }
    }

    auto* targetGraph = ResolveEditTarget();
    auto* target = targetGraph ? targetGraph->FindNode(nodeId) : nullptr;

    if (!target)
    {
        return;
    }

    const auto clearResourceSlot = [](ResourceRef& slot) {
        slot = ResourceRef{};
        slot.resourceType = "plugin";
    };

    if (resourceIndex >= 0)
    {
        if (static_cast<std::size_t>(resourceIndex) < target->resources.size())
        {
            clearResourceSlot(target->resources[static_cast<std::size_t>(resourceIndex)]);
        }
    }
    else
    {
        for (auto& slot : target->resources)
        {
            if (slot.resourceType == "plugin")
            {
                clearResourceSlot(slot);
                break;
            }
        }
    }

    if (IsCompositeEditMode())
    {
        BroadcastCompositeEditState();
        return;
    }

    if (mActivePreset)
    {
        SyncActivePresetSceneGraph();
        ApplyPreset(*mActivePreset);
        mPendingStateBroadcast = true;
    }
}

void PluginController::HandleRuntimeNodeConfigChanged(const std::string& presetId, const std::string& nodeId,
                                                      const std::string& key, const std::string& value)
{
    if (presetId.empty() || nodeId.empty() || key.empty())
    {
        return;
    }

    // A slot that is not the editing focus has no live Preset object; its working copy is
    // the cached JSON. Dropping these updates (as this used to) meant that in a
    // multi-preset mix every hosted plugin except the focused one silently stopped
    // persisting — its edits existed only inside the running plugin.
    if (!mActivePresetId.empty() && presetId != mActivePresetId)
    {
        ApplyRuntimeNodeConfigToMixerCache(presetId, nodeId, key, value);
        return;
    }

    if (!mActivePreset || IsCompositeEditMode())
    {
        return;
    }

    auto* targetGraph = ResolveEditTarget();
    auto* node = targetGraph ? targetGraph->FindNode(nodeId) : nullptr;

    if (!node)
    {
        node = mActivePreset->graph.FindNode(nodeId);
    }

    if (!node)
    {
        return;
    }

    const auto existingIt = node->config.find(key);

    if ((value.empty() && existingIt == node->config.end()) ||
        (existingIt != node->config.end() && existingIt->second == value))
    {
        return;
    }

    if (value.empty())
    {
        node->config.erase(key);
    }
    else
    {
        node->config[key] = value;
    }

    PersistHostedPluginResourceMetadata(*node, key, value);

    SyncActivePresetSceneGraph();
    mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);
    mMixerPresetJsonCache[presetId] = mActivePresetJson;

    if (key == "pluginStateBase64")
    {
        SendMessageToUI(nlohmann::json{
            {"type", "signalPathNodeConfigUpdated"},
            {"presetId", presetId},
            {"nodeId", nodeId},
            {"key", key},
            {"captured", true},
            {"valueLength", value.size()},
            {"persist", true},
            {"dirty", true},
            {"silent",
             true}}.dump());
    }

    mPendingStateBroadcast = true;
    mHost.NotifyStateChanged();
}

void PluginController::TryRemapHostedPluginResources(Preset& preset) const
{
    TryRemapHostedPluginResourcesInGraph(preset.graph);

    for (auto& scene : preset.scenes)
    {
        TryRemapHostedPluginResourcesInGraph(scene.graph);
    }
}

void PluginController::TryRemapHostedPluginResourcesInGraph(SignalGraph& graph) const
{
    const auto pluginResources = mResourceLibrary.GetResourcesByType("plugin");

    if (pluginResources.empty())
    {
        return;
    }

    for (auto& node : graph.nodes)
    {
        if (EffectRegistry::Instance().Resolve(node.type) != EffectGuids::kPluginHost)
        {
            continue;
        }

        auto resourceIt = std::find_if(node.resources.begin(), node.resources.end(), [](const ResourceRef& ref) {
            return ref.resourceType == "plugin" && ref.IsLibraryRef();
        });

        if (resourceIt == node.resources.end())
        {
            continue;
        }

        if (mResourceLibrary.HasResource("plugin", resourceIt->resourceId))
        {
            continue;
        }

        const auto stableConfigIt = node.config.find(kHostedPluginStableIdConfigKey);
        const auto identifierConfigIt = node.config.find(kHostedPluginIdentifierConfigKey);
        const auto nameConfigIt = node.config.find(kHostedPluginNameConfigKey);
        const auto manufacturerConfigIt = node.config.find(kHostedPluginManufacturerConfigKey);
        const auto formatConfigIt = node.config.find(kHostedPluginFormatConfigKey);

        const std::string normalizedStableId = stableConfigIt != node.config.end()
                                                   ? NormalizeHostedPluginIdentityToken(stableConfigIt->second)
                                                   : std::string{};
        const std::string normalizedIdentifier = identifierConfigIt != node.config.end()
                                                     ? NormalizeHostedPluginIdentityToken(identifierConfigIt->second)
                                                     : std::string{};
        const std::string normalizedName = nameConfigIt != node.config.end()
                                               ? NormalizeHostedPluginIdentityToken(nameConfigIt->second)
                                               : std::string{};
        const std::string normalizedManufacturer =
            manufacturerConfigIt != node.config.end() ? NormalizeHostedPluginIdentityToken(manufacturerConfigIt->second)
                                                      : std::string{};
        const std::string normalizedFormat = formatConfigIt != node.config.end()
                                                 ? NormalizeHostedPluginIdentityToken(formatConfigIt->second)
                                                 : std::string{};

        if (normalizedStableId.empty() && normalizedIdentifier.empty() && normalizedName.empty())
        {
            continue;
        }

        std::vector<const LibraryResource*> candidates;
        candidates.reserve(pluginResources.size());

        for (const auto& libraryResource : pluginResources)
        {
            const auto& metadata = libraryResource.metadata;
            const std::string candidateStableId = NormalizeHostedPluginIdentityToken(
                metadata.contains(kHostedPluginStableIdConfigKey) ? metadata.at(kHostedPluginStableIdConfigKey)
                                                                  : std::string{});
            const std::string candidateIdentifier = NormalizeHostedPluginIdentityToken(
                metadata.contains(kHostedPluginIdentifierConfigKey) ? metadata.at(kHostedPluginIdentifierConfigKey)
                                                                    : std::string{});
            const std::string candidateName = NormalizeHostedPluginIdentityToken(
                metadata.contains(kHostedPluginNameConfigKey) ? metadata.at(kHostedPluginNameConfigKey)
                                                              : libraryResource.name);
            const std::string candidateManufacturer = NormalizeHostedPluginIdentityToken(
                metadata.contains(kHostedPluginManufacturerConfigKey) ? metadata.at(kHostedPluginManufacturerConfigKey)
                                                                      : std::string{});

            bool match = false;

            if (!normalizedStableId.empty() && !candidateStableId.empty() && normalizedStableId == candidateStableId)
            {
                match = true;
            }
            else if (!normalizedIdentifier.empty() && !candidateIdentifier.empty() &&
                     normalizedIdentifier == candidateIdentifier)
            {
                match = true;
            }
            else if (!normalizedName.empty() && normalizedName == candidateName)
            {
                if (normalizedManufacturer.empty() || candidateManufacturer.empty() ||
                    normalizedManufacturer == candidateManufacturer)
                {
                    match = true;
                }
            }

            if (match)
            {
                candidates.push_back(&libraryResource);
            }
        }

        if (candidates.empty())
        {
            continue;
        }

        const LibraryResource* selected = nullptr;

        if (candidates.size() == 1)
        {
            selected = candidates.front();
        }
        else if (!normalizedFormat.empty())
        {
            for (const auto* candidate : candidates)
            {
                const auto formatIt = candidate->metadata.find(kHostedPluginFormatConfigKey);

                if (formatIt != candidate->metadata.end() &&
                    NormalizeHostedPluginIdentityToken(formatIt->second) == normalizedFormat)
                {
                    selected = candidate;
                    break;
                }
            }
        }

        if (!selected)
        {
            continue;
        }

        AppendSessionLog("Hosted plugin resource remapped nodeId=" + node.id + ", missingId=" + resourceIt->resourceId +
                         ", resolvedId=" + selected->id);
        resourceIt->resourceId = selected->id;
        resourceIt->filePath.clear();
    }
}

void PluginController::PersistHostedPluginResourceMetadata(const GraphNode& node, const std::string& key,
                                                           const std::string& value)
{
    if (EffectRegistry::Instance().Resolve(node.type) != EffectGuids::kPluginHost)
    {
        return;
    }

    if (key != kHostedPluginStableIdConfigKey && key != kHostedPluginIdentifierConfigKey &&
        key != kHostedPluginNameConfigKey && key != kHostedPluginManufacturerConfigKey &&
        key != kHostedPluginFormatConfigKey)
    {
        return;
    }

    const auto resourceIt = std::find_if(node.resources.begin(), node.resources.end(), [](const ResourceRef& ref) {
        return ref.resourceType == "plugin" && ref.IsLibraryRef();
    });

    if (resourceIt == node.resources.end())
    {
        return;
    }

    auto resource = mResourceLibrary.LookupResource("plugin", resourceIt->resourceId);

    if (!resource)
    {
        return;
    }

    auto updated = *resource;
    const auto existingIt = updated.metadata.find(key);

    if (value.empty())
    {
        if (existingIt == updated.metadata.end())
        {
            return;
        }

        updated.metadata.erase(existingIt);
    }
    else
    {
        if (existingIt != updated.metadata.end() && existingIt->second == value)
        {
            return;
        }

        updated.metadata[key] = value;
    }

    mResourceLibrary.UpdateResource("plugin", updated.id, updated);
    ResourceLibrary::PutInStore(Store(), updated, ResolveResourcesRoot());
}

void PluginController::CaptureRuntimePluginStates(Preset& preset, const std::string& presetId) const
{
    // Only one scene is ever loaded into the DSP, so live runtime state belongs to that
    // scene and to the top-level graph that mirrors it — nowhere else. Stamping it into
    // every scene (as this used to) collapsed a multi-scene preset down to whichever
    // scene happened to be active at save time.
    const std::string liveSceneId = GetResolvedActiveSceneId();

    // Authoritative fallback for scenes that are not live, and for the live scene when the
    // runtime cannot answer. Scoped to the *matching* scene: reaching across scenes for
    // "the first non-empty value" is the same collapse by another route.
    const auto findStoredState = [&](const std::string& sceneId, const GraphNode& node) -> std::string {
        if (!mActivePreset)
        {
            return {};
        }

        const SignalGraph* graph = nullptr;

        if (sceneId.empty())
        {
            graph = &mActivePreset->graph;
        }
        else if (const auto* scene = FindPresetScene(*mActivePreset, sceneId))
        {
            graph = &scene->graph;
        }

        if (!graph)
        {
            return {};
        }

        const auto* storedNode = graph->FindNode(node.id);

        if (!storedNode || !HostedPluginIdentityMatches(*storedNode, node))
        {
            return {};
        }

        return GetHostedPluginNodeState(*storedNode);
    };

    const auto captureRuntimeState = [&](const std::string& nodeId) -> std::string {
        if (!presetId.empty())
        {
            if (auto state = mPresetMixer.GetNodeConfig(presetId, nodeId, kHostedPluginStateConfigKey); !state.empty())
            {
                return state;
            }
        }

        // The caller may be saving the focused preset under a different slot id (a
        // save-as in flight, or a preset that has not been re-applied yet).
        if (!mActivePresetId.empty() && mActivePresetId != presetId)
        {
            if (auto state = mPresetMixer.GetNodeConfig(mActivePresetId, nodeId, kHostedPluginStateConfigKey);
                !state.empty())
            {
                return state;
            }
        }

        return {};
    };

    // sceneId is empty for the top-level graph, which mirrors the live scene.
    const auto captureGraph = [&](SignalGraph& graph, const std::string& sceneId) {
        const bool isLiveGraph = sceneId.empty() || sceneId == liveSceneId;

        for (auto& node : graph.nodes)
        {
            if (!IsHostedPluginNode(node))
            {
                continue;
            }

            // The UI-facing scrub marker must never reach persisted data.
            node.config.erase(kHostedPluginStateLengthConfigKey);

            std::string state;
            std::string source;

            if (isLiveGraph)
            {
                state = captureRuntimeState(node.id);
                source = "runtime";
            }

            if (state.empty())
            {
                state = findStoredState(sceneId, node);
                source = "stored";
            }

            // Last resort: whatever the incoming preset already carried. Covers a brand-new
            // scene that has no counterpart in the working copy yet.
            if (state.empty())
            {
                state = GetHostedPluginNodeState(node);
                source = "payload";
            }

            if (!state.empty())
            {
                node.config[kHostedPluginStateConfigKey] = state;
                AppendSessionLog("Hosted plugin runtime state selected presetId=" +
                                 (presetId.empty() ? std::string{"<none>"} : presetId) +
                                 ", scene=" + (sceneId.empty() ? std::string{"<graph>"} : sceneId) +
                                 ", live=" + std::string{isLiveGraph ? "true" : "false"} + ", nodeId=" + node.id +
                                 ", source=" + source + ", length=" + std::to_string(state.size()) +
                                 ", hash=" + HashStringForLog(state));
            }
            else
            {
                // Nothing anywhere has state for this node: normalise the key away rather
                // than persisting an empty string. This can only erase a value that was
                // already empty, so a live plugin's state is never dropped here.
                node.config.erase(kHostedPluginStateConfigKey);
                AppendSessionLog("Hosted plugin state capture unavailable for node " + node.id +
                                 ", scene=" + (sceneId.empty() ? std::string{"<graph>"} : sceneId) +
                                 " while saving preset " + preset.id);
            }
        }
    };

    captureGraph(preset.graph, {});

    for (auto& scene : preset.scenes)
    {
        captureGraph(scene.graph, scene.id);
    }
}

void PluginController::RestoreStandaloneHostedPluginState(const std::string& json)
{
    // Standalone saves its full state on exit (StandalonePluginHolder::savePluginState) and
    // hands it straight back on the next launch, but DeserializeState deliberately ignores
    // all of it — so hosted plugin state that was never written to a preset file was being
    // captured, serialised, stored, and then thrown away. In a DAW the same state survives
    // in the project; standalone users simply lost it.
    //
    // Rather than restoring the whole snapshot, this grafts only the plugin state chunks
    // onto the preset startup already loaded from the store. The store stays authoritative
    // for the graph, so unsaved *graph* edits are still discarded — a user who did not save
    // does not get their edits resurrected. Only the opaque plugin chunk, which has nowhere
    // else to live and cannot be reconstructed, is carried across.
    if (!mActivePreset || mActivePresetId.empty())
    {
        return;
    }

    try
    {
        const auto state = nlohmann::json::parse(json);

        if (!state.contains("preset") || !state["preset"].is_object())
        {
            return;
        }

        // Only continue a session still pointing at the preset the store resolved at
        // startup. If they disagree, something else moved the last preset on (another
        // instance, a settings sync) and the snapshot is stale.
        const std::string sessionPresetId = state.value("presetId", std::string{});

        if (sessionPresetId.empty() || sessionPresetId != mActivePresetId)
        {
            return;
        }

        auto sessionPreset = PresetStorage::DeserializeFromJson(state["preset"].dump());

        if (!sessionPreset)
        {
            return;
        }

        NormalizePresetScenes(*sessionPreset);
        NormalizePresetScenes(*mActivePreset);

        int grafted = 0;
        const auto graftGraph = [&](SignalGraph& target, const SignalGraph* source) {
            if (!source)
            {
                return;
            }

            for (auto& node : target.nodes)
            {
                if (!IsHostedPluginNode(node))
                {
                    continue;
                }

                const auto* sessionNode = source->FindNode(node.id);

                if (!sessionNode)
                {
                    continue;
                }

                const auto sessionState = GetHostedPluginNodeState(*sessionNode);

                if (sessionState.empty() || sessionState == GetHostedPluginNodeState(node))
                {
                    continue;
                }

                // The stored preset may have been saved against a different plugin since.
                if (!HostedPluginIdentityMatches(*sessionNode, node))
                {
                    continue;
                }

                node.config[kHostedPluginStateConfigKey] = sessionState;
                node.config.erase(kHostedPluginStateLengthConfigKey);
                ++grafted;
            }
        };

        graftGraph(mActivePreset->graph, &sessionPreset->graph);

        for (auto& scene : mActivePreset->scenes)
        {
            const auto* sessionScene = FindPresetScene(*sessionPreset, scene.id);
            graftGraph(scene.graph, sessionScene ? &sessionScene->graph : nullptr);
        }

        if (grafted == 0)
        {
            return;
        }

        AppendSessionLog("Standalone session restored hosted plugin state presetId=" + mActivePresetId + ", nodes=" +
                         std::to_string(grafted) + ", state=" + SummarizeHostedPluginState(*mActivePreset));

        mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);
        ApplyPreset(*mActivePreset);
        mPendingStateBroadcast = true;
    }
    catch (const std::exception&)
    {
        // Malformed session state is not worth failing startup over; the store's preset
        // is already loaded and usable.
    }
}

void PluginController::CaptureLiveHostedPluginStateIntoActivePreset()
{
    if (!mActivePreset || mActivePresetId.empty() || IsCompositeEditMode())
    {
        return;
    }

    bool changed = false;

    // Reads the live processor for this exact node in this exact running slot, so the value
    // belongs to the node it is written back to by construction — no identity check needed.
    const auto foldGraph = [&](SignalGraph& graph) {
        for (auto& node : graph.nodes)
        {
            if (!IsHostedPluginNode(node))
            {
                continue;
            }

            auto state = mPresetMixer.GetNodeConfig(mActivePresetId, node.id, kHostedPluginStateConfigKey);

            if (state.empty() || GetHostedPluginNodeState(node) == state)
            {
                continue;
            }

            node.config[kHostedPluginStateConfigKey] = std::move(state);
            node.config.erase(kHostedPluginStateLengthConfigKey);
            changed = true;
        }
    };

    // The top-level graph mirrors the live scene; both carry the node the DSP is running.
    foldGraph(mActivePreset->graph);

    if (auto* liveScene = FindPresetScene(*mActivePreset, GetResolvedActiveSceneId()))
    {
        foldGraph(liveScene->graph);
    }

    if (!changed)
    {
        return;
    }

    mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);
    mMixerPresetJsonCache[mActivePresetId] = mActivePresetJson;
    AppendSessionLog("Hosted plugin live state folded into working copy presetId=" + mActivePresetId +
                     ", state=" + SummarizeHostedPluginState(*mActivePreset));
    mHost.NotifyStateChanged();
}

void PluginController::CaptureMixerSlotHostedPluginState(Preset& preset, const std::string& presetId) const
{
    if (presetId.empty())
    {
        return;
    }

    // A mixer slot runs exactly one graph: the preset's top-level `graph`. Unfocused slots
    // do not track which scene that graph came from, so the default scene — the one
    // FocusMixerPreset() will materialise when the user switches to this slot — is kept in
    // step with it. Other scenes keep whatever they already hold.
    const std::string defaultSceneId = GetDefaultPresetSceneId(preset);

    for (auto& node : preset.graph.nodes)
    {
        if (!IsHostedPluginNode(node))
        {
            continue;
        }

        auto state = mPresetMixer.GetNodeConfig(presetId, node.id, kHostedPluginStateConfigKey);

        if (state.empty())
        {
            continue;
        }

        node.config[kHostedPluginStateConfigKey] = state;
        node.config.erase(kHostedPluginStateLengthConfigKey);

        if (auto* scene = FindPresetScene(preset, defaultSceneId))
        {
            if (auto* sceneNode = scene->graph.FindNode(node.id);
                sceneNode && HostedPluginIdentityMatches(node, *sceneNode))
            {
                sceneNode->config[kHostedPluginStateConfigKey] = std::move(state);
                sceneNode->config.erase(kHostedPluginStateLengthConfigKey);
            }
        }
    }
}

void PluginController::ApplyRuntimeNodeConfigToMixerCache(const std::string& presetId, const std::string& nodeId,
                                                          const std::string& key, const std::string& value)
{
    const auto cachedIt = mMixerPresetJsonCache.find(presetId);

    if (cachedIt == mMixerPresetJsonCache.end())
    {
        return;
    }

    auto presetOpt = PresetStorage::DeserializeFromJson(cachedIt->second);

    if (!presetOpt)
    {
        return;
    }

    bool changed = false;
    const auto patchGraph = [&](SignalGraph& graph) {
        auto* node = graph.FindNode(nodeId);

        if (!node)
        {
            return;
        }

        const auto existingIt = node->config.find(key);

        if (value.empty())
        {
            if (existingIt == node->config.end())
            {
                return;
            }

            node->config.erase(existingIt);
        }
        else
        {
            if (existingIt != node->config.end() && existingIt->second == value)
            {
                return;
            }

            node->config[key] = value;
        }

        changed = true;
    };

    // See CaptureMixerSlotHostedPluginState for why the default scene tracks `graph`.
    patchGraph(presetOpt->graph);

    if (auto* scene = FindPresetScene(*presetOpt, GetDefaultPresetSceneId(*presetOpt)))
    {
        patchGraph(scene->graph);
    }

    if (!changed)
    {
        return;
    }

    cachedIt->second = PresetStorage::SerializeToJson(*presetOpt);
    mPendingStateBroadcast = true;
    mHost.NotifyStateChanged();
}

bool PluginController::ClearStaleHostedPluginState(GraphNode& node, const std::string& previousIdentity)
{
    if (!IsHostedPluginNode(node))
    {
        return false;
    }

    // Resource-keyed, not config-keyed: the config identity keys are written by the plugin
    // after it loads, so right after a swap they still name the outgoing plugin.
    if (HostedPluginResourceKey(node) == previousIdentity)
    {
        return false;
    }

    const bool hadState =
        node.config.count(kHostedPluginStateConfigKey) > 0 || node.config.count(kHostedPluginStateLengthConfigKey) > 0;

    if (!hadState)
    {
        return false;
    }

    // The node now points at a different plugin. Its predecessor's state chunk is foreign
    // data: at best the new plugin rejects it, at worst it half-applies it. Drop it, along
    // with the identity keys the previous plugin published — the new load republishes its
    // own via the runtime config callback.
    node.config.erase(kHostedPluginStateConfigKey);
    node.config.erase(kHostedPluginStateLengthConfigKey);
    node.config.erase(kHostedPluginStableIdConfigKey);
    node.config.erase(kHostedPluginIdentifierConfigKey);
    node.config.erase(kHostedPluginNameConfigKey);
    node.config.erase(kHostedPluginManufacturerConfigKey);
    node.config.erase(kHostedPluginFormatConfigKey);
    return true;
}
} // namespace guitarfx
