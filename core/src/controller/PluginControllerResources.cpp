/**
 * PluginControllerResources.cpp - The resource library: importing, editing and
 * deleting NAM captures, IRs and plugin references.
 *
 * A resource is a file on disk plus an entry in the library index, and the two
 * must be created and removed together. Deleting one is gated on nothing still
 * referencing it, which is what the usage index exists to answer without
 * reading every preset from disk on each query.
 */

#include "PluginController.h"

#include "controller/internal/ControllerUtils.h"
#include "controller/internal/HostedPluginSupport.h"
#include "controller/internal/NamResourceMetadata.h"
#include "controller/internal/PresetArchiveSupport.h"
#include "presets/PresetStorage.h"
#include "resources/PluginPathUtils.h"
#include "resources/ResourceLibrary.h"
#include "util/Base64.h"
#include "util/PathEncoding.h"
#include "util/PathSanitizer.h"
#include "util/Wav.h"

#include <algorithm>
#include <fstream>
#include <unordered_set>

using namespace guitarfx::controller_detail;

namespace guitarfx
{

void PluginController::HandleImportRemoteResourceRequest(const nlohmann::json& payload)
{
    const std::string resourceType = payload.value("resourceType", "");
    const std::string resourceId = payload.value("resourceId", "");
    const std::string name = payload.value("name", resourceId);
    const std::string description = payload.value("description", "");
    const std::string category = payload.value("category", "");
    const std::string provider = payload.value("provider", "remote");
    const std::string subfolder = payload.value("subfolder", "");
    const std::string data = payload.value("data", "");
    const std::string fileName = payload.value("fileName", "");
    const std::string hash = payload.value("hash", "");
    const nlohmann::json metadataPayload = payload.value("metadata", nlohmann::json::object());
    const nlohmann::json tagsPayload = payload.value("tags", nlohmann::json::array());

    if (resourceType.empty() || resourceId.empty() || data.empty())
    {
        ReportErrorToUI("Import failed", "Missing resource metadata");
        SendMessageToUI(nlohmann::json{
            {"type", "resourceImportFailed"}, {"message", "Import failed"}, {"detail", "Missing resource metadata"}}
                            .dump());
        return;
    }

    const auto settingsDir = mFileSystem.ResolveSettingsDirectory();
    const auto sanitizedProvider = util::SanitizePathSegment(provider, true);
    auto targetDir = settingsDir / "resources" / "content" / sanitizedProvider;
    const auto sanitizedSubfolder = util::SanitizeSubfolderPath(subfolder);
    if (!sanitizedSubfolder.empty())
    {
        targetDir /= sanitizedSubfolder;
    }
    [[maybe_unused]] const auto ensuredTargetDir = mFileSystem.EnsureDirectory(targetDir);

    std::string resolvedName = fileName.empty() ? resourceId : fileName;
    resolvedName = util::SanitizeFilename(resolvedName);
    if (resolvedName.find('.') == std::string::npos)
    {
        resolvedName += resourceType == "ir" ? ".wav" : ".nam";
    }

    const auto targetPath = targetDir / resolvedName;
    const std::vector<std::uint8_t> bytes = util::DecodeBase64(data);
    if (bytes.empty())
    {
        ReportErrorToUI("Import failed", "Invalid base64 payload");
        SendMessageToUI(nlohmann::json{
            {"type", "resourceImportFailed"}, {"message", "Import failed"}, {"detail", "Invalid base64 payload"}}
                            .dump());
        return;
    }
    if (!WriteFile(targetPath, bytes))
    {
        ReportErrorToUI("Import failed", "Failed to write file");
        SendMessageToUI(nlohmann::json{
            {"type", "resourceImportFailed"}, {"message", "Import failed"}, {"detail", "Failed to write file"}}
                            .dump());
        return;
    }

    LibraryResource resource;
    resource.type = resourceType;
    resource.id = resourceId;
    resource.name = name;
    resource.category = category;
    resource.description = description;
    resource.filePath = targetPath;
    resource.hash = hash;
    if (metadataPayload.is_object())
    {
        for (const auto& entry : metadataPayload.items())
        {
            const auto& value = entry.value();
            if (value.is_string())
            {
                resource.metadata[entry.key()] = value.get<std::string>();
            }
            else if (value.is_number())
            {
                resource.metadata[entry.key()] = value.dump();
            }
            else if (value.is_boolean())
            {
                resource.metadata[entry.key()] = value.get<bool>() ? "true" : "false";
            }
        }
    }
    if (tagsPayload.is_array())
    {
        for (const auto& tagValue : tagsPayload)
        {
            if (!tagValue.is_string())
            {
                continue;
            }
            const auto tag = tagValue.get<std::string>();
            if (!tag.empty())
            {
                resource.tags.push_back(tag);
            }
        }
    }

    if (resourceType == "nam")
    {
        EnrichNamResourceMetadata(resource, targetPath);
    }

    resource.category = ResolveResourceLibraryCategory(resource, resource.category);

    mResourceLibrary.AddResource(resource);
    AppendUserLibraryResource(resource);
    BroadcastState();

    nlohmann::json msg;
    msg["type"] = "resourceImported";
    msg["resourceType"] = resourceType;
    msg["id"] = resourceId;
    msg["name"] = name;
    msg["filePath"] = util::PathToUtf8(targetPath);
    SendMessageToUI(msg.dump());
    AppendSessionLog("Imported resource " + resourceType + ":" + resourceId + " (" + targetPath.string() + ")");
}

std::optional<LibraryResource> PluginController::SaveLocalLibraryResource(const nlohmann::json& payload,
                                                                          std::string& error, bool allowCreate)
{
    const std::string resourceType = payload.value("resourceType", "");
    std::string resourceId = payload.value("resourceId", "");
    const std::string filePathValue = payload.value("filePath", "");
    const std::string data = payload.value("data", "");
    const std::string fileName = payload.value("fileName", "");
    const std::string name = payload.value("name", "");
    const std::string description = payload.value("description", "");
    const std::string category = payload.value("category", "");
    const std::string subfolder = payload.value("subfolder", "");
    const std::string providedHash = payload.value("hash", "");
    const nlohmann::json metadataPayload = payload.value("metadata", nlohmann::json::object());
    const nlohmann::json tagsPayload = payload.value("tags", nlohmann::json::array());
    const std::string payloadPluginName = payload.value("pluginName", "");
    const std::string payloadPluginManufacturer = payload.value("pluginManufacturer", "");
    const std::string payloadPluginStableId = payload.value("pluginStableId", "");

    if (resourceType.empty())
    {
        error = "Missing resource type";
        return std::nullopt;
    }

    const bool hasFilePath = !filePathValue.empty();
    const bool hasInlineData = !data.empty();
    if (!hasFilePath && !hasInlineData)
    {
        error = "Missing local file path or file data";
        return std::nullopt;
    }

    auto allResources = mResourceLibrary.GetAllResources();
    std::filesystem::path resolvedPath;
    std::string resolvedHash = providedHash;

    auto upsertMetadata = [&](LibraryResource& resource) {
        if (metadataPayload.is_object())
        {
            for (const auto& entry : metadataPayload.items())
            {
                const auto& value = entry.value();
                if (value.is_string())
                {
                    resource.metadata[entry.key()] = value.get<std::string>();
                }
                else if (value.is_number())
                {
                    resource.metadata[entry.key()] = value.dump();
                }
                else if (value.is_boolean())
                {
                    resource.metadata[entry.key()] = value.get<bool>() ? "true" : "false";
                }
            }
        }
        resource.metadata["provider"] = kLocalResourceProvider;
    };

    auto getMetadataString = [&](const std::string& key) -> std::string {
        if (!metadataPayload.is_object() || !metadataPayload.contains(key))
        {
            return {};
        }

        const auto& value = metadataPayload[key];
        if (value.is_string())
        {
            return value.get<std::string>();
        }
        if (value.is_number() || value.is_boolean())
        {
            return value.dump();
        }
        return {};
    };

    if (hasFilePath)
    {
        resolvedPath = util::PathFromUtf8(filePathValue);
        // Normalize plugin paths to the bundle root before anything is keyed off
        // them. Dialog results already arrive normalized, but paths can also come
        // from a preset, a synced library or a hand-edited entry, and path-based
        // de-duplication below only works if every route agrees.
        if (resourceType == "plugin")
        {
            resolvedPath = guitarfx::pluginpath::ResolvePluginBundlePath(resolvedPath);
        }
        if (!std::filesystem::exists(resolvedPath))
        {
            error = "Selected file does not exist";
            return std::nullopt;
        }
        if (resolvedHash.empty() && ShouldHashResourceFile(resolvedPath))
        {
            resolvedHash = mHasher.HashFile(resolvedPath);
        }
    }
    else
    {
        const auto decodedBytes = util::DecodeBase64(data);
        if (decodedBytes.empty())
        {
            error = "Invalid file data";
            return std::nullopt;
        }

        auto targetDir = GetEffectiveSettingsDirectory() / "resources" / "content" / kLocalResourceStorageFolder;
        if (!subfolder.empty())
        {
            std::filesystem::path sanitizedSubfolder;
            for (const auto& part : std::filesystem::path(subfolder))
            {
                const std::string segment = util::SanitizePathSegment(part.string(), true);
                if (segment.empty() || segment == "." || segment == "..")
                {
                    continue;
                }
                sanitizedSubfolder /= segment;
            }
            if (!sanitizedSubfolder.empty())
            {
                targetDir /= sanitizedSubfolder;
            }
        }
        [[maybe_unused]] const auto ensuredDir = mFileSystem.EnsureDirectory(targetDir);

        const auto defaultExtensionForType = [&](const std::string& type) {
            if (type == "ir")
            {
                return std::string{".wav"};
            }
            if (type == "wasm")
            {
                return std::string{".wasm"};
            }
            if (type == "nam")
            {
                return std::string{".nam"};
            }
            return std::string{".bin"};
        };

        std::string resolvedName =
            util::SanitizeFilename(fileName.empty() ? (resourceId.empty() ? name : resourceId) : fileName);
        if (resolvedName.empty())
        {
            resolvedName = "resource" + defaultExtensionForType(resourceType);
        }
        if (resolvedName.find('.') == std::string::npos)
        {
            resolvedName += defaultExtensionForType(resourceType);
        }

        resolvedPath = targetDir / resolvedName;

        if (std::filesystem::exists(resolvedPath) && !resolvedHash.empty())
        {
            const std::string existingHash = mHasher.HashFile(resolvedPath);
            if (!existingHash.empty() && existingHash != resolvedHash)
            {
                const std::filesystem::path stem = resolvedPath.stem();
                const std::filesystem::path ext = resolvedPath.extension();
                const std::string hashSuffix = resolvedHash.substr(0, std::min<std::size_t>(12, resolvedHash.size()));
                std::filesystem::path candidate = targetDir / (stem.string() + "-" + hashSuffix + ext.string());
                std::size_t suffix = 2;
                while (std::filesystem::exists(candidate))
                {
                    candidate =
                        targetDir / (stem.string() + "-" + hashSuffix + "-" + std::to_string(suffix++) + ext.string());
                }
                resolvedPath = candidate;
            }
        }

        if (!WriteFile(resolvedPath, decodedBytes))
        {
            error = "Failed to write local resource file";
            return std::nullopt;
        }
        if (resolvedHash.empty())
        {
            resolvedHash = mHasher.HashFile(resolvedPath);
        }
    }

    auto normalizedPathString = resolvedPath.lexically_normal().generic_string();
    auto existingByPath = std::find_if(allResources.begin(), allResources.end(), [&](const LibraryResource& resource) {
        return resource.type == resourceType && !resource.filePath.empty() &&
               resource.filePath.lexically_normal().generic_string() == normalizedPathString;
    });

    if (resourceId.empty() && existingByPath != allResources.end())
    {
        const bool canCompareHash = !resolvedHash.empty() && !existingByPath->hash.empty();
        const bool sameHash = canCompareHash && existingByPath->hash == resolvedHash;
        const bool unknownHash = !canCompareHash;
        if (sameHash || unknownHash)
        {
            resourceId = existingByPath->id;
        }
    }

    // For direct local file imports, keep entries path-specific to avoid mutating
    // an existing library item that happens to share a content hash.
    if (resourceId.empty() && !resolvedHash.empty() && !hasFilePath)
    {
        auto existingByHash =
            std::find_if(allResources.begin(), allResources.end(), [&](const LibraryResource& resource) {
                return resource.type == resourceType && !resource.hash.empty() && resource.hash == resolvedHash;
            });
        if (existingByHash != allResources.end())
        {
            resourceId = existingByHash->id;
        }
    }

    std::string normalizedPluginStableId;
    if (resourceType == "plugin")
    {
        std::string pluginName = payloadPluginName;
        if (pluginName.empty())
        {
            pluginName = getMetadataString(kHostedPluginNameConfigKey);
        }
        if (pluginName.empty())
        {
            pluginName = resolvedPath.stem().string();
        }

        std::string pluginManufacturer = payloadPluginManufacturer;
        if (pluginManufacturer.empty())
        {
            pluginManufacturer = getMetadataString(kHostedPluginManufacturerConfigKey);
        }

        std::string pluginStableId = payloadPluginStableId;
        if (pluginStableId.empty())
        {
            pluginStableId = getMetadataString(kHostedPluginStableIdConfigKey);
        }
        if (pluginStableId.empty())
        {
            pluginStableId = BuildHostedPluginStableId(pluginManufacturer, pluginName);
        }
        normalizedPluginStableId = NormalizeHostedPluginIdentityToken(pluginStableId);

        if (resourceId.empty() && !normalizedPluginStableId.empty())
        {
            auto existingByStableId =
                std::find_if(allResources.begin(), allResources.end(), [&](const LibraryResource& resource) {
                    if (resource.type != "plugin")
                    {
                        return false;
                    }
                    const auto it = resource.metadata.find(kHostedPluginStableIdConfigKey);
                    if (it == resource.metadata.end())
                    {
                        return false;
                    }
                    return NormalizeHostedPluginIdentityToken(it->second) == normalizedPluginStableId;
                });
            if (existingByStableId != allResources.end())
            {
                resourceId = existingByStableId->id;
            }
        }
    }

    if (resourceId.empty())
    {
        if (!allowCreate)
        {
            error = "Resource not found";
            return std::nullopt;
        }
        std::string baseId;
        if (resourceType == "plugin" && !normalizedPluginStableId.empty())
        {
            baseId = std::string{kLocalResourceProvider} + ":plugin:" + normalizedPluginStableId;
        }
        else
        {
            baseId = std::string{kLocalResourceProvider} + ":" +
                     util::SanitizePathSegment(resolvedPath.stem().string(), true);
        }
        if (baseId == std::string{kLocalResourceProvider} + ":")
        {
            baseId += "resource";
        }
        const bool allowHashSuffix = !(resourceType == "plugin" && !normalizedPluginStableId.empty());
        if (allowHashSuffix && !resolvedHash.empty())
        {
            baseId += ":" + resolvedHash.substr(0, std::min<std::size_t>(12, resolvedHash.size()));
        }
        resourceId = baseId;
        std::size_t suffix = 2;
        while (mResourceLibrary.HasResource(resourceType, resourceId))
        {
            resourceId = baseId + "-" + std::to_string(suffix++);
        }
    }

    LibraryResource resource;
    if (auto existing = mResourceLibrary.LookupResource(resourceType, resourceId))
    {
        resource = *existing;
    }
    else if (!allowCreate)
    {
        error = "Resource not found";
        return std::nullopt;
    }

    resource.type = resourceType;
    resource.id = resourceId;
    const std::string resolvedName =
        !name.empty() ? name : (!resource.name.empty() ? resource.name : resolvedPath.stem().string());
    const std::string resolvedCategory =
        !category.empty() ? category : (!resource.category.empty() ? resource.category : std::string{"Local"});
    resource.name = resolvedName.empty() ? resourceId : resolvedName;
    resource.category = resolvedCategory;
    if (!description.empty() || resource.description.empty())
    {
        resource.description = description;
    }
    resource.filePath = resolvedPath;
    resource.hash = resolvedHash;
    upsertMetadata(resource);
    resource.metadata["sourceFileName"] = resolvedPath.filename().string();
    if (payload.contains("tags"))
    {
        resource.tags.clear();
        if (tagsPayload.is_array())
        {
            for (const auto& tagValue : tagsPayload)
            {
                if (!tagValue.is_string())
                {
                    continue;
                }
                const auto tag = tagValue.get<std::string>();
                if (!tag.empty())
                {
                    resource.tags.push_back(tag);
                }
            }
        }
    }

    // Extract all NAM metadata fields from the model file header.
    if (resourceType == "nam")
    {
        EnrichNamResourceMetadata(resource, resolvedPath);
    }

    resource.category = ResolveResourceLibraryCategory(resource, resource.category);

    if (resourceType == "plugin")
    {
        const std::string pluginName = payloadPluginName.empty()
                                           ? (resource.metadata.contains(kHostedPluginNameConfigKey)
                                                  ? resource.metadata[kHostedPluginNameConfigKey]
                                                  : resolvedPath.stem().string())
                                           : payloadPluginName;
        if (!pluginName.empty())
        {
            resource.metadata[kHostedPluginNameConfigKey] = pluginName;
        }

        const std::string pluginManufacturer = payloadPluginManufacturer.empty()
                                                   ? (resource.metadata.contains(kHostedPluginManufacturerConfigKey)
                                                          ? resource.metadata[kHostedPluginManufacturerConfigKey]
                                                          : std::string{})
                                                   : payloadPluginManufacturer;
        if (!pluginManufacturer.empty())
        {
            resource.metadata[kHostedPluginManufacturerConfigKey] = pluginManufacturer;
        }

        std::string pluginStableId = payloadPluginStableId.empty()
                                         ? (resource.metadata.contains(kHostedPluginStableIdConfigKey)
                                                ? resource.metadata[kHostedPluginStableIdConfigKey]
                                                : BuildHostedPluginStableId(pluginManufacturer, pluginName))
                                         : payloadPluginStableId;
        pluginStableId = NormalizeHostedPluginIdentityToken(pluginStableId);
        if (!pluginStableId.empty())
        {
            resource.metadata[kHostedPluginStableIdConfigKey] = pluginStableId;
        }

        if (!resource.metadata.contains(kHostedPluginFormatConfigKey) ||
            resource.metadata[kHostedPluginFormatConfigKey].empty())
        {
            const std::string inferredFormat = InferPluginFormatFromPath(resolvedPath);
            if (!inferredFormat.empty())
            {
                resource.metadata[kHostedPluginFormatConfigKey] = inferredFormat;
            }
        }
    }

    AppendUserLibraryResource(resource);
    return resource;
}

void PluginController::HandleSaveLocalLibraryResourceRequest(const nlohmann::json& payload)
{
    std::string error;
    auto saved = SaveLocalLibraryResource(payload, error, true);
    if (!saved)
    {
        ReportErrorToUI("Local resource save failed", error);
        SendMessageToUI(nlohmann::json{
            {"type", "resourceImportFailed"}, {"message", "Local resource save failed"}, {"detail", error}}
                            .dump());
        return;
    }

    if (payload.contains("nodeId") && payload["nodeId"].is_string())
    {
        nlohmann::json updatePayload;
        updatePayload["nodeId"] = payload.value("nodeId", "");
        updatePayload["resourceType"] = saved->type;
        updatePayload["resourceId"] = saved->id;
        if (payload.contains("resourceIndex"))
        {
            updatePayload["resourceIndex"] = payload["resourceIndex"];
        }
        if (payload.contains("exposedResourceId"))
        {
            updatePayload["exposedResourceId"] = payload["exposedResourceId"];
        }
        HandleUpdateNodeResourceRequest(updatePayload);
    }

    if (!mResourceLibrary.HasResource(saved->type, saved->id))
    {
        BroadcastState();
        return;
    }

    BroadcastState();
    TouchSharedSyncState({"resourceLibrary"});
    nlohmann::json msg;
    msg["type"] = "resourceImported";
    msg["resourceType"] = saved->type;
    msg["id"] = saved->id;
    msg["name"] = saved->name;
    msg["filePath"] = util::PathToUtf8(saved->filePath);
    SendMessageToUI(msg.dump());
}

void PluginController::HandleRemoveLocalLibraryResourceRequest(const nlohmann::json& payload)
{
    const std::string resourceType = payload.value("resourceType", "");
    std::string resourceId = payload.value("resourceId", "");
    const std::string filePath = payload.value("filePath", "");

    if (resourceType.empty())
    {
        return;
    }

    // Resolve the id by file path when only a path was provided (e.g. folder browser).
    if (resourceId.empty() && !filePath.empty())
    {
        const auto normalize = [](std::string value) {
            std::replace(value.begin(), value.end(), '\\', '/');
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return value;
        };
        const std::string target = normalize(filePath);
        for (const auto& resource : mResourceLibrary.GetAllResources())
        {
            if (resource.type != resourceType)
            {
                continue;
            }
            if (normalize(resource.filePath.string()) == target)
            {
                resourceId = resource.id;
                break;
            }
        }
    }

    if (resourceId.empty() || !mResourceLibrary.HasResource(resourceType, resourceId))
    {
        return;
    }

    RemoveUserLibraryResource(resourceType, resourceId);
    BroadcastState();
    TouchSharedSyncState({"resourceLibrary"});

    SendMessageToUI(
        nlohmann::json{{"type", "resourceRemoved"}, {"resourceType", resourceType}, {"id", resourceId}}.dump());
}

void PluginController::HandleDeleteLibraryResourceRequest(const nlohmann::json& payload)
{
    const std::string resourceType = payload.value("resourceType", "");
    const std::string resourceId = payload.value("resourceId", "");
    if (resourceType.empty() || resourceId.empty())
    {
        SendMessageToUI(nlohmann::json{
            {"type", "resourceDeleteFailed"}, {"message", "Resource delete failed"}, {"detail", "Missing resource id"}}
                            .dump());
        return;
    }

    const auto resourceOpt = mResourceLibrary.LookupResource(resourceType, resourceId);
    if (!resourceOpt)
    {
        SendMessageToUI(nlohmann::json{
            {"type", "resourceDeleteFailed"}, {"message", "Resource delete failed"}, {"detail", "Resource not found"}}
                            .dump());
        return;
    }

    const auto firstUsingPreset = FindFirstPresetUsingResource(resourceType, resourceId);
    if (firstUsingPreset.has_value())
    {
        SendMessageToUI(nlohmann::json{{"type", "resourceDeleteFailed"},
                                       {"message", "Resource is in use"},
                                       {"detail", "Used by preset: " + *firstUsingPreset},
                                       {"resourceType", resourceType},
                                       {"id", resourceId},
                                       {"presetName", *firstUsingPreset}}
                            .dump());
        return;
    }

    const auto settingsResourcesDir = GetEffectiveSettingsDirectory() / "resources" / "content";
    const auto isUnderDirectory = [](const std::filesystem::path& candidate, const std::filesystem::path& base) {
        std::error_code ec;
        auto nc = std::filesystem::weakly_canonical(candidate, ec);
        if (ec)
        {
            return false;
        }
        auto nb = std::filesystem::weakly_canonical(base, ec);
        if (ec)
        {
            return false;
        }
        auto bi = nb.begin();
        auto ci = nc.begin();
        for (; bi != nb.end(); ++bi, ++ci)
        {
            if (ci == nc.end() || *bi != *ci)
            {
                return false;
            }
        }
        return true;
    };

    const std::filesystem::path resourcePath = resourceOpt->filePath;
    const bool shouldDeleteFile = !resourcePath.empty() && isUnderDirectory(resourcePath, settingsResourcesDir);
    if (shouldDeleteFile)
    {
        std::error_code ec;
        std::filesystem::remove(resourcePath, ec);
        if (ec)
        {
            SendMessageToUI(nlohmann::json{{"type", "resourceDeleteFailed"},
                                           {"message", "Resource delete failed"},
                                           {"detail", "Failed to delete stored file: " + resourcePath.string()}}
                                .dump());
            return;
        }
    }

    RemoveUserLibraryResource(resourceType, resourceId);
    BroadcastState();
    TouchSharedSyncState({"resourceLibrary"});

    SendMessageToUI(
        nlohmann::json{{"type", "resourceRemoved"}, {"resourceType", resourceType}, {"id", resourceId}}.dump());
}

std::optional<std::string> PluginController::FindFirstPresetUsingResource(const std::string& resourceType,
                                                                          const std::string& resourceId) const
{
    const auto graphUsesResource = [&](const SignalGraph& graph) -> bool {
        for (const auto& node : graph.nodes)
        {
            for (const auto& ref : node.resources)
            {
                if (ref.IsLibraryRef() && ref.resourceType == resourceType && ref.resourceId == resourceId)
                {
                    return true;
                }
            }
        }
        return false;
    };

    const auto presetUsesResource = [&](const Preset& preset) -> bool {
        if (graphUsesResource(preset.graph))
        {
            return true;
        }
        for (const auto& scene : preset.scenes)
        {
            if (graphUsesResource(scene.graph))
            {
                return true;
            }
        }
        return false;
    };

    const auto presetDisplayName = [](const Preset& preset) -> std::string {
        if (!preset.name.empty())
        {
            return preset.name;
        }
        if (!preset.id.empty())
        {
            return preset.id;
        }
        return "Unnamed preset";
    };

    // Check active preset
    if (mActivePreset && presetUsesResource(*mActivePreset))
    {
        return presetDisplayName(*mActivePreset);
    }

    // Consult the cached disk/archive index (built once, reused until presets change).
    EnsureResourceUsageDiskIndex();
    const std::string key = resourceType + ":" + resourceId;
    const auto it = mResourceUsageDiskIndex.find(key);
    if (it != mResourceUsageDiskIndex.end())
    {
        return it->second;
    }

    return std::nullopt;
}

void PluginController::EnsureResourceUsageDiskIndex() const
{
    if (mResourceUsageDiskIndexValid)
    {
        return;
    }

    mResourceUsageDiskIndex.clear();

    const auto indexPreset = [this](const Preset& preset) {
        std::string displayName = preset.name;
        if (displayName.empty())
        {
            displayName = !preset.id.empty() ? preset.id : "Unnamed preset";
        }

        const auto indexGraph = [&](const SignalGraph& graph) {
            for (const auto& node : graph.nodes)
            {
                for (const auto& ref : node.resources)
                {
                    if (!ref.IsLibraryRef())
                    {
                        continue;
                    }
                    const std::string key = ref.resourceType + ":" + ref.resourceId;
                    // Preserve first-found priority (user > factory > archive).
                    mResourceUsageDiskIndex.emplace(key, displayName);
                }
            }
        };

        indexGraph(preset.graph);
        for (const auto& scene : preset.scenes)
        {
            indexGraph(scene.graph);
        }
    };

    // User presets first so they win ties.
    for (const auto& preset : LoadAllUserPresets())
    {
        indexPreset(preset);
    }

    // Factory presets next.
    {
        const auto factoryDir = ResolveFactoryPresetDirectory(mHost, mResourceRoot);
        if (std::filesystem::exists(factoryDir))
        {
            const auto factoryPresets = PresetStorage::LoadAllFromDirectory(factoryDir);
            for (const auto& preset : factoryPresets)
            {
                indexPreset(preset);
            }
        }
    }

    // Factory archive presets last.
    for (const auto& [_, preset] : mFactoryArchivePresets)
    {
        indexPreset(preset);
    }

    mResourceUsageDiskIndexValid = true;
}

void PluginController::InvalidateResourceUsageIndex()
{
    mResourceUsageDiskIndexValid = false;
    mResourceUsageDiskIndex.clear();
}

void PluginController::HandleQueryResourceUsageRequest(const nlohmann::json& payload)
{
    const std::string resourceType = payload.value("resourceType", "");
    const std::string resourceId = payload.value("resourceId", "");

    if (resourceType.empty() || resourceId.empty())
    {
        SendMessageToUI(nlohmann::json{
            {"type", "resourceUsageInfo"}, {"resourceType", resourceType}, {"id", resourceId}, {"inUse", false}}
                            .dump());
        return;
    }

    const auto presetName = FindFirstPresetUsingResource(resourceType, resourceId);
    SendMessageToUI(nlohmann::json{{"type", "resourceUsageInfo"},
                                   {"resourceType", resourceType},
                                   {"id", resourceId},
                                   {"inUse", presetName.has_value()},
                                   {"presetName", presetName ? *presetName : ""}}
                        .dump());
}

void PluginController::HandleUpdateLibraryResourceRequest(const nlohmann::json& payload)
{
    const std::string resourceType = payload.value("resourceType", "");
    const std::string resourceId = payload.value("resourceId", "");
    if (resourceType.empty() || resourceId.empty())
    {
        ReportErrorToUI("Resource update failed", "Missing resource id");
        return;
    }

    auto existing = mResourceLibrary.LookupResource(resourceType, resourceId);
    if (!existing)
    {
        ReportErrorToUI("Resource update failed", "Resource not found");
        return;
    }

    LibraryResource updated = *existing;
    const std::string fileNameValue = payload.value("fileName", "");
    const std::string inlineData = payload.value("data", "");
    const auto settingsResourcesDir = GetEffectiveSettingsDirectory() / "resources" / "content";
    const auto isUnderDirectory = [](const std::filesystem::path& candidate, const std::filesystem::path& base) {
        std::error_code ec;
        auto nc = std::filesystem::weakly_canonical(candidate, ec);
        if (ec)
        {
            return false;
        }
        auto nb = std::filesystem::weakly_canonical(base, ec);
        if (ec)
        {
            return false;
        }
        auto bi = nb.begin();
        auto ci = nc.begin();
        for (; bi != nb.end(); ++bi, ++ci)
        {
            if (ci == nc.end() || *bi != *ci)
            {
                return false;
            }
        }
        return true;
    };
    if (payload.contains("name"))
    {
        updated.name = payload.value("name", updated.name);
    }
    if (payload.contains("category"))
    {
        updated.category = payload.value("category", updated.category);
    }
    if (payload.contains("description"))
    {
        updated.description = payload.value("description", updated.description);
    }
    if (payload.contains("tags"))
    {
        updated.tags.clear();
        if (payload["tags"].is_array())
        {
            for (const auto& tagValue : payload["tags"])
            {
                if (!tagValue.is_string())
                {
                    continue;
                }
                const auto tag = tagValue.get<std::string>();
                if (!tag.empty())
                {
                    updated.tags.push_back(tag);
                }
            }
        }
    }
    if (payload.contains("metadata") && payload["metadata"].is_object())
    {
        updated.metadata.clear();
        for (const auto& entry : payload["metadata"].items())
        {
            const auto& value = entry.value();
            if (value.is_string())
            {
                updated.metadata[entry.key()] = value.get<std::string>();
            }
            else if (value.is_number())
            {
                updated.metadata[entry.key()] = value.dump();
            }
            else if (value.is_boolean())
            {
                updated.metadata[entry.key()] = value.get<bool>() ? "true" : "false";
            }
        }
        if (!updated.metadata.contains("provider"))
        {
            updated.metadata["provider"] =
                existing->metadata.contains("provider") ? existing->metadata.at("provider") : kLocalResourceProvider;
        }
    }

    if (payload.contains("filePath"))
    {
        const std::string filePathValue = payload.value("filePath", "");
        if (!filePathValue.empty())
        {
            std::filesystem::path updatedPath(filePathValue);
            // Same normalization as SaveLocalLibraryResource: a plugin is stored
            // under its bundle root whichever route the path arrived by.
            if (resourceType == "plugin")
            {
                updatedPath = guitarfx::pluginpath::ResolvePluginBundlePath(updatedPath);
            }
            if (!std::filesystem::exists(updatedPath))
            {
                ReportErrorToUI("Resource update failed", "Selected file does not exist");
                return;
            }
            updated.filePath = updatedPath;
            updated.hash = ShouldHashResourceFile(updatedPath) ? mHasher.HashFile(updatedPath) : std::string{};
            updated.metadata["sourceFileName"] = updatedPath.filename().string();
        }
    }

    if (!inlineData.empty())
    {
        const std::vector<std::uint8_t> decodedBytes = util::DecodeBase64(inlineData);
        if (decodedBytes.empty())
        {
            ReportErrorToUI("Resource update failed", "Invalid file data");
            return;
        }

        std::filesystem::path targetPath = updated.filePath;
        const bool hasExistingPath = !targetPath.empty();
        if (!hasExistingPath)
        {
            ReportErrorToUI("Resource update failed", "Existing resource file path is missing");
            return;
        }

        const auto extensionForType = [&](const std::string& type) {
            if (type == "ir")
            {
                return std::string{".wav"};
            }
            if (type == "wasm")
            {
                return std::string{".wasm"};
            }
            if (type == "nam")
            {
                return std::string{".nam"};
            }
            return std::string{".bin"};
        };

        if (!fileNameValue.empty())
        {
            std::string resolvedName = util::SanitizeFilename(fileNameValue);
            if (resolvedName.empty())
            {
                resolvedName = "resource" + extensionForType(resourceType);
            }
            if (resolvedName.find('.') == std::string::npos)
            {
                resolvedName += extensionForType(resourceType);
            }
            targetPath = updated.filePath.parent_path() / resolvedName;
        }

        [[maybe_unused]] const auto ensuredTargetDir = mFileSystem.EnsureDirectory(targetPath.parent_path());
        if (!WriteFile(targetPath, decodedBytes))
        {
            ReportErrorToUI("Resource update failed", "Failed to write replacement file");
            return;
        }

        const std::filesystem::path previousPath = updated.filePath;
        updated.filePath = targetPath;
        updated.hash = mHasher.HashFile(targetPath);
        updated.metadata["sourceFileName"] = targetPath.filename().string();

        if (previousPath != targetPath && !previousPath.empty() && isUnderDirectory(previousPath, settingsResourcesDir))
        {
            std::error_code ec;
            std::filesystem::remove(previousPath, ec);
        }
    }

    if (resourceType == "nam")
    {
        EnrichNamResourceMetadata(updated, updated.filePath);
    }

    mResourceLibrary.UpdateResource(resourceType, resourceId, updated);
    AppendUserLibraryResource(updated);
    BroadcastState();
    TouchSharedSyncState({"resourceLibrary"});
    SendMessageToUI(nlohmann::json{{"type", "resourceImported"},
                                   {"resourceType", updated.type},
                                   {"id", updated.id},
                                   {"name", updated.name},
                                   {"filePath", util::PathToUtf8(updated.filePath)}}
                        .dump());
}

void PluginController::HandleBrowseLibraryResourcePathRequest(const nlohmann::json& payload)
{
    const std::string resourceType = payload.value("resourceType", "");
    const std::string resourceId = payload.value("resourceId", "");
    if (resourceType.empty() || resourceId.empty())
    {
        return;
    }

    mHost.BrowseFileAsync(ResolveBrowseFileType(resourceType), "Select Local Resource",
                          [this, payload, resourceType, resourceId](const BrowseFileResult& result) {
                              if (!result.success)
                              {
                                  return;
                              }

                              nlohmann::json updatePayload = payload;
                              updatePayload["resourceType"] = resourceType;
                              updatePayload["resourceId"] = resourceId;
                              updatePayload["filePath"] = util::PathToUtf8(result.path);
                              HandleUpdateLibraryResourceRequest(updatePayload);
                          });
}

void PluginController::HandleBrowseResourceFolderRequest()
{
    mHost.BrowseFileAsync(BrowseFileType::Folder, "Select Resource Folder", [this](const BrowseFileResult& result) {
        nlohmann::json msg;
        msg["type"] = "resourceFolderPicked";
        std::error_code ec;
        if (result.success && std::filesystem::is_directory(result.path, ec) && !ec)
        {
            msg["success"] = true;
            msg["path"] = result.path.generic_string();
            const auto leaf = result.path.filename();
            msg["name"] = leaf.empty() ? result.path.generic_string() : leaf.string();
        }
        else
        {
            msg["success"] = false;
        }
        SendMessageToUI(msg.dump());
    });
}

void PluginController::HandleListResourceFolderRequest(const nlohmann::json& payload)
{
    const std::string rawPath = payload.value("path", "");

    // Snapshot existing library (filePath -> id) on the message thread. This
    // uses the lightweight path index (two string copies per entry, no metadata
    // maps, no filesystem access, no canonicalization) so even a very large
    // library can never freeze the UI. The worker normalizes/matches off-thread.
    std::vector<std::pair<std::string, std::string>> libraryPaths = mResourceLibrary.GetResourcePathIndex();

    // Supersede any in-flight scan, then spawn a detached worker. We never join
    // on the message thread (which could block on a slow filesystem); detached
    // workers observe the bumped generation and exit quickly, and the
    // destructor waits for all of them via mFolderScanDoneCv.
    const std::uint64_t generation = mFolderScanGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
    mActiveFolderScans.fetch_add(1, std::memory_order_relaxed);

    try
    {
        std::thread worker([this, rawPath, libraryPaths = std::move(libraryPaths), generation]() mutable {
            const std::string requestedPath = rawPath;
            try
            {
                ScanResourceFolderWorker(std::move(rawPath), std::move(libraryPaths), generation);
            }
            catch (const std::exception& exception)
            {
                if (mFolderScanGeneration.load(std::memory_order_relaxed) == generation)
                {
                    SendMessageToUI(
                        nlohmann::json{{"type", "resourceFolderListingFailed"},
                                       {"path", requestedPath},
                                       {"message", std::string{"Unable to scan folder: "} + exception.what()}}
                            .dump());
                }
            }
            catch (...)
            {
                if (mFolderScanGeneration.load(std::memory_order_relaxed) == generation)
                {
                    SendMessageToUI(nlohmann::json{{"type", "resourceFolderListingFailed"},
                                                   {"path", requestedPath},
                                                   {"message", "Unable to scan folder"}}
                                        .dump());
                }
            }
            {
                std::lock_guard<std::mutex> lock(mFolderScanDoneMutex);
                mActiveFolderScans.fetch_sub(1, std::memory_order_relaxed);
            }
            mFolderScanDoneCv.notify_all();
        });
        worker.detach();
    }
    catch (const std::exception&)
    {
        // Spawning failed: undo the active-scan bump and report the error so the
        // UI doesn't sit on "Loading…" forever. Never let the exception escape
        // into the WebView native-function callback (which would skip its
        // completion handler and can wedge the message pump).
        {
            std::lock_guard<std::mutex> lock(mFolderScanDoneMutex);
            mActiveFolderScans.fetch_sub(1, std::memory_order_relaxed);
        }
        mFolderScanDoneCv.notify_all();
        SendMessageToUI(nlohmann::json{
            {"type", "resourceFolderListingFailed"}, {"path", rawPath}, {"message", "Unable to start folder scan"}}
                            .dump());
    }
}

void PluginController::ScanResourceFolderWorker(std::string requestPath,
                                                std::vector<std::pair<std::string, std::string>> libraryPaths,
                                                std::uint64_t generation)
{
    const auto superseded = [this, generation]() {
        return mFolderScanGeneration.load(std::memory_order_relaxed) != generation;
    };

    // Pure-lexical, no-filesystem normalization: lowercase + forward slashes +
    // collapse of "."/".."/redundant separators. Unlike weakly_canonical this
    // never touches the disk, so it can't stall on a slow/disconnected drive.
    const auto normalizePath = [](const std::filesystem::path& p) -> std::string {
        std::string s = util::PathToUtf8(p.lexically_normal());
        if (!s.empty() && s.back() == '/')
        {
            s.pop_back();
        }
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };

    const auto classify = [](const std::filesystem::path& p) -> std::string {
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".nam")
        {
            return std::string{"nam"};
        }
        if (ext == ".wav" || ext == ".ir" || ext == ".aif" || ext == ".aiff" || ext == ".flac")
        {
            return std::string{"ir"};
        }
        return std::string{};
    };

