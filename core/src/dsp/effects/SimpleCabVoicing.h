#pragma once

#include "dsp/BiquadDesign.h"
#include "dsp/EffectParamSpec.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>

/**
 * The Simple Cabinet's voicing, as a pure model: parameter values in, filter designs and
 * frequency responses out, with no audio state.
 *
 * SimpleCabEffect runs the designs. Everything else that needs to know what the cab sounds
 * like asks this model instead of keeping its own copy: auto level measures the design's
 * loudness, the UI's response curve and the IR export come from the same response, and IR
 * matching searches the parameters against it.
 *
 * The signal path it describes, per channel:
 *
 *     speaker drive (level-dependent, in the effect; see SpeakerDrive.h)
 *       -> cabinet sections: low cut, low resonance, mids, presence, 3 x low-pass
 *       -> each mic: direct tap + floor reflection tap, then proximity / presence / air
 *       -> mic 1 and mic 2 blended, level trimmed, mixed with the dry signal
 *
 * The defaults reproduce the original 4x12 voicing exactly, so presets saved before these
 * controls existed sound the same.
 */
namespace guitarfx::simple_cab
{
// ---------------------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------------------

enum Param : int
{
    kCabinet,
    kSize,
    kBass,
    kMids,
    kPresence,
    kBrightness,
    kMicType,
    kMicPosition,
    kMicDistance,
    kSpeakerDrive,
    kOutputGain,
    kAutoLevel,
    kMic2Blend,
    kMic2Type,
    kMic2Position,
    kMic2Distance,
    kMic2Polarity,
    kSpread,
    kMix,
    kParamCount
};

inline constexpr const char* kCabinetLabels[] = {"4x12 Closed", "1x12 Open", "2x12 Open", "2x12 Closed",  "4x10 Open",
                                                 "AM Radio",    "Telephone", "Boombox",   "Practice Amp", "VHS"};
inline constexpr const char* kMicTypeLabels[] = {"Dynamic", "Ribbon", "Condenser"};

/// In `Param` order, which is also the order the UI lays the controls out in.
inline constexpr std::array<EffectParamSpec, kParamCount> kParams = {{
    {"cabinet", "Cabinet", 0.0, 0.0, 9.0, "enum", "Cabinet", false, 1.0, kCabinetLabels},
    {"size", "Size", 0.5, 0.0, 1.0, "amount", "Cabinet", false, 0.0, {}},
    {"bass", "Bass", 0.5, 0.0, 1.0, "amount", "Cabinet", false, 0.0, {}},
    {"mids", "Mids", 0.5, 0.0, 1.0, "amount", "Cabinet", false, 0.0, {}},
    {"presence", "Presence", 0.5, 0.0, 1.0, "amount", "Cabinet", false, 0.0, {}},
    {"brightness", "Brightness", 0.5, 0.0, 1.0, "amount", "Cabinet", false, 0.0, {}},
    {"micType", "Mic", 0.0, 0.0, 2.0, "enum", "Mic", false, 1.0, kMicTypeLabels},
    {"micPosition", "Position", 0.5, 0.0, 1.0, "amount", "Mic", false, 0.0, {}},
    {"micDistance", "Distance", 0.0, 0.0, 1.0, "amount", "Mic", false, 0.0, {}},
    {"speakerDrive", "Speaker Drive", 0.0, 0.0, 1.0, "amount", "Speaker", false, 0.0, {}},
    {"outputGain", "Output", 0.0, -24.0, 24.0, "dB", "Level", false, 0.0, {}},
    {"autoLevel", "Auto Level", 0.0, 0.0, 1.0, "toggle", "Level", false, 1.0, {}},
    {"mic2Blend", "Mic Blend", 0.0, 0.0, 1.0, "blend", "Mic 2", true, 0.0, {}},
    {"mic2Type", "Mic 2", 1.0, 0.0, 2.0, "enum", "Mic 2", true, 1.0, kMicTypeLabels},
    {"mic2Position", "Mic 2 Position", 0.5, 0.0, 1.0, "amount", "Mic 2", true, 0.0, {}},
    {"mic2Distance", "Mic 2 Distance", 0.0, 0.0, 1.0, "amount", "Mic 2", true, 0.0, {}},
    {"mic2Polarity", "Mic 2 Invert", 0.0, 0.0, 1.0, "toggle", "Mic 2", true, 1.0, {}},
    {"spread", "Stereo Spread", 0.0, 0.0, 1.0, "amount", "Stereo", true, 0.0, {}},
    {"mix", "Mix", 1.0, 0.0, 1.0, "amount", "Level", true, 0.0, {}},
}};

/// Every parameter's value, indexed by `Param`, always normalised (NormaliseParamValue).
using ParamValues = std::array<double, kParamCount>;

inline constexpr ParamValues kDefaultValues = DefaultParamValues(kParams);

// ---------------------------------------------------------------------------------------
// Settings: the parameter values, typed
// ---------------------------------------------------------------------------------------

enum class Cabinet : int
{
    Closed4x12,
    Open1x12,
    Open2x12,
    Closed2x12,
    Open4x10,
    // Not guitar cabinets: small, band-limited speakers, for lo-fi tones. Appended, so the
    // indices stored in presets keep their meaning.
    AmRadio,
    Telephone,
    Boombox,
    PracticeAmp,
    Vhs,
    Count
};

enum class MicType : int
{
    Dynamic,
    Ribbon,
    Condenser,
    Count
};

struct MicSettings
{
    MicType type = MicType::Dynamic;
    double position = 0.5; ///< 0 = centre of the dust cap, 0.5 = cap edge, 1 = edge of the cone
    double distance = 0.0; ///< 0 = touching the grille, 1 = kMaxMicDistanceM away
};

struct Settings
{
    Cabinet cabinet = Cabinet::Closed4x12;
    double size = 0.5;
    double bass = 0.5;
    double mids = 0.5;
    double presence = 0.5;
    double brightness = 0.5;
    MicSettings mic;
    MicSettings mic2 = {MicType::Ribbon, 0.5, 0.0};
    double mic2Blend = 0.0;
    bool mic2Inverted = false;
    double spread = 0.0;
    double outputGainDb = 0.0;
    bool autoLevel = false;
    double mix = 1.0;
};

/// Reads the typed settings out of normalised parameter values. Speaker drive is not part
/// of it: that stage depends on level, so it cannot be described by a frequency response.
[[nodiscard]] inline Settings ToSettings(const ParamValues& v) noexcept
{
    Settings s;
    s.cabinet = static_cast<Cabinet>(static_cast<int>(v[kCabinet]));
    s.size = v[kSize];
    s.bass = v[kBass];
    s.mids = v[kMids];
    s.presence = v[kPresence];
    s.brightness = v[kBrightness];
    s.mic = {static_cast<MicType>(static_cast<int>(v[kMicType])), v[kMicPosition], v[kMicDistance]};
    s.mic2 = {static_cast<MicType>(static_cast<int>(v[kMic2Type])), v[kMic2Position], v[kMic2Distance]};
    s.mic2Blend = v[kMic2Blend];
    s.mic2Inverted = v[kMic2Polarity] > 0.5;
    s.spread = v[kSpread];
    s.outputGainDb = v[kOutputGain];
    s.autoLevel = v[kAutoLevel] > 0.5;
    s.mix = v[kMix];
    return s;
}

// ---------------------------------------------------------------------------------------
// Voicing tables
// ---------------------------------------------------------------------------------------

/// How one cabinet type sounds with every control at noon. Each "span" is how far the
/// matching control moves that value across its range.
struct CabinetVoicing
{
    double highPassHz; ///< low cut with Bass at 0; Bass lowers it by highPassBassSpanHz
    double highPassBassSpanHz;
    double resonanceHz; ///< the box's low resonance; Size moves it
    double resonanceQ;
    double resonanceDb; ///< its height with Bass at 0; Bass adds resonanceBassSpanDb
    double resonanceBassSpanDb;
    double lowMidHz;   ///< centre of the Mids control
    double lowMidDb;   ///< the cabinet's own low-mid character, before Mids
    double presenceHz; ///< with Presence at 0; Presence raises it by presenceSpanHz
    double presenceSpanHz;
    double presenceDb; ///< with Presence at 0; Presence adds presenceSpanDb
    double presenceSpanDb;
    double lowPassHz; ///< speaker roll-off with Brightness at 0
    double lowPassSpanHz;
    double speakerHeightM; ///< speaker centre above the floor, for the mic's floor bounce
};

// clang-format off
inline constexpr std::array<CabinetVoicing, static_cast<int>(Cabinet::Count)> kCabinets = {{
    // 4x12 closed back. The original voicing, unchanged: a broad resonance and a steep
    // roll-off, with the envelope of the bundled ENGL and Marshall 1960 IRs.
    {90.0, 50.0, 140.0, 0.6, 6.0, 8.0, 500.0, 0.0, 2000.0, 1500.0, -1.0, 9.0, 4200.0, 2100.0, 0.35},
    // 1x12 open back combo. An open back cancels the deepest lows, leaving a smaller,
    // narrower bump; the mids come forward and the top is more open.
    {120.0, 60.0, 115.0, 1.0, 3.0, 6.0, 700.0, 1.5, 2400.0, 1600.0, -1.0, 8.0, 4800.0, 2400.0, 0.30},
    // 2x12 open back: between the combo and the closed cabs.
    {105.0, 55.0, 105.0, 0.9, 4.0, 7.0, 600.0, 1.0, 2200.0, 1500.0, -1.0, 9.0, 4500.0, 2200.0, 0.32},
    // 2x12 closed back: a tighter, slightly smaller 4x12.
    {95.0, 50.0, 125.0, 0.7, 5.0, 8.0, 500.0, 0.0, 2100.0, 1500.0, -1.0, 9.0, 4300.0, 2100.0, 0.30},
    // 4x10 open back, tweed style: a higher, lighter low end, scooped low mids and the
    // brightest top of the set.
    {100.0, 50.0, 160.0, 0.8, 3.5, 6.0, 450.0, -1.5, 2800.0, 1700.0, 0.0, 8.0, 5200.0, 2400.0, 0.35},
    // AM radio: a three-inch speaker in a plastic case. Nothing below 300 Hz or above 2.6 kHz,
    // and a honk in between.
    {300.0, 80.0, 420.0, 1.0, 0.0, 3.0, 800.0, 2.0, 1300.0, 600.0, 4.5, 6.0, 2400.0, 800.0, 0.20},
    // Telephone: the 300 Hz to 3.4 kHz voice band, flat across it.
    {440.0, 80.0, 500.0, 0.8, 0.0, 2.0, 1000.0, 0.0, 1800.0, 800.0, -1.0, 4.0, 3000.0, 800.0, 0.20},
    // Boombox: a ported five-inch woofer, boomy around 170 Hz, with a dull paper-cone top.
    {160.0, 50.0, 170.0, 1.2, 4.0, 5.0, 500.0, -1.5, 2500.0, 1500.0, 0.0, 6.0, 5200.0, 2000.0, 0.25},
    // Practice amp: a six-inch speaker in a small open box. Boxy low mids and little else.
    {210.0, 60.0, 240.0, 1.0, 1.0, 4.0, 450.0, 3.0, 2000.0, 1200.0, -1.0, 7.0, 3600.0, 1500.0, 0.25},
    // VHS: a video tape's linear audio track. Only the band limits; the Tape effect supplies
    // the wow, hiss and saturation that go with them.
    {120.0, 40.0, 150.0, 0.6, 0.0, 2.0, 500.0, 0.0, 2500.0, 1000.0, -2.0, 4.0, 4200.0, 1600.0, 0.30},
}};
// clang-format on

/// A mic type's colour relative to the dynamic mic the original voicing assumes, in dB at
/// each of the mic sections (see kMicSection* below).
struct MicTypeVoicing
{
    double proximityDb;
    double presenceDb;
    double airDb;
    double proximityScale; ///< how strongly moving away loses the close-up bass boost
};

inline constexpr std::array<MicTypeVoicing, static_cast<int>(MicType::Count)> kMicTypes = {{
    {0.0, 0.0, 0.0, 1.0},   // dynamic: the reference
    {3.0, -2.0, -6.0, 1.3}, // ribbon: dark, smooth top; figure-8, so more proximity bass
    {-1.0, -1.5, 4.0, 1.0}, // condenser: extended top, flatter presence
}};

// Each mic is three sections at fixed frequencies; type, position and distance each add
// their dB to them.
inline constexpr double kMicProximityHz = 200.0;
inline constexpr double kMicPresenceHz = 4000.0;
inline constexpr double kMicPresenceQ = 1.2;
inline constexpr double kMicAirHz = 3000.0;

// Mic position, from the cap centre (bright, a hard presence peak) to the cone edge (dark:
// the cone beams its highs forward, and the low mids fill in).
inline constexpr double kCapPresenceDb = 3.0;
inline constexpr double kCapAirDb = 2.5;
inline constexpr double kEdgeProximityDb = 1.5;
inline constexpr double kEdgePresenceDb = -2.0;
inline constexpr double kEdgeAirDb = -9.0;

// Mic distance. The knob maps quadratically onto 0..kMaxMicDistanceM so the close range,
// where the sound changes fastest, gets most of the travel.
inline constexpr double kMaxMicDistanceM = 1.0;
inline constexpr double kSpeedOfSoundMs = 343.0;
inline constexpr double kProximityLossDb = 6.0; ///< close-up bass boost that distance removes
inline constexpr double kProximityFalloffM = 0.15;
inline constexpr double kFloorReflectance = 0.7;
inline constexpr double kReflectionCornerHz = 2500.0; ///< the floor and the off-axis path dull the bounce

[[nodiscard]] constexpr double HighestSpeakerM() noexcept
{
    double highest = 0.0;

    for (const auto& cabinet : kCabinets)
    {
        highest = std::max(highest, cabinet.speakerHeightM);
    }

    return highest;
}

/// Longest delay a mic tap can ask for: the furthest mic behind the nearest, plus the
/// floor bounce from the highest speaker. The effect sizes its delay line from this.
inline constexpr double kMaxMicDelaySeconds = (kMaxMicDistanceM + 2.0 * HighestSpeakerM()) / kSpeedOfSoundMs;

// The original voicing's own constants, kept as written so its response is unchanged.
inline constexpr double kCabinetHighPassQ = 0.707;
inline constexpr double kPresenceQ = 1.5;

inline constexpr double kMidsRangeDb = 12.0; ///< Mids covers +/- half of this
inline constexpr double kMidsQ = 0.8;
inline constexpr double kSizeRangeOctaves = 1.0; ///< Size moves the low end +/- half an octave

// Stereo spread voices the two sides like two different speakers in the same cab.
inline constexpr double kSpreadResonanceOctaves = 0.2;
inline constexpr double kSpreadPresenceOctaves = 0.25;
inline constexpr double kSpreadLowPassOctaves = 0.15;
inline constexpr double kSpreadMicPosition = 0.2;

// ---------------------------------------------------------------------------------------
// Designs
// ---------------------------------------------------------------------------------------

enum CabinetSection : int
{
    kSectionHighPass,
    kSectionResonance,
    kSectionMids,
    kSectionPresence,
    kSectionLowPass1,
    kSectionLowPass2,
    kSectionLowPass3,
    kCabinetSectionCount
};

enum MicSection : int
{
    kMicSectionProximity,
    kMicSectionPresence,
    kMicSectionAir,
    kMicSectionCount
};

inline constexpr int kMicCount = 2;

struct MicDesign
{
    std::array<BiquadCoefficients, kMicSectionCount> sections;
    double directDelaySamples = 0.0;     ///< behind the nearer mic; the nearer one reads 0
    double reflectionDelaySamples = 0.0; ///< the floor bounce's total delay
    double reflectionGain = 0.0;
    double reflectionPole = 0.0; ///< one-pole low-pass on the bounce
    double gain = 0.0;           ///< blend and polarity
};

struct ChannelDesign
{
    std::array<BiquadCoefficients, kCabinetSectionCount> cabinet;
    std::array<MicDesign, kMicCount> mics;
};

struct Design
{
    std::array<ChannelDesign, 2> channels;
    double wetGain = 1.0; ///< cabinet level trim, or the auto level correction
    double outputGain = 1.0;
    double mix = 1.0;
    bool mic2Active = false;
    bool stereo = false; ///< the channels are voiced differently
};

[[nodiscard]] inline double DbToGain(double db) noexcept
{
    return std::pow(10.0, db / 20.0);
}

/// Low resonance frequency after Size, before any stereo spread. SpeakerDrive centres its
/// excursion band on it.
[[nodiscard]] inline double ResonanceHz(const Settings& s) noexcept
{
    const CabinetVoicing& cab = kCabinets[static_cast<int>(s.cabinet)];
    return cab.resonanceHz * std::exp2((0.5 - s.size) * kSizeRangeOctaves);
}

/**
 * Turns settings into designs for one sample rate, and measures them.
 *
 * Prepare() caches what depends only on the sample rate: the reference loudness auto level
 * holds the cab to, and the trim that level-matches each cabinet type at its default
 * settings. Build() allocates nothing, so the effect can call it from SetParam on the
 * audio thread.
 */
class Voicer
{
  public:
    explicit Voicer(double sampleRate = 48000.0) noexcept
    {
        Prepare(sampleRate);
    }

