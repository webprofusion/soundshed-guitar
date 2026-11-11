#pragma once

/**
 * ClientState.h - What the native UI knows about the engine.
 *
 * Soundshed Nano Q's views never ask the controller directly: they read this mirror,
 * which UiClient keeps up to date from the same JSON messages the WebView UI receives, and
 * they change things by sending the same messages the WebView UI sends (UiCommands). The
 * engine stays the only writer of anything persisted (docs/plans/native-ui.md, rule C3).
 *
 * Presets and the global chain are held in the core's own types, parsed by the core's own
 * serialisers, so the two UIs cannot read a preset differently.
 */

#include "presets/PresetTypes.h"
#include "uiclient/EffectLayoutTypes.h"
#include "uiclient/ScaleGuide.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
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
    std::string description;
    std::vector<std::string> tags;
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
    // From the engine's labelInfo, for an enum with many labels: each label's group and
    // subgroup, so a menu can nest them. Empty, or one entry per label.
    std::vector<std::string> labelGroups;
    std::vector<std::string> labelSubgroups;
    // "picker" for an enum with too many labels to step through; empty for the default control.
    std::string control;
    // Shown only while the parameter `visibleWhenKey` holds one of `visibleWhenValues` (rounded):
    // one reverb machine's knobs under Verbal's Machine. Empty key: always shown.
    std::string visibleWhenKey;
    std::vector<int> visibleWhenValues;
};

