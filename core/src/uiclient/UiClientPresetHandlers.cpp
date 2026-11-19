/**
 * UiClientPresetHandlers.cpp - The full state, the active preset and the preset library.
 */

#include "uiclient/Automation.h"
#include "uiclient/Riffs.h"
#include "uiclient/ThemeTokens.generated.h"
#include "uiclient/UiClient.h"
#include "uiclient/UiClientParsing.h"

#include "presets/PresetStorage.h"
#include "presets/PresetTypesJson.h"

#include <algorithm>
#include <chrono>

namespace guitarfx::uiclient
{
namespace
{
/// A stored theme name as one this build knows (core/ui/ts/themes.ts), as theme-switcher.ts
/// normalises it: an earlier build's name becomes the theme it is now, anything else dark.
std::string NormalizeTheme(const std::string& theme)
{
    return std::string(theme::FindTheme(theme).id);
}
} // namespace

const CompositePreset* LoadedComposite(const MixerState& mixer)
{
    if (mixer.loadedCompositeId.empty())
    {
        return nullptr;
    }

    for (const auto& preset : mixer.compositePresets)
    {
        if (preset.id != mixer.loadedCompositeId)
        {
            continue;
        }

        auto saved = preset.presetIds;
        auto now = mixer.activePresetIds;
        std::sort(saved.begin(), saved.end());
        std::sort(now.begin(), now.end());
        return saved == now ? &preset : nullptr;
    }

    return nullptr;
}

namespace
{
void ApplyMixer(ClientState& state, const nlohmann::json& mixer)
{
    state.mixer.masterGain = mixer.value("masterGain", 1.0);
    state.mixer.mixGainDb = mixer.value("mixGainDb", 0.0);
    state.mixer.limiterEnabled = mixer.value("limiterEnabled", false);
    state.mixer.activePresetIds = parse::StringArray(mixer, "activePresetIds");
    state.mixer.presetNames.clear();
    state.mixer.slots.clear();

    if (mixer.contains("presets") && mixer["presets"].is_object())
    {
        for (const auto& [id, config] : mixer["presets"].items())
        {
            state.mixer.presetNames[id] = config.is_object() ? config.value("name", id) : id;
            MixerSlot slot;
            slot.name = state.mixer.presetNames[id];

            if (config.is_object())
            {
                slot.mix = config.value("mix", 1.0);
                slot.pan = config.value("pan", 0.0);
                slot.mute = config.value("mute", false);
                slot.solo = config.value("solo", false);
            }

            state.mixer.slots[id] = slot;
        }
    }
}

void ApplyMetronome(MetronomeState& metronome, const nlohmann::json& json)
{
    metronome.bpm = json.value("bpm", metronome.bpm);
    metronome.enabled = json.value("enabled", metronome.enabled);
    metronome.editable = json.value("editable", metronome.editable);
    metronome.source = json.value("source", metronome.source);
    metronome.volumeDb = json.value("volumeDb", metronome.volumeDb);
    metronome.pan = json.value("pan", metronome.pan);
    metronome.clickType = json.value("clickType", metronome.clickType);
    metronome.beatPattern = json.value("beatPattern", metronome.beatPattern);
    metronome.timeSigNum = json.value("timeSigNum", metronome.timeSigNum);
    metronome.timeSigDen = json.value("timeSigDen", metronome.timeSigDen);
    metronome.grouping = json.value("grouping", metronome.grouping);
    metronome.subdivision = json.value("subdivision", metronome.subdivision);

    if (json.contains("clickTypes") && json["clickTypes"].is_array())
    {
        metronome.clickTypes.clear();

        for (const auto& entry : json["clickTypes"])
        {
            if (entry.is_object())
            {
                metronome.clickTypes.emplace_back(entry.value("id", ""), entry.value("label", entry.value("id", "")));
            }
        }
    }

    if (json.contains("subdivisions") && json["subdivisions"].is_array())
    {
        metronome.subdivisions.clear();

        for (const auto& entry : json["subdivisions"])
        {
            if (entry.is_object())
            {
                metronome.subdivisions.emplace_back(entry.value("id", ""), entry.value("ticksPerBeat", 1));
            }
        }
    }
}

/// Favourite riffs as demo clips, as core/ui/ts/demoAudio.ts lists them: each by its
/// preferred take (else its first), titled with a star.
std::vector<DemoClip> FavouriteRiffClips(const nlohmann::json& library)
{
    std::vector<DemoClip> clips;

    if (!library.contains("riffs") || !library["riffs"].is_array())
    {
        return clips;
    }

    for (const auto& riff : library["riffs"])
    {
        if (!riff.is_object() || !riff.value("favorite", false) || !riff.contains("takes") ||
            !riff["takes"].is_array() || riff["takes"].empty())
        {
            continue;
        }

        const auto& takes = riff["takes"];
        auto takeId = takes.front().is_object() ? takes.front().value("id", std::string{}) : std::string{};
        const auto preferred = riff.value("preferredTakeId", std::string{});

        for (const auto& take : takes)
        {
            if (!preferred.empty() && take.is_object() && take.value("id", std::string{}) == preferred)
            {
                takeId = preferred;
            }
        }

        if (!takeId.empty())
        {
            clips.push_back({"riff:" + takeId, "\xE2\x98\x85 " + riff.value("title", std::string("Riff")), takeId});
        }
    }

    return clips;
}

std::vector<PresetFolder> ParseFolders(const nlohmann::json& folders)
{
    std::vector<PresetFolder> result;

    if (!folders.is_array())
    {
        return result;
    }

    for (const auto& folder : folders)
    {
        if (!folder.is_object())
        {
            continue;
        }

        PresetFolder parsed;
        parsed.id = folder.value("id", "");
        parsed.name = folder.value("name", parsed.id);
        parsed.presetIds = parse::StringArray(folder, "presetIds");

        if (folder.contains("children"))
        {
            parsed.children = ParseFolders(folder["children"]);
        }

        result.push_back(std::move(parsed));
    }

    return result;
}
} // namespace

void UiClient::ApplyRiffLibrary(const nlohmann::json& library)
{
    mState.demo.riffClips = FavouriteRiffClips(library);
    mState.riffs.known = true;
    mState.riffs.path = library.value("path", std::string{});
    mState.riffs.riffs = ParseRiffs(library);
    Notify(Topic::Demo);
    Notify(Topic::Riffs);
}

void UiClient::RegisterPresetHandlers()
{
    On("state", [this](const nlohmann::json& m) {
        const auto started = std::chrono::steady_clock::now();
        auto& s = mState;
        const bool full = m.contains("appSettings");

        if (m.contains("preset") && m["preset"].is_object())
        {
            s.activePreset = parse::ParsePreset(m["preset"]);
            s.activePresetId = m.value("activePresetId", s.activePreset ? s.activePreset->id : std::string{});
            s.activeSceneId = m.value("activeSceneId", std::string{});
            Notify(Topic::ActivePreset);
        }

        if (m.contains("activePresetDirty"))
        {
            s.activePresetDirty = m.value("activePresetDirty", false);
            Notify(Topic::ActivePreset);
        }

        if (m.contains("globalSignalChain") && m["globalSignalChain"].is_object())
        {
            s.globalChain = m["globalSignalChain"].get<GlobalSignalChainConfig>();
            Notify(Topic::GlobalChain);
        }

        if (m.contains("outputMuted"))
        {
            s.outputMuted = m.value("outputMuted", false);
            Notify(Topic::GlobalChain);
        }

        if (m.contains("mixer") && m["mixer"].is_object())
        {
            ApplyMixer(s, m["mixer"]);
            Notify(Topic::Mixer);
        }

        if (full)
        {
            s.haveState = true;
            s.appSettings = m["appSettings"].is_object() ? m["appSettings"] : nlohmann::json::object();
            s.theme = NormalizeTheme(s.appSettings.value("theme", s.theme));
            s.uiSettings = m.value("uiSettings", nlohmann::json::object());
            s.uiViewState = m.value("uiViewState", nlohmann::json::object());
            SyncLooperEnabled();

            // The automation slots, as they stand after an edit elsewhere (the registry comes
            // with getAutomation's answer).
            if (m.contains("automation") && m["automation"].is_array())
            {
                s.automation.slots = ParseAutomationSlots(m["automation"]);
                Notify(Topic::Automation);
            }

            // The blends a blend node can name: their name, mode and how many models they hold.
            s.blends.clear();

            if (m.contains("blendLibrary") && m["blendLibrary"].is_array())
            {
                for (const auto& b : m["blendLibrary"])
                {
                    if (!b.is_object() || !b.contains("id") || !b["id"].is_string())
                    {
                        continue;
                    }

                    BlendSummary blend;
                    blend.id = b["id"].get<std::string>();
                    blend.name = b.value("name", blend.id);

                    if (b.value("blendMode", std::string()) == "snap")
                    {
                        blend.blendMode = "snap";
                    }

                    const auto mappings = b.find("modelMappings");
                    const auto models = b.find("models");
                    blend.modelCount = mappings != b.end() && mappings->is_array() && !mappings->empty()
                                           ? mappings->size()
                                       : models != b.end() && models->is_array() ? models->size()
                                                                                 : 0;
                    s.blends.push_back(std::move(blend));
                }
            }

            if (m.contains("environment") && m["environment"].is_object())
            {
                const auto& env = m["environment"];
                s.environment.standalone = env.value("standalone", true);
                s.environment.audioDeviceSettings = env.value("audioDeviceSettings", false);
                s.environment.version = env.value("version", "");
                s.environment.os = env.value("os", "");
                s.environment.cpu = env.value("cpu", "");
            }

            if (m.contains("metronome") && m["metronome"].is_object())
            {
                ApplyMetronome(s.metronome, m["metronome"]);
                Notify(Topic::Metronome);
            }

            if (m.contains("demoClips") && m["demoClips"].is_array())
            {
                s.demo.clips.clear();

                for (const auto& clip : m["demoClips"])
                {
                    if (clip.is_object())
                    {
                        s.demo.clips.push_back({clip.value("id", ""), clip.value("title", clip.value("id", "")), {}});
                    }
                }

                Notify(Topic::Demo);
            }

            if (m.contains("riffLibrary") && m["riffLibrary"].is_object())
            {
                ApplyRiffLibrary(m["riffLibrary"]);
            }

            Notify(Topic::Session);

            mLastFullStateApplyMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        }
    });

    On("presetLoaded", [this](const nlohmann::json& m) {
        auto& s = mState;

        if (m.contains("preset") && m["preset"].is_object())
        {
            s.activePreset = parse::ParsePreset(m["preset"]);

            if (s.activePreset)
            {
                s.activePresetId = s.activePreset->id;
            }
        }

        s.activeSceneId = m.value("sceneId", s.activeSceneId);
        s.activePresetDirty = m.value("activePresetDirty", false);

        if (m.contains("activePresetIds"))
        {
            s.mixer.activePresetIds = parse::StringArray(m, "activePresetIds");
            Notify(Topic::Mixer);
        }

        Notify(Topic::ActivePreset);
    });

    // ── Multi-Rigs ──────────────────────────────────────────────────────────
    On("compositePresetList", [this](const nlohmann::json& m) {
        auto& list = mState.mixer.compositePresets;
        list.clear();

        for (const auto& entry : m.value("compositePresets", nlohmann::json::array()))
        {
            if (!entry.is_object())
            {
                continue;
            }

            CompositePreset preset;
            preset.id = entry.value("id", std::string());
            preset.name = entry.value("name", preset.id);
            preset.description = entry.value("description", std::string());
            preset.tags = parse::StringArray(entry, "tags");
            preset.mixGainDb = entry.value("mixGainDb", 0.0);

            for (const auto& slot : entry.value("slots", nlohmann::json::array()))
            {
                if (slot.is_object())
                {
                    preset.presetIds.push_back(slot.value("presetId", std::string()));
                }
            }

            list.push_back(std::move(preset));
        }

        mState.mixer.compositePresetsKnown = true;
        Notify(Topic::Mixer);
    });

    const auto noteComposite = [this](const nlohmann::json& m) {
        mState.mixer.loadedCompositeId = m.value("id", std::string());
        // A save or load changes the list's entry; ask for it again.
        Send("getCompositePresetList");
        Notify(Topic::Mixer);
    };
    On("compositePresetSaved", noteComposite);
    On("compositePresetLoaded", noteComposite);

    On("presetDirtyChanged", [this](const nlohmann::json& m) {
        mState.activePresetDirty = m.value("dirty", false);
        Notify(Topic::ActivePreset);
    });

    On("presetSaved", [this](const nlohmann::json& m) {
        auto& s = mState;

        if (m.contains("preset") && m["preset"].is_object())
        {
            s.activePreset = parse::ParsePreset(m["preset"]);

            if (s.activePreset)
            {
                s.activePresetId = s.activePreset->id;
            }
        }

        s.activeSceneId = m.value("sceneId", s.activeSceneId);
        s.activePresetDirty = false;
        Notify(Topic::ActivePreset);
        PostNotification({Notification::Kind::Info, "Preset saved", s.activePreset ? s.activePreset->name : ""});

        // A saved preset may be new, or renamed: the library list is the engine's to rebuild.
        Send("getPresetList");
    });

    On("presetList", [this](const nlohmann::json& m) {
        auto& list = mState.presetList;
        list.clear();

        if (m.contains("presets") && m["presets"].is_array())
        {
            for (const auto& entry : m["presets"])
            {
                if (!entry.is_object())
                {
                    continue;
                }

                PresetSummary summary;
                summary.id = entry.value("id", "");
                summary.name = entry.value("name", summary.id);
                summary.category = entry.value("category", "Factory");
                summary.source = entry.value("source", "");
                summary.description = entry.value("description", "");
                summary.tags = parse::StringArray(entry, "tags");

                if (!summary.id.empty())
                {
                    list.push_back(std::move(summary));
                }
            }
        }

        Notify(Topic::PresetLibrary);
    });

    On("presetFolders", [this](const nlohmann::json& m) {
        mState.presetFolders = ParseFolders(m.value("folders", nlohmann::json::array()));
        mState.activePresetFolderId = m.value("activeFolderId", "__all__");
        Notify(Topic::PresetLibrary);
    });

    On("presetFavorites", [this](const nlohmann::json& m) {
        mState.presetFavorites = parse::StringArray(m, "favorites");
        Notify(Topic::PresetLibrary);
    });

    On("presetRatings", [this](const nlohmann::json& m) {
        mState.presetRatings = m.value("ratings", nlohmann::json::object());
        Notify(Topic::PresetLibrary);
    });

    On("presetRecents", [this](const nlohmann::json& m) {
        mState.presetRecents = parse::StringArray(m, "presetIds");
        Notify(Topic::PresetLibrary);
    });

    On("setlists", [this](const nlohmann::json& m) {
        auto& s = mState;
        s.setlists.clear();

        if (m.contains("setlists") && m["setlists"].is_array())
        {
            for (const auto& entry : m["setlists"])
            {
                if (!entry.is_object())
                {
                    continue;
                }

                Setlist setlist;
                setlist.id = entry.value("id", "");
                setlist.name = entry.value("name", setlist.id);

                if (entry.contains("bank") && entry["bank"].is_number_integer())
                {
                    setlist.bank = entry["bank"].get<int>();
                }

                if (entry.contains("slots") && entry["slots"].is_array())
                {
                    for (const auto& slot : entry["slots"])
                    {
                        setlist.presetIds.push_back(slot.is_object() ? slot.value("presetId", "") : std::string{});
                    }
                }

                s.setlists.push_back(std::move(setlist));
            }
        }

        s.activeSetlistId = m.value("activeSetlistId", "");
        Notify(Topic::Setlists);
    });

    // Not part of "state": the engine sends it when the library changed since this client last
    // received it, and always right before the state that follows such a change.
    On("resourceLibrary", [this](const nlohmann::json& m) {
        if (m.contains("resourceLibrary") && m["resourceLibrary"].is_object())
        {
            mState.resources = parse::ResourceLibrary(m["resourceLibrary"]);
            Notify(Topic::Resources);
        }
    });

    On("riffLibraryState", [this](const nlohmann::json& m) {
        if (m.contains("library") && m["library"].is_object())
        {
            ApplyRiffLibrary(m["library"]);
        }
    });

    On("setlistCursorChanged", [this](const nlohmann::json& m) {
        mState.activeSetlistId = m.value("activeSetlistId", mState.activeSetlistId);
        mState.setlistCursor = m.value("cursorIndex", 0);
        Notify(Topic::Setlists);
    });

    // MIDI or host automation moved a node's parameter: mirror it into the working copy
    // this UI draws from, as the web UI does.
    On("signalPathNodeParamUpdated", [this](const nlohmann::json& m) {
        auto& s = mState;

        if (!s.activePreset)
        {
            return;
        }

        if (auto* node = s.activePreset->graph.FindNode(m.value("nodeId", "")))
        {
            node->params[m.value("key", "")] = m.value("value", 0.0);
            Notify(Topic::ActivePreset);
        }
    });

    On("signalPathNodeConfigUpdated", [this](const nlohmann::json& m) {
        auto& s = mState;

        if (!s.activePreset)
        {
            return;
        }

        if (auto* node = s.activePreset->graph.FindNode(m.value("nodeId", "")))
        {
            const auto key = m.value("key", "");
            const auto& value = m.contains("value") ? m["value"] : nlohmann::json();
            node->config[key] = value.is_string() ? value.get<std::string>() : value.dump();
            Notify(Topic::ActivePreset);
        }
    });

    // Another instance, or the other product, changed something shared: fetch it again.
    On("sharedSyncUpdated", [this](const nlohmann::json& m) {
        const auto domains = parse::StringArray(m, "domains");
        const auto touches = [&domains](const char* domain) {
            return domains.empty() || std::find(domains.begin(), domains.end(), domain) != domains.end();
        };

        if (touches("presetLibrary"))
        {
            Send("getPresetList");
        }

        if (touches("presetMetadata"))
        {
            Send("getPresetFolders");
            Send("getPresetFavorites");
            Send("getPresetRatings");
            Send("getPresetRecents");
        }

        if (touches("setlists"))
        {
            Send("getSetlists");
        }

        // The full state carries the settings, the library's revision, the blends and the custom
        // effects, so one request catches all of them up.
        if (touches("resourceLibrary") || touches("appSettings") || touches("blends") || touches("customEffects"))
        {
            Send("requestState");
        }

        if (touches("composites"))
        {
            Send("getCompositePresetList");
        }
    });
}
} // namespace guitarfx::uiclient