    void Prepare(double sampleRate) noexcept
    {
        mSampleRate = sampleRate;

        for (int i = 0; i < kLoudnessProbeCount; ++i)
        {
            const double hz = kLoudnessLowHz * std::pow(kLoudnessHighHz / kLoudnessLowHz,
                                                        static_cast<double>(i) / (kLoudnessProbeCount - 1));
            mProbeOmega[i] = 2.0 * biquad::kPi * hz / sampleRate;
        }

        const Settings reference;
        mReferenceLoudnessDb = WetLoudnessDb(BuildShape(reference));

        for (int c = 0; c < static_cast<int>(Cabinet::Count); ++c)
        {
            Settings cabinetDefault;
            cabinetDefault.cabinet = static_cast<Cabinet>(c);
            mCabinetTrimDb[c] = mReferenceLoudnessDb - WetLoudnessDb(BuildShape(cabinetDefault));
        }
    }

    [[nodiscard]] double SampleRate() const noexcept
    {
        return mSampleRate;
    }

    /// The level trim that makes `cabinet` as loud as the 4x12 at default settings.
    [[nodiscard]] double CabinetTrimDb(Cabinet cabinet) const noexcept
    {
        return mCabinetTrimDb[static_cast<int>(cabinet)];
    }

    [[nodiscard]] Design Build(const Settings& s) const noexcept
    {
        Design design = BuildShape(s);
        // Auto level holds the wet path to the default voicing's loudness; without it, only
        // the cabinet type is trimmed, so the tone controls still behave like tone controls.
        const double levelDb = s.autoLevel ? mReferenceLoudnessDb - WetLoudnessDb(design) : CabinetTrimDb(s.cabinet);
        design.wetGain = DbToGain(levelDb);
        design.outputGain = DbToGain(s.outputGainDb);
        design.mix = s.mix;
        return design;
    }