    // Validate the requested path here (off the message thread) so even a slow
    // exists()/is_directory() probe on a bad drive never freezes the UI.
    if (requestPath.empty())
    {
        if (!superseded())
        {
            SendMessageToUI(
                nlohmann::json{{"type", "resourceFolderListingFailed"}, {"message", "Missing folder path"}}.dump());
        }
        return;
    }

    const std::filesystem::path dir = util::PathFromUtf8(requestPath);
    std::error_code dec;
    if (!std::filesystem::is_directory(dir, dec) || dec)
    {
        if (!superseded())
        {
            SendMessageToUI(nlohmann::json{
                {"type", "resourceFolderListingFailed"}, {"path", requestPath}, {"message", "Folder not found"}}
                                .dump());
        }
        return;
    }

    // Build the (normalized path -> library id) lookup off-thread from the cheap
    // snapshot captured on the message thread.
    std::map<std::string, std::string> libraryIdByPath;
    for (auto& entry : libraryPaths)
    {
        if (superseded())
        {
            return;
        }
        libraryIdByPath.emplace(normalizePath(std::filesystem::path(entry.first)), std::move(entry.second));
    }

    std::error_code ec;
    constexpr std::size_t kMaxEntries = 5000;
    std::vector<nlohmann::json> dirs;
    std::vector<nlohmann::json> files;
    bool truncated = false;

