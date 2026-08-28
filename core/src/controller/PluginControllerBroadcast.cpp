/**
 * PluginControllerBroadcast.cpp - Pushing engine state out to the UI.
 *
 * BroadcastState is the full snapshot; the Send*ToUI helpers are the targeted
 * updates. The rate-limited ones (diagnostics, performance, spatial position)
 * pair a Request* that marks work pending with a TrySendPending* that the idle
 * loop calls, so a fast-moving value cannot flood the IPC channel.
 */

#include "PluginController.h"

#include "controller/MetronomeService.h"

#include "dsp/effects/InputAnalyzerEffect.h"
#include "presets/CompositePresetStorage.h"

#include "controller/internal/ControllerUtils.h"
#include "controller/internal/HostedPluginSupport.h"
#include "controller/internal/PresetArchiveSupport.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "presets/PresetStorage.h"
#include "resources/ResourceLibrary.h"
#include "util/PathEncoding.h"

#include <algorithm>
#include <cmath>

using namespace guitarfx::controller_detail;

namespace guitarfx
{
void PluginController::SendCompositePresetListToUI()
{
    const auto presets = CompositePresetStorage::ListAllFromStore(Store());
    nlohmann::json msg;
    msg["type"] = "compositePresetList";
    nlohmann::json arr = nlohmann::json::array();

    for (const auto& cp : presets)
    {
        arr.push_back(nlohmann::json(cp));
    }

    msg["compositePresets"] = std::move(arr);
    SendMessageToUI(msg.dump());
}

void PluginController::BroadcastState(StateScope scope)
{
    if (!mUIReady)
    {
        return;
    }

    const bool full = scope == StateScope::Full;

    nlohmann::json state;
    state["type"] = "state";

    // Current preset
    if (mActivePreset)
    {
        SyncActivePresetSceneGraph();
        state["preset"] = SerializePresetForUi(*mActivePreset);
        state["activePresetId"] = mActivePresetId;
        state["activeSceneId"] = GetResolvedActiveSceneId();
    }

    // App settings — UI reads "appSettings". Applying them re-runs the demo-audio, recents,
    // tone-sharing and update-check passes and echoes an input-mode message back to us, so
    // this is deliberately not resent on a preset switch.
    if (full)
    {
        state["appSettings"] = mAppSettings;
    }

    // Always sent: the UI clears its archive-session state when this key is absent.
    state["presetArchiveSession"] = {{"active", IsPresetArchiveSessionActive()}};

    if (mPresetArchiveSession)
    {
        state["presetArchiveSession"]["archiveName"] = mPresetArchiveSession->archiveName;
        state["presetArchiveSession"]["archiveKey"] = mPresetArchiveSession->archiveKey;
        state["presetArchiveSession"]["presetCount"] = mPresetArchiveSession->presetCount;
    }

    // UI settings — UI reads "uiSettings"
    if (full)
    {
        state["uiSettings"] = mUiSettings;
    }

    // UI view state — UI reads "uiViewState"
    if (full)
    {
        state["uiViewState"] = mUiViewState;
    }

    // Global chain — UI reads "globalSignalChain". Always sent: the UI asks for it with a
    // round trip when the key is missing, which would cost more than including it.
    auto chainConfig = mPresetMixer.GetGlobalChainConfig();
    state["globalSignalChain"] = chainConfig;

    // Resource library summary + per-type entries for UI rendering. By far the largest
    // section, and it stats every file on disk to fill in "fileMissing", so it is only
    // built for a full broadcast.
    if (full)
    {
        nlohmann::json libraryInfo = nlohmann::json::object();
        auto allResources = mResourceLibrary.GetAllResources();
        libraryInfo["totalCount"] = allResources.size();

        for (const auto& resource : allResources)
        {
            const std::string type = resource.type;

            if (!libraryInfo.contains(type) || !libraryInfo[type].is_array())
            {
                libraryInfo[type] = nlohmann::json::array();
            }

            nlohmann::json entry;
            entry["id"] = resource.id;
            entry["name"] = resource.name;
            entry["category"] = resource.category;
            entry["description"] = resource.description;
            entry["tags"] = resource.tags;
            entry["filePath"] = resource.filePath.empty() ? "" : util::PathToUtf8(resource.filePath);
            entry["hash"] = resource.hash;

            if (!resource.metadata.empty())
            {
                entry["metadata"] = resource.metadata;
            }

            const bool hasPath = !resource.filePath.empty();
            const bool exists = hasPath && std::filesystem::exists(resource.filePath);
            entry["fileMissing"] = !(hasPath && exists);

            libraryInfo[type].push_back(entry);
        }

        state["resourceLibrary"] = std::move(libraryInfo);
    }

    // Active preset IDs — UI reads "activePresetIds" as string array
    nlohmann::json activePresetIds = nlohmann::json::array();

    for (const auto& id : mPresetMixer.GetActivePresetIds())
    {
        activePresetIds.push_back(id);
    }

    state["activePresetIds"] = activePresetIds;

    // Mixer snapshot
    nlohmann::json mixer = nlohmann::json::object();
    mixer["masterGain"] = mPresetMixer.GetMasterGain();
    mixer["limiterEnabled"] = mPresetMixer.IsLimiterEnabled();
    mixer["activePresetIds"] = activePresetIds;
    nlohmann::json presetConfigs = nlohmann::json::object();

    for (const auto& id : mPresetMixer.GetActivePresetIds())
    {
        if (const auto cfg = mPresetMixer.GetPresetConfig(id))
        {
            presetConfigs[id] = {
                {"name", cfg->name}, {"mix", cfg->mix}, {"pan", cfg->pan}, {"mute", cfg->mute}, {"solo", cfg->solo}};
        }
    }

    mixer["presets"] = std::move(presetConfigs);

    // Full preset graphs so the UI can display the signal chain for every mixer slot.
    nlohmann::json presetGraphs = nlohmann::json::object();

    for (const auto& id : mPresetMixer.GetActivePresetIds())
    {
        auto it = mMixerPresetJsonCache.find(id);

        if (it != mMixerPresetJsonCache.end())
        {
            try
            {
                presetGraphs[id] = nlohmann::json::parse(it->second);
            }
            catch (...)
            {
            }
        }
    }

    mixer["presetGraphs"] = std::move(presetGraphs);
    state["mixer"] = std::move(mixer);

    // Everything below is app-wide state that a preset or scene switch cannot change.
    if (full)
    {
        // Metronome
        nlohmann::json metronome;
        metronome["bpm"] = GetEffectiveTempoBpm();
        metronome["editable"] = mHost.IsStandalone();
        metronome["source"] = mHost.IsStandalone() ? "app" : "host";
        mMetronome->AppendStateTo(metronome);
        state["metronome"] = std::move(metronome);

        // Environment
        state["environment"] = {
            {"standalone", mHost.IsStandalone()},
            {"version", GUITARFX_APP_VERSION},
#if defined(_WIN32)
            {"os", "Windows"},
#elif defined(__APPLE__)
            {"os", "macOS"},
#elif defined(__linux__)
            {"os", "Linux"},
#else
            {"os", "Unknown"},
#endif
#if defined(__x86_64__) || defined(_M_X64)
            {"cpu", "x64"},
#elif defined(__aarch64__) || defined(_M_ARM64)
            {"cpu", "arm64"},
#else
            {"cpu", "Unknown"},
#endif
        };

        // Blend library
        state["blendLibrary"] = mBlendLibrary;

        // Saved custom effects library
        {
            nlohmann::json customEffects = nlohmann::json::array();

            for (const auto& entry : mCustomEffectLibrary.GetAllEntries())
            {
                customEffects.push_back(SerializeCustomEffectLibraryEntry(entry));
            }

            state["customEffectLibrary"] = std::move(customEffects);
        }

        // Riff library
        {
            std::lock_guard<std::mutex> riffLock(mRiffLibraryMutex);
            state["riffLibrary"] = mRiffLibraryIndex;
        }

        // Automation slots
        state["automation"] = mAutomationSlots.GetSlotsJson();
    }

    SendMessageToUI(state.dump());

    // Also send supplementary data. Both are static libraries, not preset state.
    if (full)
    {
        SendCompositeLibraryToUI();
        SendEffectCatalogToUI();
    }

    // Notify the host of any latency change now that the graph is settled.
    UpdateHostLatency();
}

void PluginController::BroadcastCompositeEditState()
{
    if (!mEditingComposite)
    {
        return;
    }

    nlohmann::json msg;
    msg["type"] = "compositeEditState";
    msg["compositeId"] = mEditingComposite->id;
    msg["name"] = mEditingComposite->name;
    msg["category"] = mEditingComposite->category;
    msg["description"] = mEditingComposite->description;
    msg["author"] = mEditingComposite->author;
    msg["tags"] = mEditingComposite->tags;
    msg["definition"] = SerializeCompositeEffectDefinition(*mEditingComposite);

    nlohmann::json graphJson;
    nlohmann::json nodesArr = nlohmann::json::array();

    for (const auto& node : mEditingComposite->innerGraph.nodes)
    {
        nlohmann::json nj;
        nj["id"] = node.id;
        nj["type"] = node.type;
        nj["displayName"] = node.label;
        nj["category"] = node.category;
        nj["bypassed"] = !node.enabled;
        nj["params"] = nlohmann::json::object();

        for (const auto& [k, v] : node.params)
        {
            nj["params"][k] = v;
        }

        nj["config"] = nlohmann::json::object();

        for (const auto& [k, v] : node.config)
        {
            nj["config"][k] = v;
        }

        if (!node.resources.empty())
        {
            nlohmann::json resArr = nlohmann::json::array();

            for (const auto& res : node.resources)
            {
                nlohmann::json rj;
                rj["resourceType"] = res.resourceType;
                rj["resourceId"] = res.resourceId;
                rj["filePath"] = res.filePath;
                rj["embeddedId"] = res.embeddedId;
                rj["parameterId"] = res.parameterId;

                if (res.parameterValue)
                {
                    rj["parameterValue"] = *res.parameterValue;
                }
                else
                {
                    rj["parameterValue"] = nullptr;
                }

                resArr.push_back(rj);
            }

            nj["resources"] = resArr;
        }

        nodesArr.push_back(nj);
    }

    graphJson["nodes"] = nodesArr;

    nlohmann::json edgesArr = nlohmann::json::array();

    for (const auto& edge : mEditingComposite->innerGraph.edges)
    {
        nlohmann::json ej;
        ej["from"] = edge.from;
        ej["to"] = edge.to;
        ej["fromPort"] = edge.fromPort;
        ej["toPort"] = edge.toPort;
        ej["gain"] = edge.gain;
        edgesArr.push_back(ej);
    }

    graphJson["edges"] = edgesArr;
    msg["graph"] = graphJson;

    SendMessageToUI(msg.dump());
}

void PluginController::SendGlobalChainStateToUI()
{
    nlohmann::json msg;
    msg["type"] = "globalChain";
    msg["config"] = mPresetMixer.GetGlobalChainConfig();
    SendMessageToUI(msg.dump());
}

void PluginController::SendCompositeLibraryToUI()
{
    nlohmann::json msg;
    msg["type"] = "compositeLibrary";
    nlohmann::json defs = nlohmann::json::array();

    for (const auto& def : mCompositeLibrary.GetAllDefinitions())
    {
        defs.push_back(SerializeCompositeEffectDefinition(def));
    }

    msg["definitions"] = defs;
    SendMessageToUI(msg.dump());
}

void PluginController::SendCustomEffectLibraryToUI()
{
    nlohmann::json msg;
    msg["type"] = "customEffectLibrary";
    nlohmann::json entries = nlohmann::json::array();

    for (const auto& entry : mCustomEffectLibrary.GetAllEntries())
    {
        entries.push_back(SerializeCustomEffectLibraryEntry(entry));
    }

    msg["entries"] = std::move(entries);
    SendMessageToUI(msg.dump());
}

void PluginController::SendEffectCatalogToUI()
{
    auto& registry = EffectRegistry::Instance();
    auto types = registry.GetAllTypes();

    nlohmann::json msg;
    msg["type"] = "effectCatalog";
    nlohmann::json catalog = nlohmann::json::array();

    for (const auto& info : types)
    {
        nlohmann::json entry;
        entry["type"] = info.type;
        entry["name"] = info.displayName;
        entry["category"] = info.category;
        entry["requiresResource"] = info.requiresResource;

        if (!info.resourceType.empty())
        {
            entry["resourceType"] = info.resourceType;
        }

        if (!info.resourceFilterHint.empty())
        {
            entry["resourceFilterHint"] = info.resourceFilterHint;
        }

        nlohmann::json params = nlohmann::json::array();

        for (const auto& p : info.parameters)
        {
            nlohmann::json param;
            param["key"] = p.id;
            param["name"] = p.displayName;
            param["min"] = p.minValue;
            param["max"] = p.maxValue;
            param["default"] = p.defaultValue;
            param["unit"] = p.unit;

            if (!p.group.empty())
            {
                param["group"] = p.group;
            }

            if (p.advanced)
            {
                param["advanced"] = true;
            }

            if (p.step != 0.0)
            {
                param["step"] = p.step;
            }

            if (!p.labels.empty())
            {
                param["labels"] = p.labels;
            }

            params.push_back(param);
        }

        entry["parameters"] = params;

        if (!info.presets.empty())
        {
            nlohmann::json presets = nlohmann::json::array();

            for (const auto& preset : info.presets)
            {
                presets.push_back({
                    {"id", preset.id},
                    {"name", preset.displayName},
                    {"source", preset.isFactory ? "factory" : "custom"},
                    {"parameters", preset.parameters},
                    {"parameterOrder", preset.parameterOrder},
                });
            }

            entry["presets"] = std::move(presets);
        }

        if (!info.exposedResources.empty())
        {
            nlohmann::json exposedResources = nlohmann::json::array();

            for (const auto& er : info.exposedResources)
            {
                nlohmann::json resource;
                resource["resourceId"] = er.resourceId;
                resource["displayName"] = er.displayName;
                resource["nodeId"] = er.nodeId;
                resource["resourceType"] = er.resourceType;
                resource["resourceIndex"] = er.resourceIndex;
                resource["allowBrowseFile"] = er.allowBrowseFile;

                if (!er.parameterId.empty())
                {
                    resource["parameterId"] = er.parameterId;
                }

                if (er.parameterValue.has_value())
                {
                    resource["parameterValue"] = *er.parameterValue;
                }

                exposedResources.push_back(resource);
            }

            entry["exposedResources"] = exposedResources;
        }
        else if (info.type.rfind("composite:", 0) == 0)
        {
            const std::string definitionId = info.type.substr(std::string("composite:").size());

            if (const auto* def = mCompositeLibrary.GetDefinition(definitionId))
            {
                nlohmann::json exposedResources = nlohmann::json::array();

                for (const auto& er : def->exposedResources)
                {
                    nlohmann::json resource;
                    resource["resourceId"] = er.resourceId;
                    resource["displayName"] = er.displayName;
                    resource["nodeId"] = er.nodeId;
                    resource["resourceType"] = er.resourceType;
                    resource["resourceIndex"] = er.resourceIndex;
                    resource["allowBrowseFile"] = er.allowBrowseFile;

                    if (!er.parameterId.empty())
                    {
                        resource["parameterId"] = er.parameterId;
                    }

                    if (er.parameterValue.has_value())
                    {
                        resource["parameterValue"] = *er.parameterValue;
                    }

                    exposedResources.push_back(resource);
                }

                entry["exposedResources"] = exposedResources;
            }
        }

        catalog.push_back(entry);
    }

    msg["catalog"] = catalog;
    SendMessageToUI(msg.dump());
}

void PluginController::SendPresetListToUI()
{
    // Scan preset directories and send list
    nlohmann::json msg;
    msg["type"] = "presetList";
    nlohmann::json presets = nlohmann::json::array();
    const bool factoryArchiveLoadingEnabled = IsFactoryPresetArchiveLoadingEnabled();
    const bool archiveSessionActive = IsPresetArchiveSessionActive();

    // Only factory presets are still scanned from disk; user presets come from
    // the store below (or, during an archive session, from the session sandbox
    // that LoadAllUserPresets() redirects to).
    auto factoryPath = ResolveFactoryPresetDirectory(mHost, mResourceRoot);

    auto scanDir = [&](const std::filesystem::path& dir, const std::string& source) {
        if (!std::filesystem::exists(dir))
        {
            return;
        }

        for (const auto& entry : std::filesystem::directory_iterator(dir))
        {
            if (entry.path().extension() == ".json")
            {
                try
                {
                    auto presetOpt = PresetStorage::LoadFromFile(entry.path());

                    if (!presetOpt)
                    {
                        continue;
                    }

                    auto& preset = *presetOpt;

                    if (!factoryArchiveLoadingEnabled && mTrackedFactoryArchivePresetIds.contains(preset.id))
                    {
                        continue;
                    }

                    nlohmann::json p;
                    p["id"] = preset.id;
                    p["name"] = preset.name;
                    p["category"] = preset.category;
                    p["source"] = mFactoryArchivePresetIds.contains(preset.id) ? "factory" : source;
                    presets.push_back(p);
                }
                catch (...)
                {
                }
            }
        }
    };

    if (!archiveSessionActive)
    {
        scanDir(factoryPath, "factory");
    }

    // User presets come from the store; only factory presets still ship as files.
    for (const auto& preset : LoadAllUserPresets())
    {
        if (!factoryArchiveLoadingEnabled && mTrackedFactoryArchivePresetIds.contains(preset.id))
        {
            continue;
        }

        nlohmann::json p;
        p["id"] = preset.id;
        p["name"] = preset.name;
        p["category"] = preset.category;
        p["source"] = mFactoryArchivePresetIds.contains(preset.id) ? "factory" : "user";
        presets.push_back(p);
    }

    std::unordered_set<std::string> seenPresetIds;

    for (const auto& preset : presets)
    {
        if (preset.is_object())
        {
            seenPresetIds.insert(preset.value("id", ""));
        }
    }

    for (const auto& [presetId, preset] : mFactoryArchivePresets)
    {
        if (archiveSessionActive || !factoryArchiveLoadingEnabled)
        {
            continue;
        }

        if (seenPresetIds.contains(presetId))
        {
            continue;
        }

        nlohmann::json p;
        p["id"] = preset.id;
        p["name"] = preset.name;
        p["category"] = preset.category;
        p["source"] = "factory";
        presets.push_back(p);
    }

    if (archiveSessionActive)
    {
        for (auto& preset : presets)
        {
            preset["source"] = "session";
        }
    }

    msg["presets"] = presets;
    SendMessageToUI(msg.dump());
}
} // namespace guitarfx
