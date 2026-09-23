#include "MessageDispatcher.h"
#include "PluginController.h"

namespace guitarfx
{
// The engine-owned edits of controller/PluginControllerPresetEdits.cpp: what every UI asks for
// by name instead of sending an edited copy back whole.
bool MessageDispatcher::DispatchPresetEdits(PluginController& c, const nlohmann::json& msg, const std::string& type)
{
    if (type == "selectScene")
    {
        c.HandleSelectSceneRequest(msg);
        return true;
    }

    if (type == "addScene")
    {
        c.HandleAddSceneRequest(msg);
        return true;
    }

    if (type == "renameScene")
    {
        c.HandleRenameSceneRequest(msg);
        return true;
    }

    if (type == "removeScene")
    {
        c.HandleRemoveSceneRequest(msg);
        return true;
    }

    if (type == "newPreset")
    {
        c.HandleNewPresetRequest(msg);
        return true;
    }

    if (type == "setPresetFavorite")
    {
        c.HandleSetPresetFavoriteRequest(msg);
        return true;
    }

    if (type == "setPresetRating")
    {
        c.HandleSetPresetRatingRequest(msg);
        return true;
    }

    if (type == "getPresetRecents")
    {
        c.HandleGetPresetRecentsRequest();
        return true;
    }

    if (type == "selectSetlist")
    {
        c.HandleSelectSetlistRequest(msg);
        return true;
    }

    if (type == "setResourceFavorite")
    {
        c.HandleSetResourceFavoriteRequest(msg);
        return true;
    }

    if (type == "setOutputMuted")
    {
        c.HandleSetOutputMutedRequest(msg);
        return true;
    }

    return false;
}
} // namespace guitarfx
