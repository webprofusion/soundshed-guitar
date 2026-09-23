/**
 * PluginControllerPresetEdits.cpp - Preset edits the engine owns, whichever UI asks.
 *
 * Soundshed Guitar's web UI used to make these edits to its own copy of the preset (or of a
 * metadata document) and send the result back whole; Soundshed Guitar Nano asks for them by
 * name. Doing them here means the two UIs cannot write a preset, a scene list or a favourites
 * document differently (docs/plans/native-ui.md, rule C3):
 *
 *  - scenes: select, add, rename, remove (the rules of core/ui/ts/presetScenes.ts);
 *  - a new, unsaved preset from the default template;
 *  - one preset's favourite or rating, without rewriting the whole document;
 *  - the recently played list (kept in the UI settings, where the web UI always kept it);
 *  - choosing the active setlist;
 *  - one resource's favourite;
 *  - the output mute, which the mixer applies after the output gain, so a chain rebuild
 *    re-deriving that gain can no longer undo it;
 *  - the unsaved-changes flag: the working copy compared with a baseline taken when the
 *    preset was loaded or saved.
 */

#include "PluginController.h"

#include "controller/internal/ControllerUtils.h"
#include "controller/internal/HostedPluginSupport.h"
#include "presets/PresetStorage.h"

#include <algorithm>
#include <chrono>

using namespace guitarfx::controller_detail;

namespace guitarfx
{
namespace
{
/// The web UI keeps four (core/ui/ts/presets/favorites.ts, MAX_RECENT_PRESETS).
constexpr std::size_t kMaxRecentPresets = 4;

/// How often the unsaved-changes flag is re-checked: a comparison serialises the preset.
constexpr double kDirtyCheckIntervalSeconds = 0.4;

double NowSeconds()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string BuildDefaultSceneId(std::size_t index)
{
    return "scene-" + std::to_string(index + 1);
}

std::string BuildUniqueSceneId(const Preset& preset)
{
    std::size_t index = preset.scenes.size();

    for (;;)
    {
        auto candidate = BuildDefaultSceneId(index);
        const bool taken = std::any_of(preset.scenes.begin(), preset.scenes.end(),
                                       [&candidate](const PresetScene& scene) { return scene.id == candidate; });

        if (!taken)
        {
            return candidate;
        }

        ++index;
    }
}

GraphNode MakeNode(const std::string& id, const std::string& type, const std::string& label,
                   const std::string& category, bool enabled, std::map<std::string, double> params)
{
    GraphNode node;
    node.id = id;
    node.type = type;
    node.label = label;
    node.category = category;
    node.enabled = enabled;
    node.params = std::move(params);
    return node;
}

/// A new preset, as core/ui/ts/presetV2.ts's createEmptyPresetV2 has always built it: input,
/// a bypassed gate and Neural FX, a Neural Amp, a bypassed IR cab, output.
Preset BuildNewPreset()
{
    Preset preset;
    preset.id = GenerateUserPresetId();
    preset.name = "New Preset";
    preset.category = "User";
    preset.version = 2;
    preset.createdAt = BuildUtcIsoTimestamp();
    preset.modifiedAt = preset.createdAt;

    auto& graph = preset.graph;
    graph.nodes.push_back(MakeNode("__input__", kNodeTypeInput, "Input", "utility", true, {}));
    graph.nodes.push_back(MakeNode("gate_0", EffectGuids::kDynamicsGate, "Noise Gate", "dynamics", false,
                                   {{"threshold", -40.0}, {"attack", 0.5}, {"hold", 50.0}, {"release", 100.0}}));
    graph.nodes.push_back(MakeNode("fx_0", EffectGuids::kFxNam, "Neural FX", "amp", false,
                                   {{"inputGain", 0.0}, {"outputGain", 0.0}, {"useCalibration", 1.0}}));
    graph.nodes.push_back(MakeNode("amp_0", EffectGuids::kAmpNamOptimized, "Neural Amp", "amp", true,
                                   {{"inputGain", 0.0}, {"outputGain", 0.0}, {"useCalibration", 1.0}}));
    graph.nodes.push_back(MakeNode("cab_0", EffectGuids::kCabIr, "IR Cab", "cab", false,
                                   {{"mix", 1.0}, {"outputGain", 0.0}, {"quality", 1.0}}));
    graph.nodes.push_back(MakeNode("__output__", kNodeTypeOutput, "Output", "utility", true, {}));

    const char* chain[] = {"__input__", "gate_0", "fx_0", "amp_0", "cab_0", "__output__"};

    for (std::size_t i = 0; i + 1 < std::size(chain); ++i)
    {
        GraphEdge edge;
        edge.from = chain[i];
        edge.to = chain[i + 1];
        graph.edges.push_back(edge);
    }

    return preset;
}

void StripHostedPluginState(SignalGraph& graph)
{
    for (auto& node : graph.nodes)
    {
        node.config.erase(kHostedPluginStateConfigKey);
        node.config.erase(kHostedPluginStateLengthConfigKey);
    }
}
} // namespace

// ── Scenes ──────────────────────────────────────────────────────────────────

void PluginController::HandleSelectSceneRequest(const nlohmann::json& payload)
{
    if (!mActivePreset)
    {
        return;
    }

    NormalizePresetScenes(*mActivePreset);
    const auto sceneId = payload.value("sceneId", std::string{});
    const auto& scenes = mActivePreset->scenes;
    const auto it = std::find_if(scenes.begin(), scenes.end(),
                                 [&sceneId](const PresetScene& scene) { return scene.id == sceneId; });

    if (it == scenes.end())
    {
        ReportErrorToUI("Scene not found", sceneId);
        return;
    }

    // The same switch a footswitch makes: banks the outgoing scene's plugin state, applies
    // the new scene and reports it with "presetLoaded".
    SelectSceneByIndex(static_cast<int>(it - scenes.begin()));
}

void PluginController::HandleAddSceneRequest(const nlohmann::json& payload)
{
    if (!mActivePreset)
    {
        return;
    }

    CaptureLiveHostedPluginStateIntoActivePreset();
    NormalizePresetScenes(*mActivePreset);

    const auto fromSceneId = payload.value("fromSceneId", GetResolvedActiveSceneId());
    const auto* source = FindPresetScene(*mActivePreset, fromSceneId);

    PresetScene scene;
    scene.id = BuildUniqueSceneId(*mActivePreset);
    scene.title = payload.value("title", std::string{});

    if (scene.title.empty())
    {
        scene.title = "Scene " + std::to_string(mActivePreset->scenes.size() + 1);
    }

    scene.graph = source ? source->graph : mActivePreset->graph;
    const auto newSceneId = scene.id;
    mActivePreset->scenes.push_back(std::move(scene));

    // The new scene is a copy of the one playing, so switching to it changes nothing audible.
    (void)SetPresetActiveScene(*mActivePreset, newSceneId, &mActiveSceneId);
    mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);
    mMixerPresetJsonCache[mActivePresetId] = mActivePresetJson;
    SendActivePresetLoaded();
}

