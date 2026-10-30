#pragma once

#include "dsp/EffectParamSpec.h"
#include "dsp/EffectRegistry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

/**
 * What the Dispersal effect is, as opposed to how it runs (DispersalEffect.h): its constants, its
 * parameter table, the all-pass stage's design and the factory presets.
 */
namespace guitarfx::dispersal
{
constexpr double kPi = 3.14159265358979323846;

constexpr int kMaxStages = 256;

/// Frequency, Focus and Stereo are glided once per this many samples, and the stages'
/// coefficients ramp linearly in between.
constexpr int kControlInterval = 16;
/// Frequency and Focus glide in the log domain, Stereo in semitones, so a MIDI CC's 1/127 steps
/// do not zipper the sweep.
constexpr double kGlideMs = 20.0;
constexpr double kMixGlideMs = 20.0;
/// A change of Stages crossfades from the old count's output to the new one's over this long.
/// Half-way between two counts one stage apart there is a notch at the centre (that stage turns
/// it through 180 degrees), so the fade is kept short.
constexpr double kStageFadeMs = 30.0;

/// A stage's centre is held below this fraction of the sample rate, where the prewarped design
/// is still well conditioned; Stereo can push the right side's an octave above the knob's top.
constexpr double kMaxCentreFraction = 0.45;

/// Stage state below this is flushed to zero at the end of each chunk, so a chain ringing out
/// into silence does not run on subnormals. It is -240 dB, far under anything audible.
constexpr float kStateFlush = 1.0e-12f;

/// Once Stereo is back at 0 and the input is mono, the right side is dropped and the left's
/// output copied over it as soon as the two chains' states are this close.
constexpr float kLinkTolerance = 1.0e-7f;

enum Param : std::size_t
{
    kStages,
    kFrequency,
    kFocus,
    kStereo,
    kMix,
    kParamCount
};

/// In `Param` order, which is also the order the UI lays the controls out in. Stages spans more
/// than two decades, so it is on a log taper too: 1 to 16 gets a third of the travel.
inline constexpr std::array<EffectParamSpec, kParamCount> kParams = {{
    LogTaper({"stages", "Stages", 64.0, 1.0, static_cast<double>(kMaxStages), "amount", "Dispersion", false, 1.0, {}}),
    LogTaper({"frequency", "Frequency", 800.0, 100.0, 10000.0, "Hz", "Dispersion", false, 0.0, {}}),
    LogTaper({"focus", "Focus", 1.0, 0.3, 6.0, "amount", "Dispersion", false, 0.0, {}}),
    {"stereo", "Stereo", 0.0, -12.0, 12.0, "st", "Output", false, 0.0, {}},
    {"mix", "Mix", 1.0, 0.0, 1.0, "amount", "Output", false, 0.0, {}},
}};

/// Every parameter's value, indexed by `Param`, always normalised (NormaliseParamValue).
using ParamValues = std::array<double, kParamCount>;

inline constexpr ParamValues kDefaultValues = DefaultParamValues(kParams);

/// One stage's coefficients: a trapezoidal (TPT) state-variable filter whose all-pass output is
/// x - 2k * band. Every stage on a side shares them. The SVF's state stays bounded while its
/// coefficients move, which matters here: whatever a stage gained under modulation, a chain of
/// 256 would gain 256 times over.
struct StageCoefficients
{
    float a1 = 1.0f;
    float a2 = 0.0f;
    float a3 = 0.0f;
    float twoK = 2.0f;
};

[[nodiscard]] inline double ClampCentreHz(double centreHz, double sampleRate)
{
    return std::clamp(centreHz, 1.0, kMaxCentreFraction * sampleRate);
}

/// The stage centred on `centreHz` with quality factor `q` (Focus).
[[nodiscard]] inline StageCoefficients DesignStage(double centreHz, double q, double sampleRate)
{
    const double g = std::tan(kPi * ClampCentreHz(centreHz, sampleRate) / sampleRate);
    const double k = 1.0 / q;
    const double a1 = 1.0 / (1.0 + g * (g + k));
    return {static_cast<float>(a1), static_cast<float>(g * a1), static_cast<float>(g * g * a1),
            static_cast<float>(2.0 * k)};
}

/// One stage's group delay at its centre, in seconds. The analogue all-pass delays its centre
/// by 4Q/w0; the bilinear transform prewarps w0 to 2 fs g and stretches time there by
/// dW/dw = 1 + g^2, so the digital stage delays it by 2Q(1 + g^2) / (g fs), which tends to
/// 2Q / (pi f0) well below Nyquist.
[[nodiscard]] inline double StagePeakDelaySeconds(double centreHz, double q, double sampleRate)
{
    const double g = std::tan(kPi * ClampCentreHz(centreHz, sampleRate) / sampleRate);
    return 2.0 * q * (1.0 + g * g) / (g * sampleRate);
}

/// One stage's group delay at `hz`, in seconds. The bilinear transform maps the digital response
/// exactly onto the analogue one at W = 2 fs tan(pi f / fs), so this is the analogue all-pass's
/// 2kW0(W0^2 + W^2) / ((W0^2 - W^2)^2 + k^2 W0^2 W^2) stretched by dW/dw = 1 + tan^2(pi f / fs).
/// At the centre it is StagePeakDelaySeconds; towards DC it falls only to 1/(Q pi f0), half the
/// peak at Q 1, so a low Focus delays the lows too.
[[nodiscard]] inline double StageGroupDelaySeconds(double hz, double centreHz, double q, double sampleRate)
{
    const double t = std::tan(kPi * std::clamp(hz, 0.0, 0.499 * sampleRate) / sampleRate);
    const double w = 2.0 * sampleRate * t;
    const double w0 = 2.0 * sampleRate * std::tan(kPi * ClampCentreHz(centreHz, sampleRate) / sampleRate);
    const double k = 1.0 / q;
    const double d = w0 * w0 - w * w;
    return 2.0 * k * w0 * (w0 * w0 + w * w) / (d * d + k * k * w0 * w0 * w * w) * (1.0 + t * t);
}

/// The right side's centre: Frequency moved by Stereo semitones.
[[nodiscard]] inline double RightCentreHz(double frequencyHz, double stereoSemitones)
{
    return frequencyHz * std::exp2(stereoSemitones / 12.0);
}

/**
 * How a factory preset sets up the effect. A preset is a whole sound, so it sets every
 * parameter. The first is the defaults, which is also what a new node starts with. The
 * comments give the delay at Frequency against the rest of the spectrum.
 */
struct Voicing
{
    const char* id;
    const char* displayName;
    double stages;
    double frequency;
    double focus;
    double stereo;
    double mix;
};

constexpr const char* kDefaultPresetId = "zap";

// clang-format off
inline constexpr std::array<Voicing, 6> kFactoryVoicings = {{
    //  id               display name    stages freq    focus stereo mix
    {kDefaultPresetId,   "Zap",          64.0,  800.0,  1.0,  0.0,   1.0}, // 51 ms
    {"laser",            "Laser",        64.0,  1200.0, 2.0,  0.0,   1.0}, // 68 ms, narrower
    {"dub-drop",         "Dub Drop",     96.0,  250.0,  2.0,  0.0,   1.0}, // 0.49 s
    {"soft-attack",      "Soft Attack",  24.0,  2000.0, 0.5,  0.0,   1.0}, // 2 ms: rounds the pick
    {"wide-zap",         "Wide Zap",     96.0,  900.0,  1.5,  7.0,   1.0}, // 102 ms left, 68 ms right
    {"comb-phase",       "Comb Phase",   8.0,   600.0,  0.7,  0.0,   0.5}, // 8 notches against the dry
}};
// clang-format on

[[nodiscard]] inline EffectPresetDefinition MakePreset(const Voicing& voicing)
{
    EffectPresetDefinition preset;
    preset.id = voicing.id;
    preset.displayName = voicing.displayName;
    preset.isFactory = true;
    preset.isDefault = (preset.id == kDefaultPresetId);
    preset.parameters = {{"stages", voicing.stages},
                         {"frequency", voicing.frequency},
                         {"focus", voicing.focus},
                         {"stereo", voicing.stereo},
                         {"mix", voicing.mix}};
    preset.parameterOrder = {"stages", "frequency", "focus", "stereo", "mix"};
    return preset;
}

[[nodiscard]] inline std::vector<EffectPresetDefinition> FactoryPresets()
{
    std::vector<EffectPresetDefinition> presets;
    presets.reserve(kFactoryVoicings.size());

    for (const auto& voicing : kFactoryVoicings)
    {
        presets.push_back(MakePreset(voicing));
    }

    return presets;
}
} // namespace guitarfx::dispersal