    /// The complete linear response of one channel at `hz`, dry mix and output included:
    /// what the effect does to a quiet signal (speaker drive is transparent at low level).
    [[nodiscard]] std::complex<double> Response(const Design& design, int channel, double hz) const noexcept
    {
        const double omega = 2.0 * biquad::kPi * hz / mSampleRate;
        const std::complex<double> wet = WetResponse(design, channel, omega) * design.wetGain;
        return design.outputGain * ((1.0 - design.mix) + design.mix * wet);
    }

    /// Power-average magnitude of both channels in dB, as one curve for the UI.
    [[nodiscard]] double MagnitudeDb(const Design& design, double hz) const noexcept
    {
        const double left = std::norm(Response(design, 0, hz));
        const double right = design.stereo ? std::norm(Response(design, 1, hz)) : left;
        return 10.0 * std::log10(std::max(0.5 * (left + right), 1e-20));
    }

  private:
    // Loudness is judged over the guitar's band, from below a drop-tuned low string to the
    // top of the speaker, about 1/8 octave apart. A pink-weighted power average (every probe
    // counts the same, so every octave does) stands in for a guitar signal, and the close
    // spacing averages over a mic comb rather than landing on it.
    static constexpr int kLoudnessProbeCount = 54;
    static constexpr double kLoudnessLowHz = 60.0;
    static constexpr double kLoudnessHighHz = 6000.0;