    // Parallel list of (filesystem path, resourceType) for the second (metadata)
    // pass. Kept separate so the cheap listing can be sent before any file is
    // opened and parsed.
    struct PendingFile
    {
        std::filesystem::path path;
        std::string resourceType;
    };

    std::vector<PendingFile> pendingMetadata;

    // ── Phase 1: enumerate the immediate level only (no file content reads) ──
    // directory_iterator is intentionally non-recursive: we list just the folder
    // the user navigated into. This is cheap even for large folders, so the UI
    // gets a populated listing almost immediately.
    std::filesystem::directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec)
    {
        const std::string detail = ec.message();
        if (!superseded())
        {
            SendMessageToUI(nlohmann::json{
                {"type", "resourceFolderListingFailed"},
                {"path", requestPath},
                {"message", detail.empty() ? "Unable to read folder" : "Unable to read folder: " + detail}}
                                .dump());
        }
        return;
    }

    for (; it != std::filesystem::directory_iterator(); it.increment(ec))
    {
        if (ec)
        {
            break; // Stop on iteration error rather than throwing on a detached thread.
        }
        if (superseded())
        {
            return;
        }

        const auto& entry = *it;
        if (dirs.size() + files.size() >= kMaxEntries)
        {
            truncated = true;
            break;
        }

        std::error_code eec;
        const auto& entryPath = entry.path();
        if (entry.is_directory(eec) && !eec)
        {
            dirs.push_back(nlohmann::json{{"name", util::PathToUtf8(entryPath.filename())},
                                          {"path", util::PathToUtf8(entryPath)}});
            continue;
        }
        if (!entry.is_regular_file(eec) || eec)
        {
            continue;
        }

        const std::string resourceType = classify(entryPath);
        if (resourceType.empty())
        {
            continue;
        }

        nlohmann::json file;
        file["name"] = util::PathToUtf8(entryPath.filename());
        file["path"] = util::PathToUtf8(entryPath);
        file["resourceType"] = resourceType;

        std::error_code sec;
        const auto sizeBytes = std::filesystem::file_size(entryPath, sec);
        file["sizeBytes"] = sec ? 0 : static_cast<std::uint64_t>(sizeBytes);

        const auto libIt = libraryIdByPath.find(normalizePath(entryPath));
        if (libIt != libraryIdByPath.end())
        {
            file["alreadyInLibrary"] = true;
            file["libraryId"] = libIt->second;
        }
        else
        {
            file["alreadyInLibrary"] = false;
        }

        // Metadata is filled in later (Phase 2) so the listing isn't blocked.
        file["metadata"] = nlohmann::json::object();
        file["metadataPending"] = true;
        files.push_back(std::move(file));
        pendingMetadata.push_back({entryPath, resourceType});
    }

    if (superseded())
    {
        return;
    }

    const auto byName = [](const nlohmann::json& a, const nlohmann::json& b) {
        std::string an = a.value("name", "");
        std::string bn = b.value("name", "");
        std::transform(an.begin(), an.end(), an.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(bn.begin(), bn.end(), bn.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return an < bn;
    };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);

    const auto parentPath = dir.parent_path();
    std::string parentStr;
    if (!parentPath.empty() && parentPath != dir)
    {
        parentStr = util::PathToUtf8(parentPath);
    }

    const std::string folderPath = util::PathToUtf8(dir);

    nlohmann::json msg;
    msg["type"] = "resourceFolderListing";
    msg["path"] = folderPath;
    msg["parent"] = parentStr;
    const auto leaf = dir.filename();
    msg["name"] = leaf.empty() ? folderPath : util::PathToUtf8(leaf);
    msg["dirs"] = dirs;
    msg["files"] = files;
    msg["truncated"] = truncated;
    msg["metadataPending"] = !pendingMetadata.empty();

    if (superseded())
    {
        return;
    }
    SendMessageToUI(msg.dump());

    // ── Phase 2: parse per-file metadata and stream it back in batches ──
    // This is the expensive part (each file is opened/parsed). It runs after the
    // listing is already on screen, so badges/details fill in progressively
    // without ever blocking the UI.
    constexpr std::size_t kMetadataBatchSize = 40;
    nlohmann::json batch = nlohmann::json::array();

    const auto flushBatch = [&]() {
        if (batch.empty())
        {
            return true;
        }
        if (superseded())
        {
            return false;
        }
        SendMessageToUI(
            nlohmann::json{{"type", "resourceFolderMetadata"}, {"path", folderPath}, {"items", batch}}.dump());
        batch = nlohmann::json::array();
        return true;
    };

    for (const auto& pending : pendingMetadata)
    {
        if (superseded())
        {
            return;
        }

        nlohmann::json metadata = nlohmann::json::object();
        if (pending.resourceType == "nam")
        {
            LibraryResource temp;
            EnrichNamResourceMetadata(temp, pending.path);
            for (const auto& [key, value] : temp.metadata)
            {
                if (!value.empty())
                {
                    metadata[key] = value;
                }
            }
        }
        else
        {
            const util::WavHeaderInfo wav = util::ProbeWavHeader(pending.path);
            if (wav.valid)
            {
                if (wav.sampleRate > 0)
                {
                    metadata["sampleRate"] = std::to_string(wav.sampleRate);
                }
                if (wav.channels > 0)
                {
                    metadata["channels"] = std::to_string(wav.channels);
                }
                if (wav.bitsPerSample > 0)
                {
                    metadata["bitsPerSample"] = std::to_string(wav.bitsPerSample);
                }
                if (wav.durationSec > 0.0)
                {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%.3f", wav.durationSec);
                    metadata["durationSec"] = std::string(buf);
                }
            }
        }

        batch.push_back(nlohmann::json{{"path", util::PathToUtf8(pending.path)}, {"metadata", std::move(metadata)}});

        if (batch.size() >= kMetadataBatchSize)
        {
            if (!flushBatch())
            {
                return;
            }
        }
    }

    flushBatch();
}

