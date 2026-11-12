/**
 * PluginControllerLayouts.cpp - Custom effect layouts and their images.
 *
 * A layout is an index entry in one shared settings file plus a content
 * directory holding its images. Every save copies each image the layout draws
 * on into that directory, so a layout never depends on another's folder, and a
 * delete first lets every other layout take its own copy of what it borrowed.
 * Images arriving from an imported archive are named by the archive, so their
 * names are checked before anything is written.
 */

#include "PluginController.h"

#include "controller/internal/ControllerUtils.h"
#include "controller/internal/LayoutSupport.h"
#include "util/Base64.h"
#include "util/PathEncoding.h"

#include <algorithm>
#include <cctype>
#include <fstream>

using namespace guitarfx::controller_detail;

namespace guitarfx
{
void PluginController::HandleDeleteLayoutRequest(const nlohmann::json& payload)
{
    const std::string effectType = payload.value("effectType", "");
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

    nlohmann::json settings = LoadEffectLayoutsSettings(mFileSystem);
    const auto log = [this](const std::string& line) { AppendSessionLog(line); };

    // Any image picker can choose an image from another layout's folder, and this one's folder
    // goes below. Every other layout takes its own copy of what it draws on first.
    if (settings.contains("associations") && settings["associations"].is_object())
    {
        const auto factoryDir = ResolveFactoryLayoutsDir(mHost.GetBundledAssetsPath());

        for (const auto& [key, entry] : settings["associations"].items())
        {
            if (!entry.is_object() || !entry.contains("layoutIds") || !entry["layoutIds"].is_array())
            {
                continue;
            }

            for (const auto& id : entry["layoutIds"])
            {
                if (!id.is_string() || id.get<std::string>() == layoutId)
                {
                    continue;
                }

                if (const auto other = Store().Get(storage::ItemType::kLayout, id.get<std::string>()))
                {
                    CopyMissingLayoutImages(mFileSystem, factoryDir, id.get<std::string>(),
                                            CollectLayoutImageIds(*other), log);
                }
            }
        }
    }

    if (!Store().Remove(storage::ItemType::kLayout, layoutId))
    {
        ReportErrorToUI("Delete layout failed", "The layout could not be removed from the library");
        LoadLayoutLibrary(); // the UI dropped it already; put it back
        return;
    }

    AppendSessionLog("Layout deleted: " + layoutId);

    // Out of every association, not only the one the UI named: the same layout can be listed
    // under an effect type and a blend of it, and a listing left behind points at nothing.
    if (settings.contains("associations") && settings["associations"].is_object())
    {
        auto& assoc = settings["associations"];
        std::vector<std::string> emptied;

        for (auto& [key, entry] : assoc.items())
        {
            if (!entry.is_object())
            {
                continue;
            }

            const auto ids = entry.contains("layoutIds") && entry["layoutIds"].is_array() ? entry["layoutIds"]
                                                                                          : nlohmann::json::array();
            nlohmann::json updated = nlohmann::json::array();

            for (const auto& id : ids)
            {
                if (!(id.is_string() && id.get<std::string>() == layoutId))
                {
                    updated.push_back(id);
                }
            }

            entry["layoutIds"] = updated;

            if (entry.contains("defaultLayoutId") && entry["defaultLayoutId"].is_string() &&
                entry["defaultLayoutId"].get<std::string>() == layoutId)
            {
                entry["defaultLayoutId"] =
                    !updated.empty() && updated[0].is_string() ? updated[0].get<std::string>() : std::string{};
            }

            if (updated.empty())
            {
                emptied.push_back(key);
            }
        }

        for (const auto& key : emptied)
        {
            assoc.erase(key);
        }
    }

    if (!SaveEffectLayoutsSettings(mFileSystem, settings))
    {
        AppendSessionLog("Delete layout: the layout index could not be written");
    }

    // The image folder last, and only as tidying: the layout is gone from the library either
    // way, and a folder an antivirus scan holds open must not leave it half deleted.
    const auto layoutDir = ResolveLayoutDir(mFileSystem, layoutId);
    std::error_code ec;
    std::filesystem::remove_all(layoutDir, ec);

    if (ec)
    {
        AppendSessionLog("Delete layout: could not remove " + util::PathToUtf8(layoutDir) + ": " + ec.message());
    }

    LoadLayoutLibrary();
}

void PluginController::HandleSaveEffectLayoutRequest(const nlohmann::json& payload)
{
    const std::string effectType = payload.value("effectType", "");
    const std::string blendId = payload.value("blendId", "");
    std::string layoutId = payload.value("layoutId", "");
    const auto layoutIt = payload.find("layout");
    const auto reportFailure = [this, &layoutId](const std::string& reason) {
        AppendSessionLog("Save layout failed (" + layoutId + "): " + reason);
        SendMessageToUI(
            nlohmann::json{{"type", "layoutSaveFailed"}, {"layoutId", layoutId}, {"message", reason}}.dump());
    };

    if (effectType.empty() || layoutIt == payload.end() || !layoutIt->is_object())
    {
        reportFailure("Missing effect type or layout data");
        return;
    }

    const std::string lookupKey = blendId.empty() ? effectType : (effectType + "::" + blendId);

    if (layoutId.empty())
    {
        layoutId = GenerateGuidV4String();
    }

    nlohmann::json layoutJson = *layoutIt;
    layoutJson["layoutId"] = layoutId;

    if (!SaveLayoutToFile(layoutId, layoutJson))
    {
        reportFailure("The layout could not be written to the library");
        return;
    }

    // Every image the layout draws on gets a copy in its own folder, on every save rather than
    // only the first: one picked from another layout's folder would otherwise go with that layout
    // when it is deleted, and a factory layout's fork would depend on the bundled folder.
    for (const auto& imageId : CopyMissingLayoutImages(
             mFileSystem, ResolveFactoryLayoutsDir(mHost.GetBundledAssetsPath()), layoutId,
             CollectLayoutImageIds(layoutJson), [this](const std::string& line) { AppendSessionLog(line); }))
    {
        AppendSessionLog("Layout " + layoutId + " refers to an image that is not in the library: " + imageId);
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

    if (!SaveEffectLayoutsSettings(mFileSystem, settings))
    {
        reportFailure("The layout index could not be written");
        LoadLayoutLibrary();
        return;
    }

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
                nlohmann::json{{"type", "layoutExportSaved"}, {"path", util::PathToUtf8(result.path)}}.dump());
            AppendSessionLog("Layout exported: " + util::PathToUtf8(result.path));
        });
}