    [[nodiscard]] Design BuildShape(const Settings& s) const noexcept
    {
        Design design;
        design.stereo = s.spread > 0.0;
        design.mic2Active = s.mic2Blend > 0.0;
        design.channels[0] = BuildChannel(s, design.stereo ? -1.0 : 0.0);
        design.channels[1] = design.stereo ? BuildChannel(s, 1.0) : design.channels[0];

        const double mic2Gain = s.mic2Blend * (s.mic2Inverted ? -1.0 : 1.0);

        for (auto& channel : design.channels)
        {
            channel.mics[0].gain = 1.0 - s.mic2Blend;
            channel.mics[1].gain = mic2Gain;
        }

        // Only the time between the mics is heard in a blend; delaying both would just add
        // latency. The nearer mic reads the cab directly and the other waits for it. A
        // silent mic 2 takes no part, so a lone distant mic 1 adds no latency either.
        const double mic1Direct = DirectDelaySamples(s.mic);
        const double mic2Direct = design.mic2Active ? DirectDelaySamples(s.mic2) : mic1Direct;
        const double nearest = std::min(mic1Direct, mic2Direct);

        for (auto& channel : design.channels)
        {
            channel.mics[0].directDelaySamples = mic1Direct - nearest;
            channel.mics[1].directDelaySamples = mic2Direct - nearest;

            for (auto& mic : channel.mics)
            {
                mic.reflectionDelaySamples += mic.directDelaySamples;
            }
        }

        return design;
    }

