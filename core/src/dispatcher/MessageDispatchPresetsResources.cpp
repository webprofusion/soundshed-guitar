#include "MessageDispatcher.h"
#include "PluginController.h"

namespace guitarfx
{

bool MessageDispatcher::DispatchPresetsAndResources(PluginController& c, const nlohmann::json& msg,
                                                    const std::string& type)
{
    if (type == "loadPreset")
    {
        c.HandlePresetLoadRequest(msg);
        return true;
    }
    if (type == "savePreset")
    {
        c.HandleSavePresetRequest(msg);
        return true;
    }
    if (type == "deletePreset")
    {
        c.HandleDeletePresetRequest(msg);
        return true;
    }
    if (type == "startPresetArchiveSession")
    {
        c.HandleStartPresetArchiveSessionRequest(msg);
        return true;
    }
    if (type == "endPresetArchiveSession")
    {
        c.HandleEndPresetArchiveSessionRequest();
        return true;
    }
    if (type == "loadModel")
    {
        c.HandleLoadModelRequest(msg);
        return true;
    }
    if (type == "loadIR")
    {
        c.HandleLoadIRRequest(msg);
        return true;
    }
    if (type == "browseModel")
    {
        c.HandleBrowseModelRequest();
        return true;
    }
    if (type == "browseIR")
    {
        c.HandleBrowseIRRequest();
        return true;
    }
    if (type == "requestResourceData")
    {
        c.HandleRequestResourceDataRequest(msg);
        return true;
    }
    if (type == "browseResourceFolder")
    {
        c.HandleBrowseResourceFolderRequest();
        return true;
    }
    if (type == "listResourceFolder")
    {
        c.HandleListResourceFolderRequest(msg);
        return true;
    }
    return false;
}

} // namespace guitarfx
