#include "controller/internal/PresetArchiveSupport.h"

#include "IPluginHost.h"
#include "controller/internal/ControllerUtils.h"
#include "controller/internal/NamResourceMetadata.h"
#include "presets/PresetStorage.h"
#include "resources/ResourceLibrary.h"
#include "storage/JsonStore.h"
#include "util/FileIO.h"
#include "util/FileSystem.h"
#include "util/PathEncoding.h"
#include "util/PathSanitizer.h"

#include <miniz.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <unordered_set>

namespace guitarfx::controller_detail
{

std::string BuildFactoryArchiveKey(const std::filesystem::path& archivePath)
{
    std::string name = archivePath.filename().string();
    constexpr std::array<std::string_view, 4> suffixes = {
        ".soundshed.presets",
        ".soundshed.preset",
        ".presets",
        ".preset",
    };

    std::string lowerName = name;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    for (const auto suffix : suffixes)
    {
        if (lowerName.size() >= suffix.size()
            && lowerName.compare(lowerName.size() - suffix.size(), suffix.size(), suffix.data()) == 0)
        {
            name.erase(name.size() - suffix.size());
            break;
        }
    }

    auto sanitized = guitarfx::util::SanitizePathSegment(name, true);
    if (sanitized.empty())
        sanitized = "factory-archive";
    return sanitized;
}

std::string BuildScopedFactoryArchiveId(const std::string& archiveKey, const std::string& rawId)
{
    auto sanitizedRaw = guitarfx::util::SanitizePathSegment(rawId, true);
    if (sanitizedRaw.empty())
        sanitizedRaw = "item";
    return archiveKey + "__" + sanitizedRaw;
}

std::string BuildScopedPresetArchiveSessionId(const std::string& archiveKey, const std::string& rawId)
{
    auto sanitizedRaw = guitarfx::util::SanitizePathSegment(rawId, true);
    if (sanitizedRaw.empty())
        sanitizedRaw = "item";
    return std::string{"preset-archive-session__"} + archiveKey + "__" + sanitizedRaw;
}

bool IsFactoryArchiveExtension(const std::filesystem::path& path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return extension == ".preset" || extension == ".presets";
}

std::string NormalizePresetTitle(std::string value)
{
    const auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char ch)
    {
        return !isSpace(static_cast<unsigned char>(ch));
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](char ch)
    {
        return !isSpace(static_cast<unsigned char>(ch));
    }).base(), value.end());
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch)
    {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

nlohmann::json MakePresetFolderEntry(const std::string& id, const std::string& name)
{
    return nlohmann::json{
        {"id", id},
        {"name", name},
        {"children", nlohmann::json::array()},
        {"presetIds", nlohmann::json::array()},
    };
}

bool IsFactoryArchiveFolderId(const std::string& folderId)
{
    return folderId.rfind("factory-archive::", 0) == 0
        || folderId.rfind("factory-archive-folder::", 0) == 0;
}

std::string BuildFactoryArchiveNestedFolderId(const std::string& archiveKey, const std::string& folderPath)
{
    const auto sanitizedPath = guitarfx::util::SanitizeSubfolderPath(folderPath);
    std::string sanitized = sanitizedPath.generic_string<char>();
    std::replace(sanitized.begin(), sanitized.end(), '/', '_');
    if (sanitized.empty())
        sanitized = "folder";
    return "factory-archive-folder::" + archiveKey + "::" + sanitized;
}

std::string BuildPresetArchiveSessionFolderId(const std::string& archiveKey, const std::string& folderPath)
{
    const auto sanitizedPath = guitarfx::util::SanitizeSubfolderPath(folderPath);
    std::string sanitized = sanitizedPath.generic_string<char>();
    std::replace(sanitized.begin(), sanitized.end(), '/', '_');
    if (sanitized.empty())
        sanitized = "folder";
    return "preset-archive-session-folder::" + archiveKey + "::" + sanitized;
}

nlohmann::json BuildFactoryArchiveFolders(const std::string& archiveKey,
                                          const nlohmann::json& archivePresetFolders,
                                          const std::unordered_map<std::string, std::string>& presetIdMapping)
{
    std::function<nlohmann::json(const nlohmann::json&, const std::string&)> buildFolders;
    buildFolders = [&](const nlohmann::json& sourceFolders, const std::string& parentPath) -> nlohmann::json
    {
        nlohmann::json result = nlohmann::json::array();
        if (!sourceFolders.is_array())
            return result;

        for (const auto& sourceFolder : sourceFolders)
        {
            if (!sourceFolder.is_object())
                continue;

            const std::string name = sourceFolder.value("name", "");
            if (name.empty())
                continue;

            const std::string folderPath = parentPath.empty() ? name : (parentPath + "/" + name);
            nlohmann::json folder = MakePresetFolderEntry(
                BuildFactoryArchiveNestedFolderId(archiveKey, folderPath),
                name);

            if (sourceFolder.contains("presetIds") && sourceFolder["presetIds"].is_array())
            {
                for (const auto& presetIdValue : sourceFolder["presetIds"])
                {
                    if (!presetIdValue.is_string())
                        continue;
                    const auto mappedIt = presetIdMapping.find(presetIdValue.get<std::string>());
                    if (mappedIt == presetIdMapping.end())
                        continue;
                    folder["presetIds"].push_back(mappedIt->second);
                }
            }

            folder["children"] = buildFolders(sourceFolder.value("children", nlohmann::json::array()), folderPath);
            result.push_back(std::move(folder));
        }

        return result;
    };

    return buildFolders(archivePresetFolders, std::string{});
}

bool IsFactoryArchiveTopLevelFolder(const std::string& archiveKey, const nlohmann::json& folder)
{
    if (!folder.is_object())
        return false;

    const std::string folderId = folder.value("id", "");
    const std::string expectedPrefix = "factory-archive-folder::" + archiveKey + "::";
    return folderId.rfind(expectedPrefix, 0) == 0;
}

void UpdateFactoryPresetFolders(guitarfx::storage::JsonStore& store,
                                const std::string& archiveKey,
                                const nlohmann::json& archivePresetFolders,
                                const std::unordered_map<std::string, std::string>& presetIdMapping,
                                const std::vector<std::string>&)
{
    auto payload = store.Get(guitarfx::storage::ItemType::kDocument, "preset-folders")
                     .value_or(nlohmann::json::object());
    if (!payload.is_object())
        payload = nlohmann::json::object();

    if (!payload.contains("folders") || !payload["folders"].is_array())
        payload["folders"] = nlohmann::json::array();
    if (!payload.contains("activeFolderId") || !payload["activeFolderId"].is_string())
        payload["activeFolderId"] = "__all__";

    nlohmann::json filteredFolders = nlohmann::json::array();
    for (const auto& folder : payload["folders"])
    {
        if (!IsFactoryArchiveTopLevelFolder(archiveKey, folder))
            filteredFolders.push_back(folder);
    }

    auto archiveFolders = BuildFactoryArchiveFolders(
        archiveKey,
        archivePresetFolders,
        presetIdMapping);

    for (const auto& folder : archiveFolders)
        filteredFolders.push_back(folder);

    payload["folders"] = std::move(filteredFolders);

    store.Put(guitarfx::storage::ItemType::kDocument, "preset-folders", payload);
}

nlohmann::json BuildPresetArchiveSessionFolders(const std::string& archiveKey,
                                                const nlohmann::json& archivePresetFolders,
                                                const std::unordered_map<std::string, std::string>& presetIdMapping)
{
    std::function<nlohmann::json(const nlohmann::json&, const std::string&)> buildFolders;
    buildFolders = [&](const nlohmann::json& sourceFolders, const std::string& parentPath) -> nlohmann::json
    {
        nlohmann::json result = nlohmann::json::array();
        if (!sourceFolders.is_array())
            return result;

        for (const auto& sourceFolder : sourceFolders)
        {
            if (!sourceFolder.is_object())
                continue;

            const std::string name = sourceFolder.value("name", "");
            if (name.empty())
                continue;

            const std::string folderPath = parentPath.empty() ? name : (parentPath + "/" + name);
            nlohmann::json folder = MakePresetFolderEntry(
                BuildPresetArchiveSessionFolderId(archiveKey, folderPath),
                name);

            if (sourceFolder.contains("presetIds") && sourceFolder["presetIds"].is_array())
            {
                for (const auto& presetIdValue : sourceFolder["presetIds"])
                {
                    if (!presetIdValue.is_string())
                        continue;
                    const auto mappedIt = presetIdMapping.find(presetIdValue.get<std::string>());
                    if (mappedIt == presetIdMapping.end())
                        continue;
                    folder["presetIds"].push_back(mappedIt->second);
                }
            }

            folder["children"] = buildFolders(sourceFolder.value("children", nlohmann::json::array()), folderPath);
            result.push_back(std::move(folder));
        }

        return result;
    };

    return buildFolders(archivePresetFolders, std::string{});
}

std::optional<std::vector<std::uint8_t>> ExtractZipEntry(const std::vector<std::uint8_t>& zipBytes,
                                                         const std::string& entryName)
{
    mz_zip_archive archive{};
    if (!mz_zip_reader_init_mem(&archive, zipBytes.data(), zipBytes.size(), 0))
        return std::nullopt;

    const int fileIndex = mz_zip_reader_locate_file(&archive, entryName.c_str(), nullptr, 0);
    if (fileIndex < 0)
    {
        mz_zip_reader_end(&archive);
        return std::nullopt;
    }

    size_t extractedSize = 0;
    void* extracted = mz_zip_reader_extract_to_heap(&archive, static_cast<mz_uint>(fileIndex), &extractedSize, 0);
    if (!extracted)
    {
        mz_zip_reader_end(&archive);
        return std::nullopt;
    }

    std::vector<std::uint8_t> bytes(static_cast<std::uint8_t*>(extracted),
                                    static_cast<std::uint8_t*>(extracted) + extractedSize);
    mz_free(extracted);
    mz_zip_reader_end(&archive);
    return bytes;
}

std::optional<ParsedFactoryPresetArchive> ParseFactoryPresetArchive(const std::filesystem::path& archivePath,
                                                                    const std::vector<std::uint8_t>& zipBytes,
                                                                    std::string& error)
{
    mz_zip_archive archive{};
    if (!mz_zip_reader_init_mem(&archive, zipBytes.data(), zipBytes.size(), 0))
    {
        error = "Invalid zip archive";
        return std::nullopt;
    }

    auto finishWithError = [&](std::string message) -> std::optional<ParsedFactoryPresetArchive>
    {
        error = std::move(message);
        mz_zip_reader_end(&archive);
        return std::nullopt;
    };

    const int presetIndex = mz_zip_reader_locate_file(&archive, "preset.json", nullptr, 0);
    const int presetsIndex = mz_zip_reader_locate_file(&archive, "presets.json", nullptr, 0);
    if (presetIndex < 0 && presetsIndex < 0)
        return finishWithError("Archive is missing preset.json or presets.json");

    auto extractJsonEntry = [&](int index) -> std::optional<nlohmann::json>
    {
        if (index < 0)
            return std::nullopt;

        size_t extractedSize = 0;
        void* extracted = mz_zip_reader_extract_to_heap(&archive, static_cast<mz_uint>(index), &extractedSize, 0);
        if (!extracted)
            return std::nullopt;

        std::string text(static_cast<const char*>(extracted), extractedSize);
        mz_free(extracted);

        try
        {
            return nlohmann::json::parse(text);
        }
        catch (const std::exception& ex)
        {
            error = ex.what();
            return std::nullopt;
        }
    };

    ParsedFactoryPresetArchive parsed;
    const auto archiveJson = extractJsonEntry(presetIndex >= 0 ? presetIndex : presetsIndex);
    if (!archiveJson || !archiveJson->is_object())
        return finishWithError(error.empty() ? "Archive JSON is invalid" : error);

    const nlohmann::json& root = *archiveJson;
    if (root.contains("resources") && root["resources"].is_array())
    {
        for (const auto& resourceJson : root["resources"])
        {
            if (!resourceJson.is_object())
                continue;

            FactoryArchiveResourceEntry resource;
            resource.id = resourceJson.value("id", "");
            resource.name = resourceJson.value("name", resource.id);
            resource.category = resourceJson.value("category", "");
            resource.type = resourceJson.value("type", "");
            resource.fileName = resourceJson.value("fileName", "");
            resource.hash = resourceJson.value("hash", "");
            if (resource.type.empty() || resource.id.empty() || resource.fileName.empty())
                continue;

            const auto resourceBytes = ExtractZipEntry(zipBytes, "resources/" + resource.fileName);
            if (!resourceBytes)
                return finishWithError("Archive resource missing: resources/" + resource.fileName);

            resource.bytes = *resourceBytes;
            parsed.resources.push_back(std::move(resource));
        }
    }

    if (root.contains("blends") && root["blends"].is_array())
    {
        for (const auto& blend : root["blends"])
        {
            if (blend.is_object())
                parsed.blends.push_back(blend);
        }
    }

    if (root.contains("presetFolders") && root["presetFolders"].is_array())
        parsed.presetFolders = root["presetFolders"];

    if (root.contains("tone3000Resources") && root["tone3000Resources"].is_array())
        parsed.tone3000ResourceCount = root["tone3000Resources"].size();

    auto appendPreset = [&](const nlohmann::json& presetJson) -> bool
    {
        if (!presetJson.is_object())
            return true;
        const auto presetOpt = guitarfx::PresetStorage::DeserializeFromJson(presetJson.dump());
        if (!presetOpt)
        {
            error = "Failed to parse preset JSON from archive " + archivePath.filename().string();
            return false;
        }
        parsed.presets.push_back(*presetOpt);
        return true;
    };

    if (presetIndex >= 0)
    {
        if (!root.contains("preset") || !root["preset"].is_object())
            return finishWithError("Archive has no preset data");
        if (!appendPreset(root["preset"]))
            return finishWithError(error);
    }
    else
    {
        if (!root.contains("presets") || !root["presets"].is_array() || root["presets"].empty())
            return finishWithError("Archive has no presets data");
        for (const auto& presetJson : root["presets"])
        {
            if (!appendPreset(presetJson))
                return finishWithError(error);
        }
    }

    mz_zip_reader_end(&archive);
    return parsed;
}

void RemapPresetGraphResources(guitarfx::SignalGraph& graph,
                               const std::unordered_map<std::string, std::string>& resourceIdMap,
                               const std::unordered_map<std::string, std::string>& blendIdMap)
{
    for (auto& node : graph.nodes)
    {
        const auto blendIt = node.config.find("blendId");
        if (blendIt != node.config.end())
        {
            const auto mappedBlend = blendIdMap.find(blendIt->second);
            if (mappedBlend != blendIdMap.end())
                blendIt->second = mappedBlend->second;
        }

        for (auto& resource : node.resources)
        {
            if (!resource.IsLibraryRef())
                continue;
            const auto mappedResource = resourceIdMap.find(resource.resourceId);
            if (mappedResource != resourceIdMap.end())
                resource.resourceId = mappedResource->second;
        }
    }
}

void RemapPresetArchiveReferences(guitarfx::Preset& preset,
                                  const std::unordered_map<std::string, std::string>& resourceIdMap,
                                  const std::unordered_map<std::string, std::string>& blendIdMap)
{
    RemapPresetGraphResources(preset.graph, resourceIdMap, blendIdMap);
    for (auto& scene : preset.scenes)
        RemapPresetGraphResources(scene.graph, resourceIdMap, blendIdMap);
}

std::filesystem::path ResolveFactoryPresetDirectory(const guitarfx::IPluginHost& host,
                                                    const std::filesystem::path& legacyResourceRoot)
{
    const auto bundledRoot = host.GetBundledAssetsPath();
    if (!bundledRoot.empty())
    {
        const auto bundledUiFactoryDir = bundledRoot / "ui" / "presets" / "factory";
        if (std::filesystem::exists(bundledUiFactoryDir))
            return bundledUiFactoryDir;

        const auto bundledLegacyFactoryDir = bundledRoot / "presets" / "factory";
        if (std::filesystem::exists(bundledLegacyFactoryDir))
            return bundledLegacyFactoryDir;
    }

    return legacyResourceRoot / "presets" / "factory";
}

std::filesystem::path NormalizePresetArchiveSavePath(const std::filesystem::path& path)
{
    const std::string filename = path.filename().string();
    std::string normalized = filename;
    std::string lowerNormalized = normalized;
    std::transform(lowerNormalized.begin(), lowerNormalized.end(), lowerNormalized.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    constexpr std::array<std::string_view, 2> suffixes = {
        ".soundshed.presets",
        ".soundshed.preset",
    };

    for (const auto suffix : suffixes)
    {
        while (lowerNormalized.size() >= suffix.size() * 2
               && lowerNormalized.compare(lowerNormalized.size() - suffix.size(), suffix.size(), suffix) == 0
               && lowerNormalized.compare(lowerNormalized.size() - (suffix.size() * 2), suffix.size(), suffix) == 0)
        {
            normalized.erase(normalized.size() - suffix.size());
            lowerNormalized.erase(lowerNormalized.size() - suffix.size());
        }
    }

    if (normalized == filename)
        return path;

    return path.parent_path() / normalized;
}

} // namespace guitarfx::controller_detail
