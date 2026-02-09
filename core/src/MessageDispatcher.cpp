/**
 * MessageDispatcher.cpp — Canonical message routing for all WebView messages.
 *
 * This is the single source of truth for all UI↔engine message types.
 * It merges ~50 message types from the iPlug2 version and ~30 from the
 * JUCE version into one unified dispatch table.
 */

#include "MessageDispatcher.h"
#include "PluginController.h"
#include "presets/PresetStorage.h"

#include <nlohmann/json.hpp>

namespace guitarfx
{

void MessageDispatcher::Dispatch(PluginController& c, const std::string& jsonMessage)
{
    nlohmann::json msg;
    try
    {
        msg = nlohmann::json::parse(jsonMessage);
    }
    catch (const std::exception&)
    {
        return; // Ignore malformed JSON
    }

    const std::string type = msg.value("type", "");
    if (type.empty()) return;

    // ── State & lifecycle ──────────────────────────────────────────
    if (type == "requestState" || type == "uiReady")
    {
        if (type == "uiReady")
            c.OnWebContentLoaded();
        c.HandleStateRequest();
    }
    else if (type == "getGlobalChain")
    {
        c.HandleGetGlobalChainRequest();
    }
    else if (type == "setGlobalChain")
    {
        c.HandleSetGlobalChainRequest(msg);
    }
    else if (type == "getEffectCatalog")
    {
        c.HandleGetEffectCatalogRequest();
    }
    else if (type == "getPresetList")
    {
        c.HandleGetPresetListRequest();
    }
    else if (type == "getSignalDiagnostics")
    {
        c.HandleGetSignalDiagnosticsRequest();
    }
    else if (type == "getPerformanceStats")
    {
        c.HandleGetPerformanceStatsRequest();
    }

    // ── Parameters ─────────────────────────────────────────────────
    else if (type == "setParameter")
    {
        c.HandleSetParameterRequest(msg);
    }
    else if (type == "setGlobalChainParam")
    {
        c.HandleSetGlobalChainParamRequest(msg);
    }

    // ── Settings ───────────────────────────────────────────────────
    else if (type == "setSetting")
    {
        // Inline: update a single app setting
        std::string key = msg.value("key", "");
        if (!key.empty() && msg.contains("value"))
        {
            c.mAppSettings[key] = msg["value"];
            c.SaveAppSettings();
        }
    }
    else if (type == "uiSettingsChanged")
    {
        // UI visual settings (zoom, theme) — store in app settings
        if (msg.contains("zoom"))
            c.mAppSettings["uiZoom"] = msg["zoom"];
        if (msg.contains("theme"))
            c.mAppSettings["theme"] = msg["theme"];
        c.SaveAppSettings();
    }
    else if (type == "uiVisibility")
    {
        // Notification that the UI is visible or hidden
        // No-op in the shared core; host may override.
    }

    // ── Preset management ──────────────────────────────────────────
    else if (type == "loadPreset")
    {
        c.HandlePresetLoadRequest(msg);
    }
    else if (type == "savePreset")
    {
        c.HandleSavePresetRequest(msg);
    }

    // ── Resource loading ───────────────────────────────────────────
    else if (type == "loadModel")
    {
        c.HandleLoadModelRequest(msg);
    }
    else if (type == "loadIR")
    {
        c.HandleLoadIRRequest(msg);
    }
    else if (type == "browseModel")
    {
        c.HandleBrowseModelRequest();
    }
    else if (type == "browseIR")
    {
        c.HandleBrowseIRRequest();
    }
    else if (type == "requestResourceData")
    {
        c.HandleRequestResourceDataRequest(msg);
    }

    // ── Signal path editing ────────────────────────────────────────
    else if (type == "updateSignalPathNodeParam")
    {
        c.HandleUpdateSignalPathNodeParamRequest(msg);
    }
    else if (type == "updateSignalPathNodeBypass")
    {
        c.HandleUpdateSignalPathNodeBypassRequest(msg);
    }
    else if (type == "updateNodeResource")
    {
        c.HandleUpdateNodeResourceRequest(msg);
    }
    else if (type == "browseNodeResource")
    {
        c.HandleBrowseNodeResourceRequest(msg);
    }
    else if (type == "addSignalPathNode")
    {
        c.HandleAddSignalPathNodeRequest(msg);
    }
    else if (type == "splitSignalPathEdge")
    {
        c.HandleSplitSignalPathEdgeRequest(msg);
    }
    else if (type == "collapseSignalPathSplit")
    {
        c.HandleCollapseSignalPathSplitRequest(msg);
    }
    else if (type == "replaceSignalPathNode")
    {
        c.HandleReplaceSignalPathNodeRequest(msg);
    }
    else if (type == "reorderSignalPathNode")
    {
        c.HandleReorderSignalPathNodeRequest(msg);
    }
    else if (type == "deleteSignalPathNode")
    {
        c.HandleDeleteSignalPathNodeRequest(msg);
    }

    // ── Node-level control (JUCE naming convention) ────────────────
    else if (type == "setNodeEnabled")
    {
        c.HandleSetNodeEnabledRequest(msg);
    }
    else if (type == "setNodeParam")
    {
        c.HandleSetNodeParamRequest(msg);
    }
    else if (type == "loadNodeResource")
    {
        c.HandleLoadNodeResourceRequest(msg);
    }

    // ── Multi-preset mixer ─────────────────────────────────────────
    else if (type == "addActivePreset")
    {
        try
        {
            auto presetJson = msg.value("preset", nlohmann::json());
            auto presetOpt = PresetStorage::DeserializeFromJson(presetJson.dump());
            if (presetOpt)
            {
                Preset preset = std::move(*presetOpt);
                std::string presetId = msg.value("presetId", preset.id);
                std::string name = msg.value("name", preset.name);
                c.AddActivePreset(preset, presetId, name);
            }
        }
        catch (...) {}
    }
    else if (type == "removeActivePreset" || type == "removePreset")
    {
        std::string presetId = msg.value("presetId", "");
        if (!presetId.empty())
            c.RemoveActivePreset(presetId);
    }
    else if (type == "setPresetMix")
    {
        std::string presetId = msg.value("presetId", "p1");
        double value = msg.contains("value") ? msg["value"].get<double>() : msg.value("mix", 1.0);
        c.SetActivePresetMix(presetId, value);
    }
    else if (type == "setPresetPan")
    {
        std::string presetId = msg.value("presetId", "p1");
        double value = msg.contains("value") ? msg["value"].get<double>() : msg.value("pan", 0.0);
        c.SetActivePresetPan(presetId, value);
    }
    else if (type == "setPresetMute")
    {
        std::string presetId = msg.value("presetId", "p1");
        bool mute = msg.contains("value") ? msg["value"].get<bool>() : msg.value("mute", false);
        c.SetActivePresetMute(presetId, mute);
    }
    else if (type == "setPresetSolo")
    {
        std::string presetId = msg.value("presetId", "p1");
        bool solo = msg.contains("value") ? msg["value"].get<bool>() : msg.value("solo", false);
        c.SetActivePresetSolo(presetId, solo);
    }
    else if (type == "setMasterGain")
    {
        double gain = msg.contains("gain") ? msg["gain"].get<double>() : msg.value("value", 1.0);
        c.SetMasterGain(gain);
    }
    else if (type == "setLimiterEnabled")
    {
        bool enabled = msg.value("enabled", false);
        c.SetLimiterEnabled(enabled);
    }

    // ── Diagnostics ────────────────────────────────────────────────
    else if (type == "setSignalDiagnosticsEnabled")
    {
        c.HandleSetSignalDiagnosticsEnabledRequest(msg);
    }
    else if (type == "runSignalPathTest")
    {
        c.HandleSignalTestRequest(msg);
    }

    // ── Tuner ──────────────────────────────────────────────────────
    else if (type == "tuner")
    {
        c.HandleTunerRequest(msg);
    }
    else if (type == "setTunerEnabled")
    {
        c.HandleSetTunerEnabledRequest(msg);
    }
    else if (type == "setTunerReference")
    {
        c.HandleSetTunerReferenceRequest(msg);
    }

    // ── Input/Output settings ──────────────────────────────────────
    else if (type == "setInputMode")
    {
        c.HandleSetInputModeRequest(msg);
    }
    else if (type == "setAmpCabState")
    {
        c.HandleSetAmpCabStateRequest(msg);
    }
    else if (type == "setAutoLevel")
    {
        c.HandleSetAutoLevelRequest(msg);
    }

    // ── Metronome ──────────────────────────────────────────────────
    else if (type == "setMetronome")
    {
        c.HandleSetMetronomeRequest(msg);
    }

    // ── Demo audio preview ─────────────────────────────────────────
    else if (type == "previewDemoAudio")
    {
        c.HandlePreviewDemoRequest(msg);
    }
    else if (type == "stopDemoAudio")
    {
        c.HandleStopDemoRequest();
    }

    // ── NAM calibration ────────────────────────────────────────────
    else if (type == "rerunNamCalibration")
    {
        c.HandleRerunNamCalibrationRequest(msg);
    }

    // ── Remote resources (Tone3000 integration) ────────────────────
    else if (type == "importRemoteResource")
    {
        c.HandleImportRemoteResourceRequest(msg);
    }
    else if (type == "previewRemoteResource")
    {
        c.HandlePreviewRemoteResourceRequest(msg);
    }
    else if (type == "cancelPreviewResource")
    {
        c.HandleCancelPreviewResourceRequest(msg);
    }

    // ── Blend definitions ──────────────────────────────────────────
    else if (type == "saveBlendDefinition")
    {
        c.HandleSaveBlendDefinitionRequest(msg);
    }
    else if (type == "saveBlendArchive")
    {
        c.HandleSaveBlendArchiveRequest(msg);
    }

    // ── Archive export ─────────────────────────────────────────────
    else if (type == "savePresetArchive")
    {
        c.HandleSavePresetArchiveRequest(msg);
    }
    else if (type == "saveLibraryArchive")
    {
        c.HandleSaveLibraryArchiveRequest(msg);
    }

    // ── Effect layouts ─────────────────────────────────────────────
    else if (type == "saveEffectLayout")
    {
        c.HandleSaveEffectLayoutRequest(msg);
    }
    else if (type == "exportEffectLayout")
    {
        c.HandleExportEffectLayoutRequest(msg);
    }
    else if (type == "browseLayoutImage")
    {
        c.HandleBrowseLayoutImageRequest(msg);
    }
    else if (type == "saveLayoutImage")
    {
        c.HandleSaveLayoutImageRequest(msg);
    }

    // ── Resource library management ────────────────────────────────
    else if (type == "cleanupResourceLibrary")
    {
        c.HandleCleanupResourceLibraryRequest(msg);
    }

    // ── Composite effects ──────────────────────────────────────────
    else if (type == "saveCompositeDefinition")
    {
        c.HandleSaveCompositeDefinitionRequest(msg);
    }
    else if (type == "deleteCompositeDefinition")
    {
        c.HandleDeleteCompositeDefinitionRequest(msg);
    }
    else if (type == "requestCompositeLibrary")
    {
        c.SendCompositeLibraryToUI();
    }
    else if (type == "enterCompositeEditMode")
    {
        c.HandleEnterCompositeEditModeRequest(msg);
    }
    else if (type == "exitCompositeEditMode")
    {
        c.HandleExitCompositeEditModeRequest(msg);
    }

    // ── Host-specific ──────────────────────────────────────────────
    else if (type == "openAudioPreferences")
    {
        c.HandleOpenAudioPreferencesRequest();
    }

    // Unknown message type — silently ignored
}

} // namespace guitarfx