    /// One side's voicing. `spreadSign` is -1 for the left, +1 for the right and 0 when
    /// both sides are the same.
    [[nodiscard]] ChannelDesign BuildChannel(const Settings& s, double spreadSign) const noexcept
    {
        const CabinetVoicing& cab = kCabinets[static_cast<int>(s.cabinet)];
        const double spread = s.spread * spreadSign;
        const double fs = mSampleRate;
        ChannelDesign channel;

        // Size moves the whole low end together: a bigger box resonates and cuts off lower.
        const double sizeScale = std::exp2((0.5 - s.size) * kSizeRangeOctaves);
        const double highPassHz = (cab.highPassHz - s.bass * cab.highPassBassSpanHz) * sizeScale;
        const double resonanceHz = cab.resonanceHz * sizeScale * std::exp2(spread * kSpreadResonanceOctaves);
        const double resonanceDb = cab.resonanceDb + s.bass * cab.resonanceBassSpanDb;
        const double midsDb = cab.lowMidDb + (s.mids - 0.5) * kMidsRangeDb;
        const double presenceHz =
            (cab.presenceHz + s.presence * cab.presenceSpanHz) * std::exp2(-spread * kSpreadPresenceOctaves);
        const double presenceDb = cab.presenceDb + s.presence * cab.presenceSpanDb;
        const double lowPassHz =
            (cab.lowPassHz + s.brightness * cab.lowPassSpanHz) * std::exp2(spread * kSpreadLowPassOctaves);

        channel.cabinet[kSectionHighPass] = biquad::HighPass(highPassHz, kCabinetHighPassQ, fs);
        channel.cabinet[kSectionResonance] = biquad::Peaking(resonanceHz, cab.resonanceQ, resonanceDb, fs);
        channel.cabinet[kSectionMids] = biquad::Peaking(cab.lowMidHz, kMidsQ, midsDb, fs);
        channel.cabinet[kSectionPresence] = biquad::Peaking(presenceHz, kPresenceQ, presenceDb, fs);
        // Three sections at the Butterworth Qs make a smooth sixth-order speaker roll-off.
        channel.cabinet[kSectionLowPass1] = biquad::LowPass(lowPassHz, biquad::ButterworthSectionQ(6, 0), fs);
        channel.cabinet[kSectionLowPass2] = biquad::LowPass(lowPassHz, biquad::ButterworthSectionQ(6, 1), fs);
        channel.cabinet[kSectionLowPass3] = biquad::LowPass(lowPassHz, biquad::ButterworthSectionQ(6, 2), fs);

        MicSettings mic2 = s.mic2;
        MicSettings mic1 = s.mic;
        mic1.position = std::clamp(mic1.position + spread * kSpreadMicPosition, 0.0, 1.0);
        mic2.position = std::clamp(mic2.position + spread * kSpreadMicPosition, 0.0, 1.0);
        channel.mics[0] = BuildMic(mic1, cab);
        channel.mics[1] = BuildMic(mic2, cab);
        return channel;
    }