struct EffectPresetInfo
{
    std::string id;
    std::string name;
    std::string source; // "factory", "factoryPack" (from a factory archive) or "custom"
    std::map<std::string, double> parameters;
    std::vector<std::string> parameterOrder; // a registry factory preset's apply order, if it has one
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

/// A blend of NAM models (the blend library, state.blendLibrary): what a blend node plays.
struct BlendSummary
{
    std::string id;
    std::string name;
    std::string blendMode = "interpolate"; // "interpolate" or "snap"
    std::size_t modelCount = 0;
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
    std::map<std::string, double> nodeLatencySamples; // keyed as nodeProcessingUs
    double totalProcessingUs = 0.0;
    std::optional<double> peakDspLoadPercent; // the worst block since the frame before
    double sampleRate = 0.0;
    unsigned frameCounter = 0;
};

/// One node's input spectrum behind an EQ curve (sldS): `binsDb` log-spaced from minHz to
/// maxHz, both ends included, tilted +3 dB/octave about 1 kHz so pink noise reads flat.
struct NodeSpectrum
{
    double minHz = 20.0;
    double maxHz = 20000.0;
    double floorDb = -96.0;
    double ceilingDb = 0.0;
    std::vector<double> binsDb;
    double receivedAt = 0.0; // UiClient's clock; a spectrum that stops arriving is cleared
};

/// BS.1770 loudness of a node's output or the app's output (sldL); each empty until its window
/// has filled, for silence, or (integrated) until anything has passed the gates.
struct LoudnessReading
{
    std::optional<double> momentaryLufs;
    std::optional<double> shortTermLufs;
    std::optional<double> integratedLufs;
    double integratedSeconds = 0.0;
    double receivedAt = 0.0;
};

/// A Signal Analyzer node's frame (sldA), with the display ranges its roster gave.
struct AnalyzerFrame
{
    double peakPercent = 0.0;
    double rmsPercent = 0.0;
    double rmsDbu = 0.0;
    double rmsDbv = 0.0;
    double rmsVolts = 0.0;
    std::optional<double> momentaryLufs;
    std::optional<double> shortTermLufs;
    std::optional<double> integratedLufs;
    int activeChannelCount = 0;
    bool stereo = false;
    bool loudnessValid = false;
    std::vector<double> spectrogramDb;
    std::vector<double> barkDb;
    double timeMs = 0.0;
};

/// The ranges every analyzer frame is drawn against: [minDb, maxDb, minHz, maxHz].
struct AnalyzerRanges
{
    std::array<double, 4> spectrogram{-96.0, 0.0, 20.0, 20000.0};
    std::array<double, 4> bark{-96.0, 0.0, 20.0, 20000.0};
};

/// Where a 3D Spatial node's source is right now (spatialPosition), for its panner's puck.
struct SpatialPosition
{
    double azimuth = 0.0;
    double elevation = 0.0;
    double distance = 1.0;
    double itdUs = 0.0;
    double ildDb = 0.0;
    double rateHz = 0.0;
    bool moving = false;
};

/// One band of a Multi-band Gate's listen.
struct GateLearnBandStatus
{
    std::optional<double> levelDb;
    std::optional<double> noiseFloorDb;
    std::optional<double> spreadDb;
    std::optional<double> thresholdDb;
};

/// A noise gate Learn's progress (gateLearnStatus).
struct GateLearnStatus
{
    std::string state; // listening, done, failed, cancelled
    double progress = 0.0;
    std::optional<double> levelDb;
    std::optional<double> noiseFloorDb;
    std::optional<double> spreadDb;
    std::optional<double> thresholdDb;
    std::string reason;
    // A Multi-band Gate's listen, low band first, in place of the single figures above; empty for
    // the noise gate. The bands' last learned noise floors are kept under the live levels.
    std::vector<GateLearnBandStatus> bands;
};

// ── MIDI and automation (Topic::Automation) ─────────────────────────────────────────────
// The engine's automation slots: each drives one address ("global.inputTrim",
// "node.<effectType>.<param>", "setlist.bankUp") from the DAW, a MIDI mapping or keys.

/// A slot's MIDI mapping. eventType: 0 CC, 1 Program Change, 2 Note On, 3 Note Off, 4 Pitch
/// Bend; channel 0-15, or -1 for any; mode: 0 Absolute, 1 Relative, 2 Toggle, 3 Pickup.
struct AutomationMidiMap
{
    int eventType = 0;
    int channel = 0;
    int controller = 0;
    int mode = 0;
    double sensitivity = 0.1;
    double pickupRange = 0.1;
};

/// A key that fires a slot: mode 0 Trigger, 1 Set Value (to `value`, 0..1).
struct AutomationKeyMap
{
    std::string key; // as a DOM KeyboardEvent.key names it, lower case for one character
    int mode = 0;
    double value = 0.0;
};

struct AutomationSlotInfo
{
    std::string slotId; // "default.inputLevel", "custom.3", "preset.2"
    std::string label;
    std::string address;
    std::string nodeSelector;
    bool isDefault = false;
    std::string presetId; // set for a per-preset MIDI mapping, live only while that preset is active
    double value = 0.0;
    std::optional<AutomationMidiMap> midiMap;
    std::vector<AutomationKeyMap> keyMap;
};

/// An address the engine's registry offers (the global and setlist ones; node addresses are
/// built from the effect catalog).
struct AutomationRegistryEntry
{
    std::string address;
    std::string label;
    std::string unit;
    double min = 0.0;
    double max = 1.0;
    bool isStepped = false;
    bool isTrigger = false;
};

/// One incoming MIDI event, as the MIDI Log shows it.
struct MidiLogEntry
{
    double receivedAt = 0.0; // UiClient's clock (Tick), seconds
    std::string clock;       // the wall clock then, "HH:MM:SS.mmm", as the log shows it
    std::string type;        // "CC", "NoteOn", ...
    int channel = 0;         // 0-15
    int data1 = 0;
    int data2 = 0;
};

/// What a MIDI learn captured (midiLearnCapture).
struct MidiLearnCaptureInfo
{
    std::string slotId;
    int eventType = 0;
    int channel = 0;
    int controller = 0;
};

struct AutomationState
{
    bool loaded = false; // the engine has sent its slots and registry
    std::vector<AutomationSlotInfo> slots;
    std::vector<AutomationRegistryEntry> registry;
    int maxCustomSlots = 16;
    int maxPresetSlots = 16;
    /// The slot the engine's key learn waits on (armKeyLearn), empty when none.
    std::string keyLearnSlotId;
    /// The slot this UI armed for MIDI learn, until a capture or a cancel. One listens at a time.
    std::string midiLearnSlotId;
    /// The learn that completed last; a UI announces it once and clears it.
    std::optional<MidiLearnCaptureInfo> capture;
    /// Whether this UI asked the engine for the MIDI Log, and what it has sent, oldest first
    /// (the last kMidiLogLimit kept).
    bool midiLogEnabled = false;
    std::vector<MidiLogEntry> midiLog;
    static constexpr std::size_t kMidiLogLimit = 500;