void PluginController::HandleRenameSceneRequest(const nlohmann::json& payload)
{
    if (!mActivePreset)
    {
        return;
    }

    NormalizePresetScenes(*mActivePreset);
    auto* scene = FindPresetScene(*mActivePreset, payload.value("sceneId", std::string{}));

    if (!scene)
    {
        return;
    }

    auto title = payload.value("title", std::string{});
    title.erase(0, title.find_first_not_of(" \t\r\n"));
    title.erase(title.find_last_not_of(" \t\r\n") + 1);
    scene->title = title.empty() ? std::string("Scene") : title;

    mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);
    mMixerPresetJsonCache[mActivePresetId] = mActivePresetJson;
    SendActivePresetLoaded();
}

void PluginController::HandleRemoveSceneRequest(const nlohmann::json& payload)
{
    if (!mActivePreset)
    {
        return;
    }

    CaptureLiveHostedPluginStateIntoActivePreset();
    NormalizePresetScenes(*mActivePreset);
    auto& scenes = mActivePreset->scenes;

    if (scenes.size() <= 1)
    {
        ReportErrorToUI("Cannot remove scene", "A preset must keep at least one scene");
        return;
    }

    const auto sceneId = payload.value("sceneId", std::string{});
    const auto it =
        std::find_if(scenes.begin(), scenes.end(), [&sceneId](const PresetScene& s) { return s.id == sceneId; });

    if (it == scenes.end())
    {
        return;
    }

    const bool wasActive = sceneId == GetResolvedActiveSceneId();
    const auto removeIndex = static_cast<std::size_t>(it - scenes.begin());
    scenes.erase(it);

    // As the web UI does: the scene now in the removed one's place, or the new last one.
    const auto& fallback = scenes[std::min(removeIndex, scenes.size() - 1)];
    const auto nextSceneId = wasActive ? fallback.id : GetResolvedActiveSceneId();

    (void)SetPresetActiveScene(*mActivePreset, nextSceneId, &mActiveSceneId);

    if (wasActive)
    {
        // The removed scene was playing: the one that replaces it has to be heard. In a
        // multi-preset mix only this preset's slot is rebuilt.
        ApplyActivePresetInItsSlot();
    }

    mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);
    mMixerPresetJsonCache[mActivePresetId] = mActivePresetJson;
    SendActivePresetLoaded();
}

// ── New preset ──────────────────────────────────────────────────────────────

