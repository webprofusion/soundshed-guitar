/**
 * PluginControllerCustomEffects.cpp - Blends, custom effects and composites.
 *
 * These three share a shape: a definition stored in its own library, an entry
 * in the effect catalog so it can be placed in a chain, and a runtime
 * descriptor built from the definition. Saving one has to update all three or
 * the catalog offers something the engine cannot build.
 */

#include "PluginController.h"

#include "controller/internal/ControllerUtils.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#if defined(GUITARFX_ENABLE_WASM_EFFECTS)
    #include "dsp/effects/WasmEffect.h"
#endif
#include "presets/CompositePresetStorage.h"
#include "presets/CompositePresetTypes.h"
#include "presets/PresetStorage.h"
#include "resources/CustomEffectLibrary.h"
#include "resources/ResourceLibrary.h"
#include "util/Base64.h"
#include "util/PathEncoding.h"
#include "util/PathSanitizer.h"

#include <algorithm>
#include <fstream>

using namespace guitarfx::controller_detail;

namespace guitarfx
{

void PluginController::HandleSaveBlendDefinitionRequest(const nlohmann::json& payload)
{
    const nlohmann::json blend = payload.value("blend", nlohmann::json::object());
    if (!blend.is_object())
    {
        ReportErrorToUI("Blend save failed", "Missing blend payload");
        return;
    }

    const std::string id = blend.value("id", "");
    if (id.empty())
    {
        ReportErrorToUI("Blend save failed", "Missing blend id");
        return;
    }

    const std::string category = blend.value("category", "");
    static const std::array<std::string, 5> allowedCategories = {"pedal", "preamp", "amp", "full-rig", "cab"};
    if (!category.empty())
    {
        if (!std::any_of(allowedCategories.begin(), allowedCategories.end(),
                         [&](const std::string& e) { return e == category; }))
        {
            ReportErrorToUI("Blend save failed", "Invalid category");
            return;
        }
    }

    if (!mBlendLibrary.is_array())
    {
        mBlendLibrary = nlohmann::json::array();
    }

    nlohmann::json updated = nlohmann::json::array();
    for (const auto& item : mBlendLibrary)
    {
        if (item.value("id", "") != id)
        {
            updated.push_back(item);
        }
    }
    updated.push_back(blend);
    mBlendLibrary = std::move(updated);

    SaveBlendLibrary();
    BroadcastState();
}

void PluginController::HandleSaveCustomEffectEntryRequest(const nlohmann::json& payload)
{
#if !defined(GUITARFX_ENABLE_WASM_EFFECTS)
    (void)payload;
    ReportErrorToUI("Custom Effect save failed", "Custom Effects are not supported in this build");
    return;
#else
    const nlohmann::json entryJson = payload.value("entry", nlohmann::json::object());
    if (!entryJson.is_object())
    {
        ReportErrorToUI("Custom Effect save failed", "Missing entry payload");
        return;
    }

    std::string parseError;
    auto entryOpt = DeserializeCustomEffectLibraryEntry(entryJson, &parseError);
    if (!entryOpt)
    {
        ReportErrorToUI("Custom Effect save failed", parseError.empty() ? "Invalid entry" : parseError);
        return;
    }

    auto entry = *entryOpt;
    entry.baseEffectType = EffectRegistry::Instance().Resolve(entry.baseEffectType);
    if (entry.baseEffectType != EffectGuids::kWasmHost)
    {
        ReportErrorToUI("Custom Effect save failed", "baseEffectType must resolve to wasm_host");
        return;
    }

    if (!mResourceLibrary.HasResource(entry.moduleResourceType, entry.moduleResourceId))
    {
        ReportErrorToUI("Custom Effect save failed", "Referenced module resource was not found in the local library");
        return;
    }

    const auto* existing = mCustomEffectLibrary.GetEntry(entry.id);
    if (entry.category.empty())
    {
        entry.category = existing && !existing->category.empty() ? existing->category : "utility";
    }
    if (entry.createdAt.empty())
    {
        entry.createdAt = existing && !existing->createdAt.empty() ? existing->createdAt : BuildTimestampUtcIso();
    }
    if (entry.updatedAt.empty())
    {
        entry.updatedAt = BuildTimestampUtcIso();
    }
    if (entry.origin.empty() && existing && !existing->origin.empty())
    {
        entry.origin = existing->origin;
    }

    mCustomEffectLibrary.UpsertEntry(entry);
    SaveCustomEffectLibrary();
    BroadcastState();
#endif
}

void PluginController::HandleSaveCurrentCustomEffectRequest(const nlohmann::json& payload)
{
#if !defined(GUITARFX_ENABLE_WASM_EFFECTS)
    (void)payload;
    ReportErrorToUI("Custom Effect save failed", "Custom Effects are not supported in this build");
    return;
#else
    const std::string nodeId = payload.value("nodeId", "");
    if (nodeId.empty())
    {
        ReportErrorToUI("Custom Effect save failed", "Missing node id");
        return;
    }

    auto* targetGraph = ResolveEditTarget();
    auto* node = targetGraph ? targetGraph->FindNode(nodeId) : nullptr;
    if (!node)
    {
        ReportErrorToUI("Custom Effect save failed", "Selected node was not found");
        return;
    }

    const std::string resolvedType = EffectRegistry::Instance().Resolve(node->type);
    if (resolvedType != EffectGuids::kWasmHost)
    {
        ReportErrorToUI("Custom Effect save failed", "Selected node is not a Custom Effect");
        return;
    }

    const bool applyToNode = payload.value("applyToNode", false);
    const nlohmann::json entryJson = payload.value("entry", nlohmann::json::object());
    if (!entryJson.is_object())
    {
        ReportErrorToUI("Custom Effect save failed", "Missing entry payload");
        return;
    }

    const std::string linkedEntryId = [&]() -> std::string {
        if (const auto it = node->config.find("customEffectId"); it != node->config.end())
        {
            return it->second;
        }
        return {};
    }();
    const auto* linkedEntry = linkedEntryId.empty() ? nullptr : mCustomEffectLibrary.GetEntry(linkedEntryId);

    const auto getOptionalString = [&](const char* key) -> std::optional<std::string> {
        if (!entryJson.contains(key) || !entryJson[key].is_string())
        {
            return std::nullopt;
        }
        return entryJson[key].get<std::string>();
    };

    const std::optional<std::string> requestedNameOpt = getOptionalString("name");
    const std::optional<std::string> requestedCategoryOpt = getOptionalString("category");
    const std::optional<std::string> requestedDescriptionOpt = getOptionalString("description");
    const std::optional<std::string> requestedIdOpt = getOptionalString("id");
    const std::optional<std::string> requestedOriginOpt = getOptionalString("origin");
    const std::optional<std::string> requestedThumbnailOpt = getOptionalString("thumbnailDataUrl");
    const std::optional<std::string> requestedRevisionIdOpt = getOptionalString("latestRevisionId");

    ResourceRef moduleRef;
    if (!node->resources.empty())
    {
        moduleRef = node->resources.front();
    }
    if (moduleRef.resourceType.empty())
    {
        moduleRef.resourceType = "wasm";
    }

    if (moduleRef.resourceType != "wasm")
    {
        ReportErrorToUI("Custom Effect save failed", "The selected node is missing a WASM module resource");
        return;
    }

    std::optional<LibraryResource> moduleResource;
    if (!moduleRef.resourceId.empty())
    {
        moduleResource = mResourceLibrary.LookupResource(moduleRef.resourceType, moduleRef.resourceId);
    }

    if (!moduleResource && !moduleRef.filePath.empty())
    {
        nlohmann::json savePayload;
        savePayload["resourceType"] = moduleRef.resourceType;
        savePayload["filePath"] = util::PathToUtf8(moduleRef.filePath);
        savePayload["name"] = requestedNameOpt && !requestedNameOpt->empty()
                                  ? *requestedNameOpt
                                  : (!std::filesystem::path(moduleRef.filePath).stem().string().empty()
                                         ? std::filesystem::path(moduleRef.filePath).stem().string()
                                         : "Custom Effect Module");
        savePayload["category"] = "Local";
        savePayload["metadata"] = nlohmann::json::object({{"provider", kLocalResourceProvider}});

        std::string resourceSaveError;
        moduleResource = SaveLocalLibraryResource(savePayload, resourceSaveError, true);
        if (!moduleResource)
        {
            ReportErrorToUI("Custom Effect save failed",
                            resourceSaveError.empty() ? "Failed to save local WASM module" : resourceSaveError);
            return;
        }
    }

    if (!moduleResource)
    {
        if (!moduleRef.embeddedId.empty())
        {
            ReportErrorToUI("Custom Effect save failed",
                            "Embedded module resources are not supported by this save flow yet");
        }
        else
        {
            ReportErrorToUI("Custom Effect save failed", "Select a WASM module before saving this Custom Effect");
        }
        return;
    }

    GraphNode descriptorNode = *node;
    if (descriptorNode.resources.empty())
    {
        descriptorNode.resources.resize(1);
    }
    descriptorNode.resources.front().resourceType = moduleResource->type;
    descriptorNode.resources.front().resourceId = moduleResource->id;
    descriptorNode.resources.front().filePath.clear();
    descriptorNode.resources.front().embeddedId.clear();
    RefreshWasmNodeDescriptor(descriptorNode);

    std::optional<WasmModuleDescriptor> descriptor;
    if (const auto descriptorIt = descriptorNode.config.find(WasmEffect::kDescriptorConfigKey);
        descriptorIt != descriptorNode.config.end())
    {
        std::string parseError;
        descriptor = WasmEffect::ParseDescriptorConfig(descriptorIt->second, &parseError);
        if (!descriptor && !parseError.empty())
        {
            AppendSessionLog("WASM descriptor cache parse failed while saving current Custom Effect " + nodeId + ": " +
                             parseError);
        }
    }

    std::string entryName = requestedNameOpt.value_or("");
    if (entryName.empty() && linkedEntry && !linkedEntry->name.empty())
    {
        entryName = linkedEntry->name;
    }
    if (entryName.empty() && descriptor && !descriptor->displayName.empty())
    {
        entryName = descriptor->displayName;
    }
    if (entryName.empty() && !node->label.empty())
    {
        entryName = node->label;
    }
    if (entryName.empty() && !moduleResource->name.empty())
    {
        entryName = moduleResource->name;
    }
    if (entryName.empty())
    {
        entryName = "Custom Effect";
    }

    std::string entryId = requestedIdOpt.value_or("");
    if (entryId.empty())
    {
        entryId = linkedEntryId;
    }
    if (entryId.empty())
    {
        std::string baseId = util::SanitizePathSegment(entryName, true);
        if (baseId.empty())
        {
            baseId = "custom-effect";
        }
        entryId = baseId;
        std::size_t suffix = 2;
        while (mCustomEffectLibrary.GetEntry(entryId) != nullptr)
        {
            entryId = baseId + "-" + std::to_string(suffix++);
        }
    }

    std::string entryCategory = requestedCategoryOpt.value_or("");
    if (entryCategory.empty() && linkedEntry && !linkedEntry->category.empty())
    {
        entryCategory = linkedEntry->category;
    }
    if (entryCategory.empty() && descriptor && !descriptor->category.empty())
    {
        entryCategory = descriptor->category;
    }
    if (entryCategory.empty() && !node->category.empty())
    {
        entryCategory = node->category;
    }
    if (entryCategory.empty())
    {
        entryCategory = "utility";
    }

    std::string entryDescription = requestedDescriptionOpt.has_value() ? *requestedDescriptionOpt : std::string{};
    if (!requestedDescriptionOpt.has_value() && linkedEntry && !linkedEntry->description.empty())
    {
        entryDescription = linkedEntry->description;
    }
    if (!requestedDescriptionOpt.has_value() && entryDescription.empty() && descriptor &&
        !descriptor->description.empty())
    {
        entryDescription = descriptor->description;
    }

    nlohmann::json descriptorSummary = linkedEntry && linkedEntry->descriptorSummary.is_object()
                                           ? linkedEntry->descriptorSummary
                                           : nlohmann::json::object();
    if (descriptor)
    {
        descriptorSummary = nlohmann::json::object();
        if (!descriptor->displayName.empty())
        {
            descriptorSummary["displayName"] = descriptor->displayName;
        }
        if (!descriptor->version.empty())
        {
            descriptorSummary["version"] = descriptor->version;
        }
        if (!descriptor->category.empty())
        {
            descriptorSummary["category"] = descriptor->category;
        }
        descriptorSummary["parameterCount"] = descriptor->parameters.size();
        descriptorSummary["resourceCount"] = descriptor->exposedResources.size();
    }

    std::vector<std::string> entryTags = linkedEntry ? linkedEntry->tags : std::vector<std::string>{};
    if (entryJson.contains("tags") && entryJson["tags"].is_array())
    {
        entryTags.clear();
        for (const auto& tagValue : entryJson["tags"])
        {
            if (tagValue.is_string())
            {
                entryTags.push_back(tagValue.get<std::string>());
            }
        }
    }

    std::string entryOrigin = requestedOriginOpt.value_or("");
    if (entryOrigin.empty() && linkedEntry && !linkedEntry->origin.empty())
    {
        entryOrigin = linkedEntry->origin;
    }
    if (entryOrigin.empty())
    {
        entryOrigin = "imported";
    }

    std::string latestRevisionId = requestedRevisionIdOpt.value_or("");
    if (latestRevisionId.empty() && linkedEntry && !linkedEntry->latestRevisionId.empty())
    {
        latestRevisionId = linkedEntry->latestRevisionId;
    }
    if (latestRevisionId.empty())
    {
        if (const auto revisionIt = moduleResource->metadata.find("customEffectRevisionId");
            revisionIt != moduleResource->metadata.end())
        {
            latestRevisionId = revisionIt->second;
        }
    }

    std::string thumbnailDataUrl = requestedThumbnailOpt.value_or("");
    if (thumbnailDataUrl.empty() && linkedEntry && !linkedEntry->thumbnailDataUrl.empty())
    {
        thumbnailDataUrl = linkedEntry->thumbnailDataUrl;
    }
    if (thumbnailDataUrl.empty() && descriptor && !descriptor->thumbnailDataUrl.empty())
    {
        thumbnailDataUrl = descriptor->thumbnailDataUrl;
    }

    CustomEffectLibraryEntry entry;
    entry.id = entryId;
    entry.name = entryName;
    entry.category = entryCategory;
    entry.description = entryDescription;
    entry.baseEffectType = EffectGuids::kWasmHost;
    entry.moduleResourceType = moduleResource->type;
    entry.moduleResourceId = moduleResource->id;
    entry.latestRevisionId = latestRevisionId;
    entry.thumbnailDataUrl = thumbnailDataUrl;
    entry.tags = std::move(entryTags);
    entry.defaultParams = descriptorNode.params;
    entry.descriptorSummary = std::move(descriptorSummary);
    entry.origin = entryOrigin;
    entry.createdAt = linkedEntry && !linkedEntry->createdAt.empty() ? linkedEntry->createdAt : BuildTimestampUtcIso();
    entry.updatedAt = BuildTimestampUtcIso();

    mCustomEffectLibrary.UpsertEntry(entry);
    SaveCustomEffectLibrary();

    if (applyToNode)
    {
        if (node->resources.empty())
        {
            node->resources.resize(1);
        }
        node->resources.front().resourceType = entry.moduleResourceType;
        node->resources.front().resourceId = entry.moduleResourceId;
        node->resources.front().filePath.clear();
        node->resources.front().embeddedId.clear();
        node->config["customEffectId"] = entry.id;
        node->label = entry.name;
        node->category = entry.category;
        RefreshWasmNodeDescriptor(*node);

        if (IsCompositeEditMode())
        {
            BroadcastCompositeEditState();
        }
        else if (mActivePreset)
        {
            SyncActivePresetSceneGraph();
            ApplyPreset(*mActivePreset);
        }
    }

    BroadcastState();
    SendMessageToUI(nlohmann::json{
        {"type", "customEffectSaved"},
        {"id", entry.id},
        {"name", entry.name},
        {"applyToNode", applyToNode},
        {"nodeId", nodeId},
    }
                        .dump());
#endif
}

void PluginController::HandleImportGeneratedCustomEffectRequest(const nlohmann::json& payload)
{
#if !defined(GUITARFX_ENABLE_WASM_EFFECTS)
    (void)payload;
    ReportErrorToUI("Generated Custom Effect import failed", "Custom Effects are not supported in this build");
    return;
#else
    const std::string nodeId = payload.value("nodeId", "");
    if (nodeId.empty())
    {
        ReportErrorToUI("Generated Custom Effect import failed", "Missing node id");
        return;
    }

    auto* targetGraph = ResolveEditTarget();
    auto* node = targetGraph ? targetGraph->FindNode(nodeId) : nullptr;
    if (!node)
    {
        ReportErrorToUI("Generated Custom Effect import failed", "Selected node was not found");
        return;
    }

    const std::string resolvedType = EffectRegistry::Instance().Resolve(node->type);
    if (resolvedType != EffectGuids::kWasmHost)
    {
        ReportErrorToUI("Generated Custom Effect import failed", "Selected node is not a Custom Effect");
        return;
    }

    const nlohmann::json entryJson = payload.value("entry", nlohmann::json::object());
    const nlohmann::json moduleJson = payload.value("module", nlohmann::json::object());
    if (!entryJson.is_object() || !moduleJson.is_object())
    {
        ReportErrorToUI("Generated Custom Effect import failed", "Missing entry or module payload");
        return;
    }

    const std::string moduleData = moduleJson.value("data", "");
    if (moduleData.empty())
    {
        ReportErrorToUI("Generated Custom Effect import failed", "Generated module data is missing");
        return;
    }

    const bool applyToNode = payload.value("applyToNode", false);
    const auto getOptionalString = [&](const nlohmann::json& json, const char* key) -> std::optional<std::string> {
        if (!json.contains(key) || !json[key].is_string())
        {
            return std::nullopt;
        }
        return json[key].get<std::string>();
    };

    const std::string linkedEntryId = [&]() -> std::string {
        if (const auto it = node->config.find("customEffectId"); it != node->config.end())
        {
            return it->second;
        }
        return {};
    }();
    const auto* linkedEntry = linkedEntryId.empty() ? nullptr : mCustomEffectLibrary.GetEntry(linkedEntryId);

    const std::optional<std::string> requestedIdOpt = getOptionalString(entryJson, "id");
    const std::optional<std::string> requestedNameOpt = getOptionalString(entryJson, "name");
    const std::optional<std::string> requestedCategoryOpt = getOptionalString(entryJson, "category");
    const std::optional<std::string> requestedDescriptionOpt = getOptionalString(entryJson, "description");
    const std::optional<std::string> requestedOriginOpt = getOptionalString(entryJson, "origin");
    const std::optional<std::string> requestedThumbnailOpt = getOptionalString(entryJson, "thumbnailDataUrl");
    const std::optional<std::string> requestedRevisionIdOpt = getOptionalString(entryJson, "latestRevisionId");
    const std::string descriptorText = moduleJson.value("descriptorText", std::string{});
    const std::string specText = moduleJson.value("specText", std::string{});
    const nlohmann::json manifestJson = moduleJson.value("manifest", nlohmann::json::object());

    nlohmann::json savePayload = nlohmann::json::object();
    savePayload["resourceType"] = "wasm";
    savePayload["data"] = moduleData;
    savePayload["fileName"] = moduleJson.value("fileName", std::string{});
    savePayload["resourceId"] = moduleJson.value("resourceId", std::string{});
    savePayload["name"] = moduleJson.value("name", requestedNameOpt.value_or("Generated Custom Effect"));
    savePayload["category"] = moduleJson.value("category", std::string{"Custom Effects"});
    savePayload["subfolder"] = moduleJson.value("subfolder", std::string{});
    savePayload["metadata"] = moduleJson.value("metadata", nlohmann::json::object());
    if (!requestedRevisionIdOpt.value_or("").empty())
    {
        savePayload["metadata"]["customEffectRevisionId"] = *requestedRevisionIdOpt;
    }
    if (!payload.value("sessionId", std::string{}).empty())
    {
        savePayload["metadata"]["customEffectSessionId"] = payload.value("sessionId", std::string{});
    }
    savePayload["metadata"]["customEffectOrigin"] = requestedOriginOpt.value_or("generated");

    std::string resourceSaveError;
    auto moduleResource = SaveLocalLibraryResource(savePayload, resourceSaveError, true);
    if (!moduleResource)
    {
        ReportErrorToUI("Generated Custom Effect import failed",
                        resourceSaveError.empty() ? "Failed to save generated WASM module" : resourceSaveError);
        return;
    }

    const auto writeTextArtifact = [&](const std::filesystem::path& targetPath, const std::string& content) {
        const std::vector<std::uint8_t> bytes(content.begin(), content.end());
        return WriteFile(targetPath, bytes);
    };

    const auto persistArtifactPath = [&](const char* metadataKey, const std::filesystem::path& path) {
        moduleResource->metadata[metadataKey] = path.lexically_normal().generic_string();
    };

    const auto bundleDir = moduleResource->filePath.parent_path();
    if (!bundleDir.empty())
    {
        if (!descriptorText.empty())
        {
            const auto descriptorPath = bundleDir / "descriptor.txt";
            if (!writeTextArtifact(descriptorPath, descriptorText))
            {
                ReportErrorToUI("Generated Custom Effect import failed",
                                "Failed to write generated descriptor artifact");
                return;
            }
            persistArtifactPath("customEffectDescriptorPath", descriptorPath);
        }

        if (!specText.empty())
        {
            const auto specPath = bundleDir / "spec.txt";
            if (!writeTextArtifact(specPath, specText))
            {
                ReportErrorToUI("Generated Custom Effect import failed",
                                "Failed to write generated implementation spec artifact");
                return;
            }
            persistArtifactPath("customEffectSpecPath", specPath);
        }

        if (manifestJson.is_object() && !manifestJson.empty())
        {
            const auto manifestPath = bundleDir / "manifest.json";
            if (!writeTextArtifact(manifestPath, manifestJson.dump(2)))
            {
                ReportErrorToUI("Generated Custom Effect import failed", "Failed to write generated manifest artifact");
                return;
            }
            persistArtifactPath("customEffectManifestPath", manifestPath);

            const auto validationIt = manifestJson.find("validation");
            if (validationIt != manifestJson.end() && validationIt->is_object())
            {
                const auto validationPath = bundleDir / "validation-report.json";
                if (!writeTextArtifact(validationPath, validationIt->dump(2)))
                {
                    ReportErrorToUI("Generated Custom Effect import failed",
                                    "Failed to write generated validation artifact");
                    return;
                }
                persistArtifactPath("customEffectValidationPath", validationPath);
            }
        }
    }

    mResourceLibrary.UpdateResource(moduleResource->type, moduleResource->id, *moduleResource);
    AppendUserLibraryResource(*moduleResource);

    GraphNode descriptorNode = *node;
    if (descriptorNode.resources.empty())
    {
        descriptorNode.resources.resize(1);
    }
    descriptorNode.resources.front().resourceType = moduleResource->type;
    descriptorNode.resources.front().resourceId = moduleResource->id;
    descriptorNode.resources.front().filePath.clear();
    descriptorNode.resources.front().embeddedId.clear();

    std::map<std::string, double> requestedDefaultParams;
    if (entryJson.contains("defaultParams") && entryJson["defaultParams"].is_object())
    {
        for (const auto& item : entryJson["defaultParams"].items())
        {
            if (item.value().is_number())
            {
                requestedDefaultParams[item.key()] = item.value().get<double>();
            }
        }
    }
    if (!requestedDefaultParams.empty())
    {
        descriptorNode.params = requestedDefaultParams;
    }

    RefreshWasmNodeDescriptor(descriptorNode);

    std::optional<WasmModuleDescriptor> descriptor;
    if (const auto descriptorIt = descriptorNode.config.find(WasmEffect::kDescriptorConfigKey);
        descriptorIt != descriptorNode.config.end())
    {
        std::string parseError;
        descriptor = WasmEffect::ParseDescriptorConfig(descriptorIt->second, &parseError);
        if (!descriptor && !parseError.empty())
        {
            AppendSessionLog("WASM descriptor cache parse failed while importing generated Custom Effect " + nodeId +
                             ": " + parseError);
        }
    }

    std::string entryName = requestedNameOpt.value_or("");
    if (entryName.empty() && linkedEntry && !linkedEntry->name.empty())
    {
        entryName = linkedEntry->name;
    }
    if (entryName.empty() && descriptor && !descriptor->displayName.empty())
    {
        entryName = descriptor->displayName;
    }
    if (entryName.empty() && !moduleResource->name.empty())
    {
        entryName = moduleResource->name;
    }
    if (entryName.empty())
    {
        entryName = "Custom Effect";
    }

    std::string entryId = requestedIdOpt.value_or("");
    if (entryId.empty())
    {
        entryId = linkedEntryId;
    }
    if (entryId.empty())
    {
        std::string baseId = util::SanitizePathSegment(entryName, true);
        if (baseId.empty())
        {
            baseId = "custom-effect";
        }
        entryId = baseId;
        std::size_t suffix = 2;
        while (mCustomEffectLibrary.GetEntry(entryId) != nullptr)
        {
            entryId = baseId + "-" + std::to_string(suffix++);
        }
    }

    std::string entryCategory = requestedCategoryOpt.value_or("");
    if (entryCategory.empty() && linkedEntry && !linkedEntry->category.empty())
    {
        entryCategory = linkedEntry->category;
    }
    if (entryCategory.empty() && descriptor && !descriptor->category.empty())
    {
        entryCategory = descriptor->category;
    }
    if (entryCategory.empty() && !node->category.empty())
    {
        entryCategory = node->category;
    }
    if (entryCategory.empty())
    {
        entryCategory = "utility";
    }

    std::string entryDescription = requestedDescriptionOpt.value_or("");
    if (entryDescription.empty() && linkedEntry && !linkedEntry->description.empty())
    {
        entryDescription = linkedEntry->description;
    }
    if (entryDescription.empty() && descriptor && !descriptor->description.empty())
    {
        entryDescription = descriptor->description;
    }

    nlohmann::json descriptorSummary =
        entryJson.contains("descriptorSummary") && entryJson["descriptorSummary"].is_object()
            ? entryJson["descriptorSummary"]
            : nlohmann::json::object();
    if (descriptorSummary.empty() && linkedEntry && linkedEntry->descriptorSummary.is_object())
    {
        descriptorSummary = linkedEntry->descriptorSummary;
    }
    if (descriptor)
    {
        descriptorSummary = nlohmann::json::object();
        if (!descriptor->displayName.empty())
        {
            descriptorSummary["displayName"] = descriptor->displayName;
        }
        if (!descriptor->version.empty())
        {
            descriptorSummary["version"] = descriptor->version;
        }
        if (!descriptor->category.empty())
        {
            descriptorSummary["category"] = descriptor->category;
        }
        descriptorSummary["parameterCount"] = descriptor->parameters.size();
        descriptorSummary["resourceCount"] = descriptor->exposedResources.size();
    }

    std::vector<std::string> entryTags = linkedEntry ? linkedEntry->tags : std::vector<std::string>{};
    if (entryJson.contains("tags") && entryJson["tags"].is_array())
    {
        entryTags.clear();
        for (const auto& tagValue : entryJson["tags"])
        {
            if (tagValue.is_string())
            {
                entryTags.push_back(tagValue.get<std::string>());
            }
        }
    }

    std::string entryOrigin = requestedOriginOpt.value_or("");
    if (entryOrigin.empty() && linkedEntry && !linkedEntry->origin.empty())
    {
        entryOrigin = linkedEntry->origin;
    }
    if (entryOrigin.empty())
    {
        entryOrigin = "generated";
    }

    std::string latestRevisionId = requestedRevisionIdOpt.value_or("");
    if (latestRevisionId.empty() && linkedEntry && !linkedEntry->latestRevisionId.empty())
    {
        latestRevisionId = linkedEntry->latestRevisionId;
    }
    if (latestRevisionId.empty())
    {
        if (const auto revisionIt = moduleResource->metadata.find("customEffectRevisionId");
            revisionIt != moduleResource->metadata.end())
        {
            latestRevisionId = revisionIt->second;
        }
    }

    std::string thumbnailDataUrl = requestedThumbnailOpt.value_or("");
    if (thumbnailDataUrl.empty() && linkedEntry && !linkedEntry->thumbnailDataUrl.empty())
    {
        thumbnailDataUrl = linkedEntry->thumbnailDataUrl;
    }
    if (thumbnailDataUrl.empty() && descriptor && !descriptor->thumbnailDataUrl.empty())
    {
        thumbnailDataUrl = descriptor->thumbnailDataUrl;
    }

    CustomEffectLibraryEntry entry;
    entry.id = entryId;
    entry.name = entryName;
    entry.category = entryCategory;
    entry.description = entryDescription;
    entry.baseEffectType = EffectGuids::kWasmHost;
    entry.moduleResourceType = moduleResource->type;
    entry.moduleResourceId = moduleResource->id;
    entry.latestRevisionId = latestRevisionId;
    entry.thumbnailDataUrl = thumbnailDataUrl;
    entry.tags = std::move(entryTags);
    entry.defaultParams = !requestedDefaultParams.empty() ? requestedDefaultParams : descriptorNode.params;
    entry.descriptorSummary = std::move(descriptorSummary);
    entry.origin = entryOrigin;
    entry.createdAt = linkedEntry && !linkedEntry->createdAt.empty() ? linkedEntry->createdAt : BuildTimestampUtcIso();
    entry.updatedAt = BuildTimestampUtcIso();

    mCustomEffectLibrary.UpsertEntry(entry);
    SaveCustomEffectLibrary();

    if (applyToNode)
    {
        if (node->resources.empty())
        {
            node->resources.resize(1);
        }
        node->resources.front().resourceType = entry.moduleResourceType;
        node->resources.front().resourceId = entry.moduleResourceId;
        node->resources.front().filePath.clear();
        node->resources.front().embeddedId.clear();
        node->config["customEffectId"] = entry.id;
        node->label = entry.name;
        node->category = entry.category;
        if (!entry.defaultParams.empty())
        {
            node->params = entry.defaultParams;
        }
        RefreshWasmNodeDescriptor(*node);

        if (IsCompositeEditMode())
        {
            BroadcastCompositeEditState();
        }
        else if (mActivePreset)
        {
            SyncActivePresetSceneGraph();
            ApplyPreset(*mActivePreset);
        }
    }

    BroadcastState();
    SendMessageToUI(nlohmann::json{
        {"type", "customEffectSaved"},
        {"id", entry.id},
        {"name", entry.name},
        {"applyToNode", applyToNode},
        {"nodeId", nodeId},
    }
                        .dump());
#endif
}

void PluginController::HandleExportGeneratedCustomEffectBundleRequest(const nlohmann::json& payload)
{
    const std::string dataEncoded = payload.value("data", "");
    const std::string suggestedName = payload.value("fileName", "custom-effect.custom-effect.zip");

    if (dataEncoded.empty())
    {
        SendMessageToUI(
            nlohmann::json{{"type", "generatedCustomEffectBundleExportFailed"}, {"message", "Missing export data"}}
                .dump());
        return;
    }

    mHost.SaveFileAsync(BrowseFileType::ArchiveFile, "Download Custom Effect Bundle", suggestedName,
                        [this, dataEncoded](const BrowseFileResult& result) {
                            if (!result.success)
                            {
                                SendMessageToUI(nlohmann::json{{"type", "generatedCustomEffectBundleExportFailed"},
                                                               {"message", "Export cancelled"}}
                                                    .dump());
                                return;
                            }

                            const auto decodedBytes = util::DecodeBase64(dataEncoded);
                            if (decodedBytes.empty())
                            {
                                SendMessageToUI(nlohmann::json{{"type", "generatedCustomEffectBundleExportFailed"},
                                                               {"message", "Invalid export data"}}
                                                    .dump());
                                return;
                            }

                            if (!WriteFile(result.path, decodedBytes))
                            {
                                SendMessageToUI(nlohmann::json{{"type", "generatedCustomEffectBundleExportFailed"},
                                                               {"message", "Failed to write file"}}
                                                    .dump());
                                return;
                            }

                            SendMessageToUI(nlohmann::json{{"type", "generatedCustomEffectBundleExportSaved"},
                                                           {"path", result.path.generic_string()}}
                                                .dump());
                            AppendSessionLog("Generated Custom Effect bundle exported: " +
                                             result.path.generic_string());
                        });
}

void PluginController::HandleDeleteBlendDefinitionRequest(const nlohmann::json& payload)
{
    const std::string id = payload.value("blendId", "");
    if (id.empty())
    {
        ReportErrorToUI("Blend delete failed", "Missing blend id");
        return;
    }

    if (!mBlendLibrary.is_array())
    {
        mBlendLibrary = nlohmann::json::array();
    }

    nlohmann::json updated = nlohmann::json::array();
    bool removed = false;
    for (const auto& item : mBlendLibrary)
    {
        if (item.value("id", "") == id)
        {
            removed = true;
            continue;
        }
        updated.push_back(item);
    }

    if (!removed)
    {
        ReportErrorToUI("Blend delete failed", "Blend not found");
        return;
    }

    mBlendLibrary = std::move(updated);
    SaveBlendLibrary();
    BroadcastState();
}

void PluginController::HandleDeleteCustomEffectEntryRequest(const nlohmann::json& payload)
{
    const std::string id = payload.value("id", "");
    if (id.empty())
    {
        ReportErrorToUI("Custom Effect delete failed", "Missing entry id");
        return;
    }

    if (!mCustomEffectLibrary.RemoveEntry(id))
    {
        ReportErrorToUI("Custom Effect delete failed", "Entry not found");
        return;
    }

    SaveCustomEffectLibrary();
    BroadcastState();
}

// ════════════════════════════════════════════════════════════════════
// Composite (Multi-Rig) Preset handlers
// ════════════════════════════════════════════════════════════════════

void PluginController::HandleSaveCompositePresetRequest(const nlohmann::json& payload)
{
    const std::string name = payload.value("name", "");
    if (name.empty())
    {
        ReportErrorToUI("Save Multi-Rig failed", "A name is required");
        return;
    }

    const std::string description = payload.value("description", "");
    const nlohmann::json tagsPayload = payload.value("tags", nlohmann::json::array());

    // Build CompositePreset from current mixer state
    CompositePreset cp;
    cp.name = name;
    cp.description = description;
    cp.masterGain = mPresetMixer.GetMasterGain();
    cp.limiterEnabled = mPresetMixer.IsLimiterEnabled();

    for (const auto& pid : mPresetMixer.GetActivePresetIds())
    {
        const auto cfgOpt = mPresetMixer.GetPresetConfig(pid);
        if (!cfgOpt)
        {
            continue;
        }
        CompositePresetSlot slot;
        slot.slotId = cfgOpt->id;
        slot.presetId = pid;
        slot.mix = cfgOpt->mix;
        slot.pan = cfgOpt->pan;
        slot.mute = cfgOpt->mute;
        slot.solo = cfgOpt->solo;
        cp.slots.push_back(std::move(slot));
    }

    if (cp.slots.empty())
    {
        ReportErrorToUI("Save Multi-Rig failed", "No active presets in mixer");
        return;
    }

    if (tagsPayload.is_array())
    {
        for (const auto& tagValue : tagsPayload)
        {
            if (!tagValue.is_string())
            {
                continue;
            }
            std::string tag = tagValue.get<std::string>();
            const auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
            tag.erase(tag.begin(), std::find_if(tag.begin(), tag.end(),
                                                [&](char ch) { return !isSpace(static_cast<unsigned char>(ch)); }));
            tag.erase(std::find_if(tag.rbegin(), tag.rend(),
                                   [&](char ch) { return !isSpace(static_cast<unsigned char>(ch)); })
                          .base(),
                      tag.end());
            if (!tag.empty())
            {
                cp.tags.push_back(tag);
            }
        }
    }

    // Assign id and timestamps
    const std::string existingId = payload.value("id", "");
    if (!existingId.empty())
    {
        cp.id = existingId;
    }
    else
    {
        // Generate a simple id from name + timestamp
        const auto now = std::chrono::system_clock::now();
        const auto ts = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
        cp.id = guitarfx::util::SanitizeFilename(name) + "_" + std::to_string(ts);
    }

    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&tt), "%Y-%m-%dT%H:%M:%SZ");
    const std::string ts = oss.str();

    if (cp.createdAt.empty())
    {
        cp.createdAt = ts;
    }
    cp.modifiedAt = ts;

    if (!CompositePresetStorage::SaveToStore(Store(), cp))
    {
        ReportErrorToUI("Save Multi-Rig failed", "Could not write the Multi-Rig to the library");
        return;
    }

    // Confirm save to UI and send updated list
    SendMessageToUI(nlohmann::json{{"type", "compositePresetSaved"}, {"id", cp.id}, {"name", cp.name}}.dump());
    SendCompositePresetListToUI();
}

