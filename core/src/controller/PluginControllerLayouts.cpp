/**
 * PluginControllerLayouts.cpp - Custom effect layouts and their images.
 *
 * A layout is an index entry in one shared settings file plus a content
 * directory holding its layout.json and images. Deleting one has to remove
 * both halves, and saving one has to copy any newly referenced image into the
 * content directory before the index points at it.
 */

#include "PluginController.h"

#include "controller/internal/ControllerUtils.h"
#include "controller/internal/LayoutSupport.h"
#include "util/Base64.h"

#include <algorithm>
#include <fstream>

using namespace guitarfx::controller_detail;

namespace guitarfx
{
void PluginController::HandleDeleteLayoutRequest(const nlohmann::json& payload)
{
    const std::string effectType = payload.value("effectType", "");
    const std::string blendId = payload.value("blendId", "");
    const std::string layoutId = payload.value("layoutId", "");

    if (effectType.empty())
    {
        ReportErrorToUI("Delete layout failed", "Missing effect type");
        return;
    }

    if (layoutId.empty())
    {
        ReportErrorToUI("Delete layout failed", "Missing layoutId");
        return;
    }

    const std::string lookupKey = blendId.empty() ? effectType : (effectType + "::" + blendId);

    Store().Remove(storage::ItemType::kLayout, layoutId);

    // The document is in the store now, but the image sidecars are still files.
    const auto layoutDir = ResolveLayoutDir(mFileSystem, layoutId);
    std::error_code ec;

    if (std::filesystem::exists(layoutDir, ec))
    {
        std::filesystem::remove_all(layoutDir, ec);

        if (ec)
        {
            ReportErrorToUI("Delete layout failed", "Unable to remove the layout's image folder");
            return;
        }
    }

    AppendSessionLog("Layout deleted: " + layoutId);

    // Update associations mapping
    nlohmann::json settings = LoadEffectLayoutsSettings(mFileSystem);

    if (settings.contains("associations") && settings["associations"].is_object())
    {
        auto& assoc = settings["associations"];

        if (assoc.contains(lookupKey) && assoc[lookupKey].is_object())
        {
            auto& entry = assoc[lookupKey];
            auto ids = entry.value("layoutIds", nlohmann::json::array());

            if (!ids.is_array())
            {
                ids = nlohmann::json::array();
            }

            nlohmann::json updated = nlohmann::json::array();

            for (const auto& id : ids)
            {
                if (id.is_string() && id.get<std::string>() == layoutId)
                {
                    continue;
                }

                updated.push_back(id);
            }

            entry["layoutIds"] = updated;

            const std::string currentDefault = entry.value("defaultLayoutId", "");

            if (currentDefault == layoutId)
            {
                if (!updated.empty() && updated[0].is_string())
                {
                    entry["defaultLayoutId"] = updated[0].get<std::string>();
                }
                else
                {
                    entry["defaultLayoutId"] = "";
                }
            }

            // Remove empty association entries
            if (entry.value("layoutIds", nlohmann::json::array()).empty())
            {
                assoc.erase(lookupKey);
            }
        }
    }

    SaveEffectLayoutsSettings(mFileSystem, settings);

    LoadLayoutLibrary();
}

void PluginController::HandleSaveEffectLayoutRequest(const nlohmann::json& payload)
{
    const std::string effectType = payload.value("effectType", "");
    const std::string blendId = payload.value("blendId", "");
    std::string layoutId = payload.value("layoutId", "");
    const bool isNewLayout = payload.value("isNewLayout", false);
    const auto layoutIt = payload.find("layout");

    if (effectType.empty() || layoutIt == payload.end() || !layoutIt->is_object())
    {
        ReportErrorToUI("Save layout failed", "Missing effect type or layout data");
        return;
    }

    const std::string lookupKey = blendId.empty() ? effectType : (effectType + "::" + blendId);

    if (layoutId.empty())
    {
        layoutId = GenerateGuidV4String();
    }

    // Persist layout JSON in its own subdirectory.
    nlohmann::json layoutJson = *layoutIt;
    layoutJson["layoutId"] = layoutId;
    SaveLayoutToFile(layoutId, layoutJson);

    // When saving a new layout (first time or forked from factory), copy any referenced
    // images from wherever they currently live into this layout's images/ directory so
    // the layout is self-contained.
    if (isNewLayout)
    {
        const auto destImagesDir = ResolveLayoutDir(mFileSystem, layoutId) / "images";
        [[maybe_unused]] const auto ensuredDest = mFileSystem.EnsureDirectory(destImagesDir);

        const auto referencedIt = payload.find("referencedImageIds");

        if (referencedIt != payload.end() && referencedIt->is_array())
        {
            const auto settingsDir = mFileSystem.ResolveSettingsDirectory();
            const auto userLayoutsRoot = settingsDir / "layouts" / "content";

            for (const auto& idVal : *referencedIt)
            {
                if (!idVal.is_string())
                {
                    continue;
                }

                const std::string imageId = idVal.get<std::string>();

                // Search: first in other user layout image dirs, then legacy fallback dir.
                std::filesystem::path sourcePath;
                std::error_code ec;

                if (std::filesystem::exists(userLayoutsRoot, ec))
                {
                    for (const auto& layoutDir : std::filesystem::directory_iterator(userLayoutsRoot, ec))
                    {
                        if (!layoutDir.is_directory())
                        {
                            continue;
                        }

                        const auto imagesDir = layoutDir.path() / "images";

                        if (!std::filesystem::exists(imagesDir))
                        {
                            continue;
                        }

                        for (const auto& imgEntry : std::filesystem::directory_iterator(imagesDir, ec))
                        {
                            if (!imgEntry.is_regular_file())
                            {
                                continue;
                            }

                            if (imgEntry.path().stem().string() == imageId)
                            {
                                sourcePath = imgEntry.path();
                                break;
                            }
                        }

                        if (!sourcePath.empty())
                        {
                            break;
                        }
                    }
                }

                if (sourcePath.empty())
                {
                    continue; // image not found, skip
                }

                const auto destPath = destImagesDir / sourcePath.filename();

                if (destPath != sourcePath)
                {
                    std::filesystem::copy_file(sourcePath, destPath, std::filesystem::copy_options::overwrite_existing,
                                               ec);

                    if (ec)
                    {
                        AppendSessionLog("Failed to copy layout image " + sourcePath.generic_string() + ": " +
                                         ec.message());
                    }
                }
            }
        }
    }

    // Update association mapping
    nlohmann::json settings = LoadEffectLayoutsSettings(mFileSystem);

    if (!settings.contains("associations") || !settings["associations"].is_object())
    {
        settings["associations"] = nlohmann::json::object();
    }

    if (!settings["associations"].contains(lookupKey) || !settings["associations"][lookupKey].is_object())
    {
        settings["associations"][lookupKey] =
            nlohmann::json::object({{"defaultLayoutId", layoutId}, {"layoutIds", nlohmann::json::array()}});
    }

    auto& assocEntry = settings["associations"][lookupKey];
    auto ids = assocEntry.value("layoutIds", nlohmann::json::array());

    if (!ids.is_array())
    {
        ids = nlohmann::json::array();
    }

    bool found = false;

    for (const auto& id : ids)
    {
        if (id.is_string() && id.get<std::string>() == layoutId)
        {
            found = true;
            break;
        }
    }

    if (!found)
    {
        ids.push_back(layoutId);
    }

    assocEntry["layoutIds"] = ids;
    assocEntry["defaultLayoutId"] = layoutId;

    SaveEffectLayoutsSettings(mFileSystem, settings);

    SendMessageToUI(nlohmann::json{
        {"type", "layoutSaved"},
        {"effectType", effectType},
        {"blendId", blendId},
        {"lookupKey", lookupKey},
        {"layoutId", layoutId},
        {"layout",
         layoutJson}}.dump());

    AppendSessionLog("Effect layout saved: " + lookupKey + " -> " + layoutId);

    // Broadcast updated library so UI can select/apply immediately.
    LoadLayoutLibrary();
}

void PluginController::HandleExportEffectLayoutRequest(const nlohmann::json& payload)
{
    const std::string dataEncoded = payload.value("data", "");
    const std::string suggestedName = payload.value("fileName", "layout.sgfxlayout.zip");

    if (dataEncoded.empty())
    {
        SendMessageToUI(nlohmann::json{{"type", "layoutExportFailed"}, {"message", "Missing export data"}}.dump());
        return;
    }

    mHost.SaveFileAsync(
        BrowseFileType::ArchiveFile, "Export Effect Layout", suggestedName,
        [this, dataEncoded](const BrowseFileResult& result) {
            if (!result.success)
            {
                SendMessageToUI(nlohmann::json{{"type", "layoutExportFailed"}, {"message", "Export cancelled"}}.dump());
                return;
            }

            const auto decodedBytes = util::DecodeBase64(dataEncoded);

            if (decodedBytes.empty())
            {
                SendMessageToUI(
                    nlohmann::json{{"type", "layoutExportFailed"}, {"message", "Invalid export data"}}.dump());
                return;
            }

            if (!WriteFile(result.path, decodedBytes))
            {
                SendMessageToUI(
                    nlohmann::json{{"type", "layoutExportFailed"}, {"message", "Failed to write file"}}.dump());
                return;
            }

            SendMessageToUI(
                nlohmann::json{{"type", "layoutExportSaved"}, {"path", result.path.generic_string()}}.dump());
            AppendSessionLog("Layout exported: " + result.path.generic_string());
        });
}

void PluginController::HandleBrowseLayoutImageRequest(const nlohmann::json& payload)
{
    const std::string purpose = payload.value("purpose", "");
    const int layerIndex = payload.value("layerIndex", 0);
    const std::string paramKey = payload.value("paramKey", "");
    const std::string layoutId = payload.value("layoutId", "");

    mHost.BrowseFileAsync(
        BrowseFileType::ImageFile, "Select Image",
        [this, purpose, layerIndex, paramKey, layoutId](const BrowseFileResult& result) {
            if (!result.success)
            {
                return;
            }

            const auto imagesDir = layoutId.empty()
                                       ? mFileSystem.ResolveSettingsDirectory() / "layouts" / "content" / "images"
                                       : ResolveLayoutDir(mFileSystem, layoutId) / "images";
            [[maybe_unused]] const auto ensuredImagesDir = mFileSystem.EnsureDirectory(imagesDir);

            const auto selectedPath = result.path;
            const auto timestamp = std::chrono::system_clock::now().time_since_epoch().count();
            const std::string imageId = selectedPath.stem().string() + "_" + std::to_string(timestamp);
            const std::string destFilename = imageId + selectedPath.extension().string();
            const auto destPath = imagesDir / destFilename;

            try
            {
                std::filesystem::copy_file(selectedPath, destPath, std::filesystem::copy_options::overwrite_existing);

                std::ifstream imageFile(destPath, std::ios::binary);

                if (!imageFile)
                {
                    ReportErrorToUI("Image import failed", "Failed to read copied image file");
                    return;
                }

                std::vector<std::uint8_t> imageData((std::istreambuf_iterator<char>(imageFile)),
                                                    std::istreambuf_iterator<char>());
                imageFile.close();

                const std::string base64Data = util::EncodeBase64(imageData);
                std::string mimeType = "image/png";
                const auto ext = selectedPath.extension().string();

                if (ext == ".jpg" || ext == ".jpeg")
                {
                    mimeType = "image/jpeg";
                }

                const std::string dataUrl = "data:" + mimeType + ";base64," + base64Data;

                SendMessageToUI(nlohmann::json{
                    {"type", "layoutImageSelected"},
                    {"purpose", purpose},
                    {"imageId", imageId},
                    {"fileName", destFilename},
                    {"dataUrl", dataUrl},
                    {"layerIndex", layerIndex},
                    {"paramKey",
                     paramKey}}.dump());
            }
            catch (const std::exception& e)
            {
                AppendSessionLog("Failed to copy layout image: " + std::string(e.what()));
                ReportErrorToUI("Image import failed", "Failed to copy image file");
            }
        });
}

void PluginController::HandleSaveLayoutImageRequest(const nlohmann::json& payload)
{
    const std::string imageId = payload.value("imageId", "");
    const std::string fileName = payload.value("fileName", "");
    const std::string dataEncoded = payload.value("data", "");
    const std::string layoutId = payload.value("layoutId", "");

    if (imageId.empty() || fileName.empty() || dataEncoded.empty())
    {
        AppendSessionLog("SaveLayoutImage: missing required fields");
        return;
    }

    const auto imagesDir = layoutId.empty() ? mFileSystem.ResolveSettingsDirectory() / "layouts" / "content" / "images"
                                            : ResolveLayoutDir(mFileSystem, layoutId) / "images";
    [[maybe_unused]] const auto ensuredImagesDir = mFileSystem.EnsureDirectory(imagesDir);

    const auto decodedBytes = util::DecodeBase64(dataEncoded);

    if (decodedBytes.empty())
    {
        AppendSessionLog("SaveLayoutImage: failed to decode base64 data for " + imageId);
        return;
    }

    const auto destPath = imagesDir / fileName;

    if (WriteFile(destPath, decodedBytes))
    {
        AppendSessionLog("Layout image saved from import: " + destPath.generic_string());
    }
    else
    {
        AppendSessionLog("SaveLayoutImage: failed to write " + destPath.generic_string());
    }
}

void PluginController::LoadLayoutLibrary()
{
    nlohmann::json library;
    library["byEffectType"] = nlohmann::json::object();
    library["defaults"] = nlohmann::json::object();
    // Layout images (base64-encoded backgrounds) are intentionally omitted here to keep
    // app startup lightweight; they are loaded on demand via BuildLayoutImages() when the
    // layout designer/manager requests them. Per-layout thumbnailDataUrl values remain
    // embedded in each layout entry below since they are used for signal-path node avatars.
    library["images"] = nlohmann::json::array();

    // Build user layout library from the associations index.
    // Each layout lives in its own subfolder: layouts/content/<layoutId>/layout.json
    // Images live in: layouts/content/<layoutId>/images/
    nlohmann::json settings = LoadEffectLayoutsSettings(mFileSystem);

    if (settings.contains("associations") && settings["associations"].is_object())
    {
        for (auto it = settings["associations"].begin(); it != settings["associations"].end(); ++it)
        {
            const std::string lookupKey = it.key();
            const auto& assocEntry = it.value();

            if (!assocEntry.is_object())
            {
                continue;
            }

            const std::string defaultLayoutId = assocEntry.value("defaultLayoutId", "");
            const auto ids = assocEntry.value("layoutIds", nlohmann::json::array());

            if (!ids.is_array())
            {
                continue;
            }

            nlohmann::json entries = nlohmann::json::array();

            for (const auto& id : ids)
            {
                if (!id.is_string())
                {
                    continue;
                }

                const std::string layoutId = id.get<std::string>();
                auto layoutJson = Store().Get(storage::ItemType::kLayout, layoutId);

                if (!layoutJson || !layoutJson->is_object())
                {
                    continue;
                }

                // Ensure layoutId is embedded for UI round-trip.
                (*layoutJson)["layoutId"] = layoutId;

                nlohmann::json layoutEntry;
                layoutEntry["layout"] = std::move(*layoutJson);
                layoutEntry["isDefault"] = (layoutId == defaultLayoutId);
                layoutEntry["layoutId"] = layoutId;
                // Sidecar images still live on disk next to where the layout
                // document used to be; the designer resolves them from here.
                layoutEntry["filePath"] = ResolveLayoutFilePath(mFileSystem, layoutId).generic_string();
                entries.push_back(layoutEntry);
            }

            if (!entries.empty())
            {
                library["byEffectType"][lookupKey] = entries;

                if (!defaultLayoutId.empty())
                {
                    library["defaults"][lookupKey] = defaultLayoutId;
                }
            }
        }
    }

    // Load factory layouts from the bundled assets directory.
    // Structure: ui/assets/layouts/<folder-name>/layout.json  (+ images/ subfolder)
    // Each folder represents one exported layout. Factory layouts are read-only and are
    // added as defaults only when no user-defined default already exists for that key.
    {
        const auto bundledRoot = mHost.GetBundledAssetsPath();
        const auto factoryLayoutsDir = bundledRoot / "ui" / "assets" / "layouts";

        if (std::filesystem::exists(factoryLayoutsDir))
        {
            for (const auto& layoutFolder : std::filesystem::directory_iterator(factoryLayoutsDir))
            {
                if (!layoutFolder.is_directory())
                {
                    continue;
                }

                const auto layoutJsonPath = layoutFolder.path() / "layout.json";

                if (!std::filesystem::exists(layoutJsonPath))
                {
                    continue;
                }

                try
                {
                    std::ifstream input(layoutJsonPath);

                    if (!input)
                    {
                        continue;
                    }

                    nlohmann::json archive;
                    input >> archive;

                    if (!archive.is_object() || !archive.contains("layout") || !archive["layout"].is_object())
                    {
                        continue;
                    }

                    nlohmann::json layoutJson = archive["layout"];

                    const std::string effectType = layoutJson.value("effectType", "");

                    if (effectType.empty())
                    {
                        continue;
                    }

                    const std::string blendId = layoutJson.value("blendId", "");
                    const std::string lookupKey = blendId.empty() ? effectType : (effectType + "::" + blendId);

                    // Use embedded layoutId or derive a stable one from the folder name.
                    std::string layoutId = layoutJson.value("layoutId", "");

                    if (layoutId.empty())
                    {
                        layoutId = "factory::" + layoutFolder.path().filename().string();
                        layoutJson["layoutId"] = layoutId;
                    }

                    // Load images referenced in the manifest and add them to the library image list.
                    const auto imagesDir = layoutFolder.path() / "images";
                    (void)imagesDir; // Factory images are loaded on demand via BuildLayoutImages().

                    // Build the library entry and prepend it so user layouts (added below)
                    // can override/supplement without losing the factory entry.
                    nlohmann::json factoryEntry;
                    factoryEntry["layout"] = layoutJson;
                    factoryEntry["isDefault"] = false; // resolved after user entries are built
                    factoryEntry["layoutId"] = layoutId;
                    factoryEntry["isFactory"] = true;
                    factoryEntry["filePath"] = layoutJsonPath.generic_string();

                    if (!library["byEffectType"].contains(lookupKey))
                    {
                        library["byEffectType"][lookupKey] = nlohmann::json::array();
                    }

                    // Prepend so factory entries appear first; user entries appended later.
                    library["byEffectType"][lookupKey].insert(library["byEffectType"][lookupKey].begin(), factoryEntry);
                }
                catch (const std::exception& e)
                {
                    AppendSessionLog("Failed to load factory layout from " + layoutFolder.path().generic_string() +
                                     ": " + e.what());
                }
            }
        }
    }

    // Resolve defaults: for each key without a user-defined default, use the first
    // factory layout found (if any).
    for (auto& [key, entries] : library["byEffectType"].items())
    {
        if (!library["defaults"].contains(key) || library["defaults"][key].get<std::string>().empty())
        {
            for (const auto& entry : entries)
            {
                if (entry.value("isFactory", false))
                {
                    const std::string fid = entry.value("layoutId", "");

                    if (!fid.empty())
                    {
                        library["defaults"][key] = fid;
                        break;
                    }
                }
            }
        }

        // Stamp isDefault on each entry.
        const std::string defaultId = library["defaults"].value(key, "");

        for (auto& entry : entries)
        {
            entry["isDefault"] = (!defaultId.empty() && entry.value("layoutId", "") == defaultId);
        }
    }

    SendMessageToUI(nlohmann::json{{"type", "layoutLibraryLoaded"}, {"layoutLibrary", library}}.dump());
}

nlohmann::json PluginController::BuildLayoutImages()
{
    nlohmann::json images = nlohmann::json::array();

    // Helper: load all images from a directory into the image list.
    const auto appendImagesFromDir = [&images](const std::filesystem::path& imagesDir) {
        if (!std::filesystem::exists(imagesDir))
        {
            return;
        }

        for (const auto& entry : std::filesystem::directory_iterator(imagesDir))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }

            const auto ext = entry.path().extension().string();

            if (ext != ".png" && ext != ".jpg" && ext != ".jpeg")
            {
                continue;
            }

            std::ifstream imageFile(entry.path(), std::ios::binary);

            if (!imageFile)
            {
                continue;
            }

            std::vector<std::uint8_t> imageData((std::istreambuf_iterator<char>(imageFile)),
                                                std::istreambuf_iterator<char>());
            imageFile.close();

            const std::string base64Data = util::EncodeBase64(imageData);
            std::string mimeType = "image/png";

            if (ext == ".jpg" || ext == ".jpeg")
            {
                mimeType = "image/jpeg";
            }

            const std::string dataUrl = "data:" + mimeType + ";base64," + base64Data;

            const std::string imageId = entry.path().stem().string();
            bool replaced = false;

            for (auto& existing : images)
            {
                if (existing.is_object() && existing.value("imageId", std::string{}) == imageId)
                {
                    existing["fileName"] = entry.path().filename().string();
                    existing["dataUrl"] = dataUrl;
                    replaced = true;
                    break;
                }
            }

            if (!replaced)
            {
                nlohmann::json imageRef;
                imageRef["imageId"] = imageId;
                imageRef["fileName"] = entry.path().filename().string();
                imageRef["dataUrl"] = dataUrl;
                images.push_back(imageRef);
            }
        }
    };

    // User layout images: one images/ folder per associated layoutId.
    nlohmann::json settings = LoadEffectLayoutsSettings(mFileSystem);

    if (settings.contains("associations") && settings["associations"].is_object())
    {
        for (auto it = settings["associations"].begin(); it != settings["associations"].end(); ++it)
        {
            const auto& assocEntry = it.value();

            if (!assocEntry.is_object())
            {
                continue;
            }

            const auto ids = assocEntry.value("layoutIds", nlohmann::json::array());

            if (!ids.is_array())
            {
                continue;
            }

            for (const auto& id : ids)
            {
                if (!id.is_string())
                {
                    continue;
                }

                const std::string layoutId = id.get<std::string>();
                appendImagesFromDir(ResolveLayoutDir(mFileSystem, layoutId) / "images");
            }
        }
    }

    // Factory layout images: referenced via each layout's manifest.
    {
        const auto bundledRoot = mHost.GetBundledAssetsPath();
        const auto factoryLayoutsDir = bundledRoot / "ui" / "assets" / "layouts";

        if (std::filesystem::exists(factoryLayoutsDir))
        {
            for (const auto& layoutFolder : std::filesystem::directory_iterator(factoryLayoutsDir))
            {
                if (!layoutFolder.is_directory())
                {
                    continue;
                }

                const auto layoutJsonPath = layoutFolder.path() / "layout.json";

                if (!std::filesystem::exists(layoutJsonPath))
                {
                    continue;
                }

                try
                {
                    std::ifstream input(layoutJsonPath);

                    if (!input)
                    {
                        continue;
                    }

                    nlohmann::json archive;
                    input >> archive;

                    if (!archive.is_object() || !archive.contains("images") || !archive["images"].is_array())
                    {
                        continue;
                    }

                    const auto imagesDir = layoutFolder.path() / "images";

                    if (!std::filesystem::exists(imagesDir))
                    {
                        continue;
                    }

                    for (const auto& imgRef : archive["images"])
                    {
                        if (!imgRef.is_object())
                        {
                            continue;
                        }

                        const std::string imageId = imgRef.value("imageId", "");
                        const std::string fileName = imgRef.value("fileName", "");

                        if (imageId.empty() || fileName.empty())
                        {
                            continue;
                        }

                        const auto imgPath = imagesDir / fileName;

                        if (!std::filesystem::exists(imgPath))
                        {
                            continue;
                        }

                        std::ifstream imgFile(imgPath, std::ios::binary);

                        if (!imgFile)
                        {
                            continue;
                        }

                        std::vector<std::uint8_t> imgData((std::istreambuf_iterator<char>(imgFile)),
                                                          std::istreambuf_iterator<char>());
                        imgFile.close();

                        const std::string base64Data = util::EncodeBase64(imgData);
                        const auto ext = imgPath.extension().string();
                        std::string mimeType = "image/png";

                        if (ext == ".jpg" || ext == ".jpeg")
                        {
                            mimeType = "image/jpeg";
                        }

                        const std::string dataUrl = "data:" + mimeType + ";base64," + base64Data;

                        bool replaced = false;

                        for (auto& existing : images)
                        {
                            if (existing.is_object() && existing.value("imageId", std::string{}) == imageId)
                            {
                                existing["fileName"] = fileName;
                                existing["dataUrl"] = dataUrl;
                                replaced = true;
                                break;
                            }
                        }

                        if (!replaced)
                        {
                            nlohmann::json imageRef;
                            imageRef["imageId"] = imageId;
                            imageRef["fileName"] = fileName;
                            imageRef["dataUrl"] = dataUrl;
                            images.push_back(imageRef);
                        }
                    }
                }
                catch (const std::exception& e)
                {
                    AppendSessionLog("Failed to load factory layout images from " +
                                     layoutFolder.path().generic_string() + ": " + e.what());
                }
            }
        }
    }

    return images;
}

void PluginController::HandleRequestLayoutImagesRequest()
{
    SendMessageToUI(nlohmann::json{{"type", "layoutImagesLoaded"}, {"images", BuildLayoutImages()}}.dump());
}

void PluginController::SaveLayoutToFile(const std::string& layoutId, const nlohmann::json& layoutJson)
{
    // The layout document goes to the store; its image sidecars stay in
    // layouts/content/<id>/images/, so the directory is still created.
    const auto layoutDir = ResolveLayoutDir(mFileSystem, layoutId);
    [[maybe_unused]] const auto ensuredDir = mFileSystem.EnsureDirectory(layoutDir);

    if (Store().Put(storage::ItemType::kLayout, layoutId, layoutJson))
    {
        AppendSessionLog("Layout saved: " + layoutId);
    }
    else
    {
        AppendSessionLog("Failed to save layout: " + layoutId);
    }
}
} // namespace guitarfx