void PluginController::HandleImportToneSharingPackRequest(const nlohmann::json& payload)
{
    const std::string packId = payload.value("packId", "");
    const std::string data = payload.value("data", "");
    std::string fileName = payload.value("fileName", "");

    if (data.empty())
    {
        SendMessageToUI(
            nlohmann::json{{"type", "toneSharingPackImportFailed"}, {"message", "Missing pack data"}}.dump());
        return;
    }

    if (fileName.empty())
    {
        fileName = packId.empty() ? "tone-sharing-pack.zip" : ("tone-sharing-pack-" + packId + ".zip");
    }

    fileName = util::SanitizeFilename(fileName);
    if (fileName.find('.') == std::string::npos)
    {
        fileName += ".zip";
    }

    const std::vector<std::uint8_t> bytes = util::DecodeBase64(data);
    if (bytes.empty())
    {
        SendMessageToUI(
            nlohmann::json{{"type", "toneSharingPackImportFailed"}, {"message", "Invalid pack payload"}}.dump());
        return;
    }

    const auto settingsDir = mFileSystem.ResolveSettingsDirectory();
    const auto importsDir = settingsDir / "imports" / "tone-sharing";
    [[maybe_unused]] const auto ensuredImportsDir = mFileSystem.EnsureDirectory(importsDir);

    auto targetPath = importsDir / fileName;
    if (!WriteFile(targetPath, bytes))
    {
        SendMessageToUI(
            nlohmann::json{{"type", "toneSharingPackImportFailed"}, {"message", "Failed to write imported pack"}}
                .dump());
        return;
    }

    nlohmann::json result;
    result["type"] = "toneSharingPackImported";
    result["packId"] = packId;
    result["fileName"] = fileName;
    result["path"] = targetPath.generic_string();
    result["byteSize"] = bytes.size();
    SendMessageToUI(result.dump());

    AppendSessionLog("Imported tone sharing pack " + (packId.empty() ? std::string{"(unknown)"} : packId) + " -> " +
                     targetPath.generic_string());
}