void PluginController::HandleLoadCompositePresetRequest(const nlohmann::json& payload)
{
    const std::string id = payload.value("id", "");
    if (id.empty())
    {
        ReportErrorToUI("Load Multi-Rig failed", "Missing preset id");
        return;
    }

    const auto cpOpt = CompositePresetStorage::LoadFromStore(Store(), id);
    if (!cpOpt)
    {
        ReportErrorToUI("Load Multi-Rig failed", "Preset not found: " + id);
        return;
    }

    const auto& cp = *cpOpt;

    // Clear existing mixer slots
    for (const auto& pid : mPresetMixer.GetActivePresetIds())
    {
        RemoveActivePreset(pid);
    }

    // Load each slot
    for (const auto& slot : cp.slots)
    {
        if (!AddActivePresetById(slot.presetId))
        {
            continue;
        }
        SetActivePresetMix(slot.presetId, slot.mix);
        SetActivePresetPan(slot.presetId, slot.pan);
        SetActivePresetMute(slot.presetId, slot.mute);
        SetActivePresetSolo(slot.presetId, slot.solo);
    }

    // Restore master settings
    SetMasterGain(cp.masterGain);
    SetLimiterEnabled(cp.limiterEnabled);

    // Notify UI
    SendMessageToUI(nlohmann::json{{"type", "compositePresetLoaded"}, {"id", cp.id}, {"name", cp.name}}.dump());
    BroadcastState();
}

