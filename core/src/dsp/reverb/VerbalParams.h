#pragma once

/**
 * VerbalParams.h — Verbal's machines and parameters: the one place their ranges, defaults and
 * labels live. Registration builds the registry's ParameterDefs from kParams, and SetParam
 * clamps against it.
 *
 * Enum orders are stored in presets as indices: append, never reorder.
 */

#include "dsp/EffectParamSpec.h"

#include <array>
#include <cstddef>
#include <iterator>
#include <string_view>

namespace guitarfx::verbal
{
enum class Machine : int
{
    Room = 0,
    Hall,
    Plate,
    Spring,
    Swell,
    Bloom,
    Cloud,
    Choir,
    Shimmer,
    TapeEcho,
    Nonlinear,
    Reflections,
    Count
};

inline constexpr std::size_t kMachineCount = static_cast<std::size_t>(Machine::Count);

inline constexpr const char* kMachineLabels[] = {"Room",  "Hall",  "Plate",   "Spring",    "Swell",     "Bloom",
                                                 "Cloud", "Choir", "Shimmer", "Tape Echo", "Nonlinear", "Reflections"};
static_assert(std::size(kMachineLabels) == kMachineCount);

/// One line under each machine in the picker.
inline constexpr const char* kMachineDetails[] = {
    "Studio to club ambience",
    "Concert hall or arena, slow to build",
    "Studio plate, dense and bright",
    "Spring tank with a driven preamp",
    "Reverb that swells in behind each note",
    "A tail that blooms after the note",
    "Huge, diffuse ambient wash",
    "Formant voices singing the tail",
    "Pitch-shifted voices climbing the tail",
    "Multi-head tape echo into a wash",
    "Shaped, gated and reversed reverbs",
    "Early reflections of a small room",
};
static_assert(std::size(kMachineDetails) == kMachineCount);

inline constexpr const char* kHoldModeLabels[] = {"Infinite", "Freeze"};
inline constexpr const char* kRoomSizeLabels[] = {"Studio", "Club"};
inline constexpr const char* kHallSizeLabels[] = {"Concert", "Arena"};
inline constexpr const char* kPlateSizeLabels[] = {"Small", "Large"};
inline constexpr const char* kDwellLabels[] = {"Clean", "Combo", "Tube", "Overdrive"};
inline constexpr const char* kSpringsLabels[] = {"1", "2", "3"};
inline constexpr const char* kSwellModeLabels[] = {"Wet", "Dry"};
inline constexpr const char* kVowelLabels[] = {"Aah-Ooh", "Aah", "Aah-Oh", "Oh", "Ooh-Oh", "Ooh", "Random"};
inline constexpr const char* kResonanceLabels[] = {"Mild", "Medium", "High"};
inline constexpr const char* kShimmerModeLabels[] = {"Input", "Regen", "Input + Regen"};
inline constexpr const char* kHeadsLabels[] = {"3", "4", "6"};
inline constexpr const char* kSpacingLabels[] = {"Even", "Uneven"};
inline constexpr const char* kShapeLabels[] = {"Swoosh", "Reverse", "Ramp", "Gate", "Gauss", "Bounce"};
inline constexpr const char* kRoomShapeLabels[] = {"Square", "Rectangle", "Oblong"};

/// Shimmer intervals in semitones, in the order the Shift pickers list them. ±0.1 is the ten-cent
/// detune; +19 and +24 are an octave and a fifth, and two octaves.
inline constexpr std::array<double, 28> kShiftSemitones = {
    -12.0, -11.0, -10.0, -9.0, -8.0, -7.0, -6.0, -5.0, -4.0, -3.0, -2.0, -1.0, -0.1, 0.1,
    1.0,   2.0,   3.0,   4.0,  5.0,  6.0,  7.0,  8.0,  9.0,  10.0, 11.0, 12.0, 19.0, 24.0};
inline constexpr const char* kShiftLabels[] = {"-Oct", "-M7", "-m7", "-M6", "-m6",  "-P5",      "-Tritone",
                                               "-P4",  "-M3", "-m3", "-M2", "-m2",  "-10c",     "+10c",
                                               "+m2",  "+M2", "+m3", "+M3", "+P4",  "+Tritone", "+P5",
                                               "+m6",  "+M6", "+m7", "+M7", "+Oct", "+Oct+5th", "+2 Oct"};
static_assert(std::size(kShiftLabels) == kShiftSemitones.size());
inline constexpr const char* kShift2Labels[] = {
    "Off",      "-Oct", "-M7", "-m7",  "-M6",  "-m6", "-P5",  "-Tritone", "-P4",   "-M3",
    "-m3",      "-M2",  "-m2", "-10c", "+10c", "+m2", "+M2",  "+m3",      "+M3",   "+P4",
    "+Tritone", "+P5",  "+m6", "+M6",  "+m7",  "+M7", "+Oct", "+Oct+5th", "+2 Oct"};
static_assert(std::size(kShift2Labels) == kShiftSemitones.size() + 1);

// Which machines show each machine-specific control (indices into Machine).
inline constexpr int kAllButReflections[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
inline constexpr int kPreDelayMachines[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 11};
inline constexpr int kLowEndMachines[] = {0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 11};
inline constexpr int kDiffusionMachines[] = {0, 6, 9, 10};
inline constexpr int kRoomOnly[] = {0};
inline constexpr int kHallOnly[] = {1};
inline constexpr int kPlateOnly[] = {2};
inline constexpr int kSpringOnly[] = {3};
inline constexpr int kSwellOnly[] = {4};
inline constexpr int kBloomOnly[] = {5};
inline constexpr int kChoirOnly[] = {7};
inline constexpr int kShimmerOnly[] = {8};
inline constexpr int kTapeOnly[] = {9};
inline constexpr int kNonlinearOnly[] = {10};
inline constexpr int kReflectionsOnly[] = {11};

enum Param : std::size_t
{
    kMachine,
    kDecay,
    kPreDelay,
    kTone,
    kMod,
    kLowEnd,
    kMix,
    kOutputGain,
    kHold,
    kHoldMode,
    kRoomSize,
    kHallSize,
    kMidEq,
    kPlateSize,
    kDwell,
    kSprings,
    kRiseTime,
    kSwellMode,
    kBloomLength,
    kBloomFeedback,
    kDiffusion,
    kVowel,
    kResonance,
    kShimmerMode,
    kShift1,
    kShift2,
    kShimmerAmount,
    kEchoTime,
    kEchoFeedback,
    kHeads,
    kSpacing,
    kShapeTime,
    kShape,
    kShapeFeedback,
    kLateLevel,
    kModSpeed,
    kSize,
    kRoomShape,
    kListenerX,
    kListenerY,
    kParamCount
};

/// Builds a spec shown only for `machines`.
[[nodiscard]] consteval EffectParamSpec For(EffectParamSpec spec, std::span<const int> machines)
{
    spec.visibleKey = "machine";
    spec.visibleFor = machines;
    return spec;
}

/// Entries are in `Param` order. The common controls sit in "Reverb"; each machine's own sit in
/// "Machine" and show only under that machine.
inline constexpr std::array<EffectParamSpec, kParamCount> kParams = {{
    {"machine", "Machine", 1.0, 0.0, 11.0, "enum", "Reverb", false, 1.0, kMachineLabels},
    For(LogTaper({"decay", "Decay", 3.0, 0.2, 60.0, "s", "Reverb"}), kAllButReflections),
    For(LogTaper({"preDelay", "Pre-Delay", 20.0, 1.0, 1500.0, "ms", "Reverb"}), kPreDelayMachines),
    {"tone", "Tone", 0.6, 0.0, 1.0, "amount", "Reverb"},
    {"mod", "Mod", 0.4, 0.0, 1.0, "amount", "Reverb"},
    For({"lowEnd", "Low End", 0.5, 0.0, 1.0, "amount", "Reverb"}, kLowEndMachines),
    {"mix", "Mix", 0.35, 0.0, 1.0, "amount", "Reverb"},
    {"outputGain", "Output", 0.0, -12.0, 12.0, "dB", "Reverb"},
    {"hold", "Hold", 0.0, 0.0, 1.0, "toggle", "Hold", false, 1.0},
    {"holdMode", "Hold Mode", 0.0, 0.0, 1.0, "enum", "Hold", false, 1.0, kHoldModeLabels},
    For({"roomSize", "Size", 0.0, 0.0, 1.0, "enum", "Machine", false, 1.0, kRoomSizeLabels}, kRoomOnly),
    For({"hallSize", "Size", 0.0, 0.0, 1.0, "enum", "Machine", false, 1.0, kHallSizeLabels}, kHallOnly),
    For({"midEq", "Mid EQ", 0.5, 0.0, 1.0, "amount", "Machine"}, kHallOnly),
    For({"plateSize", "Size", 1.0, 0.0, 1.0, "enum", "Machine", false, 1.0, kPlateSizeLabels}, kPlateOnly),
    For({"dwell", "Dwell", 1.0, 0.0, 3.0, "enum", "Machine", false, 1.0, kDwellLabels}, kSpringOnly),
    For({"springs", "Springs", 1.0, 0.0, 2.0, "enum", "Machine", false, 1.0, kSpringsLabels}, kSpringOnly),
    For(LogTaper({"riseTime", "Rise Time", 0.6, 0.08, 4.0, "s", "Machine"}), kSwellOnly),
    For({"swellMode", "Swell", 0.0, 0.0, 1.0, "enum", "Machine", false, 1.0, kSwellModeLabels}, kSwellOnly),
    For({"bloomLength", "Length", 0.3, 0.0, 1.0, "amount", "Machine"}, kBloomOnly),
    For({"bloomFeedback", "Feedback", 0.1, 0.0, 1.0, "amount", "Machine"}, kBloomOnly),
    For({"diffusion", "Diffusion", 0.6, 0.0, 1.0, "amount", "Machine"}, kDiffusionMachines),
    For({"vowel", "Vowel", 0.0, 0.0, 6.0, "enum", "Machine", false, 1.0, kVowelLabels}, kChoirOnly),
    For({"resonance", "Resonance", 1.0, 0.0, 2.0, "enum", "Machine", false, 1.0, kResonanceLabels}, kChoirOnly),
    For({"shimmerMode", "Mode", 1.0, 0.0, 2.0, "enum", "Machine", false, 1.0, kShimmerModeLabels}, kShimmerOnly),
    For({"shift1", "Shift 1", 25.0, 0.0, 27.0, "enum", "Machine", false, 1.0, kShiftLabels}, kShimmerOnly),
    For({"shift2", "Shift 2", 0.0, 0.0, 28.0, "enum", "Machine", false, 1.0, kShift2Labels}, kShimmerOnly),
    For({"shimmerAmount", "Amount", 0.6, 0.0, 1.0, "amount", "Machine"}, kShimmerOnly),
    For(LogTaper({"echoTime", "Echo Time", 400.0, 200.0, 1500.0, "ms", "Machine"}), kTapeOnly),
    For({"echoFeedback", "Feedback", 0.5, 0.0, 1.0, "amount", "Machine"}, kTapeOnly),
    For({"heads", "Heads", 0.0, 0.0, 2.0, "enum", "Machine", false, 1.0, kHeadsLabels}, kTapeOnly),
    For({"spacing", "Spacing", 0.0, 0.0, 1.0, "enum", "Machine", false, 1.0, kSpacingLabels}, kTapeOnly),
    For(LogTaper({"shapeTime", "Time", 800.0, 50.0, 2000.0, "ms", "Machine"}), kNonlinearOnly),
    For({"shape", "Shape", 1.0, 0.0, 5.0, "enum", "Machine", false, 1.0, kShapeLabels}, kNonlinearOnly),
    For({"shapeFeedback", "Feedback", 0.0, 0.0, 1.0, "amount", "Machine"}, kNonlinearOnly),
    For({"lateLevel", "Late Level", 0.45, 0.0, 1.0, "amount", "Machine"}, kNonlinearOnly),
    For({"modSpeed", "Mod Speed", 0.5, 0.0, 1.0, "amount", "Machine"}, kNonlinearOnly),
    For({"size", "Room Size", 1.0, 0.0, 1.0, "amount", "Machine"}, kReflectionsOnly),
    For({"roomShape", "Room Shape", 2.0, 0.0, 2.0, "enum", "Machine", false, 1.0, kRoomShapeLabels}, kReflectionsOnly),
    For({"listenerX", "Position L-R", 0.33, 0.0, 1.0, "amount", "Machine"}, kReflectionsOnly),
    For({"listenerY", "Position F-B", 0.0, 0.0, 1.0, "amount", "Machine"}, kReflectionsOnly),
}};

[[nodiscard]] inline std::size_t FindParam(std::string_view key)
{
    return FindParamSpec(kParams, key);
}
} // namespace guitarfx::verbal