    [[nodiscard]] const AutomationSlotInfo* FindSlot(const std::string& slotId) const;
    [[nodiscard]] const AutomationRegistryEntry* FindRegistryEntry(const std::string& address) const;
};

/// The analysis feeds, keyed as the meters key nodes (NodeFeedKey: "pre::id", "post::id",
/// "<presetId>::id"). Topic::Analysis.
struct AnalysisState
{
    std::map<std::string, NodeSpectrum> spectra;
    std::map<std::string, LoudnessReading> loudness;
    LoudnessReading outputLoudness;
    std::map<std::string, AnalyzerFrame> analyzers; // by node id
    AnalyzerRanges analyzerRanges;
    std::map<std::string, SpatialPosition> spatial;
    std::map<std::string, GateLearnStatus> gateLearn; // "global::global_gate" for the global gate
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
    /// The input level over the reading, and, while the Scale Guide listens for chords, the
    /// input's energy per pitch class (C first). `readings` counts every update, so a view can
    /// tell a new reading from the same one.
    double levelDb = -120.0;
    bool hasChroma = false;
    std::array<float, 12> chroma{};
    std::uint64_t readings = 0;
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
    std::vector<std::pair<std::string, std::string>> clickTypes; // id, label
    std::vector<std::pair<std::string, int>> subdivisions;       // id, ticks per beat

    // The last beat the engine reported, for the beat display.
    int beatIndex = -1;
    int beatsPerBar = 4;
    std::string beatLevel;
    unsigned beatCounter = 0;
};

/// One preset in the Multi-Rig mix.
struct MixerSlot
{
    std::string name;
    double mix = 1.0; // 0..1
    double pan = 0.0; // -1..1
    bool mute = false;
    bool solo = false;
};

/// A saved Multi-Rig (compositePresetList).
struct CompositePreset
{
    std::string id;
    std::string name;
    std::string description;
    std::vector<std::string> tags;
    std::vector<std::string> presetIds; // its slots, in order
    double mixGainDb = 0.0;
};

struct MixerState
{
    double masterGain = 1.0;
    double mixGainDb = 0.0; // the Multi-Rig's own level
    bool limiterEnabled = false;
    std::vector<std::string> activePresetIds;
    std::map<std::string, std::string> presetNames;
    std::map<std::string, MixerSlot> slots; // by preset id

    std::vector<CompositePreset> compositePresets;
    bool compositePresetsKnown = false;
    /// The Multi-Rig last saved or loaded; it stands while the mix holds the same presets.
    std::string loadedCompositeId;
};

/// The saved Multi-Rig the mix is, while its membership is unchanged since it was saved or
/// loaded; empty otherwise (the web UI then offers Save rather than Update and Delete).
[[nodiscard]] const CompositePreset* LoadedComposite(const MixerState& mixer);

// ── Input levelling (Topic::InputLevelling) ─────────────────────────────────────────────

/// One trained guitar: the peak it produced and the trim that brings it to the target.
struct InputLevellingProfile
{
    std::string id;
    std::string name;
    std::string description;
    std::optional<double> capturedPeakDbfs;
    double targetPeakDbfs = -12.0;
    double gainDb = 0.0;
    std::string createdAt;
    std::string updatedAt;
};

/// "inputLevellingTraining": the raw input as the engine hears it while a profile is trained.
struct InputLevellingTraining
{
    bool active = false;
    std::optional<double> livePeakDbfs;
    std::optional<double> capturedPeakDbfs;
    std::optional<double> recommendedTrimDb;
    bool clipped = false;
};

struct InputLevellingState
{
    bool known = false; // an "inputLevelling" has arrived
    std::vector<InputLevellingProfile> profiles;
    std::string activeId; // empty: no levelling
    bool trainingActive = false;
    double targetPeakDbfs = -12.0;
    double maxTrimDb = 24.0;
    InputLevellingTraining training;
    /// The last "inputLevellingSaved"; `serial` counts them, so a dialog can tell a new one.
    int saveSerial = 0;
    bool saveOk = false;
    std::string saveMessage;

    [[nodiscard]] const InputLevellingProfile* Find(const std::string& id) const;

    [[nodiscard]] const InputLevellingProfile* Active() const
    {
        return Find(activeId);
    }
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
    std::string id; // a built-in clip's id, or "riff:<takeId>"
    std::string title;
    std::string takeId; // set for a favourite riff, played through previewRiffTake
};

struct DemoState
{
    std::vector<DemoClip> clips;     // the built-in clips (ui/demo/clips.json)
    std::vector<DemoClip> riffClips; // favourite riffs from the riff library, as the web UI lists them
    bool playing = false;
    std::string title;
    std::string previewId; // what is playing: a clip's id, a riff take's id, or the captured take's
    std::string lastRenderPath;
    // The Scale Guide's Play (`scaleGuidePlayback`): when each note falls, and the client's clock
    // when that was told. Empty until the playing position's notes arrive.
    scaleguide::Playback scaleGuide;
    double scaleGuideAt = 0.0;
};

// ── Riffs (Topic::Riffs) ────────────────────────────────────────────────────────
// The riff library and the take being captured, as the engine reports them
// (PluginControllerRiffs.cpp; the web UI's riffLibrary.ts).

struct RiffTake
{
    std::string id;
    std::string filePath;
    double durationSec = 0.0;
    int bars = 0;
    double tempoBpm = 0.0;
    int timeSigNum = 4;
    int timeSigDen = 4;
    std::string presetName;
    std::string createdAt;
};

struct Riff
{
    std::string id;
    std::string title;
    std::vector<std::string> categories;
    std::vector<std::string> tags;
    std::string notes;
    bool favorite = false;
    bool used = false;
    std::string usedSongTitle;
    std::string preferredTakeId;
    std::vector<RiffTake> takes;