void PluginController::HandleGetCompositePresetListRequest()
{
    SendCompositePresetListToUI();
}

void PluginController::HandleRemoveCompositePresetRequest(const nlohmann::json& payload)
{
    const std::string id = payload.value("id", "");
    if (id.empty())
    {
        ReportErrorToUI("Remove Multi-Rig failed", "Missing preset id");
        return;
    }

    const bool removed = CompositePresetStorage::DeleteFromStore(Store(), id);
    if (!removed)
    {
        ReportErrorToUI("Remove Multi-Rig failed", "Preset not found: " + id);
        return;
    }

    SendCompositePresetListToUI();
}

void PluginController::HandleSaveBlendArchiveRequest(const nlohmann::json& payload)
{
    const std::string dataEncoded = payload.value("data", "");
    const std::string suggestedName = payload.value("fileName", "blend.namz");
    if (dataEncoded.empty())
    {
        SendMessageToUI(nlohmann::json{{"type", "blendExportFailed"}, {"message", "Missing export data"}}.dump());
        return;
    }

    mHost.SaveFileAsync(
        BrowseFileType::ArchiveFile, "Save Blend Archive", suggestedName,
        [this, dataEncoded](const BrowseFileResult& result) {
            if (!result.success)
            {
                SendMessageToUI(nlohmann::json{{"type", "blendExportFailed"}, {"message", "Save cancelled"}}.dump());
                return;
            }

            const auto decodedBytes = util::DecodeBase64(dataEncoded);
            if (decodedBytes.empty())
            {
                SendMessageToUI(
                    nlohmann::json{{"type", "blendExportFailed"}, {"message", "Invalid export data"}}.dump());
                return;
            }

            if (!WriteFile(result.path, decodedBytes))
            {
                SendMessageToUI(
                    nlohmann::json{{"type", "blendExportFailed"}, {"message", "Failed to save file"}}.dump());
                return;
            }

            SendMessageToUI(
                nlohmann::json{{"type", "blendExportSaved"}, {"path", result.path.generic_string()}}.dump());
            AppendSessionLog("Blend export saved: " + result.path.generic_string());
        });
}