    [[nodiscard]] MicDesign BuildMic(const MicSettings& mic, const CabinetVoicing& cab) const noexcept
    {
        const MicTypeVoicing& type = kMicTypes[static_cast<int>(mic.type)];
        const double towardEdge = std::max(0.0, mic.position - 0.5) * 2.0;
        const double towardCap = std::max(0.0, 0.5 - mic.position) * 2.0;
        const double distanceM = MicDistanceM(mic);
        // A directional mic's bass boost is strongest touching the grille and gone by about
        // half a metre.
        const double proximityLoss = 1.0 - std::exp(-distanceM / kProximityFalloffM);

        const double proximityDb =
            type.proximityDb + towardEdge * kEdgeProximityDb - proximityLoss * kProximityLossDb * type.proximityScale;
        const double presenceDb = type.presenceDb + towardCap * kCapPresenceDb + towardEdge * kEdgePresenceDb;
        const double airDb = type.airDb + towardCap * kCapAirDb + towardEdge * kEdgeAirDb;

        const double fs = mSampleRate;
        MicDesign design;
        design.sections[kMicSectionProximity] =
            biquad::LowShelf(kMicProximityHz, biquad::kButterworthQ, proximityDb, fs);
        design.sections[kMicSectionPresence] = biquad::Peaking(kMicPresenceHz, kMicPresenceQ, presenceDb, fs);
        design.sections[kMicSectionAir] = biquad::HighShelf(kMicAirHz, biquad::kButterworthQ, airDb, fs);

        // The floor bounce: the image of the speaker in the floor is 2h lower, so its path is
        // longer by sqrt(d^2 + 4h^2) - d and it arrives quieter by the ratio of the two paths.
        // Touching the grille it is silent; at a metre it is a clear comb.
        const double reflectedPathM = std::hypot(distanceM, 2.0 * cab.speakerHeightM);
        design.reflectionDelaySamples = (reflectedPathM - distanceM) / kSpeedOfSoundMs * fs;
        design.reflectionGain = kFloorReflectance * distanceM / reflectedPathM;
        design.reflectionPole = std::exp(-2.0 * biquad::kPi * kReflectionCornerHz / fs);
        return design;
    }