    /// The preferred take, else the first; null when the riff has none.
    [[nodiscard]] const RiffTake* PreferredTake() const;
};

/// The take being captured, or the one captured and not yet saved (the "pending" take).
struct RiffCapture
{
    enum class Phase
    {
        Idle,  // nothing pending
        Armed, // counting in, or waiting for the first note
        Recording,
        Complete // a take waits to be trimmed, previewed and saved
    };
    Phase phase = Phase::Idle;
    std::string takeId;
    double tempoBpm = 0.0;
    int timeSigNum = 4;
    int timeSigDen = 4;
    int bars = 0; // armed: the most it can hold; complete: its length in bars
    std::int64_t capturedSamples = 0;
    double sampleRate = 0.0;
    bool hasAudio = false;
    std::vector<float> peaks;               // 0..1; while recording, over the whole buffer the take can fill
    std::int64_t barAlignOffsetSamples = 0; // where the first bar starts, from an armed capture
    std::string source;                     // "", "import", "trim", "editLoad" or "looper"
    std::string originalTakeId;             // editLoad: the library take it came from
    int serial = 0;                         // counts takes made complete, so a view can reset its markers
};

struct RiffState
{
    bool known = false; // a riff library has arrived
    std::string path;
    std::vector<Riff> riffs;
    RiffCapture capture;
    int savedSerial = 0; // counts "riffSaved", so a dialog can close on its own save
    std::string lastSavedRiffId;

    [[nodiscard]] const Riff* Find(const std::string& riffId) const;
};

// ── Looper (Topic::Looper) ──────────────────────────────────────────────────────
// As the engine reports it (LooperServiceMessages.cpp BuildState and BuildLayers,
// LooperService.cpp's feeds); docs/user-interface.md has the fields.

struct LooperLayer
{
    std::string id;
    std::string title;
    int order = 0;
    bool recording = false;
    double level = 1.0;   // 0..2
    double balance = 0.0; // -1..1
    bool muted = false;
    bool solo = false;
    std::string presetName;
    bool toneChanged = false;
    bool canRevert = false;
    double retoneProgress = -1.0; // 0..1 while it is re-amped, else -1
    std::vector<float> peaksL;    // 256 buckets across the loop, once the layer is finished
    std::vector<float> peaksR;
};

struct LooperSettings
{
    bool enabled = false;
    int maxSeconds = 60;
    std::string start = "press"; // "press" or "armed"
    std::string length = "free"; // "free" or "bars"
    int bars = 4;
    int countInBars = 1;
    bool clickWhileRecording = true;
    std::string afterRecord = "play"; // "play" or "overdub"
    double alignOffsetMs = 0.0;
};

struct LooperProject
{
    std::string id;
    std::string title;
    std::string modified;
    double lengthSec = 0.0;
    int layerCount = 0;
    double tempoBpm = 0.0;
    int bars = 0;
};

struct LooperState
{
    bool known = false;     // a looperState has arrived
    bool available = false; // the standalone app only
    std::string state = "empty";
    bool playing = false;
    double positionSec = 0.0;
    double positionAt = 0.0; // when positionSec was reported (LooperClockNow)
    double lengthSec = 0.0;
    bool canUndo = false;
    bool canRedo = false;
    int layerCount = 0;
    double memoryMb = 0.0;
    bool tempoLocked = false;
    bool click = false;
    double clickBar = 0.0;
    int bars = 0;     // a Bars loop's length, else 0
    int barIndex = 0; // 1-based
    double tempoBpm = 0.0;
    int countInBeatsLeft = 0;
    double recordedSec = 0.0;
    LooperSettings settings;
    std::string projectId;
    std::string projectTitle;
    bool dirty = false;
    bool recoverable = false;
    std::vector<LooperLayer> layers; // newest first
    std::vector<float> livePeaks;    // the take being recorded, 256 buckets
    std::vector<LooperProject> projects;
};

// ── Practice Tool (Topic::PracticeTool) ────────────────────────────────────────
// The backing track as the engine reports it (PracticeToolService.cpp:
// practiceToolFileLoaded, practiceToolTransportState, practiceToolPlaybackEnded). Loops,
// faders and projects are the UI's (uiclient/PracticeTool.h), as in the web UI.

struct PracticeToolTrack
{
    std::string filePath; // a real path, or a bare file name for one loaded from bytes
    std::string title;
    double durationSec = 0.0;
    std::vector<float> peaksL, peaksR;
    int loads = 0; // counts practiceToolFileLoaded: a change is a new track
    bool playing = false;
    double positionSec = 0.0;
    double positionAt = 0.0; // when positionSec was reported (LooperClockNow)
    int endings = 0;         // counts practiceToolPlaybackEnded
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
    std::string detail;                 // why it failed
    std::vector<std::string> presetIds; // what it installed
};

/// The engine's answer to an importRemoteResource, by the request id it was sent with.
struct ResourceImport
{
    enum class Status
    {
        Importing,
        Imported,
        Failed
    };

