#pragma once

/**
 * ClientState.h - What the native UI knows about the engine.
 *
 * Soundshed Guitar Nano's views never ask the controller directly: they read this mirror,
 * which UiClient keeps up to date from the same JSON messages the WebView UI receives, and
 * they change things by sending the same messages the WebView UI sends (UiCommands). The
 * engine stays the only writer of anything persisted (docs/plans/native-ui.md, rule C3).
 *
 * Presets and the global chain are held in the core's own types, parsed by the core's own
 * serialisers, so the two UIs cannot read a preset differently.
 */

#include "presets/PresetTypes.h"

#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace guitarfx::uiclient
{
struct PresetSummary
{
    std::string id;
    std::string name;
    std::string category;
    std::string source; // "factory", "user" or "session"
};

struct PresetFolder
{
    std::string id;
    std::string name;
    std::vector<PresetFolder> children;
    std::vector<std::string> presetIds;
};

struct Setlist
{
    std::string id;
    std::string name;
    std::optional<int> bank;
    std::vector<std::string> presetIds;
};

struct EffectParamInfo
{
    std::string key;
    std::string name;
    std::string unit;
    std::string group;
    double minValue = 0.0;
    double maxValue = 1.0;
    double defaultValue = 0.0;
    double step = 0.0;
    bool advanced = false;
    bool logTaper = false;
    std::vector<std::string> labels;
};

struct EffectPresetInfo
{
    std::string id;
    std::string name;
    std::string source; // "factory" or "custom"
    std::map<std::string, double> parameters;
};

struct EffectTypeInfo
{
    std::string type;
    std::string name;
    std::string category;
    bool requiresResource = false;
    std::string resourceType;
    std::vector<EffectParamInfo> parameters;
    std::vector<EffectPresetInfo> presets;

    [[nodiscard]] const EffectParamInfo* FindParam(const std::string& key) const;
};

struct LibraryResource
{
    std::string id;
    std::string type; // "nam", "ir", ...
    std::string name;
    std::string category;
    std::string description;
    std::string filePath;
    std::string hash;
    bool fileMissing = false;
    std::vector<std::string> tags;
    nlohmann::json metadata = nlohmann::json::object();
};

/// One level tuple from the signal diagnostics feed ("sld").
struct LevelReading
{
    double peakDb = -120.0;
    double rmsDb = -120.0;
    int clipCount = 0;
    bool clipped = false;
    int channelCount = 0; // 0: did not run last block
};

struct TelemetryState
{
    LevelReading rawInput;
    LevelReading input;
    LevelReading output;

    /// Per node, keyed "<scope>::<nodeId>" as the engine keys dspPerformance: scope is
    /// "pre", "post", or the preset id.
    std::map<std::string, LevelReading> nodes;

    double dspLoadPercent = 0.0;
    int totalLatencySamples = 0;
    std::map<std::string, double> nodeProcessingUs;
    unsigned frameCounter = 0;
};

struct TunerState
{
    bool active = false;
    bool liveMode = true;
    double referenceFrequency = 440.0;
    bool detected = false;
    std::string noteName;
    int octave = 0;
    double frequency = 0.0;
    double centOffset = 0.0;
    double confidence = 0.0;
};

struct MetronomeState
{
    double bpm = 120.0;
    bool enabled = false;
    bool editable = true;
    std::string source = "app";
    double volumeDb = -12.0;
    double pan = 0.0;
    std::string clickType;
    std::string beatPattern;
    int timeSigNum = 4;
    int timeSigDen = 4;
    std::string grouping;
    std::string subdivision;
    std::vector<std::pair<std::string, std::string>> clickTypes;   // id, label
    std::vector<std::pair<std::string, int>> subdivisions;         // id, ticks per beat

    // The last beat the engine reported, for the beat display.
    int beatIndex = -1;
    int beatsPerBar = 4;
    std::string beatLevel;
    unsigned beatCounter = 0;
};

struct MixerState
{
    double masterGain = 1.0;
    double mixGainDb = 0.0;
    bool limiterEnabled = false;
    std::vector<std::string> activePresetIds;
    std::map<std::string, std::string> presetNames;
};

struct DeviceState
{
    bool known = false;     // an audioDeviceState has arrived
    bool available = false; // false in a plugin: the host owns the devices
    std::string error;
    nlohmann::json state = nlohmann::json::object();
    double inputLevelDb = -100.0;
    int xruns = -1;
};

struct DemoClip
{
    std::string id;     // a built-in clip's id, or "riff:<takeId>"
    std::string title;
    std::string takeId; // set for a favourite riff, played through previewRiffTake
};

struct DemoState
{
    std::vector<DemoClip> clips;      // the built-in clips (ui/demo/clips.json)
    std::vector<DemoClip> riffClips;  // favourite riffs from the riff library, as the web UI lists them
    bool playing = false;
    std::string title;
    std::string lastRenderPath;
};

struct Environment
{
    bool standalone = true;
    bool audioDeviceSettings = false;
    std::string version;
    std::string os;
    std::string cpu;
};

struct Notification
{
    enum class Kind
    {
        Info,
        Error
    };

    Kind kind = Kind::Info;
    std::string title;
    std::string detail;
};

/// A tone being installed from tone sharing ("installPresetArchives"), by the installedPacks
/// entry id it is recorded under. What is installed already is the setting itself.
struct ToneInstall
{
    enum class Status
    {
        Installing,
        Installed,
        Failed
    };

    Status status = Status::Installing;
    std::string detail;                  // why it failed
    std::vector<std::string> presetIds; // what it installed
};

struct ClientState
{
    bool haveState = false;
    Environment environment;
    nlohmann::json appSettings = nlohmann::json::object();
    nlohmann::json uiSettings = nlohmann::json::object();
    nlohmann::json uiViewState = nlohmann::json::object();
    std::string theme = "dark";

    // The active preset (the engine's working copy) and where it came from.
    std::optional<Preset> activePreset;
    std::string activePresetId;
    std::string activeSceneId;
    bool activePresetDirty = false;
    std::vector<std::string> missingNodeResources;

    // The preset library, in the engine's order (preset stepping follows it).
    std::vector<PresetSummary> presetList;
    std::vector<PresetFolder> presetFolders;
    std::string activePresetFolderId = "__all__";
    std::vector<std::string> presetFavorites;
    nlohmann::json presetRatings = nlohmann::json::object();
    std::vector<std::string> presetRecents;

    std::vector<Setlist> setlists;
    std::string activeSetlistId;
    int setlistCursor = 0;

    std::vector<EffectTypeInfo> catalog;
    std::map<std::string, std::vector<EffectPresetInfo>> customEffectPresets; // by effect type

    std::map<std::string, std::vector<LibraryResource>> resources; // by resource type

    GlobalSignalChainConfig globalChain;
    bool outputMuted = false;
    MixerState mixer;

    TelemetryState telemetry;
    TunerState tuner;
    MetronomeState metronome;
    DeviceState device;
    DemoState demo;

    std::map<std::string, ToneInstall> toneInstalls;

    [[nodiscard]] const EffectTypeInfo* FindEffectType(const std::string& type) const;
    [[nodiscard]] const PresetSummary* FindPresetSummary(const std::string& id) const;
    // Not "FindResource": <windows.h> defines that as a macro in the JUCE translation units.
    [[nodiscard]] const LibraryResource* FindLibraryResource(const std::string& type, const std::string& id) const;

    /// The active scene's title, or empty when the preset has one unnamed scene.
    [[nodiscard]] const PresetScene* ActiveScene() const;
};
} // namespace guitarfx::uiclient