void PluginController::HandleNewPresetRequest(const nlohmann::json&)
{
    CaptureLiveHostedPluginStateIntoActivePreset();

    Preset preset = BuildNewPreset();
    NormalizePresetScenes(preset);
    (void)SetPresetActiveScene(preset, std::string{}, &mActiveSceneId);
    mActivePresetId = preset.id;
    ApplyPreset(preset);
    mPendingPresetStateBroadcast = true;

    // Unsaved until the user saves it: its baseline is itself, so it opens clean.
    ResetActivePresetBaseline();
    SendActivePresetLoaded(true);
}

void PluginController::SendActivePresetLoaded(bool created)
{
    if (!mActivePreset)
    {
        return;
    }

    nlohmann::json loaded;
    loaded["type"] = "presetLoaded";
    loaded["preset"] = SerializePresetForUi(*mActivePreset);
    loaded["activePresetIds"] = SnapshotActivePresetIds();
    loaded["sceneId"] = GetResolvedActiveSceneId();
    loaded["activePresetDirty"] = mActivePresetDirty;

    if (created)
    {
        loaded["created"] = true;
    }

    SendMessageToUI(loaded.dump());
}

// ── Per-preset marks ────────────────────────────────────────────────────────

void PluginController::HandleSetPresetFavoriteRequest(const nlohmann::json& payload)
{
    const auto presetId = payload.value("presetId", std::string{});

    if (presetId.empty())
    {
        return;
    }

    const bool favorite = payload.value("favorite", true);
    auto document = LoadUiStorageJson("preset-favorites.json", nlohmann::json::object());
    auto favorites = document.value("favorites", nlohmann::json::array());

    if (!favorites.is_array())
    {
        favorites = nlohmann::json::array();
    }

    const auto it = std::find(favorites.begin(), favorites.end(), nlohmann::json(presetId));

    if (favorite && it == favorites.end())
    {
        favorites.push_back(presetId);
    }
    else if (!favorite && it != favorites.end())
    {
        favorites.erase(it);
    }
    else
    {
        return;
    }

    document["favorites"] = favorites;
    SaveUiStorageJson("preset-favorites.json", document);
    HandleGetPresetFavoritesRequest();
}

void PluginController::HandleSetPresetRatingRequest(const nlohmann::json& payload)
{
    const auto presetId = payload.value("presetId", std::string{});

    if (presetId.empty())
    {
        return;
    }

    const int rating = std::clamp(payload.value("rating", 0), 0, 5);
    auto document = LoadUiStorageJson("preset-ratings.json", nlohmann::json::object());
    auto ratings = document.value("ratings", nlohmann::json::object());

    if (!ratings.is_object())
    {
        ratings = nlohmann::json::object();
    }

    if (rating == 0)
    {
        ratings.erase(presetId);
    }
    else
    {
        ratings[presetId] = rating;
    }

    document["ratings"] = ratings;
    SaveUiStorageJson("preset-ratings.json", document);
    HandleGetPresetRatingsRequest();
}

void PluginController::HandleGetPresetRecentsRequest()
{
    nlohmann::json ids = nlohmann::json::array();

    if (mUiSettings.is_object() && mUiSettings.contains("presetRecents") && mUiSettings["presetRecents"].is_array())
    {
        ids = mUiSettings["presetRecents"];
    }

    nlohmann::json msg;
    msg["type"] = "presetRecents";
    msg["presetIds"] = ids;
    SendMessageToUI(msg.dump());
}

void PluginController::RecordPresetRecent(const std::string& presetId)
{
    if (presetId.empty())
    {
        return;
    }

    if (!mUiSettings.is_object())
    {
        mUiSettings = nlohmann::json::object();
    }

    // The web UI's trackRecentPreset: a preset already in the list keeps its place; a new one
    // goes to the front, and the list keeps four.
    std::vector<std::string> ids;

    if (mUiSettings.contains("presetRecents") && mUiSettings["presetRecents"].is_array())
    {
        for (const auto& entry : mUiSettings["presetRecents"])
        {
            if (entry.is_string() && !entry.get<std::string>().empty() &&
                std::find(ids.begin(), ids.end(), entry.get<std::string>()) == ids.end())
            {
                ids.push_back(entry.get<std::string>());
            }
        }
    }

    if (std::find(ids.begin(), ids.end(), presetId) != ids.end())
    {
        return;
    }

    ids.insert(ids.begin(), presetId);
    ids.resize(std::min(ids.size(), kMaxRecentPresets));

    mUiSettings["presetRecents"] = ids;
    mAppSettings["uiSettings"] = mUiSettings;
    SaveAppSettings();
    NotifyHostStateChanged();
    HandleGetPresetRecentsRequest();
}

// ── Setlists ────────────────────────────────────────────────────────────────

