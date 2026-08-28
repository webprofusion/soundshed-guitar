/**
 * PluginControllerEffectPresets.cpp - User-saved parameter presets for a
 * single effect type.
 *
 * Factory presets live in the effect registry (EffectTypeInfo::presets); these
 * are the custom half of that same vocabulary, kept in UI storage so they are
 * shared across presets rather than baked into any one of them.
 */

#include "PluginController.h"

#include "controller/internal/ControllerUtils.h"

using namespace guitarfx::controller_detail;

namespace guitarfx
{

namespace
{
constexpr const char* kEffectPresetsFile = "effect-presets.json";

nlohmann::json NormalizeEffectPresetsDocument(nlohmann::json document)
{
    if (!document.is_object())
    {
        document = nlohmann::json::object();
    }
    if (!document.contains("byEffectType") || !document["byEffectType"].is_object())
    {
        document["byEffectType"] = nlohmann::json::object();
    }
    return document;
}
} // namespace

void PluginController::BroadcastEffectPresets()
{
    const auto document =
        NormalizeEffectPresetsDocument(LoadUiStorageJson(kEffectPresetsFile, nlohmann::json::object()));
    nlohmann::json msg;
    msg["type"] = "effectPresets";
    msg["byEffectType"] = document["byEffectType"];
    SendMessageToUI(msg.dump());
}

void PluginController::HandleGetEffectPresetsRequest()
{
    BroadcastEffectPresets();
}

void PluginController::HandleSaveEffectPresetRequest(const nlohmann::json& payload)
{
    const std::string effectType = payload.value("effectType", "");
    const std::string name = payload.value("name", "");
    if (effectType.empty() || name.empty())
    {
        return;
    }

    const auto parameters = payload.value("parameters", nlohmann::json::object());
    if (!parameters.is_object())
    {
        return;
    }

    auto document = NormalizeEffectPresetsDocument(LoadUiStorageJson(kEffectPresetsFile, nlohmann::json::object()));

    auto& presets = document["byEffectType"][effectType];
    if (!presets.is_array())
    {
        presets = nlohmann::json::array();
    }

    // Saving under an existing name overwrites that entry in place, keeping its id
    // so any UI selection pointing at it stays valid.
    for (auto& existing : presets)
    {
        if (existing.value("name", "") == name)
        {
            existing["parameters"] = parameters;
            SaveUiStorageJson(kEffectPresetsFile, document);
            BroadcastEffectPresets();
            return;
        }
    }

    nlohmann::json entry;
    entry["id"] = "efp-" + GenerateGuidV4String();
    entry["name"] = name;
    entry["parameters"] = parameters;
    presets.push_back(std::move(entry));

    SaveUiStorageJson(kEffectPresetsFile, document);
    BroadcastEffectPresets();
}

void PluginController::HandleDeleteEffectPresetRequest(const nlohmann::json& payload)
{
    const std::string effectType = payload.value("effectType", "");
    const std::string presetId = payload.value("presetId", "");
    if (effectType.empty() || presetId.empty())
    {
        return;
    }

    auto document = NormalizeEffectPresetsDocument(LoadUiStorageJson(kEffectPresetsFile, nlohmann::json::object()));
    if (!document["byEffectType"].contains(effectType))
    {
        return;
    }

    auto& presets = document["byEffectType"][effectType];
    if (!presets.is_array())
    {
        return;
    }

    nlohmann::json remaining = nlohmann::json::array();
    for (const auto& entry : presets)
    {
        if (entry.value("id", "") != presetId)
        {
            remaining.push_back(entry);
        }
    }

    if (remaining.size() == presets.size())
    {
        return; // nothing matched — leave the file untouched
    }

    if (remaining.empty())
    {
        document["byEffectType"].erase(effectType);
    }
    else
    {
        presets = std::move(remaining);
    }

    SaveUiStorageJson(kEffectPresetsFile, document);
    BroadcastEffectPresets();
}

} // namespace guitarfx