void PluginController::HandleDeleteImportedToneSharingPackRequest(const nlohmann::json& payload)
{
    const std::string rawPath = payload.value("path", "");
    if (rawPath.empty())
    {
        SendMessageToUI(
            nlohmann::json{{"type", "toneSharingPackDeleteFailed"}, {"message", "Missing pack path"}}.dump());
        return;
    }

    const auto settingsDir = mFileSystem.ResolveSettingsDirectory();
    const auto importsDir = settingsDir / "imports" / "tone-sharing";
    const auto requestedPath = std::filesystem::path(rawPath);

    std::error_code ec;
    const auto canonicalImports = std::filesystem::weakly_canonical(importsDir, ec);
    if (ec)
    {
        SendMessageToUI(
            nlohmann::json{{"type", "toneSharingPackDeleteFailed"}, {"message", "Unable to resolve import directory"}}
                .dump());
        return;
    }

    ec.clear();
    const auto canonicalRequested = std::filesystem::weakly_canonical(requestedPath, ec);
    if (ec)
    {
        SendMessageToUI(
            nlohmann::json{{"type", "toneSharingPackDeleteFailed"}, {"message", "Imported pack path is invalid"}}
                .dump());
        return;
    }

    auto requestedIt = canonicalRequested.begin();
    bool insideImports = true;
    for (auto importsIt = canonicalImports.begin(); importsIt != canonicalImports.end(); ++importsIt)
    {
        if (requestedIt == canonicalRequested.end() || *requestedIt != *importsIt)
        {
            insideImports = false;
            break;
        }
        ++requestedIt;
    }

    if (!insideImports)
    {
        SendMessageToUI(nlohmann::json{{"type", "toneSharingPackDeleteFailed"},
                                       {"message", "Refusing to delete outside tone-sharing imports"}}
                            .dump());
        return;
    }

    ec.clear();
    const bool removed = std::filesystem::remove(canonicalRequested, ec);
    if (ec)
    {
        SendMessageToUI(
            nlohmann::json{{"type", "toneSharingPackDeleteFailed"}, {"message", "Failed to delete imported pack"}}
                .dump());
        return;
    }

    nlohmann::json result;
    result["type"] = "toneSharingPackDeleted";
    result["path"] = canonicalRequested.generic_string();
    result["removed"] = removed;
    SendMessageToUI(result.dump());

    AppendSessionLog("Deleted imported tone sharing pack -> " + canonicalRequested.generic_string());
}