void PluginController::HandleSaveCompositeDefinitionRequest(const nlohmann::json& payload)
{
    const nlohmann::json defJson = payload.value("definition", nlohmann::json::object());
    if (!defJson.is_object() || defJson.empty())
    {
        ReportErrorToUI("Composite save failed", "Missing definition payload");
        return;
    }

    CompositeEffectDefinition def;
    try
    {
        def = DeserializeCompositeEffectDefinition(defJson);
    }
    catch (const std::exception& e)
    {
        ReportErrorToUI("Composite save failed", std::string("Invalid definition: ") + e.what());
        return;
    }

    if (!def.IsValid())
    {
        ReportErrorToUI("Composite save failed", "Definition is invalid (missing id/name/innerGraph)");
        return;
    }

    const auto userDir = mFileSystem.ResolveSettingsDirectory() / "composites" / "user";
    if (!mCompositeLibrary.SaveDefinition(def, userDir))
    {
        ReportErrorToUI("Composite save failed", "Could not write definition file");
        return;
    }

    mCompositeLibrary.AddDefinition(def);
    TouchSharedSyncState({"composites"});

    nlohmann::json response;
    response["type"] = "compositeDefinitionAdded";
    response["definition"] = SerializeCompositeEffectDefinition(def);
    SendMessageToUI(response.dump());
    BroadcastState();
}

