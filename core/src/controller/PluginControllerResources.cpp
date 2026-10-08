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

#include "controller/internal/BlendSupport.h"
#include "controller/internal/ControllerUtils.h"
#include "controller/internal/HostedPluginSupport.h"
#include "controller/internal/IrResourceCategory.h"
#include "controller/internal/NamResourceMetadata.h"
#include "controller/internal/PresetArchiveSupport.h"
#include "presets/PresetStorage.h"
#include "resources/PluginPathUtils.h"
#include "resources/ResourceLibrary.h"
#include "util/Base64.h"
#include "util/PathEncoding.h"
#include "util/PathSanitizer.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <span>
#include <string_view>
#include <unordered_set>

using namespace guitarfx::controller_detail;

namespace guitarfx
{
namespace
{
/// Calls fn(ref) for each library reference the graph's nodes hold.
template <typename Fn> void ForEachLibraryRef(const SignalGraph& graph, Fn&& fn)
{
    for (const auto& node : graph.nodes)
    {
        for (const auto& ref : node.resources)
        {
            if (ref.IsLibraryRef())
            {
                fn(ref);
            }
        }
    }
}

/// The same across every graph the preset carries, scenes included.
template <typename Fn> void ForEachLibraryRef(const Preset& preset, Fn&& fn)
{
    ForEachPresetGraph(preset, [&](const SignalGraph& graph) { ForEachLibraryRef(graph, fn); });
}

std::string ResourceKey(const std::string& type, const std::string& id)
{
    return type + ":" + id;
}

std::string NameOrId(const std::string& name, const std::string& id, const char* fallback)
{
    return !name.empty() ? name : !id.empty() ? id : fallback;
}

/// How a refusal words a kind of use: "Used by <label>: <name>".
std::string ResourceUseLabel(const std::string& kind)
{
    if (kind == "effectPreset")
    {
        return "effect preset";
    }

    if (kind == "customEffect")
    {
        return "custom effect";
    }

    if (kind == "globalChain")
    {
        return "global chain";
    }

    return kind;
}

/// What uses a resource, as a delete refusal and a usage answer both report it. presetName and
/// blendName were the only fields before the other kinds were checked, and older UIs read them.
void AddResourceUseFields(nlohmann::json& msg, const std::string& kind, const std::string& name)
{
    msg["detail"] = "Used by " + ResourceUseLabel(kind) + ": " + name;
    msg["usageKind"] = kind;
    msg["usageName"] = name;

    if (kind == "preset")
    {
        msg["presetName"] = name;
    }
    else if (kind == "blend")
    {
        msg["blendName"] = name;
    }
}
} // namespace

void PluginController::HandleImportRemoteResourceRequest(const nlohmann::json& payload)
{
    const std::string resourceType = payload.value("resourceType", "");
    const std::string resourceId = payload.value("resourceId", "");
    const std::string requestId = payload.value("requestId", "");
    const std::string data = payload.value("data", "");
    const nlohmann::json metadataPayload = payload.value("metadata", nlohmann::json::object());
    const nlohmann::json tagsPayload = payload.value("tags", nlohmann::json::array());

    const auto fail = [&](const std::string& detail) {
        ReportErrorToUI("Import failed", detail);
        SendMessageToUI(nlohmann::json{
            {"type", "resourceImportFailed"}, {"requestId", requestId}, {"message", "Import failed"}, {"detail", detail}}
                            .dump());
    };

    if (resourceType.empty() || resourceId.empty() || data.empty())
    {
        fail("Missing resource metadata");
        return;
    }

    const std::vector<std::uint8_t> bytes = util::DecodeBase64(data);

    if (bytes.empty())
    {
        fail("Invalid base64 payload");
        return;
    }

    LibraryResource resource;
    resource.type = resourceType;
    resource.id = resourceId;
    resource.name = payload.value("name", resourceId);
    resource.category = payload.value("category", "");
    resource.description = payload.value("description", "");

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
            if (tagValue.is_string() && !tagValue.get<std::string>().empty())
            {
                resource.tags.push_back(tagValue.get<std::string>());
            }
        }
    }

    std::string error;
    const auto imported = ImportResourceFile(std::move(resource), payload.value("provider", "remote"),
                                             payload.value("subfolder", ""), payload.value("fileName", ""), bytes, error);

    if (!imported)
    {
        fail(error);
        return;
    }

    BroadcastState();
    // As local saves and deletes do: other instances reload the library on it.
    TouchSharedSyncState({"resourceLibrary"});

    nlohmann::json msg;
    msg["type"] = "resourceImported";
    msg["requestId"] = requestId;
    msg["resourceType"] = imported->type;
    msg["id"] = imported->id;
    msg["name"] = imported->name;
    msg["filePath"] = util::PathToUtf8(imported->filePath);
    SendMessageToUI(msg.dump());
}