void PluginController::HandlePreviewRemoteResourceRequest(const nlohmann::json& payload)
{
    const std::string resourceType = payload.value("resourceType", "");
    const std::string tempResourceId = payload.value("tempResourceId", "");
    const std::string nodeId = payload.value("nodeId", "");
    const int resourceIndex = payload.value("resourceIndex", 0);
    const std::string data = payload.value("data", "");
    const bool isZip = payload.value("isZip", false);

    if (resourceType.empty() || data.empty())
    {
        AppendSessionLog("Preview failed: missing resource type or data");
        return;
    }

    const std::vector<std::uint8_t> bytes = util::DecodeBase64(data);
    if (bytes.empty())
    {
        AppendSessionLog("Preview failed: invalid base64 payload");
        return;
    }

    const auto tempDir = mFileSystem.ResolveSettingsDirectory() / "temp";
    [[maybe_unused]] const auto ensuredTempDir = mFileSystem.EnsureDirectory(tempDir);

    const std::string extension = resourceType == "ir" ? ".wav" : ".nam";
    std::filesystem::path tempPath =
        tempDir / ("preview_" + std::to_string(std::hash<std::string>{}(tempResourceId)) + extension);

    if (isZip)
    {
        if (!ExtractFirstResourceFromZip(bytes, resourceType, tempPath))
        {
            AppendSessionLog("Preview failed: no matching resource in zip");
            return;
        }
    }
    else
    {
        if (!WriteFile(tempPath, bytes))
        {
            AppendSessionLog("Preview failed: could not write temp file");
            return;
        }
    }

    mPreviewState.active = true;
    mPreviewState.nodeId = nodeId;
    mPreviewState.resourceIndex = resourceIndex;
    mPreviewState.resourceType = resourceType;
    mPreviewState.tempFilePath = tempPath;

    if (mActivePreset)
    {
        GraphNode* node = mActivePreset->graph.FindNode(nodeId);
        if (node && resourceIndex >= 0 && static_cast<size_t>(resourceIndex) < node->resources.size())
        {
            mPreviewState.originalResourceRef = node->resources[resourceIndex];
        }
    }

    if (!nodeId.empty())
    {
        nlohmann::json updatePayload;
        updatePayload["nodeId"] = nodeId;
        updatePayload["resourceType"] = resourceType;
        updatePayload["resourceId"] = "";
        updatePayload["filePath"] = util::PathToUtf8(tempPath);
        updatePayload["resourceIndex"] = resourceIndex;
        HandleUpdateNodeResourceRequest(updatePayload);
    }

    AppendSessionLog("Preview started: " + resourceType + " at " + tempPath.string());
}