void PluginController::HandleDeleteCompositeDefinitionRequest(const nlohmann::json& payload)
{
    const std::string id = payload.value("id", "");
    if (id.empty())
    {
        ReportErrorToUI("Composite delete failed", "Missing definition id");
        return;
    }

    const auto userDir = mFileSystem.ResolveSettingsDirectory() / "composites" / "user";
    mCompositeLibrary.DeleteDefinition(id, userDir);
    TouchSharedSyncState({"composites"});

    nlohmann::json response;
    response["type"] = "compositeDefinitionRemoved";
    response["id"] = id;
    SendMessageToUI(response.dump());
    BroadcastState();
}

void PluginController::HandleEnterCompositeEditModeRequest(const nlohmann::json& payload)
{
    const std::string compositeId = payload.value("compositeId", "");
    if (compositeId.empty())
    {
        ReportErrorToUI("Enter composite edit failed", "Missing compositeId");
        return;
    }

    const auto* def = mCompositeLibrary.GetDefinition(compositeId);
    if (!def)
    {
        ReportErrorToUI("Enter composite edit failed", "Composite not found: " + compositeId);
        return;
    }

    mEditingComposite = *def;
    std::cout << "[Plugin] Entered composite edit mode: " << compositeId << " (" << def->name << ")" << std::endl;
    BroadcastCompositeEditState();
}