void PluginController::HandleBrowseLayoutImageRequest(const nlohmann::json& payload)
{
    const std::string purpose = payload.value("purpose", "");
    const int layerIndex = payload.value("layerIndex", 0);
    const std::string paramKey = payload.value("paramKey", "");
    const std::string layoutId = payload.value("layoutId", "");
    // An image picker's own request, echoed so the reply goes back to that picker.
    const std::string pickerId = payload.value("pickerId", "");

    // An image goes in the folder of the layout it is for. Without an id it would land in the
    // legacy shared folder, which the loader only reads for old layouts' sake.
    if (layoutId.empty())
    {
        ReportErrorToUI("Image import failed", "Missing layoutId");
        return;
    }

    mHost.BrowseFileAsync(
        BrowseFileType::ImageFile, "Select Image",
        [this, purpose, layerIndex, paramKey, layoutId, pickerId](const BrowseFileResult& result) {
            if (!result.success)
            {
                return;
            }

            const auto selectedPath = result.path;
            const std::string mimeType = LayoutImageMimeType(selectedPath.extension());

            if (mimeType.empty())
            {
                ReportErrorToUI("Image import failed", "Layouts take PNG, JPEG, GIF, WebP or SVG images");
                return;
            }

            std::error_code sizeEc;
            const auto size = std::filesystem::file_size(selectedPath, sizeEc);

            if (!sizeEc && size > kMaxLayoutImageBytes)
            {
                ReportErrorToUI("Image import failed", "The image is larger than 64 MB");
                return;
            }

            const auto imagesDir = ResolveLayoutDir(mFileSystem, layoutId) / "images";

            const auto sendSelected = [&](const std::string& imageId, const std::filesystem::path& path,
                                          const std::string& category, bool factory) {
                // The UI fetches the pixels from the entry's URL; the layout may not be saved yet,
                // which ResolveLayoutImageFile allows for. "type" is the message's own, so the
                // image's category goes as "category".
                auto message = LayoutImageEntry(imageId, path);
                message["type"] = "layoutImageSelected";
                message["category"] = category;
                message["factory"] = factory;
                message["purpose"] = purpose;
                message["layerIndex"] = layerIndex;
                message["paramKey"] = paramKey;

                if (!pickerId.empty())
                {
                    message["pickerId"] = pickerId;
                }

                SendMessageToUI(message.dump());
            };

            // A picture the library already has is used as it is, not copied again under a new id.
            // Saving the layout copies it into this layout's folder if it lives in another's.
            if (const auto existing = FindIdenticalLayoutImage(
                    mFileSystem, ResolveFactoryLayoutsDir(mHost.GetBundledAssetsPath()), selectedPath, imagesDir))
            {
                AppendSessionLog("Layout image already in the library as " + existing->imageId);

                // An image with no category yet takes the one it was picked for.
                std::string category = existing->category;

                if (!existing->factory)
                {
                    const auto categories = LoadLayoutImageCategories(mFileSystem);
                    const auto filed = categories.find(existing->imageId);

                    if (filed != categories.end())
                    {
                        category = filed->second;
                    }
                    else if (IsLayoutImageCategory(purpose) &&
                             SaveLayoutImageCategory(mFileSystem, existing->imageId, purpose))
                    {
                        category = purpose;
                    }
                }

                sendSelected(existing->imageId, existing->path, category, existing->factory);
                return;
            }

            [[maybe_unused]] const auto ensuredImagesDir = mFileSystem.EnsureDirectory(imagesDir);

            // The extension in lower case, so "IMG_0042.JPG" is read back like any other JPEG.
            std::string extension = util::PathToUtf8(selectedPath.extension());
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const auto timestamp = std::chrono::system_clock::now().time_since_epoch().count();
            const std::string imageId = util::PathToUtf8(selectedPath.stem()) + "_" + std::to_string(timestamp);
            const std::string destFilename = imageId + extension;
            const auto destPath = imagesDir / util::PathFromUtf8(destFilename);

            try
            {
                std::filesystem::copy_file(selectedPath, destPath, std::filesystem::copy_options::overwrite_existing);
                InvalidateLayoutImageIndex();

                // A new image is filed under what it was picked for: a knob or a background.
                const bool filed =
                    IsLayoutImageCategory(purpose) && SaveLayoutImageCategory(mFileSystem, imageId, purpose);
                sendSelected(imageId, destPath, filed ? purpose : std::string(), false);
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

    if (imageId.empty() || fileName.empty() || dataEncoded.empty() || layoutId.empty())
    {
        AppendSessionLog("SaveLayoutImage: missing required fields");
        ReportErrorToUI("Image import failed", "The layout's image was incomplete");
        return;
    }

    // The name comes from an imported archive's manifest, so the archive chooses it. A name
    // with a folder or a drive in it would be written wherever it points.
    if (!IsSafeLayoutImageFileName(fileName, imageId))
    {
        AppendSessionLog("SaveLayoutImage: refused the file name " + fileName);
        ReportErrorToUI("Image import failed", "The layout's image has a file name that cannot be used");
        return;
    }

    const auto decodedBytes = util::DecodeBase64(dataEncoded);

    if (decodedBytes.empty() || decodedBytes.size() > kMaxLayoutImageBytes ||
        !LooksLikeLayoutImage(decodedBytes, util::PathFromUtf8(fileName).extension()))
    {
        AppendSessionLog("SaveLayoutImage: refused " + fileName + " (" + std::to_string(decodedBytes.size()) +
                         " bytes): not an image of its type, or too large");
        ReportErrorToUI("Image import failed", "The layout's image is not a supported image, or is larger than 64 MB");
        return;
    }

    const auto imagesDir = ResolveLayoutDir(mFileSystem, layoutId) / "images";
    [[maybe_unused]] const auto ensuredImagesDir = mFileSystem.EnsureDirectory(imagesDir);
    const auto destPath = imagesDir / util::PathFromUtf8(fileName);

    if (WriteFile(destPath, decodedBytes))
    {
        InvalidateLayoutImageIndex();

        if (const std::string category = payload.value("category", ""); IsLayoutImageCategory(category))
        {
            [[maybe_unused]] const auto filed = SaveLayoutImageCategory(mFileSystem, imageId, category);
        }

        // The licence and credit the archive carries go with the image.
        if (const LayoutImageCredit credit{payload.value("license", ""), payload.value("credit", "")};
            !credit.license.empty() || !credit.credit.empty())
        {
            [[maybe_unused]] const auto credited = SaveLayoutImageCredit(mFileSystem, imageId, credit);
        }

        AppendSessionLog("Layout image saved from import: " + util::PathToUtf8(destPath));
    }
    else
    {
        AppendSessionLog("SaveLayoutImage: failed to write " + util::PathToUtf8(destPath));
        ReportErrorToUI("Image import failed", "The layout's image could not be written");
    }
}

void PluginController::LoadLayoutLibrary()
{
    // Every save, delete and import ends here, so the next image request sees the new folders.
    InvalidateLayoutImageIndex();

    nlohmann::json library;
    library["byEffectType"] = nlohmann::json::object();
    library["defaults"] = nlohmann::json::object();
    // The image list comes separately, on request (BuildLayoutImages), and lists images by URL.
    // Per-layout thumbnailDataUrl values remain embedded in each layout entry below since they
    // are used for signal-path node avatars.
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
                layoutEntry["filePath"] = util::PathToUtf8(ResolveLayoutFilePath(mFileSystem, layoutId));
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
        const auto factoryLayoutsDir = ResolveFactoryLayoutsDir(bundledRoot);

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
                        layoutId = "factory::" + util::PathToUtf8(layoutFolder.path().filename());
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
                    factoryEntry["filePath"] = util::PathToUtf8(layoutJsonPath);

                    if (!library["byEffectType"].contains(lookupKey))
                    {
                        library["byEffectType"][lookupKey] = nlohmann::json::array();
                    }

                    // Prepend so factory entries appear first; user entries appended later.
                    library["byEffectType"][lookupKey].insert(library["byEffectType"][lookupKey].begin(), factoryEntry);
                }
                catch (const std::exception& e)
                {
                    AppendSessionLog("Failed to load factory layout from " + util::PathToUtf8(layoutFolder.path()) +
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
    const auto categories = LoadLayoutImageCategories(mFileSystem);
    const auto credits = LoadLayoutImageCredits(mFileSystem);

    // One entry per id, resolved as every other reader of an image id resolves it.
    for (const auto& source :
         CollectLayoutImageSources(mFileSystem, ResolveFactoryLayoutsDir(mHost.GetBundledAssetsPath())))
    {
        auto entry = LayoutImageEntry(source.imageId, source.path);
        const auto filed = categories.find(source.imageId);

        // A factory category folder decides; otherwise the category the designer filed it under.
        if (!source.category.empty())
        {
            entry["type"] = source.category;
        }
        else if (filed != categories.end())
        {
            entry["type"] = filed->second;
        }

        if (source.factory)
        {
            entry["factory"] = true;
        }

        if (const auto credit = credits.find(source.imageId); credit != credits.end())
        {
            entry["license"] = credit->second.license;
            entry["credit"] = credit->second.credit;
        }

        images.push_back(std::move(entry));
    }

    return images;
}

nlohmann::json PluginController::LayoutImageEntry(const std::string& imageId, const std::filesystem::path& path) const
{
    std::error_code ec;
    const auto version = LayoutImageVersion(path);
    const auto bytes = std::filesystem::file_size(path, ec);

    // The version goes as a string: clock ticks are past what a JavaScript number holds exactly.
    nlohmann::json entry{{"imageId", imageId},
                         {"fileName", util::PathToUtf8(path.filename())},
                         {"url", LayoutImageUrl(imageId, version)},
                         {"version", std::to_string(version)},
                         {"bytes", ec ? 0 : bytes}};

    if (const auto size = ReadLayoutImageSize(path))
    {
        entry["width"] = size->width;
        entry["height"] = size->height;
    }

    // Smaller copies made for this version of the file; a copy of an older version is not listed.
    nlohmann::json renditions = nlohmann::json::array();
    const auto renditionsDir = ResolveLayoutRenditionsDir(mFileSystem);

    for (const int edge : {1024, 2048, 4096})
    {
        const auto copy = renditionsDir / util::PathFromUtf8(LayoutRenditionFileName(imageId, edge, version));
        const auto size = std::filesystem::is_regular_file(copy, ec) ? ReadLayoutImageSize(copy) : std::nullopt;

        if (size)
        {
            renditions.push_back({{"edge", edge},
                                  {"width", size->width},
                                  {"height", size->height},
                                  {"url", LayoutRenditionUrl(imageId, edge, version)}});
        }
    }

    if (!renditions.empty())
    {
        entry["renditions"] = std::move(renditions);
    }

    return entry;
}

void PluginController::HandleSetLayoutImageCategoryRequest(const nlohmann::json& payload)
{
    const std::string imageId = payload.value("imageId", "");
    const std::string category = payload.value("category", "");

    // A factory image keeps the category it ships under.
    const auto sources =
        CollectLayoutImageSources(mFileSystem, ResolveFactoryLayoutsDir(mHost.GetBundledAssetsPath()), true);
    const auto source = std::find_if(sources.begin(), sources.end(),
                                     [&imageId](const LayoutImageSource& s) { return s.imageId == imageId; });

    if (source == sources.end() || source->factory || !SaveLayoutImageCategory(mFileSystem, imageId, category))
    {
        AppendSessionLog("SetLayoutImageCategory: could not file " + imageId + " under " + category);
        return;
    }

    AppendSessionLog("Layout image " + imageId + " filed under " + category);
}

void PluginController::HandleSetLayoutImageCreditRequest(const nlohmann::json& payload)
{
    const std::string imageId = payload.value("imageId", "");
    const LayoutImageCredit credit{payload.value("license", ""), payload.value("credit", "")};

    if (!SaveLayoutImageCredit(mFileSystem, imageId, credit))
    {
        AppendSessionLog("SetLayoutImageCredit: could not record the credit for " + imageId);
    }
}

void PluginController::HandleSaveLayoutImageRenditionRequest(const nlohmann::json& payload)
{
    const std::string imageId = payload.value("imageId", "");
    const std::string versionText = payload.value("version", "");
    const int edge = payload.value("edge", 0);
    const std::string dataEncoded = payload.value("data", "");
    const auto source = ResolveLayoutImageFile(imageId);

    // A copy for a file that has since changed, or of a size not kept, is dropped quietly: the UI
    // makes copies in the background and will make them again for the new file.
    if (!source || !IsLayoutRenditionEdge(edge) || dataEncoded.empty() ||
        versionText != std::to_string(LayoutImageVersion(*source)))
    {
        AppendSessionLog("SaveLayoutImageRendition: dropped a copy of " + imageId + " at " + std::to_string(edge));
        return;
    }

    const auto sourceSize = ReadLayoutImageSize(*source);
    const auto bytes = util::DecodeBase64(dataEncoded);

    if (!sourceSize || static_cast<std::uint32_t>(edge) >= std::max(sourceSize->width, sourceSize->height) ||
        bytes.empty() || bytes.size() > kMaxLayoutImageBytes || !LooksLikeLayoutImage(bytes, ".webp"))
    {
        AppendSessionLog("SaveLayoutImageRendition: refused a copy of " + imageId + " at " + std::to_string(edge));
        return;
    }

    const auto version = LayoutImageVersion(*source);
    const auto dir = ResolveLayoutRenditionsDir(mFileSystem);
    [[maybe_unused]] const auto ensured = mFileSystem.EnsureDirectory(dir);
    const auto fileName = LayoutRenditionFileName(imageId, edge, version);
    const auto dest = dir / util::PathFromUtf8(fileName);

    if (!WriteFile(dest, bytes))
    {
        AppendSessionLog("SaveLayoutImageRendition: could not write " + util::PathToUtf8(dest));
        return;
    }

    // The copy must be what it says: a WebP whose longest edge is the one asked for.
    const auto written = ReadLayoutImageSize(dest);
    std::error_code ec;

    if (!written || std::max(written->width, written->height) != static_cast<std::uint32_t>(edge))
    {
        std::filesystem::remove(dest, ec);
        AppendSessionLog("SaveLayoutImageRendition: removed a copy of " + imageId + " that is not " +
                         std::to_string(edge) + " px");
        return;
    }

    // Copies of the file's earlier versions at this size are no use any more.
    const std::string stalePrefix = fileName.substr(0, fileName.rfind('-') + 1);

    for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
    {
        const std::string name = util::PathToUtf8(entry.path().filename());

        if (name != fileName && name.rfind(stalePrefix, 0) == 0)
        {
            std::error_code removeEc;
            std::filesystem::remove(entry.path(), removeEc);
        }
    }

    SendMessageToUI(nlohmann::json{{"type", "layoutImageRenditionSaved"},
                                   {"imageId", imageId},
                                   {"edge", edge},
                                   {"width", written->width},
                                   {"height", written->height},
                                   {"url", LayoutRenditionUrl(imageId, edge, version)}}
                        .dump());
}

std::optional<std::filesystem::path> PluginController::ResolveLayoutImageFile(const std::string& imageId)
{
    if (imageId.empty())
    {
        return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(mLayoutImageIndexMutex);

    const auto build = [this] {
        std::map<std::string, std::filesystem::path> index;

        // Unindexed folders too, at the lowest priority: an image just picked for a layout that
        // has not been saved yet lives in one, and the designer draws it straight away.
        for (auto& source :
             CollectLayoutImageSources(mFileSystem, ResolveFactoryLayoutsDir(mHost.GetBundledAssetsPath()), true))
        {
            index.emplace(std::move(source.imageId), std::move(source.path));
        }

        return index;
    };

    if (!mLayoutImageIndex)
    {
        mLayoutImageIndex = build();
    }

    auto found = mLayoutImageIndex->find(imageId);
    std::error_code ec;

    // Folders change outside the handlers that invalidate (a file removed by hand); look again once.
    if (found == mLayoutImageIndex->end() || !std::filesystem::is_regular_file(found->second, ec))
    {
        mLayoutImageIndex = build();
        found = mLayoutImageIndex->find(imageId);
    }

    return found != mLayoutImageIndex->end() ? std::optional<std::filesystem::path>(found->second) : std::nullopt;
}

std::optional<std::pair<std::filesystem::path, std::string>> PluginController::ResolveLayoutImageResource(
    const std::string& resourcePath)
{
    // A smaller copy: served only while it was made from the image's current file.
    if (const auto rendition = LayoutRenditionFromResourcePath(resourcePath))
    {
        const auto source = ResolveLayoutImageFile(rendition->first);
        const auto copy = source ? ResolveLayoutRenditionsDir(mFileSystem) /
                                       util::PathFromUtf8(LayoutRenditionFileName(rendition->first, rendition->second,
                                                                                  LayoutImageVersion(*source)))
                                 : std::filesystem::path{};
        std::error_code ec;

        if (!source || !std::filesystem::is_regular_file(copy, ec))
        {
            return std::nullopt;
        }

        return std::make_pair(copy, std::string("image/webp"));
    }

    const auto file = ResolveLayoutImageFile(LayoutImageIdFromResourcePath(resourcePath));
    const std::string mimeType = file ? LayoutImageMimeType(file->extension()) : std::string{};

    if (!file || mimeType.empty())
    {
        return std::nullopt;
    }

    return std::make_pair(*file, mimeType);
}

void PluginController::InvalidateLayoutImageIndex()
{
    std::lock_guard<std::mutex> lock(mLayoutImageIndexMutex);
    mLayoutImageIndex.reset();
}

void PluginController::HandleRequestLayoutImagesRequest()
{
    SendMessageToUI(nlohmann::json{{"type", "layoutImagesLoaded"}, {"images", BuildLayoutImages()}}.dump());
}

bool PluginController::SaveLayoutToFile(const std::string& layoutId, const nlohmann::json& layoutJson)
{
    // The layout document goes to the store; its image sidecars stay in
    // layouts/content/<id>/images/, so the directory is still created.
    const auto layoutDir = ResolveLayoutDir(mFileSystem, layoutId);
    [[maybe_unused]] const auto ensuredDir = mFileSystem.EnsureDirectory(layoutDir);

    if (Store().Put(storage::ItemType::kLayout, layoutId, layoutJson))
    {
        AppendSessionLog("Layout saved: " + layoutId);
        return true;
    }

    AppendSessionLog("Failed to save layout: " + layoutId);
    return false;
}
} // namespace guitarfx
