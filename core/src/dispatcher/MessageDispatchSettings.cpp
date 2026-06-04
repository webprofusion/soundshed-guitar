#include "MessageDispatcher.h"
#include "PluginController.h"

namespace guitarfx
{

bool MessageDispatcher::DispatchSettings(PluginController& c,
                                         const nlohmann::json& msg,
                                         const std::string& type)
{
    if (type == "setUserInputCalibrationTrainingActive")
    {
        c.mUserInputCalibrationTrainingActive = msg.value("active", false);
        c.ApplyUserInputCalibrationSettingsFromAppSettings();
        return true;
    }
    if (type == "setSetting")
    {
        std::string key = msg.value("key", "");
        if (!key.empty() && msg.contains("value"))
        {
            const bool affectsUserInputCalibration =
                key == "audio.userInputCalibration.profiles"
                || key == "audio.userInputCalibration.activeProfileId"
                || key == "audio.interfaceCalibration.enabled"
                || key == "audio.interfaceCalibration.referenceDbu";
            const bool affectsDspLevelTargets =
                key == "audio.dsp.nominalOperatingLevelDbfs"
                || key == "audio.dsp.outputProtectionCeilingDbfs";
            const bool affectsNamSlimmable = key == "audio.nam.slimmableSize";

            if (key == "diagnostics.signalLevelsEnabled")
            {
                nlohmann::json payload;
                payload["enabled"] = true;
                c.HandleSetSignalDiagnosticsEnabledRequest(payload);
                return true;
            }
            if (msg["value"].is_null())
            {
                c.mAppSettings.erase(key);
                if (affectsUserInputCalibration)
                    c.ApplyUserInputCalibrationSettingsFromAppSettings();
                if (affectsDspLevelTargets)
                {
                    c.ApplyDspLevelTargetSettingsFromAppSettings();
                    c.mPendingStateBroadcast = true;
                }
                if (affectsNamSlimmable)
                {
                    c.ApplyNamSlimmableSettingsFromAppSettings();
                    c.mPendingStateBroadcast = true;
                }
                c.SaveAppSettings();
                return true;
            }
            c.mAppSettings[key] = msg["value"];
            if (affectsUserInputCalibration)
                c.ApplyUserInputCalibrationSettingsFromAppSettings();
            if (affectsDspLevelTargets)
            {
                c.ApplyDspLevelTargetSettingsFromAppSettings();
                c.mPendingStateBroadcast = true;
            }
            if (affectsNamSlimmable)
            {
                c.ApplyNamSlimmableSettingsFromAppSettings();
                c.mPendingStateBroadcast = true;
            }
            c.SaveAppSettings();
        }
        return true;
    }
    if (type == "uiSettingsChanged")
    {
        if (msg.contains("settings") && msg["settings"].is_object())
        {
            c.mUiSettings = msg["settings"];
            c.mAppSettings["uiSettings"] = c.mUiSettings;
            if (c.mUiSettings.contains("zoom"))
                c.mAppSettings["uiZoom"] = c.mUiSettings["zoom"];
            if (c.mUiSettings.contains("bounds"))
                c.mAppSettings["uiBounds"] = c.mUiSettings["bounds"];
            c.SaveAppSettings();
            c.mHost.NotifyStateChanged();
            return true;
        }
        if (msg.contains("zoom"))
            c.mAppSettings["uiZoom"] = msg["zoom"];
        if (msg.contains("theme"))
            c.mAppSettings["theme"] = msg["theme"];
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
        return true;
    }
    return false;
}

} // namespace guitarfx
