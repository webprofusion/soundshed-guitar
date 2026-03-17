#include "MessageDispatcher.h"
#include "PluginController.h"

namespace guitarfx
{

bool MessageDispatcher::DispatchLibraryAndComposite(PluginController& c,
                                                    const nlohmann::json& msg,
                                                    const std::string& type)
{
    if (type == "rerunNamCalibration")
    {
        c.HandleRerunNamCalibrationRequest(msg);
        return true;
    }
    if (type == "importRemoteResource")
    {
        c.HandleImportRemoteResourceRequest(msg);
        return true;
    }
    if (type == "saveLocalLibraryResource")
    {
        c.HandleSaveLocalLibraryResourceRequest(msg);
        return true;
    }
    if (type == "updateLibraryResource")
    {
        c.HandleUpdateLibraryResourceRequest(msg);
        return true;
    }
    if (type == "browseLibraryResourcePath")
    {
        c.HandleBrowseLibraryResourcePathRequest(msg);
        return true;
    }
    if (type == "importToneSharingPack")
    {
        c.HandleImportToneSharingPackRequest(msg);
        return true;
    }
    if (type == "deleteImportedToneSharingPack")
    {
        c.HandleDeleteImportedToneSharingPackRequest(msg);
        return true;
    }
    if (type == "previewRemoteResource")
    {
        c.HandlePreviewRemoteResourceRequest(msg);
        return true;
    }
    if (type == "cancelPreviewResource")
    {
        c.HandleCancelPreviewResourceRequest(msg);
        return true;
    }
    if (type == "saveBlendDefinition")
    {
        c.HandleSaveBlendDefinitionRequest(msg);
        return true;
    }
    if (type == "deleteBlendDefinition")
    {
        c.HandleDeleteBlendDefinitionRequest(msg);
        return true;
    }
    if (type == "saveBlendArchive")
    {
        c.HandleSaveBlendArchiveRequest(msg);
        return true;
    }
    if (type == "savePresetArchive")
    {
        c.HandleSavePresetArchiveRequest(msg);
        return true;
    }
    if (type == "saveLibraryArchive")
    {
        c.HandleSaveLibraryArchiveRequest(msg);
        return true;
    }
    if (type == "saveEffectLayout")
    {
        c.HandleSaveEffectLayoutRequest(msg);
        return true;
    }
    if (type == "deleteLayout")
    {
        c.HandleDeleteLayoutRequest(msg);
        return true;
    }
    if (type == "exportEffectLayout")
    {
        c.HandleExportEffectLayoutRequest(msg);
        return true;
    }
    if (type == "browseLayoutImage")
    {
        c.HandleBrowseLayoutImageRequest(msg);
        return true;
    }
    if (type == "saveLayoutImage")
    {
        c.HandleSaveLayoutImageRequest(msg);
        return true;
    }
    if (type == "cleanupResourceLibrary")
    {
        c.HandleCleanupResourceLibraryRequest(msg);
        return true;
    }
    if (type == "saveCompositeDefinition")
    {
        c.HandleSaveCompositeDefinitionRequest(msg);
        return true;
    }
    if (type == "deleteCompositeDefinition")
    {
        c.HandleDeleteCompositeDefinitionRequest(msg);
        return true;
    }
    if (type == "requestCompositeLibrary")
    {
        c.SendCompositeLibraryToUI();
        return true;
    }
    if (type == "enterCompositeEditMode")
    {
        c.HandleEnterCompositeEditModeRequest(msg);
        return true;
    }
    if (type == "exitCompositeEditMode")
    {
        c.HandleExitCompositeEditModeRequest(msg);
        return true;
    }
    if (type == "openAudioPreferences")
    {
        c.HandleOpenAudioPreferencesRequest();
        return true;
    }
    if (type == "saveRiffTake")
    {
        c.HandleSaveRiffTakeRequest(msg);
        return true;
    }
    if (type == "loadRiffTakeForEdit")
    {
        c.HandleLoadRiffTakeForEditRequest(msg);
        return true;
    }
    if (type == "deleteRiff")
    {
        c.HandleDeleteRiffRequest(msg);
        return true;
    }
    if (type == "setRiffFavorite")
    {
        c.HandleSetRiffFavoriteRequest(msg);
        return true;
    }
    if (type == "markRiffUsed")
    {
        c.HandleMarkRiffUsedRequest(msg);
        return true;
    }
    // ── Composite presets (Multi-Rig) ──────────────────────────────────────
    if (type == "saveCompositePreset")
    {
        c.HandleSaveCompositePresetRequest(msg);
        return true;
    }
    if (type == "loadCompositePreset")
    {
        c.HandleLoadCompositePresetRequest(msg);
        return true;
    }
    if (type == "getCompositePresetList")
    {
        c.HandleGetCompositePresetListRequest();
        return true;
    }
    if (type == "removeCompositePreset")
    {
        c.HandleRemoveCompositePresetRequest(msg);
        return true;
    }
    return false;
}

} // namespace guitarfx