    Status status = Status::Importing;
    std::string resourceType;
    std::string resourceId; // the library id it was stored under
    std::string detail;     // why it failed
};

/// One folder of model and IR files, as the engine lists it for the resource browser's Folder
/// tab (listResourceFolder, ResourceFolderScanner).
struct ResourceFolderListing
{
    struct Dir
    {
        std::string name, path;
    };

    struct File
    {
        std::string name, path, resourceType;
        std::int64_t sizeBytes = 0;
        bool inLibrary = false;
        std::string libraryId;
        nlohmann::json metadata = nlohmann::json::object(); // filled in by resourceFolderMetadata
    };

    std::string path, parent, name;
    std::vector<Dir> dirs;
    std::vector<File> files;
    bool truncated = false;
    std::string error; // resourceFolderListingFailed
};

struct ClientState
{
    bool haveState = false;
    Environment environment;
    nlohmann::json appSettings = nlohmann::json::object();
    nlohmann::json uiSettings = nlohmann::json::object();
    nlohmann::json uiViewState = nlohmann::json::object();
    std::string theme = "dark";

    /// Nodes whose third-party plugin is loading: from the request until the engine answers
    /// (hostedPluginResourceLoadCompleted or Failed) or the file dialog is cancelled.
    std::set<std::string> pluginLoadsPending;

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
    /// Factory presets a factory archive ships, choosing its models, IRs and blends; by effect type.
    std::map<std::string, std::vector<EffectPresetInfo>> factoryPackEffectPresets;

    std::map<std::string, std::vector<LibraryResource>> resources; // by resource type
    std::vector<BlendSummary> blends;

    /// What uses each library resource, by "type:id", as queryResourceUsage answered.
    struct ResourceUsage
    {
        bool inUse = false;
        std::string detail; // e.g. "Used by preset Clean Bass"
    };

    std::map<std::string, ResourceUsage> resourceUsage;

    GlobalSignalChainConfig globalChain;
    bool outputMuted = false;
    MixerState mixer;

    TelemetryState telemetry;
    AnalysisState analysis;
    AutomationState automation;
    TunerState tuner;
    MetronomeState metronome;
    DeviceState device;
    DemoState demo;
    InputLevellingState inputLevelling;
    RiffState riffs;
    LooperState looper;
    PracticeToolTrack practiceTool;
    LayoutState layouts; // custom effect layouts and their images (Topic::EffectLayouts)

    std::map<std::string, ToneInstall> toneInstalls;
    std::map<std::string, ResourceImport> resourceImports; // by request id (Topic::Resources)
    ResourceFolderListing resourceFolder;                  // the folder last listed (Topic::ResourceFolders)
    std::string pickedResourceFolder;                      // a folder the user just picked to add, until taken

    [[nodiscard]] const EffectTypeInfo* FindEffectType(const std::string& type) const;
    [[nodiscard]] const PresetSummary* FindPresetSummary(const std::string& id) const;
    // Not "FindResource": <windows.h> defines that as a macro in the JUCE translation units.
    [[nodiscard]] const LibraryResource* FindLibraryResource(const std::string& type, const std::string& id) const;

    /// The active scene's title, or empty when the preset has one unnamed scene.
    [[nodiscard]] const PresetScene* ActiveScene() const;
};
} // namespace guitarfx::uiclient
