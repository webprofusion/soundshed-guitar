#include "MessageDispatcher.h"
#include "PluginController.h"
#include "presets/PresetStorage.h"

namespace guitarfx
{

bool MessageDispatcher::DispatchMixerAndMonitoring(PluginController& c,
                                                   const nlohmann::json& msg,
                                                   const std::string& type)
{
    if (type == "addActivePreset")
    {
        try
        {
            auto presetJson = msg.value("preset", nlohmann::json());
            if (!presetJson.is_null() && presetJson.is_object())
            {
                auto presetOpt = PresetStorage::DeserializeFromJson(presetJson.dump());
                if (presetOpt)
                {
                    Preset preset = std::move(*presetOpt);
                    std::string presetId = msg.value("presetId", preset.id);
                    std::string name = msg.value("name", preset.name);
                    c.AddActivePreset(preset, presetId, name);
                }
            }
            else
            {
                std::string presetId = msg.value("presetId", "");
                if (!presetId.empty())
                {
                    c.AddActivePresetById(presetId);
                }
            }
        }
        catch (...) {}
        // Send updated mixer state (including presetGraphs) back to the UI
        // so it can display the signal chain for the newly-added slot.
        c.BroadcastState();
        return true;
    }
    if (type == "removeActivePreset" || type == "removePreset")
    {
        std::string presetId = msg.value("presetId", "");
        if (!presetId.empty())
            c.RemoveActivePreset(presetId);
        c.BroadcastState();
        return true;
    }
    if (type == "setPresetMix")
    {
        std::string presetId = msg.value("presetId", "p1");
        double value = msg.contains("value") ? msg["value"].get<double>() : msg.value("mix", 1.0);
        c.SetActivePresetMix(presetId, value);
        return true;
    }
    if (type == "setPresetPan")
    {
        std::string presetId = msg.value("presetId", "p1");
        double value = msg.contains("value") ? msg["value"].get<double>() : msg.value("pan", 0.0);
        c.SetActivePresetPan(presetId, value);
        return true;
    }
    if (type == "setPresetMute")
    {
        std::string presetId = msg.value("presetId", "p1");
        bool mute = msg.contains("value") ? msg["value"].get<bool>() : msg.value("mute", false);
        c.SetActivePresetMute(presetId, mute);
        return true;
    }
    if (type == "setPresetSolo")
    {
        std::string presetId = msg.value("presetId", "p1");
        bool solo = msg.contains("value") ? msg["value"].get<bool>() : msg.value("solo", false);
        c.SetActivePresetSolo(presetId, solo);
        return true;
    }
    if (type == "setMasterGain")
    {
        double gain = msg.contains("gain") ? msg["gain"].get<double>() : msg.value("value", 1.0);
        c.SetMasterGain(gain);
        return true;
    }
    if (type == "setLimiterEnabled")
    {
        bool enabled = msg.value("enabled", false);
        c.SetLimiterEnabled(enabled);
        return true;
    }
    if (type == "setSignalDiagnosticsEnabled")
    {
        c.HandleSetSignalDiagnosticsEnabledRequest(msg);
        return true;
    }
    if (type == "runSignalPathTest")
    {
        c.HandleSignalTestRequest(msg);
        return true;
    }
    if (type == "tuner")
    {
        c.HandleTunerRequest(msg);
        return true;
    }
    if (type == "setTunerEnabled")
    {
        c.HandleSetTunerEnabledRequest(msg);
        return true;
    }
    if (type == "setTunerReference")
    {
        c.HandleSetTunerReferenceRequest(msg);
        return true;
    }
    if (type == "setInputMode")
    {
        c.HandleSetInputModeRequest(msg);
        return true;
    }
    if (type == "setAmpCabState")
    {
        c.HandleSetAmpCabStateRequest(msg);
        return true;
    }
    if (type == "setAutoLevel")
    {
        c.HandleSetAutoLevelRequest(msg);
        return true;
    }
    if (type == "setMetronome")
    {
        c.HandleSetMetronomeRequest(msg);
        return true;
    }
    if (type == "previewDemoAudio")
    {
        c.HandlePreviewDemoRequest(msg);
        return true;
    }
    if (type == "stopDemoAudio")
    {
        c.HandleStopDemoRequest();
        return true;
    }
    if (type == "armRiffCapture")
    {
        c.HandleArmRiffCaptureRequest(msg);
        return true;
    }
    if (type == "startRiffCapture")
    {
        c.HandleStartRiffCaptureRequest(msg);
        return true;
    }
    if (type == "stopRiffCapture")
    {
        c.HandleStopRiffCaptureRequest(msg);
        return true;
    }
    if (type == "importRiffWav")
    {
        c.HandleImportRiffWavRequest(msg);
        return true;
    }
    if (type == "trimCapturedRiff")
    {
        c.HandleTrimCapturedRiffRequest(msg);
        return true;
    }
    if (type == "previewRiffTake")
    {
        c.HandlePreviewRiffTakeRequest(msg);
        return true;
    }
    if (type == "previewCapturedRiff")
    {
        c.HandlePreviewCapturedRiffRequest(msg);
        return true;
    }
    return false;
}

} // namespace guitarfx
