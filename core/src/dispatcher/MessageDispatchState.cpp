#include "MessageDispatcher.h"
#include "PluginController.h"

namespace guitarfx
{

bool MessageDispatcher::DispatchStateAndLists(PluginController& c,
                                              const nlohmann::json& msg,
                                              const std::string& type)
{
    if (type == "requestState" || type == "uiReady")
    {
        if (type == "uiReady")
            c.OnWebContentLoaded();
        c.HandleStateRequest();
        return true;
    }
    if (type == "captureDebugSnapshot")
    {
        c.HandleCaptureDebugSnapshotRequest(msg);
        return true;
    }
    if (type == "debugReportUiState")
    {
        c.HandleDebugReportUiStateRequest(msg);
        return true;
    }
    if (type == "getAppInfo")
    {
        c.HandleGetAppInfoRequest();
        return true;
    }
    if (type == "getGlobalChain")
    {
        c.HandleGetGlobalChainRequest();
        return true;
    }
    if (type == "setGlobalChain")
    {
        c.HandleSetGlobalChainRequest(msg);
        return true;
    }
    if (type == "getEffectCatalog")
    {
        c.HandleGetEffectCatalogRequest();
        return true;
    }
    if (type == "getPresetList")
    {
        c.HandleGetPresetListRequest();
        return true;
    }
    if (type == "getPresetById")
    {
        c.HandleGetPresetByIdRequest(msg);
        return true;
    }
    if (type == "getPresetFolders")
    {
        c.HandleGetPresetFoldersRequest();
        return true;
    }
    if (type == "setPresetFolders")
    {
        c.HandleSetPresetFoldersRequest(msg);
        return true;
    }
    if (type == "getPresetFavorites")
    {
        c.HandleGetPresetFavoritesRequest();
        return true;
    }
    if (type == "setPresetFavorites")
    {
        c.HandleSetPresetFavoritesRequest(msg);
        return true;
    }
    if (type == "getPresetRatings")
    {
        c.HandleGetPresetRatingsRequest();
        return true;
    }
    if (type == "setPresetRatings")
    {
        c.HandleSetPresetRatingsRequest(msg);
        return true;
    }
    if (type == "getSetlists")
    {
        c.HandleGetSetlistsRequest();
        return true;
    }
    if (type == "setSetlists")
    {
        c.HandleSetSetlistsRequest(msg);
        return true;
    }
    if (type == "getTheme")
    {
        c.HandleGetThemeRequest();
        return true;
    }
    if (type == "setTheme")
    {
        c.HandleSetThemeRequest(msg);
        return true;
    }
    if (type == "getSignalDiagnostics")
    {
        c.HandleGetSignalDiagnosticsRequest();
        return true;
    }
    if (type == "getPerformanceStats")
    {
        c.HandleGetPerformanceStatsRequest();
        return true;
    }
    if (type == "getRiffLibrary")
    {
        c.HandleGetRiffLibraryRequest();
        return true;
    }
    if (type == "setRiffLibraryPath")
    {
        c.HandleSetRiffLibraryPathRequest(msg);
        return true;
    }
    return false;
}

} // namespace guitarfx