void PluginController::HandleSelectSetlistRequest(const nlohmann::json& payload)
{
    const auto setlistId = payload.value("setlistId", std::string{});
    auto document = LoadUiStorageJson("setlists.json", nlohmann::json::object());
    const auto setlists = document.value("setlists", nlohmann::json::array());
    const bool exists = std::any_of(setlists.begin(), setlists.end(), [&setlistId](const nlohmann::json& s) {
        return s.is_object() && s.value("id", std::string{}) == setlistId;
    });

    if (!exists || document.value("activeSetlistId", std::string{}) == setlistId)
    {
        return;
    }

    // As a bank select does: the new setlist, its cursor at the first slot, nothing loaded.
    mSetlistCursorIndex = 0;
    document["activeSetlistId"] = setlistId;
    document["cursorIndex"] = 0;
    SaveUiStorageJson("setlists.json", document);

    nlohmann::json msg;
    msg["type"] = "setlistCursorChanged";
    msg["activeSetlistId"] = setlistId;
    msg["cursorIndex"] = 0;
    SendMessageToUI(msg.dump());
}

// ── Resources ───────────────────────────────────────────────────────────────

void PluginController::HandleSetResourceFavoriteRequest(const nlohmann::json& payload)
{
    const auto resourceId = payload.value("resourceId", std::string{});

    if (resourceId.empty())
    {
        return;
    }

    const bool favorite = payload.value("favorite", true);
    auto favorites = mAppSettings.value("resources.favorites", nlohmann::json::array());

    if (!favorites.is_array())
    {
        favorites = nlohmann::json::array();
    }

    const auto it = std::find(favorites.begin(), favorites.end(), nlohmann::json(resourceId));

    if (favorite == (it != favorites.end()))
    {
        return;
    }

    if (favorite)
    {
        favorites.push_back(resourceId);
    }
    else
    {
        favorites.erase(it);
    }

    mAppSettings["resources.favorites"] = favorites;
    SaveAppSettings();

    nlohmann::json msg;
    msg["type"] = "appSettingChanged";
    msg["key"] = "resources.favorites";
    msg["value"] = favorites;
    SendMessageToUI(msg.dump());
}

// ── Output ──────────────────────────────────────────────────────────────────

void PluginController::HandleSetOutputMutedRequest(const nlohmann::json& payload)
{
    mOutputMuted = payload.value("muted", false);
    mPresetMixer.SetOutputMuted(mOutputMuted);

    nlohmann::json msg;
    msg["type"] = "outputMutedChanged";
    msg["muted"] = mOutputMuted;
    SendMessageToUI(msg.dump());
}

// ── Unsaved changes ─────────────────────────────────────────────────────────

std::string PluginController::ActivePresetComparisonForm() const
{
    if (!mActivePreset)
    {
        return {};
    }

    // What makes two presets the same to the person editing them: the scenes (the active
    // graph is only a copy of one, so switching scenes is not a change) and not the live
    // state of a hosted plugin, which moves whenever the plugin does.
    Preset copy = *mActivePreset;

    if (!copy.scenes.empty())
    {
        copy.graph = {};
    }

    StripHostedPluginState(copy.graph);

    for (auto& scene : copy.scenes)
    {
        StripHostedPluginState(scene.graph);
    }

    copy.modifiedAt.clear();
    return PresetStorage::SerializeToJson(copy);
}

void PluginController::ResetActivePresetBaseline()
{
    // A preset that was not playing before is also what the recently played list records.
    const bool presetChanged = mActivePresetId != mActivePresetBaselineId;
    mActivePresetBaseline = ActivePresetComparisonForm();
    mActivePresetBaselineId = mActivePresetId;
    mNextDirtyCheckSeconds = 0.0;

    if (mActivePresetDirty)
    {
        mActivePresetDirty = false;
        SendMessageToUI(nlohmann::json{{"type", "presetDirtyChanged"}, {"dirty", false}}.dump());
    }

    if (presetChanged)
    {
        RecordPresetRecent(mActivePresetId);
    }
}

void PluginController::UpdateActivePresetDirty()
{
    if (!mActivePreset)
    {
        return;
    }

    // A different preset is playing (a setlist step, a program change, a load from any UI):
    // its baseline is what it loaded as.
    if (mActivePresetId != mActivePresetBaselineId)
    {
        ResetActivePresetBaseline();
        return;
    }

    const double now = NowSeconds();

    if (now < mNextDirtyCheckSeconds)
    {
        return;
    }

    mNextDirtyCheckSeconds = now + kDirtyCheckIntervalSeconds;
    const bool dirty = ActivePresetComparisonForm() != mActivePresetBaseline;

    if (dirty != mActivePresetDirty)
    {
        mActivePresetDirty = dirty;
        SendMessageToUI(nlohmann::json{{"type", "presetDirtyChanged"}, {"dirty", dirty}}.dump());
    }
}
} // namespace guitarfx