void PluginController::HandleCancelPreviewResourceRequest(const nlohmann::json& payload)
{
    if (!mPreviewState.active)
    {
        return;
    }

    const bool restoreOriginal = payload.value("restoreOriginal", true);

    if (restoreOriginal && !mPreviewState.nodeId.empty() && mPreviewState.originalResourceRef.has_value())
    {
        const auto& original = mPreviewState.originalResourceRef.value();
        nlohmann::json updatePayload;
        updatePayload["nodeId"] = mPreviewState.nodeId;
        updatePayload["resourceType"] = mPreviewState.resourceType;
        updatePayload["resourceId"] = original.resourceId;
        updatePayload["filePath"] = util::PathToUtf8(original.filePath);
        updatePayload["resourceIndex"] = mPreviewState.resourceIndex;
        HandleUpdateNodeResourceRequest(updatePayload);
    }

    if (!mPreviewState.tempFilePath.empty())
    {
        std::error_code ec;
        std::filesystem::remove(mPreviewState.tempFilePath, ec);
    }

    mPreviewState = PreviewState{};
    AppendSessionLog("Preview cancelled");
}

void PluginController::HandleRequestResourceDataRequest(const nlohmann::json& payload)
{
    const std::string requestId = payload.value("requestId", "");
    const std::string resourceType = payload.value("resourceType", "");
    const std::string resourceId = payload.value("resourceId", "");

    if (requestId.empty() || resourceType.empty() || resourceId.empty())
    {
        SendMessageToUI(nlohmann::json{
            {"type", "resourceDataFailed"}, {"requestId", requestId}, {"message", "Missing resource request info"}}
                            .dump());
        return;
    }

    ResourceRef ref;
    ref.resourceType = resourceType;
    ref.resourceId = resourceId;
    const auto resolvedPath = ResolveResourceRef(ref);
    if (!resolvedPath || resolvedPath->empty())
    {
        SendMessageToUI(
            nlohmann::json{{"type", "resourceDataFailed"}, {"requestId", requestId}, {"message", "Resource not found"}}
                .dump());
        return;
    }

    std::ifstream input(*resolvedPath, std::ios::binary);
    if (!input)
    {
        SendMessageToUI(nlohmann::json{
            {"type", "resourceDataFailed"}, {"requestId", requestId}, {"message", "Failed to open resource file"}}
                            .dump());
        return;
    }

    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (data.empty())
    {
        SendMessageToUI(
            nlohmann::json{{"type", "resourceDataFailed"}, {"requestId", requestId}, {"message", "Resource file empty"}}
                .dump());
        return;
    }

    const std::string encoded = util::EncodeBase64(data);
    nlohmann::json response;
    response["type"] = "resourceData";
    response["requestId"] = requestId;
    response["resourceType"] = resourceType;
    response["resourceId"] = resourceId;
    response["fileName"] = resolvedPath->filename().string();
    response["data"] = encoded;
    SendMessageToUI(response.dump());
}

void PluginController::HandleSaveLibraryArchiveRequest(const nlohmann::json& payload)
{
    const std::string dataEncoded = payload.value("data", "");
    const std::string suggestedName = payload.value("fileName", "library.soundshed-library.zip");
    if (dataEncoded.empty())
    {
        SendMessageToUI(nlohmann::json{{"type", "libraryExportFailed"}, {"message", "Missing export data"}}.dump());
        return;
    }

    mHost.SaveFileAsync(
        BrowseFileType::ArchiveFile, "Save Library Export", suggestedName,
        [this, dataEncoded](const BrowseFileResult& result) {
            if (!result.success)
            {
                SendMessageToUI(nlohmann::json{{"type", "libraryExportFailed"}, {"message", "Save cancelled"}}.dump());
                return;
            }

            const auto decodedBytes = util::DecodeBase64(dataEncoded);
            if (decodedBytes.empty())
            {
                SendMessageToUI(
                    nlohmann::json{{"type", "libraryExportFailed"}, {"message", "Invalid export data"}}.dump());
                return;
            }

            if (!WriteFile(result.path, decodedBytes))
            {
                SendMessageToUI(
                    nlohmann::json{{"type", "libraryExportFailed"}, {"message", "Failed to save file"}}.dump());
                return;
            }

            SendMessageToUI(
                nlohmann::json{{"type", "libraryExportSaved"}, {"path", result.path.generic_string()}}.dump());
            AppendSessionLog("Library export saved: " + result.path.generic_string());
        });
}

