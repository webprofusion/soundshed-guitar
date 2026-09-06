#include "MessageDispatcher.h"
#include "PluginController.h"
#include "controller/TelemetryPublisher.h"

namespace guitarfx
{
bool MessageDispatcher::DispatchSettings(PluginController& c, const nlohmann::json& msg, const std::string& type)
{
    if (type == "setUserInputCalibrationTrainingActive")
    {
        c.mUserInputCalibrationTrainingActive = msg.value("active", false);
        c.ApplyUserInputCalibrationSettingsFromAppSettings();
        return true;
    }

    if (type == "setSetting")
    {
        const std::string key = msg.value("key", "");

        if (key.empty() || !msg.contains("value"))
        {
            return true;
        }

        // A null value means "unset"; anything else is the new value. Both then re-derive
        // whatever the key feeds, so the two cases only differ in this one line.
        if (msg["value"].is_null())
        {
            c.mAppSettings.erase(key);
        }
        else
        {
            c.mAppSettings[key] = msg["value"];
        }

        if (key == "audio.userInputCalibration.profiles" || key == "audio.userInputCalibration.activeProfileId" ||
            key == "audio.interfaceCalibration.enabled" || key == "audio.interfaceCalibration.referenceDbu")
        {
            c.ApplyUserInputCalibrationSettingsFromAppSettings();
        }

        if (key == "audio.dsp.nominalOperatingLevelDbfs" || key == "audio.dsp.outputProtectionCeilingDbfs" ||
            key == "audio.dsp.outputLimiterEnabled")
        {
            c.ApplyDspLevelTargetSettingsFromAppSettings();
            c.mPendingStateBroadcast = true;
        }

        if (PluginController::IsNamQualitySettingKey(key))
        {
            c.ApplyNamQualitySettings();
        }

        if (key == "audio.nam.interfaceCalibrationLevelDbu" || key == "audio.nam.autoInputCalibration")
        {
            c.ApplyNamInterfaceCalibrationFromAppSettings();
        }

        // No instance-owned check here: SaveAppSettings() filters those keys itself, so
        // the ownership rule has exactly one definition rather than one per call site.
        c.SaveAppSettings();
        return true;
    }

    if (type == "uiSettingsChanged")
    {
        if (msg.contains("settings") && msg["settings"].is_object())
        {
            c.mUiSettings = msg["settings"];
            c.mAppSettings["uiSettings"] = c.mUiSettings;

            // Legacy flattened aliases, kept so an older build reading this store still
            // finds the zoom and window rect. Only ApplyUiSettingsFromAppSettings' fallback
            // reads them back here.
            if (c.mUiSettings.contains("zoom"))
            {
                c.mAppSettings["uiZoom"] = c.mUiSettings["zoom"];
            }

            if (c.mUiSettings.contains("bounds"))
            {
                c.mAppSettings["uiBounds"] = c.mUiSettings["bounds"];
            }

            c.SaveAppSettings();
            c.mHost.NotifyStateChanged();
            return true;
        }

        if (msg.contains("zoom"))
        {
            c.mAppSettings["uiZoom"] = msg["zoom"];
        }

        if (msg.contains("theme"))
        {
            c.mAppSettings["theme"] = msg["theme"];
        }

        c.SaveAppSettings();
        c.mHost.NotifyStateChanged();
        return true;
    }

    if (type == "uiViewStateChanged")
    {
        if (msg.contains("viewState") && msg["viewState"].is_object())
        {
            c.mUiViewState = msg["viewState"];
            c.mHost.NotifyStateChanged();
            return true;
        }

        return true;
    }

    if (type == "uiVisibility")
    {
        // Gates the periodic telemetry feeds (signal diagnostics, DSP performance). Absent
        // or non-boolean means "assume visible" — never silently stop feeding a live UI.
        const bool visible = msg.contains("visible") && msg["visible"].is_boolean() ? msg["visible"].get<bool>() : true;
        c.mTelemetry->SetUiVisible(visible);
        // Suppressing the feeds is only half of it: with nobody reading them, the metering
        // and per-node timing that fill them are pure cost on the audio thread. Switch the
        // DSP off at the source too, so a hidden editor is genuinely free.
        c.mPresetMixer.SetSignalDiagnosticsEnabled(visible);
        return true;
    }

    return false;
}
} // namespace guitarfx
