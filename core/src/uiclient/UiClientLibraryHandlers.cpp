/**
 * UiClientLibraryHandlers.cpp - The effect catalog, effect presets, the resource library,
 * the global chain, settings and errors.
 */

#include "uiclient/UiClient.h"
#include "uiclient/UiClientParsing.h"

#include "presets/PresetTypesJson.h"

#include <algorithm>

namespace guitarfx::uiclient
{
void UiClient::RegisterLibraryHandlers()
{
    On("effectCatalog", [this](const nlohmann::json& m) {
        mState.catalog = parse::EffectCatalog(m.value("catalog", nlohmann::json::array()));
        Notify(Topic::Catalog);
    });

    On("effectPresets", [this](const nlohmann::json& m) {
        auto& presets = mState.customEffectPresets;
        presets.clear();

        if (m.contains("byEffectType") && m["byEffectType"].is_object())
        {
            for (const auto& [effectType, list] : m["byEffectType"].items())
            {
                if (!list.is_array())
                {
                    continue;
                }

                auto& entries = presets[effectType];

                for (const auto& entry : list)
                {
                    if (entry.is_object())
                    {
                        entries.push_back(parse::EffectPreset(entry, "custom"));
                    }
                }
            }
        }

        Notify(Topic::Catalog);
    });

    On("resourceImported", [this](const nlohmann::json& m) {
        const auto type = m.value("resourceType", "");
        const auto id = m.value("id", "");

        if (type.empty() || id.empty())
        {
            return;
        }

        auto& list = mState.resources[type];
        const auto it = std::find_if(list.begin(), list.end(), [&id](const LibraryResource& r) { return r.id == id; });
        LibraryResource& entry = it != list.end() ? *it : list.emplace_back();
        entry.id = id;
        entry.type = type;
        entry.name = m.value("name", entry.name.empty() ? id : entry.name);
        entry.filePath = m.value("filePath", entry.filePath);
        entry.fileMissing = false;
        Notify(Topic::Resources);
    });

    On("resourceRemoved", [this](const nlohmann::json& m) {
        const auto type = m.value("resourceType", "");
        const auto id = m.value("id", "");
        auto& list = mState.resources[type];
        list.erase(std::remove_if(list.begin(), list.end(), [&id](const LibraryResource& r) { return r.id == id; }),
                   list.end());
        Notify(Topic::Resources);
    });

    On("globalChain", [this](const nlohmann::json& m) {
        if (m.contains("config") && m["config"].is_object())
        {
            mState.globalChain = m["config"].get<GlobalSignalChainConfig>();
            Notify(Topic::GlobalChain);
        }
    });

    On("inputModeChanged", [this](const nlohmann::json& m) {
        mState.globalChain.monoMode = m.value("monoMode", mState.globalChain.monoMode);
        mState.globalChain.inputChannel = m.value("inputChannel", mState.globalChain.inputChannel);
        Notify(Topic::GlobalChain);
    });

    On("outputMutedChanged", [this](const nlohmann::json& m) {
        mState.outputMuted = m.value("muted", false);
        Notify(Topic::GlobalChain);
    });

    // One app setting the engine changed itself (a resource favourite).
    On("appSettingChanged", [this](const nlohmann::json& m) {
        const auto key = m.value("key", std::string{});

        if (!key.empty())
        {
            mState.appSettings[key] = m.contains("value") ? m["value"] : nlohmann::json();
            Notify(Topic::Session);
        }
    });

    On("theme", [this](const nlohmann::json& m) {
        const auto theme = m.value("theme", "dark");
        mState.theme = theme == "light" || theme == "classic" ? theme : "dark";
        Notify(Topic::Session);
    });

    On("error", [this](const nlohmann::json& m) {
        PostNotification({Notification::Kind::Error, m.value("message", "Error"), m.value("detail", "")});
    });

    On("resourceImportFailed", [this](const nlohmann::json& m) {
        PostNotification({Notification::Kind::Error, "Import failed", m.value("message", "")});
    });

    On("resourceDeleteFailed", [this](const nlohmann::json& m) {
        PostNotification({Notification::Kind::Error, m.value("message", "Could not delete"), m.value("detail", "")});
    });

    On("hostedPluginResourceLoadFailed", [this](const nlohmann::json& m) {
        PostNotification({Notification::Kind::Error, "Plugin failed to load", m.value("message", "")});
    });
}
} // namespace guitarfx::uiclient
