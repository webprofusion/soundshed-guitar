#include "MessageDispatcher.h"
#include "PluginController.h"

namespace guitarfx
{

bool MessageDispatcher::DispatchSettings(PluginController& c,
                                         const nlohmann::json& msg,
                                         const std::string& type)
{
    if (type == "setSetting")
    {
        std::string key = msg.value("key", "");
        if (!key.empty() && msg.contains("value"))
        {
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
                c.SaveAppSettings();
                return true;
            }
            c.mAppSettings[key] = msg["value"];
            if (key == "audio.interfaceCalibration.enabled"
                || key == "audio.interfaceCalibration.referenceDbu")
            {
                c.ApplyInterfaceCalibrationSettingsFromAppSettings();
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