    [[nodiscard]] static double MicDistanceM(const MicSettings& mic) noexcept
    {
        return kMaxMicDistanceM * mic.distance * mic.distance;
    }

    [[nodiscard]] double DirectDelaySamples(const MicSettings& mic) const noexcept
    {
        return MicDistanceM(mic) / kSpeedOfSoundMs * mSampleRate;
    }

    /// The wet path of one channel at angular frequency `omega` (radians per sample),
    /// before the level trim.
    [[nodiscard]] static std::complex<double> WetResponse(const Design& design, int channel, double omega) noexcept
    {
        const ChannelDesign& ch = design.channels[channel];
        const std::complex<double> z1 = std::polar(1.0, -omega);
        std::complex<double> cabinet = 1.0;

        for (const auto& section : ch.cabinet)
        {
            cabinet *= biquad::Response(section, z1);
        }

        std::complex<double> mics = 0.0;

        for (int m = 0; m < kMicCount; ++m)
        {
            const MicDesign& mic = ch.mics[m];

            if (mic.gain == 0.0)
            {
                continue;
            }

            std::complex<double> taps = std::polar(1.0, -omega * mic.directDelaySamples);

            if (mic.reflectionGain > 0.0)
            {
                const std::complex<double> absorption = (1.0 - mic.reflectionPole) / (1.0 - mic.reflectionPole * z1);
                taps += mic.reflectionGain * absorption * std::polar(1.0, -omega * mic.reflectionDelaySamples);
            }

            for (const auto& section : mic.sections)
            {
                taps *= biquad::Response(section, z1);
            }

            mics += mic.gain * taps;
        }

        return cabinet * mics;
    }

    [[nodiscard]] double WetLoudnessDb(const Design& design) const noexcept
    {
        double power = 0.0;

        for (const double omega : mProbeOmega)
        {
            const double left = std::norm(WetResponse(design, 0, omega));
            power += design.stereo ? 0.5 * (left + std::norm(WetResponse(design, 1, omega))) : left;
        }

        return 10.0 * std::log10(std::max(power / kLoudnessProbeCount, 1e-20));
    }

    double mSampleRate = 48000.0;
    double mReferenceLoudnessDb = 0.0;
    std::array<double, kLoudnessProbeCount> mProbeOmega = {};
    std::array<double, static_cast<int>(Cabinet::Count)> mCabinetTrimDb = {};
};
} // namespace guitarfx::simple_cab