void PluginController::HandleCleanupResourceLibraryRequest(const nlohmann::json& payload)
{
    const nlohmann::json resources = payload.value("resources", nlohmann::json::array());
    const std::string scope = payload.value("scope", "all");
    const bool removeFiles = payload.value("removeFiles", true);

    if (!resources.is_array())
    {
        ReportErrorToUI("Cleanup failed", "Missing resource list");
        return;
    }

    const auto settingsDir = mFileSystem.ResolveSettingsDirectory();
    const auto resourcesDir = settingsDir / "resources";
    const auto libraryDir = resourcesDir / "indexes";
    const auto libraryFile = libraryDir / "resources-index.json";
    const auto resourceFilesDir = resourcesDir / "content";

    nlohmann::json entries = nlohmann::json::array();
    if (std::filesystem::exists(libraryFile))
    {
        std::ifstream input(libraryFile);
        if (input)
        {
            nlohmann::json parsed;
            input >> parsed;
            if (parsed.is_array())
            {
                entries = std::move(parsed);
            }
        }
    }

    auto makeKey = [](const std::string& type, const std::string& id) { return type + ":" + id; };

    std::unordered_set<std::string> userKeys;
    for (const auto& e : entries)
    {
        const std::string t = e.value("type", ""), i = e.value("id", "");
        if (!t.empty() && !i.empty())
        {
            userKeys.insert(makeKey(t, i));
        }
    }

    std::unordered_set<std::string> usedKeys;
    auto addUsedPreset = [&](const Preset& preset) {
        for (const auto& n : preset.graph.nodes)
        {
            for (const auto& r : n.resources)
            {
                if (r.IsLibraryRef())
                {
                    usedKeys.insert(makeKey(r.resourceType, r.resourceId));
                }
            }
        }
    };

    if (mActivePreset)
    {
        addUsedPreset(*mActivePreset);
    }
    for (const auto& p : LoadAllUserPresets())
    {
        addUsedPreset(p);
    }

    if (mBlendLibrary.is_array())
    {
        for (const auto& blend : mBlendLibrary)
        {
            if (blend.is_object())
            {
                for (const auto& mid : blend.value("models", nlohmann::json::array()))
                {
                    if (mid.is_string())
                    {
                        usedKeys.insert(makeKey("nam", mid.get<std::string>()));
                    }
                }
            }
        }
    }

    auto isScopeMatch = [&](const std::string& type) { return scope == "all" || scope == type; };

    auto isUnderDirectory = [](const std::filesystem::path& candidate, const std::filesystem::path& base) {
        std::error_code ec;
        auto nc = std::filesystem::weakly_canonical(candidate, ec);
        if (ec)
        {
            return false;
        }
        auto nb = std::filesystem::weakly_canonical(base, ec);
        if (ec)
        {
            return false;
        }
        auto bi = nb.begin();
        auto ci = nc.begin();
        for (; bi != nb.end(); ++bi, ++ci)
        {
            if (ci == nc.end() || *bi != *ci)
            {
                return false;
            }
        }
        return true;
    };

    std::vector<std::string> removedKeys;
    std::size_t skipped = 0, skippedUsed = 0;

    for (const auto& item : resources)
    {
        if (!item.is_object())
        {
            ++skipped;
            continue;
        }
        const std::string t = item.value("type", ""), i = item.value("id", "");
        if (t.empty() || i.empty())
        {
            ++skipped;
            continue;
        }
        if (!isScopeMatch(t))
        {
            continue;
        }

        const std::string key = makeKey(t, i);
        if (usedKeys.count(key) > 0)
        {
            ++skippedUsed;
            continue;
        }

        const auto resourceOpt = mResourceLibrary.LookupResource(t, i);
        if (!resourceOpt)
        {
            ++skipped;
            continue;
        }

        const bool isUserEntry = userKeys.count(key) > 0;
        const bool isUserFile =
            !resourceOpt->filePath.empty() && isUnderDirectory(resourceOpt->filePath, resourceFilesDir);
        if (!isUserEntry && !isUserFile)
        {
            ++skipped;
            continue;
        }

        mResourceLibrary.RemoveResource(t, i);
        removedKeys.push_back(key);

        if (removeFiles && isUserFile)
        {
            std::error_code ec;
            std::filesystem::remove(resourceOpt->filePath, ec);
        }
    }

    if (!removedKeys.empty())
    {
        std::unordered_set<std::string> removedSet(removedKeys.begin(), removedKeys.end());
        nlohmann::json updated = nlohmann::json::array();
        for (const auto& e : entries)
        {
            const std::string t = e.value("type", ""), i = e.value("id", "");
            if (!t.empty() && !i.empty() && removedSet.count(makeKey(t, i)) > 0)
            {
                continue;
            }
            updated.push_back(e);
        }
        [[maybe_unused]] const auto ensuredLibraryDir = mFileSystem.EnsureDirectory(libraryDir);
        std::ofstream output(libraryFile);
        if (output)
        {
            output << updated.dump(2);
        }
        TouchSharedSyncState({"resourceLibrary"});
    }

    BroadcastState();
    nlohmann::json msg;
    msg["type"] = "resourceCleanupResult";
    msg["requested"] = resources.size();
    msg["removed"] = removedKeys.size();
    msg["skipped"] = skipped;
    msg["skippedUsed"] = skippedUsed;
    SendMessageToUI(msg.dump());
}

std::optional<std::filesystem::path> PluginController::ResolveResourceRef(const ResourceRef& ref) const
{
    if (auto resolved = mResourceLibrary.ResolveResource(ref))
    {
        return resolved;
    }
    if (!ref.filePath.empty())
    {
        return ref.filePath;
    }
    return std::nullopt;
}

std::filesystem::path PluginController::ResolveResourcesRoot() const
{
    // Also the real profile: stored resource paths are relative to it, and a
    // session-relative base would make every persisted path wrong once the
    // session ended. Session-only resources are held in memory and never reach
    // the store, so they do not need a session-relative base.
    return mFileSystem.ResolveSettingsDirectory() / "resources";
}

void PluginController::AppendUserLibraryResource(const LibraryResource& resource)
{
    mResourceLibrary.AddResource(resource);
    // One row, not a rewrite of the whole index. A bulk import is now N cheap
    // upserts rather than N full-file writes.
    ResourceLibrary::PutInStore(Store(), resource, ResolveResourcesRoot());
}

void PluginController::RemoveUserLibraryResource(const std::string& type, const std::string& id)
{
    mResourceLibrary.RemoveResource(type, id);
    ResourceLibrary::RemoveFromStore(Store(), type, id);
}

bool PluginController::ExtractFirstResourceFromZip(const std::vector<std::uint8_t>& /*zipData*/,
                                                   const std::string& /*resourceType*/,
                                                   const std::filesystem::path& /*outputPath*/)
{
    // Zip extraction not yet supported — would require adding miniz or similar dependency.
    // Preview only works with non-zip model downloads.
    AppendSessionLog("Preview from zip not supported - select a non-zip model");
    return false;
}

// ── NAM level-state normalization ─────────────────────────────────

void PluginController::LoadResourceLibraries()
{
    mResourceLibrary.LoadFromStore(Store(), ResolveResourcesRoot());
    CleanupResourceLibraryCategoriesOnStartup();
    std::cout << "[Plugin] Loaded " << mResourceLibrary.GetAllResources().size() << " resources from "
              << ResolveDocumentStorePath().string() << std::endl;
}

void PluginController::CleanupResourceLibraryCategoriesOnStartup()
{
    const auto resourcesRoot = ResolveResourcesRoot();
    auto allResources = mResourceLibrary.GetAllResources();
    std::vector<LibraryResource> changed;

    for (auto& resource : allResources)
    {
        if (resource.type != "nam")
        {
            continue;
        }

        // Backfill NAM metadata before category resolution so older entries can
        // be reassigned from file-native metadata (e.g. gear_type).
        EnrichNamResourceMetadata(resource, resource.filePath);

        const std::string resolvedCategory = ResolveResourceLibraryCategory(resource, resource.category);
        if (resolvedCategory.empty() || resolvedCategory == resource.category)
        {
            continue;
        }

        resource.category = resolvedCategory;
        mResourceLibrary.UpdateResource(resource.type, resource.id, resource);
        changed.push_back(resource);
    }

    if (!changed.empty())
    {
        // One transaction for the whole normalization pass: either every row is
        // reclassified or none is, so a crash here cannot leave the library
        // half-categorized.
        Store().Transact([&]() {
            for (const auto& resource : changed)
            {
                if (!ResourceLibrary::PutInStore(Store(), resource, resourcesRoot))
                {
                    return false;
                }
            }
            return true;
        });
        AppendSessionLog("Normalized resource categories at startup: " + std::to_string(changed.size()));
    }
}

} // namespace guitarfx