std::optional<LibraryResource> PluginController::ImportResourceFile(LibraryResource resource, const std::string& provider,
                                                                    const std::string& subfolder,
                                                                    const std::string& fileName,
                                                                    const std::vector<std::uint8_t>& bytes,
                                                                    std::string& error)
{
    const std::string resourceType = resource.type;
    const std::string resourceId = resource.id;

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

    // Hashed here whatever the payload says: a caller's `hash` can be another algorithm (the
    // browser's SHA-256) or stale, and the UI's de-duplication and the library's missing-file
    // fallback both match on this one.
    const std::string contentHash = mHasher.HashBytes(bytes);

    // Two of a tone's models, or two entries of a zip, can share a file name. A file already there
    // is overwritten only when it is this resource's own (a re-import) or nothing owns it and it
    // holds these bytes already. Another resource's file is never touched, even an identical one:
    // deleting either entry deletes its file. This one goes beside it under a hash-suffixed name,
    // as local saves do.
    const auto canWriteTo = [&](const std::filesystem::path& candidate) {
        std::error_code ec;

        if (!std::filesystem::exists(candidate, ec))
        {
            return true;
        }

        const std::string candidateKey = util::PathToUtf8(candidate.lexically_normal());
        bool ownedByThisResource = false;

        for (const auto& existing : mResourceLibrary.GetAllResources())
        {
            if (existing.filePath.empty() || util::PathToUtf8(existing.filePath.lexically_normal()) != candidateKey)
            {
                continue;
            }

            if (existing.type != resourceType || existing.id != resourceId)
            {
                return false;
            }

            ownedByThisResource = true;
        }

        return ownedByThisResource || mHasher.HashFile(candidate) == contentHash;
    };

    const std::filesystem::path requestedPath = targetDir / util::PathFromUtf8(resolvedName);
    std::filesystem::path targetPath = requestedPath;

    if (!canWriteTo(targetPath))
    {
        const auto candidateNamed = [&](const std::string& suffixText) {
            std::filesystem::path named = targetDir / requestedPath.stem();
            named += suffixText;
            named += requestedPath.extension();
            return named;
        };
        const std::string hashSuffix = "-" + contentHash.substr(0, std::min<std::size_t>(12, contentHash.size()));
        targetPath = candidateNamed(hashSuffix);
        std::size_t suffix = 2;

        while (!canWriteTo(targetPath))
        {
            targetPath = candidateNamed(hashSuffix + "-" + std::to_string(suffix++));
        }
    }

    if (!WriteFile(targetPath, bytes))
    {
        error = "Failed to write file";
        return std::nullopt;
    }

    resource.filePath = targetPath;
    resource.hash = contentHash;

    if (resourceType == "nam")
    {
        EnrichNamResourceMetadata(resource, targetPath);
    }

    resource.category = ResolveResourceLibraryCategory(resource, resource.category);

    mResourceLibrary.AddResource(resource);
    AppendUserLibraryResource(resource);
    AppendSessionLog("Imported resource " + resourceType + ":" + resourceId + " (" + util::PathToUtf8(targetPath) + ")");
    return resource;
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

            for (const auto& part : util::PathFromUtf8(subfolder))
            {
                const std::string segment = util::SanitizePathSegment(util::PathToUtf8(part), true);

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
                // Appended as path parts, not through path::string() and back: that is the
                // ANSI code page on Windows.
                const auto candidateNamed = [&](const std::string& suffixText) {
                    std::filesystem::path named = targetDir / stem;
                    named += suffixText;
                    named += ext;
                    return named;
                };
                std::filesystem::path candidate = candidateNamed("-" + hashSuffix);
                std::size_t suffix = 2;

                while (std::filesystem::exists(candidate))
                {
                    candidate = candidateNamed("-" + hashSuffix + "-" + std::to_string(suffix++));
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

    // UTF-8 on both sides: generic_string() is the ANSI code page on Windows, and throws for a
    // path it cannot hold, which stopped a model named with "√" from importing at all.
    auto normalizedPathString = util::PathToUtf8(resolvedPath.lexically_normal());
    auto existingByPath = std::find_if(allResources.begin(), allResources.end(), [&](const LibraryResource& resource) {
        return resource.type == resourceType && !resource.filePath.empty() &&
               util::PathToUtf8(resource.filePath.lexically_normal()) == normalizedPathString;
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
            pluginName = util::PathToUtf8(resolvedPath.stem());
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
                     util::SanitizePathSegment(util::PathToUtf8(resolvedPath.stem()), true);
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
        !name.empty() ? name : (!resource.name.empty() ? resource.name : util::PathToUtf8(resolvedPath.stem()));
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
    resource.metadata["sourceFileName"] = util::PathToUtf8(resolvedPath.filename());

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
                                                  : util::PathToUtf8(resolvedPath.stem()))
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
        AnnounceLocalResourceSaveFailure(error);
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

    AnnounceSavedLocalResource(*saved);
}

void PluginController::AnnounceSavedLocalResource(const LibraryResource& saved, const std::string& requestId)
{
    BroadcastState();

    if (!mResourceLibrary.HasResource(saved.type, saved.id))
    {
        return;
    }

    TouchSharedSyncState({"resourceLibrary"});
    nlohmann::json msg;
    msg["type"] = "resourceImported";

    if (!requestId.empty())
    {
        msg["requestId"] = requestId;
    }

    msg["resourceType"] = saved.type;
    msg["id"] = saved.id;
    msg["name"] = saved.name;
    msg["filePath"] = util::PathToUtf8(saved.filePath);
    SendMessageToUI(msg.dump());
}

void PluginController::AnnounceLocalResourceSaveFailure(const std::string& error, const std::string& requestId)
{
    ReportErrorToUI("Local resource save failed", error);
    nlohmann::json msg = {
        {"type", "resourceImportFailed"}, {"message", "Local resource save failed"}, {"detail", error}};

    if (!requestId.empty())
    {
        msg["requestId"] = requestId;
    }

    SendMessageToUI(msg.dump());
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

    // Stored presets and effect presets are read fresh rather than from the usage index: the
    // file cannot come back, and the index can miss what another instance saved moments ago.
    if (const auto use = FindFirstResourceUse(resourceType, resourceId, StoredUseSource::Fresh))
    {
        nlohmann::json msg = {{"type", "resourceDeleteFailed"},
                              {"message", "Resource is in use"},
                              {"resourceType", resourceType},
                              {"id", resourceId}};
        AddResourceUseFields(msg, use->kind, use->name);
        SendMessageToUI(msg.dump());
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
            SendMessageToUI(
                nlohmann::json{{"type", "resourceDeleteFailed"},
                               {"message", "Resource delete failed"},
                               {"detail", "Failed to delete stored file: " + util::PathToUtf8(resourcePath)}}
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

std::optional<PluginController::ResourceUse> PluginController::FindFirstResourceUse(const std::string& resourceType,
                                                                                    const std::string& resourceId,
                                                                                    StoredUseSource storedUses) const
{
    const std::string wanted = ResourceKey(resourceType, resourceId);
    std::optional<ResourceUse> first;

    ForEachResourceUse(storedUses, [&](const std::string& key, const ResourceUse& use) {
        if (!first && key == wanted)
        {
            first = use;
        }
    });

    return first;
}

void PluginController::EnsureResourceUsageDiskIndex() const
{
    if (mResourceUsageDiskIndexValid)
    {
        return;
    }

    mResourceUsageDiskIndex.clear();

    ForEachStoredResourceUse([this](const std::string& key, const ResourceUse& use) {
        // Preserve first-found priority (user > factory > archive presets, then effect presets).
        mResourceUsageDiskIndex.emplace(key, use);
    });

    mResourceUsageDiskIndexValid = true;
}

void PluginController::InvalidateResourceUsageIndex()
{
    mResourceUsageDiskIndexValid = false;
    mResourceUsageDiskIndex.clear();
}

void PluginController::ForEachStoredPreset(const std::function<void(const Preset&)>& visit) const
{
    for (const auto& preset : LoadAllUserPresets())
    {
        visit(preset);
    }

    const auto factoryDir = ResolveFactoryPresetDirectory(mHost, mResourceRoot);

    if (std::filesystem::exists(factoryDir))
    {
        for (const auto& preset : PresetStorage::LoadAllFromDirectory(factoryDir))
        {
            visit(preset);
        }
    }

    for (const auto& [_, preset] : mFactoryArchivePresets)
    {
        visit(preset);
    }
}

void PluginController::ForEachStoredResourceUse(
    const std::function<void(const std::string& key, const ResourceUse& use)>& visit) const
{
    ForEachStoredPreset([&](const Preset& preset) {
        const ResourceUse use{"preset", NameOrId(preset.name, preset.id, "Unnamed preset")};
        ForEachLibraryRef(preset,
                          [&](const ResourceRef& ref) { visit(ResourceKey(ref.resourceType, ref.resourceId), use); });
    });

    ForEachEffectPresetResourceRef([&](const ResourceRef& ref, const std::string& presetName) {
        if (ref.IsLibraryRef())
        {
            visit(ResourceKey(ref.resourceType, ref.resourceId), ResourceUse{"effectPreset", presetName});
        }
    });
}

void PluginController::ForEachResourceUse(
    StoredUseSource storedUses, const std::function<void(const std::string& key, const ResourceUse& use)>& visit) const
{
    const auto visitGraph = [&](const SignalGraph& graph, const ResourceUse& use) {
        ForEachLibraryRef(graph,
                          [&](const ResourceRef& ref) { visit(ResourceKey(ref.resourceType, ref.resourceId), use); });
    };
    const auto visitPreset = [&](const Preset& preset) {
        const ResourceUse use{"preset", NameOrId(preset.name, preset.id, "Unnamed preset")};
        ForEachLibraryRef(preset,
                          [&](const ResourceRef& ref) { visit(ResourceKey(ref.resourceType, ref.resourceId), use); });
    };

    if (mActivePreset)
    {
        visitPreset(*mActivePreset);
    }

    // A Multi-Rig slot can be playing a preset that was never saved, or has since been deleted.
    for (const auto& [_, presetJson] : mMixerPresetJsonCache)
    {
        if (const auto preset = PresetStorage::DeserializeFromJson(presetJson))
        {
            visitPreset(*preset);
        }
    }

    if (storedUses == StoredUseSource::Fresh)
    {
        ForEachStoredResourceUse(visit);
    }
    else
    {
        EnsureResourceUsageDiskIndex();

        for (const auto& [key, use] : mResourceUsageDiskIndex)
        {
            visit(key, use);
        }
    }

    // Presets name a blend node's blend, not its models.
    if (mBlendLibrary.is_array())
    {
        for (const auto& blend : mBlendLibrary)
        {
            const ResourceUse use{"blend", NameOrId(blend.value("name", ""), blend.value("id", ""), "Unnamed blend")};

            for (const auto& modelId : CollectBlendModelIds(blend))
            {
                visit(ResourceKey("nam", modelId), use);
            }
        }
    }

    if (mEditingComposite)
    {
        visitGraph(mEditingComposite->innerGraph,
                   {"composite", NameOrId(mEditingComposite->name, mEditingComposite->id, "Unnamed composite")});
    }

    for (const auto& definition : mCompositeLibrary.GetAllDefinitions())
    {
        visitGraph(definition.innerGraph, {"composite", NameOrId(definition.name, definition.id, "Unnamed composite")});
    }

    for (const auto& entry : mCustomEffectLibrary.GetAllEntries())
    {
        if (!entry.moduleResourceType.empty() && !entry.moduleResourceId.empty())
        {
            visit(ResourceKey(entry.moduleResourceType, entry.moduleResourceId),
                  {"customEffect", NameOrId(entry.name, entry.id, "Unnamed custom effect")});
        }
    }

    const auto& globalChain = mPresetMixer.GetGlobalChainConfig();
    visitGraph(globalChain.preChainGraph, {"globalChain", "pre-chain"});
    visitGraph(globalChain.postChainGraph, {"globalChain", "post-chain"});
}

std::unordered_set<std::string> PluginController::CollectResourceKeysInUse() const
{
    std::unordered_set<std::string> keys;
    ForEachResourceUse(StoredUseSource::Fresh, [&](const std::string& key, const ResourceUse&) { keys.insert(key); });
    return keys;
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

    // The resource browser asks once per row it shows, so what is stored comes from the index.
    // It only greys out the delete button; the delete itself checks again, reading fresh.
    const auto use = FindFirstResourceUse(resourceType, resourceId, StoredUseSource::UsageIndex);
    nlohmann::json msg = {{"type", "resourceUsageInfo"},
                          {"resourceType", resourceType},
                          {"id", resourceId},
                          {"inUse", use.has_value()},
                          {"presetName", ""},
                          {"blendName", ""},
                          {"usageKind", ""},
                          {"usageName", ""},
                          {"detail", ""}};

    if (use)
    {
        AddResourceUseFields(msg, use->kind, use->name);
    }

    SendMessageToUI(msg.dump());
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
        const std::string category = payload.value("category", updated.category);

        // An IR's category is its slot. An edit that names neither keeps the one it had.
        if (updated.type != "ir")
        {
            updated.category = category;
        }
        else if (auto mapped = MapToIrLibraryCategory(category))
        {
            updated.category = *mapped;
        }
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
            std::filesystem::path updatedPath = util::PathFromUtf8(filePathValue);

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
            updated.metadata["sourceFileName"] = util::PathToUtf8(updatedPath.filename());
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
        updated.metadata["sourceFileName"] = util::PathToUtf8(targetPath.filename());

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
            msg["path"] = util::PathToUtf8(result.path);
            const auto leaf = result.path.filename();
            msg["name"] = leaf.empty() ? util::PathToUtf8(result.path) : util::PathToUtf8(leaf);
        }
        else
        {
            msg["success"] = false;
        }

        SendMessageToUI(msg.dump());
    });
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
    const auto requestedPath = util::PathFromUtf8(rawPath);

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
    result["path"] = util::PathToUtf8(canonicalRequested);
    result["removed"] = removed;
    SendMessageToUI(result.dump());

    AppendSessionLog("Deleted imported tone sharing pack -> " + util::PathToUtf8(canonicalRequested));
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

    // A preview on the slot already previewing replaces that one and keeps its original. The slot
    // holds the earlier temp file by now, which is deleted below, so taking that as the original
    // would make closing the browser restore a missing file. A preview anywhere else first puts
    // the previewing slot back.
    const bool replacesActivePreview =
        mPreviewState.active && mPreviewState.nodeId == nodeId && mPreviewState.resourceIndex == resourceIndex;

    if (mPreviewState.active && !replacesActivePreview)
    {
        HandleCancelPreviewResourceRequest(nlohmann::json{{"restoreOriginal", true}});
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
            ReportErrorToUI("Preview failed",
                            resourceType == "ir" ? "No IR in the archive" : "No NAM model in the archive");
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

    const std::filesystem::path replacedTempFile =
        replacesActivePreview ? mPreviewState.tempFilePath : std::filesystem::path{};

    if (!replacesActivePreview)
    {
        mPreviewState = PreviewState{};

        if (mActivePreset)
        {
            GraphNode* node = mActivePreset->graph.FindNode(nodeId);

            if (node && resourceIndex >= 0 && static_cast<size_t>(resourceIndex) < node->resources.size())
            {
                mPreviewState.originalResourceRef = node->resources[resourceIndex];
            }
        }
    }

    mPreviewState.active = true;
    mPreviewState.nodeId = nodeId;
    mPreviewState.resourceIndex = resourceIndex;
    mPreviewState.resourceType = resourceType;
    mPreviewState.tempFilePath = tempPath;

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

    // Only once the slot has moved on to the new file. The same model previewed again reuses
    // its path, which is then the file now playing.
    if (!replacedTempFile.empty() && replacedTempFile != tempPath)
    {
        std::error_code ec;
        std::filesystem::remove(replacedTempFile, ec);
    }

    AppendSessionLog("Preview started: " + resourceType + " at " + util::PathToUtf8(tempPath));
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
    response["fileName"] = util::PathToUtf8(resolvedPath->filename());
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
                nlohmann::json{{"type", "libraryExportSaved"}, {"path", util::PathToUtf8(result.path)}}.dump());
            AppendSessionLog("Library export saved: " + util::PathToUtf8(result.path));
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

    const auto resourceFilesDir = ResolveResourcesRoot() / "content";
    const auto usedKeys = CollectResourceKeysInUse();

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

    std::size_t removed = 0, skipped = 0, skippedUsed = 0;

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

        if (usedKeys.contains(ResourceKey(t, i)))
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

        // The store is the library's only persistence, so a row there is what makes an entry
        // the user's. One held only in memory, such as a preset archive session's, is not.
        const bool isUserEntry = Store().Has(storage::ItemType::kResource, ResourceLibrary::MakeStoreId(t, i));
        const bool isUserFile =
            !resourceOpt->filePath.empty() && isUnderDirectory(resourceOpt->filePath, resourceFilesDir);

        if (!isUserEntry && !isUserFile)
        {
            ++skipped;
            continue;
        }

        // The file before the entry, as a single delete does: a file that cannot go keeps its
        // entry, rather than being left in the content folder with nothing pointing at it.
        if (removeFiles && isUserFile)
        {
            std::error_code ec;
            std::filesystem::remove(resourceOpt->filePath, ec);

            if (ec)
            {
                AppendSessionLog("Resource cleanup could not delete " + util::PathToUtf8(resourceOpt->filePath) + ": " +
                                 ec.message());
                ++skipped;
                continue;
            }
        }

        // From the store as well as memory, or it is back at the next start.
        RemoveUserLibraryResource(t, i);
        ++removed;
    }

    if (removed > 0)
    {
        TouchSharedSyncState({"resourceLibrary"});
    }

    BroadcastState();
    nlohmann::json msg;
    msg["type"] = "resourceCleanupResult";
    msg["requested"] = resources.size();
    msg["removed"] = removed;
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

bool PluginController::ExtractFirstResourceFromZip(const std::vector<std::uint8_t>& zipData,
                                                   const std::string& resourceType,
                                                   const std::filesystem::path& outputPath)
{
    // The entries the UI's own zip import takes (tone3000Shared.ts), in the same order, so a
    // preview plays the model that importing the zip would select.
    static constexpr std::array<std::string_view, 2> kNamExtensions = {".nam", ".json"};
    static constexpr std::array<std::string_view, 2> kIrExtensions = {".wav", ".ir"};

    if (resourceType != "nam" && resourceType != "ir")
    {
        AppendSessionLog("Preview from zip: unsupported resource type " + resourceType);
        return false;
    }

    const auto entry = ExtractFirstZipEntryWithExtension(
        zipData, resourceType == "ir" ? std::span<const std::string_view>(kIrExtensions)
                                      : std::span<const std::string_view>(kNamExtensions));

    if (!entry || entry->bytes.empty())
    {
        return false;
    }

    if (!WriteFile(outputPath, entry->bytes))
    {
        AppendSessionLog("Preview from zip: could not write " + util::PathToUtf8(outputPath));
        return false;
    }

    AppendSessionLog("Preview from zip: using " + entry->name);
    return true;
}

// ── NAM level-state normalization ─────────────────────────────────

void PluginController::LoadResourceLibraries()
{
    mResourceLibrary.LoadFromStore(Store(), ResolveResourcesRoot());
    CleanupResourceLibraryCategoriesOnStartup();
    AppendSessionLog("Loaded " + std::to_string(mResourceLibrary.GetAllResources().size()) + " resources from " +
                     util::PathToUtf8(ResolveDocumentStorePath()));
}

void PluginController::CleanupResourceLibraryCategoriesOnStartup()
{
    const auto resourcesRoot = ResolveResourcesRoot();
    auto allResources = mResourceLibrary.GetAllResources();
    std::vector<LibraryResource> changed;

    for (auto& resource : allResources)
    {
        if (resource.type == "ir")
        {
            // Older builds filed IRs under whatever the import said: Tone3000's gear,
            // a folder name, "Local". Settle each on cab or reverb once, keeping a pack
            // or folder name as a tag so that grouping is not lost.
            if (IsIrLibraryCategory(resource.category))
            {
                continue;
            }

            if (auto tag = IrCategoryToKeepAsTag(resource.category);
                tag && std::find(resource.tags.begin(), resource.tags.end(), *tag) == resource.tags.end())
            {
                resource.tags.push_back(*tag);
            }

            resource.category = ResolveIrLibraryCategory(resource, resource.category);
            mResourceLibrary.UpdateResource(resource.type, resource.id, resource);
            changed.push_back(resource);
            continue;
        }

        if (resource.type != "nam")
        {
            continue;
        }

        // Backfill NAM metadata before category resolution so older entries can
        // be reassigned from file-native metadata (e.g. gear_type). Each file is read
        // once: an import reads it, and so does this pass for an entry from before
        // imports did, saving what it found. namFileVersion is the file's own
        // "version", which every model has, so a row carrying it has been read. Reading
        // every model on every launch cost 28 MB of disk reads on a 440-model library,
        // seconds from a cold disk.
        const auto metadataBefore = resource.metadata;

        if (resource.metadata.count("namFileVersion") == 0)
        {
            EnrichNamResourceMetadata(resource, resource.filePath);
        }

        const std::string resolvedCategory = ResolveResourceLibraryCategory(resource, resource.category);
        const bool categoryChanged = !resolvedCategory.empty() && resolvedCategory != resource.category;

        if (!categoryChanged && resource.metadata == metadataBefore)
        {
            continue;
        }

        if (categoryChanged)
        {
            resource.category = resolvedCategory;
        }

        mResourceLibrary.UpdateResource(resource.type, resource.id, resource);
        changed.push_back(resource);
    }

    if (!changed.empty())
    {
        // One transaction for the whole normalization pass: either every row is
        // reclassified or none is, so a crash here cannot leave the library
        // half-categorized. A row that only gained metadata is saved too, or the
        // next launch would read its file again.
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
        AppendSessionLog("Normalized resource categories or NAM metadata at startup: " +
                         std::to_string(changed.size()));
    }
}
} // namespace guitarfx