void PluginController::HandleExitCompositeEditModeRequest(const nlohmann::json& payload)
{
    const bool save = payload.value("save", false);

    if (save && mEditingComposite)
    {
        const auto userDir = mFileSystem.ResolveSettingsDirectory() / "composites" / "user";
        if (mCompositeLibrary.SaveDefinition(*mEditingComposite, userDir))
        {
            mCompositeLibrary.AddDefinition(*mEditingComposite);
            TouchSharedSyncState({"composites"});

            nlohmann::json response;
            response["type"] = "compositeDefinitionAdded";
            response["definition"] = SerializeCompositeEffectDefinition(*mEditingComposite);
            SendMessageToUI(response.dump());

            std::cout << "[Plugin] Saved composite from edit mode: " << mEditingComposite->id << std::endl;
        }
        else
        {
            ReportErrorToUI("Composite save failed", "Could not write definition file on exit");
        }
    }

    const std::string exitId = mEditingComposite ? mEditingComposite->id : "";
    mEditingComposite.reset();

    std::cout << "[Plugin] Exited composite edit mode" << (save ? " (saved)" : " (cancelled)") << std::endl;

    nlohmann::json exitMsg;
    exitMsg["type"] = "compositeEditModeExited";
    exitMsg["compositeId"] = exitId;
    exitMsg["saved"] = save;
    SendMessageToUI(exitMsg.dump());

    BroadcastState();
}

