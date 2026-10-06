#pragma once

#include "dsp/EffectParamSpec.h"
#include "dsp/EffectRegistry.h"
#include "dsp/MusicalScale.h"
#include "dsp/PitchTracker.h"
#include "dsp/effects/FactoryPresetSupport.h"

#include <array>
#include <cstddef>
#include <iterator>
#include <vector>

/**
 * What the harmonizer is, as opposed to how it runs (HarmonizerEffect.h): its constants, its
 * parameter table and its factory presets.
 */
namespace guitarfx::harmonizer
{
constexpr int kVoiceCount = 4;

/// Scale mode harmonises in a key: each voice is a number of scale steps from the note played,
/// so a "3rd up" is a major or a minor third as the key has it. Fixed mode moves each voice by
/// its own number of semitones whatever is played, chords included.
enum class Mode : int
{
    Scale,
    Fixed
};

/// Clean holds a voice back after each pick until the new note's pitch is known, then starts it
/// from the pick; Fast lets it play on at the old interval until the pitch arrives.
enum class Tracking : int
{
    Clean,
    Fast
};

inline constexpr const char* kModeLabels[] = {"Scale", "Fixed"};
inline constexpr const char* kTrackingLabels[] = {"Clean", "Fast"};

/// A voice fades in and out over this long when it is switched, ducked or reaches its first note.
constexpr double kVoiceFadeSeconds = 0.005;
/// A Clean voice ducks under a new pick this fast, before much of the pick has played at the old
/// interval...
constexpr double kDuckFadeSeconds = 0.002;
/// ...and comes back over this long, from a little before the pick: the tap starts on the old
/// note's tail, so it is faded in rather than cut in.
constexpr double kReturnFadeSeconds = 0.001;
/// How far before the attack detector's decision a returning Clean voice starts: the detector
/// follows the high band over about 1.5 ms, so it fires that long into the pick.
constexpr double kPickPreRollSeconds = 0.003;
/// A Clean voice waits at most this long after a pick for its note, then starts from the pick at
/// the interval it had: a chord or a palm-muted chug often has no pitch to find. On real riffs
/// from E2 to C3 a note is confirmed 20-32 ms after its pick, higher ones sooner.
constexpr double kPickWaitSeconds = 0.035;
/// Each voice's SpliceTransposer window. Longer than Transpose's 30 ms, so a Clean voice can start
/// from a pick kPickWaitSeconds + kPickPreRollSeconds back; it plays 21 ms behind on average.
constexpr double kVoiceWindowSeconds = 0.040;
/// The deepest a voice's own delay goes, plus the humanised drift around it.
constexpr double kMaxVoiceDelayMs = 100.0;
/// Humanize at 1: a slow random drift of up to this many cents on each voice...
constexpr double kHumanizeCents = 8.0;
/// ...and of up to this many milliseconds in its timing...
constexpr double kHumanizeDelayMs = 3.0;
/// ...moving no faster than this (samples per sample: 0.004 bends the pitch by 7 cents).
constexpr double kHumanizeDelayMaxStep = 0.004;
/// A new random target for each voice's drift about this often, and the time constant the
/// detune follows it with.
constexpr double kHumanizeIntervalSeconds = 0.35;
constexpr double kHumanizeSmoothingSeconds = 0.25;
/// Level, pan and the voice delay follow their parameters with this time constant.
constexpr double kControlSmoothingMs = 20.0;
/// A High Cut at or above this is off: the harmony is left unfiltered.
constexpr double kHighCutOffHz = 19500.0;
constexpr double kMaxHighCutFraction = 0.45;

/// One voice's parameters, at kVoiceFirst + voice * kVoiceFieldCount + field.
enum VoiceField : std::size_t
{
    kOn,
    kInterval,
    kSemitones,
    kLevel,
    kPan,
    kDetune,
    kDelay,
    kVoiceFieldCount
};

enum Param : std::size_t
{
    kMode,
    kKey,
    kScale,
    kTracking,
    kGlide,
    kLowestNote,
    kVoiceFirst,
    kDry = kVoiceFirst + kVoiceFieldCount * kVoiceCount,
    kHarmonyLevel,
    kHighCut,
    kHumanize,
    kParamCount
};

[[nodiscard]] constexpr std::size_t VoiceParam(int voice, VoiceField field) noexcept
{
    return kVoiceFirst + static_cast<std::size_t>(voice) * kVoiceFieldCount + field;
}

/// In `Param` order, which is also the order the UI lays the controls out in.
inline constexpr std::array<EffectParamSpec, kParamCount> kParams = {{
    {"mode", "Mode", 0.0, 0.0, 1.0, "enum", "Harmony", false, 1.0, kModeLabels},
    {"key", "Key", 0.0, 0.0, 11.0, "enum", "Harmony", false, 1.0, music::kKeyLabels},
    {"scale", "Scale", 0.0, 0.0, 9.0, "enum", "Harmony", false, 1.0, music::kScaleLabels},
    {"tracking", "Tracking", 0.0, 0.0, 1.0, "enum", "Harmony", false, 1.0, kTrackingLabels},
    {"glide", "Glide", 0.0, 0.0, 500.0, "ms", "Harmony", true, 0.0, {}},
    {"lowestNote", "Lowest Note", 1.0, 0.0, 3.0, "enum", "Harmony", true, 1.0, pitch_tracker::kLowestNoteLabels},
    // Voice 1 a 3rd up; the others off until wanted, at the usual next choices.
    {"voice1On", "On", 1.0, 0.0, 1.0, "toggle", "Voice 1", false, 1.0, {}},
    {"voice1Interval", "Interval", 2.0, music::kMinScaleSteps, music::kMaxScaleSteps, "enum", "Voice 1", false, 1.0,
     music::kIntervalLabels},
    {"voice1Semitones", "Semitones", 4.0, -24.0, 24.0, "st", "Voice 1", false, 1.0, {}},
    {"voice1Level", "Level", 0.0, -40.0, 6.0, "dB", "Voice 1", false, 0.0, {}},
    {"voice1Pan", "Pan", 0.0, -1.0, 1.0, "", "Voice 1", false, 0.0, {}},
    {"voice1Detune", "Detune", 0.0, -50.0, 50.0, "cents", "Voice 1", true, 0.0, {}},
    {"voice1Delay", "Delay", 0.0, 0.0, kMaxVoiceDelayMs, "ms", "Voice 1", true, 0.0, {}},
    {"voice2On", "On", 0.0, 0.0, 1.0, "toggle", "Voice 2", false, 1.0, {}},
    {"voice2Interval", "Interval", 4.0, music::kMinScaleSteps, music::kMaxScaleSteps, "enum", "Voice 2", false, 1.0,
     music::kIntervalLabels},
    {"voice2Semitones", "Semitones", 7.0, -24.0, 24.0, "st", "Voice 2", false, 1.0, {}},
    {"voice2Level", "Level", 0.0, -40.0, 6.0, "dB", "Voice 2", false, 0.0, {}},
    {"voice2Pan", "Pan", 0.0, -1.0, 1.0, "", "Voice 2", false, 0.0, {}},
    {"voice2Detune", "Detune", 0.0, -50.0, 50.0, "cents", "Voice 2", true, 0.0, {}},
    {"voice2Delay", "Delay", 0.0, 0.0, kMaxVoiceDelayMs, "ms", "Voice 2", true, 0.0, {}},
    {"voice3On", "On", 0.0, 0.0, 1.0, "toggle", "Voice 3", false, 1.0, {}},
    {"voice3Interval", "Interval", -7.0, music::kMinScaleSteps, music::kMaxScaleSteps, "enum", "Voice 3", false, 1.0,
     music::kIntervalLabels},
    {"voice3Semitones", "Semitones", -12.0, -24.0, 24.0, "st", "Voice 3", false, 1.0, {}},
    {"voice3Level", "Level", 0.0, -40.0, 6.0, "dB", "Voice 3", false, 0.0, {}},
    {"voice3Pan", "Pan", 0.0, -1.0, 1.0, "", "Voice 3", false, 0.0, {}},
    {"voice3Detune", "Detune", 0.0, -50.0, 50.0, "cents", "Voice 3", true, 0.0, {}},
    {"voice3Delay", "Delay", 0.0, 0.0, kMaxVoiceDelayMs, "ms", "Voice 3", true, 0.0, {}},
    {"voice4On", "On", 0.0, 0.0, 1.0, "toggle", "Voice 4", false, 1.0, {}},
    {"voice4Interval", "Interval", 7.0, music::kMinScaleSteps, music::kMaxScaleSteps, "enum", "Voice 4", false, 1.0,
     music::kIntervalLabels},
    {"voice4Semitones", "Semitones", 12.0, -24.0, 24.0, "st", "Voice 4", false, 1.0, {}},
    {"voice4Level", "Level", 0.0, -40.0, 6.0, "dB", "Voice 4", false, 0.0, {}},
    {"voice4Pan", "Pan", 0.0, -1.0, 1.0, "", "Voice 4", false, 0.0, {}},
    {"voice4Detune", "Detune", 0.0, -50.0, 50.0, "cents", "Voice 4", true, 0.0, {}},
    {"voice4Delay", "Delay", 0.0, 0.0, kMaxVoiceDelayMs, "ms", "Voice 4", true, 0.0, {}},
    {"dry", "Dry", 1.0, 0.0, 1.0, "amount", "Output", false, 0.0, {}},
    {"harmonyLevel", "Harmony", 0.0, -24.0, 6.0, "dB", "Output", false, 0.0, {}},
    LogTaper({"highCut", "High Cut", 20000.0, 1000.0, 20000.0, "Hz", "Output", false, 0.0, {}}),
    {"humanize", "Humanize", 0.0, 0.0, 1.0, "amount", "Output", true, 0.0, {}},
}};

static_assert(kParams[kMode].maxValue == static_cast<double>(std::size(kModeLabels) - 1));
static_assert(kParams[kKey].maxValue == static_cast<double>(std::size(music::kKeyLabels) - 1));
static_assert(kParams[kScale].maxValue == static_cast<double>(std::size(music::kScaleLabels) - 1));
static_assert(kParams[kTracking].maxValue == static_cast<double>(std::size(kTrackingLabels) - 1));
static_assert(kParams[kLowestNote].maxValue == static_cast<double>(std::size(pitch_tracker::kLowestNoteLabels) - 1));
static_assert(kParams[VoiceParam(3, kDelay)].maxValue == kMaxVoiceDelayMs);

/// Keep ids stable once shipped; the UIs list them. Key is the song's and Lowest Note the
/// guitar's, so no preset sets them. Levels keep each preset within a few dB of the default on
/// the demo DI: the more voices, the lower each.
[[nodiscard]] inline std::vector<EffectPresetDefinition> FactoryPresets(const std::vector<ParameterDef>& params)
{
    const factory_presets::Builder b(params, {"key", "lowestNote"});
    constexpr double kFixed = 1.0;
    constexpr double kOn = 1.0;
    constexpr double kMinor = 1.0;
    constexpr double kHarmonicMinor = 2.0;
    constexpr double kThirdUp = 2.0;
    constexpr double kFifthUp = 4.0;
    constexpr double kSixthDown = -5.0;
    constexpr double kOctaveUp = 7.0;
    constexpr double kOctaveDown = -7.0;

    return {
        b.Defaults("third-up", "3rd Up"),
        // Two guitars a third apart, the second one a few milliseconds behind and to the right.
        b.Make("twin-leads", "Twin Leads",
               {{"scale", kMinor},
                {"voice1Level", -2.0},
                {"voice1Pan", 0.6},
                {"voice1Delay", 12.0},
                {"voice1Detune", 4.0},
                {"humanize", 0.3}}),
        b.Make(
            "thirds-and-fifths", "Thirds and Fifths",
            {{"voice1Level", -3.0}, {"voice1Pan", -0.5}, {"voice2On", kOn}, {"voice2Level", -4.0}, {"voice2Pan", 0.5}}),
        b.Make("neo-classical", "Neo-Classical",
               {{"scale", kHarmonicMinor},
                {"voice1Level", -3.0},
                {"voice1Pan", -0.4},
                {"voice1Delay", 8.0},
                {"voice2On", kOn},
                {"voice2Interval", kFifthUp},
                {"voice2Level", -5.0},
                {"voice2Pan", 0.4},
                {"voice2Delay", 15.0}}),
        b.Make("country-sixths", "Country Sixths",
               {{"voice1Interval", kSixthDown}, {"voice1Level", -2.0}, {"glide", 30.0}}),
        b.Make("choir", "Choir",
               {{"voice1Interval", kThirdUp},
                {"voice1Level", -6.0},
                {"voice1Pan", -0.6},
                {"voice1Delay", 14.0},
                {"voice2On", kOn},
                {"voice2Level", -7.0},
                {"voice2Pan", 0.6},
                {"voice2Delay", 22.0},
                {"voice3On", kOn},
                {"voice3Interval", kOctaveDown},
                {"voice3Level", -8.0},
                {"voice3Delay", 6.0},
                {"voice4On", kOn},
                {"voice4Interval", kOctaveUp},
                {"voice4Level", -12.0},
                {"voice4Delay", 30.0},
                {"highCut", 7000.0},
                {"humanize", 0.7}}),
        // Fixed mode works on chords: every note moves by the same interval.
        b.Make("octave-stack", "Octave Stack",
               {{"mode", kFixed},
                {"voice1Semitones", -12.0},
                {"voice1Level", -3.0},
                {"voice2On", kOn},
                {"voice2Semitones", 12.0},
                {"voice2Level", -8.0},
                {"highCut", 9000.0}}),
        b.Make("power-fifths", "Power Fifths",
               {{"mode", kFixed},
                {"voice1Semitones", 7.0},
                {"voice1Level", -4.0},
                {"voice2On", kOn},
                {"voice2Semitones", -12.0},
                {"voice2Level", -4.0}}),
        // Unison voices, detuned and late: a second and third take of the same part.
        b.Make("double-tracked", "Double Tracked",
               {{"mode", kFixed},
                {"voice1Semitones", 0.0},
                {"voice1Detune", 7.0},
                {"voice1Delay", 24.0},
                {"voice1Pan", 1.0},
                {"voice1Level", -2.0},
                {"voice2On", kOn},
                {"voice2Semitones", 0.0},
                {"voice2Detune", -7.0},
                {"voice2Delay", 33.0},
                {"voice2Pan", -1.0},
                {"voice2Level", -2.0},
                {"dry", 0.8},
                {"humanize", 0.5}}),
    };
}
} // namespace guitarfx::harmonizer
