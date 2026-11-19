/**
 * UiClientLibraryHandlers.cpp - The effect catalog, effect presets, the resource library,
 * the global chain, settings and errors.
 */

#include "uiclient/UiClient.h"
#include "uiclient/ResourceThumbnails.h"
#include "uiclient/UiClientParsing.h"
#include "uiclient/ThemeTokens.generated.h"

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
        const auto read = [&m](const char* key, const char* source,
                               std::map<std::string, std::vector<EffectPresetInfo>>& presets) {
            presets.clear();

            if (!m.contains(key) || !m[key].is_object())
            {
                return;
            }

            for (const auto& [effectType, list] : m[key].items())
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
                        entries.push_back(parse::EffectPreset(entry, source));
                    }
                }
            }
        };

        read("byEffectType", "custom", mState.customEffectPresets);
        read("factoryByEffectType", "factoryPack", mState.factoryPackEffectPresets);
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

        if (const auto requestId = m.value("requestId", ""); !requestId.empty())
        {
            mState.resourceImports[requestId] = {ResourceImport::Status::Imported, type, id, {}};
        }

        Notify(Topic::Resources);
    });

    On("resourceImportFailed", [this](const nlohmann::json& m) {
        if (const auto requestId = m.value("requestId", ""); !requestId.empty())
        {
            auto& import = mState.resourceImports[requestId];
            import.status = ResourceImport::Status::Failed;
            import.detail = m.value("detail", m.value("message", std::string("The import failed")));
            Notify(Topic::Resources);
        }
    });

    // ── The resource browser's folders (ResourceFolderScanner) ──
    On("resourceFolderListing", [this](const nlohmann::json& m) {
        ResourceFolderListing listing;
        listing.path = m.value("path", "");
        listing.parent = m.value("parent", "");
        listing.name = m.value("name", "");
        listing.truncated = m.value("truncated", false);

        for (const auto& d : m.value("dirs", nlohmann::json::array()))
        {
            if (d.is_object())
            {
                listing.dirs.push_back({d.value("name", ""), d.value("path", "")});
            }
        }

        for (const auto& f : m.value("files", nlohmann::json::array()))
        {
            if (!f.is_object())
            {
                continue;
            }

            ResourceFolderListing::File file;
            file.name = f.value("name", "");
            file.path = f.value("path", "");
            file.resourceType = f.value("resourceType", "");
            file.sizeBytes = f.value("sizeBytes", std::int64_t{0});
            file.inLibrary = f.value("alreadyInLibrary", false);
            file.libraryId = f.value("libraryId", "");

            if (const auto meta = f.find("metadata"); meta != f.end() && meta->is_object())
            {
                file.metadata = *meta;
            }

            listing.files.push_back(std::move(file));
        }

        mState.resourceFolder = std::move(listing);
        Notify(Topic::ResourceFolders);
    });

    On("resourceFolderMetadata", [this](const nlohmann::json& m) {
        // Parsed in batches after the listing: the files' metadata, by path.
        if (m.value("path", "") != mState.resourceFolder.path)
        {
            return;
        }

        for (const auto& item : m.value("items", nlohmann::json::array()))
        {
            if (!item.is_object())
            {
                continue;
            }

            const auto path = item.value("path", "");

            for (auto& file : mState.resourceFolder.files)
            {
                if (file.path == path)
                {
                    if (const auto meta = item.find("metadata"); meta != item.end() && meta->is_object())
                    {
                        file.metadata = *meta;
                    }
                }
            }
        }

        Notify(Topic::ResourceFolders);
    });

    On("resourceFolderListingFailed", [this](const nlohmann::json& m) {
        mState.resourceFolder = {};
        mState.resourceFolder.path = m.value("path", "");
        mState.resourceFolder.error = m.value("message", std::string("The folder could not be read"));
        Notify(Topic::ResourceFolders);
    });

    On("resourceFolderPicked", [this](const nlohmann::json& m) {
        if (m.value("success", false))
        {
            mState.pickedResourceFolder = m.value("path", "");
            Notify(Topic::ResourceFolders);
        }
    });

    // The pictures library resources show in the resource browser (ResourceThumbnails.h).
    On("resourceThumbnails", [this](const nlohmann::json& m) {
        mThumbnails->Apply(m);
        Notify(Topic::Thumbnails);
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
        mState.globalChain.dualMono = m.value("dualMono", mState.globalChain.dualMono);
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
            SyncLooperEnabled();
        }
    });

    On("theme", [this](const nlohmann::json& m) {
        mState.theme = std::string(theme::FindTheme(m.value("theme", "dark")).id);
        Notify(Topic::Session);
    });

    On("error", [this](const nlohmann::json& m) {
        PostNotification({Notification::Kind::Error, m.value("message", "Error"), m.value("detail", "")});
        // A capture the engine refused (busy, no audio device) never started: the dialog's
        // Record button would otherwise wait on it.
        auto& capture = mState.riffs.capture;

        if (m.value("message", "") == "Riff Capture" &&
            (capture.phase == RiffCapture::Phase::Armed || capture.phase == RiffCapture::Phase::Recording))
        {
            capture = RiffCapture{};
            Notify(Topic::Riffs);
        }
    });

    // Tone sharing installs. A failure's reason also arrives as the engine's own error.
    On("presetArchivesInstalled", [this](const nlohmann::json& m) {
        auto& install = mState.toneInstalls[m.value("entryId", std::string{})];
        install.status = ToneInstall::Status::Installed;
        install.detail.clear();
        install.presetIds.clear();

        for (const auto& id : m.value("presetIds", nlohmann::json::array()))
        {
            if (id.is_string())
            {
                install.presetIds.push_back(id.get<std::string>());
            }
        }

        Notify(Topic::Tones);
    });

    // An export through the engine's save dialog: the web UI's notices (messages/presetHandlers.ts).
    On("presetExportSaved", [this](const nlohmann::json& m) {
        PostNotification({Notification::Kind::Info, "Preset exported", m.value("path", std::string{})});

        for (const auto& warning : m.value("warnings", nlohmann::json::array()))
        {
            if (warning.is_string() && !warning.get<std::string>().empty())
            {
                PostNotification({Notification::Kind::Info, "Export warning", warning.get<std::string>()});
            }
        }
    });

    On("presetExportFailed", [this](const nlohmann::json& m) {
        PostNotification({Notification::Kind::Error, "Preset export failed", m.value("message", std::string{})});
    });

    On("presetArchivesInstallFailed", [this](const nlohmann::json& m) {
        auto& install = mState.toneInstalls[m.value("entryId", std::string{})];
        install.status = ToneInstall::Status::Failed;
        install.detail = m.value("detail", m.value("message", std::string{}));
        Notify(Topic::Tones);
    });

    On("installedPresetArchiveDeleted", [this](const nlohmann::json& m) {
        mState.toneInstalls.erase(m.value("id", std::string{}));
        Notify(Topic::Tones);
    });

    On("resourceImportFailed", [this](const nlohmann::json& m) {
        PostNotification({Notification::Kind::Error, "Import failed", m.value("message", "")});
    });

    // exportLibraryArchive's answer, and what it could not carry, as the web UI shows them.
    On("libraryExportSaved", [this](const nlohmann::json& m) {
        PostNotification({Notification::Kind::Info, "Library exported", m.value("path", "")});

        if (const auto warnings = m.find("warnings"); warnings != m.end() && warnings->is_array())
        {
            for (const auto& warning : *warnings)
            {
                if (warning.is_string() && !warning.get<std::string>().empty())
                {
                    PostNotification({Notification::Kind::Error, "Export warning", warning.get<std::string>()});
                }
            }
        }
    });

    On("libraryExportFailed", [this](const nlohmann::json& m) {
        PostNotification({Notification::Kind::Error, "Library export failed", m.value("message", "")});
    });

    On("resourceDeleteFailed", [this](const nlohmann::json& m) {
        PostNotification({Notification::Kind::Error, m.value("message", "Could not delete"), m.value("detail", "")});
    });

    On("resourceUsageInfo", [this](const nlohmann::json& m) {
        ClientState::ResourceUsage usage;
        usage.inUse = m.value("inUse", false);
        usage.detail = m.value("detail", std::string());

        if (usage.detail.empty() && usage.inUse)
        {
            const auto preset = m.value("presetName", std::string());
            const auto blend = m.value("blendName", std::string());
            usage.detail = !preset.empty()  ? "Used by preset " + preset
                           : !blend.empty() ? "Used by blend " + blend
                                            : "In use";
        }

        MutableState().resourceUsage[m.value("resourceType", std::string()) + ":" + m.value("id", std::string())] =
            usage;
        Notify(Topic::Resources);
    });

    On("hostedPluginResourceLoadFailed", [this](const nlohmann::json& m) {
        MutableState().pluginLoadsPending.erase(m.value("nodeId", ""));
        Notify(Topic::ActivePreset);
        PostNotification({Notification::Kind::Error, "Plugin failed to load", m.value("message", "")});
    });

    // A plugin loaded, or the file dialog for one was cancelled: the node is no longer loading.
    On("hostedPluginResourceLoadCompleted", [this](const nlohmann::json& m) {
        MutableState().pluginLoadsPending.erase(m.value("nodeId", ""));
        Notify(Topic::ActivePreset);
    });
    On("nodeResourceBrowseCancelled", [this](const nlohmann::json& m) {
        if (MutableState().pluginLoadsPending.erase(m.value("nodeId", "")) > 0)
        {
            Notify(Topic::ActivePreset);
        }
    });
}
} // namespace guitarfx::uiclient