void PluginController::ApplyBlendDefinitions(Preset& preset)
{
    if (!mBlendLibrary.is_array())
    {
        return;
    }

    auto findBlend = [&](const std::string& id) -> nlohmann::json {
        for (const auto& blend : mBlendLibrary)
        {
            if (blend.is_object() && blend.value("id", "") == id)
            {
                return blend;
            }
        }
        return nlohmann::json::object();
    };

    for (auto& node : preset.graph.nodes)
    {
        if (node.type != EffectGuids::kAmpNamBlend)
        {
            continue;
        }

        const auto blendIt = node.config.find("blendId");
        if (blendIt == node.config.end())
        {
            continue;
        }

        const std::string blendId = blendIt->second;
        if (blendId.empty())
        {
            continue;
        }

        const nlohmann::json blend = findBlend(blendId);
        if (!blend.is_object())
        {
            continue;
        }

        const auto mappingsJson = blend.value("modelMappings", nlohmann::json::array());
        const auto modelsJson = blend.value("models", nlohmann::json::array());
        if ((!mappingsJson.is_array() || mappingsJson.empty()) && (!modelsJson.is_array() || modelsJson.empty()))
        {
            continue;
        }

        node.resources.clear();

        if (mappingsJson.is_array() && !mappingsJson.empty())
        {
            const std::size_t count = mappingsJson.size();
            for (std::size_t i = 0; i < count; ++i)
            {
                const auto& mapping = mappingsJson[i];
                if (!mapping.is_object())
                {
                    continue;
                }

                const std::string modelId = mapping.value("id", "");
                if (modelId.empty())
                {
                    continue;
                }

                ResourceRef ref;
                ref.resourceType = "nam";
                ref.resourceId = modelId;
                const std::string parameterId = mapping.value("parameterId", "");
                if (!parameterId.empty())
                {
                    ref.parameterId = parameterId;
                }
                if (mapping.contains("parameterValue") && mapping["parameterValue"].is_number())
                {
                    ref.parameterValue = mapping["parameterValue"].get<double>();
                }
                else if (count > 1)
                {
                    ref.parameterValue = static_cast<double>(i) / static_cast<double>(count - 1);
                }

                if (mapping.contains("parameters") && mapping["parameters"].is_object())
                {
                    for (const auto& [key, value] : mapping["parameters"].items())
                    {
                        if (value.is_number())
                        {
                            ref.parameters[key] = value.get<double>();
                        }
                    }
                }

                if (ref.parameters.empty() && !ref.parameterId.empty() && ref.parameterValue.has_value())
                {
                    ref.parameters[ref.parameterId] = *ref.parameterValue;
                }
                else
                {
                    ref.parameterValue = 0.0;
                }

                node.resources.push_back(std::move(ref));
            }
        }
        else if (modelsJson.is_array())
        {
            const std::size_t count = modelsJson.size();
            for (std::size_t i = 0; i < count; ++i)
            {
                if (!modelsJson[i].is_string())
                {
                    continue;
                }

                ResourceRef ref;
                ref.resourceType = "nam";
                ref.resourceId = modelsJson[i].get<std::string>();
                ref.parameterValue = (count > 1) ? static_cast<double>(i) / static_cast<double>(count - 1) : 0.0;
                node.resources.push_back(std::move(ref));
            }
        }

        const std::string blendMode = blend.value("blendMode", "interpolate");
        node.config["blendMode"] = blendMode;
        if (node.label.empty())
        {
            node.label = blend.value("name", "");
        }
    }
}

void PluginController::LoadBlendLibrary()
{
    mBlendLibrary = nlohmann::json::array();

    for (const auto& item : Store().List(storage::ItemType::kBlend))
    {
        if (auto parsed = item.Parse())
        {
            mBlendLibrary.push_back(std::move(*parsed));
        }
    }
}

void PluginController::LoadCustomEffectLibrary()
{
    mCustomEffectLibrary.LoadFromStore(Store());
}

void PluginController::SaveBlendLibrary() const
{
    // Factory-archive blends re-register themselves from the archive on every
    // launch, so they are deliberately not persisted here.
    std::vector<storage::StoreItem> items;
    if (mBlendLibrary.is_array())
    {
        for (const auto& blend : mBlendLibrary)
        {
            const std::string id = blend.value("id", "");
            if (id.empty() || mFactoryArchiveBlendIds.contains(id))
            {
                continue;
            }

            storage::StoreItem item;
            item.type = storage::ItemType::kBlend;
            item.id = id;
            item.json = blend.dump();
            items.push_back(std::move(item));
        }
    }

    if (Store().ReplaceAll(storage::ItemType::kBlend, items))
    {
        TouchSharedSyncState({"blends"});
    }
    else
    {
        AppendSessionLog("Failed to save the blend library");
    }
}

void PluginController::SaveCustomEffectLibrary() const
{
    mCustomEffectLibrary.SaveToStore(Store());
    TouchSharedSyncState({"customEffects"});
}

void PluginController::LoadCompositeLibrary()
{
    try
    {
        const auto bundledRoot = mHost.GetBundledAssetsPath();
        const auto factoryDir = bundledRoot / "ui" / "assets" / "composites";
        if (std::filesystem::exists(factoryDir))
        {
            mCompositeLibrary.LoadFromDirectory(factoryDir);
            std::cout << "[Plugin] Loaded factory composite definitions from " << factoryDir.string() << ": "
                      << mCompositeLibrary.GetAllDefinitions().size() << std::endl;
        }
        else
        {
            // Backward-compatible fallback for older layouts.
            const auto legacyFactoryDir = mResourceRoot / "composites";
            if (std::filesystem::exists(legacyFactoryDir))
            {
                mCompositeLibrary.LoadFromDirectory(legacyFactoryDir);
                std::cout << "[Plugin] Loaded legacy factory composite definitions from " << legacyFactoryDir.string()
                          << ": " << mCompositeLibrary.GetAllDefinitions().size() << std::endl;
            }
        }

        const auto userDir = mFileSystem.ResolveSettingsDirectory() / "composites";
        if (std::filesystem::exists(userDir))
        {
            mCompositeLibrary.LoadFromDirectory(userDir);
            std::cout << "[Plugin] Composite library total definitions: "
                      << mCompositeLibrary.GetAllDefinitions().size() << std::endl;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Plugin] Failed to load composite library: " << e.what() << std::endl;
    }
}

} // namespace guitarfx
