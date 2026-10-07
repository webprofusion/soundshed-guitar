#include "dsp/BiquadDesign.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/ImpulseResponseAnalysis.h"
#include "dsp/effects/SimpleCabEffect.h"
#include "dsp/effects/SimpleCabMatch.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace
{
using guitarfx::SimpleCabEffect;
namespace cab = guitarfx::simple_cab;

constexpr double kSampleRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

using Settings = std::vector<std::pair<std::string, double>>;

int gFailures = 0;

void Check(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << what << '\n';
        ++gFailures;
    }
}

bool CheckRange(const std::string& label, double value, double minimum, double maximum)
{
    const bool ok = guitarfx::IsFinite(value) && value >= minimum && value <= maximum;
    Check(ok, label + ": expected " + std::to_string(minimum) + ".." + std::to_string(maximum) + ", got " +
                  std::to_string(value));
    return ok;
}

void Apply(SimpleCabEffect& effect, const Settings& settings)
{
    for (const auto& [key, value] : settings)
    {
        effect.SetParam(key, value);
    }
}

/// Runs `left` (and `right`, or `left` again) through a fresh, prepared cab.
std::pair<std::vector<float>, std::vector<float>> Run(const Settings& settings, const std::vector<float>& left,
                                                      const std::vector<float>* right = nullptr,
                                                      double sampleRate = kSampleRate)
{
    SimpleCabEffect effect;
    Apply(effect, settings);
    effect.Prepare(sampleRate, 512);
    std::vector<float> inL = left, inR = right ? *right : left;
    std::vector<float> outL(left.size()), outR(left.size());
    float* inputs[2] = {inL.data(), inR.data()};
    float* outputs[2] = {outL.data(), outR.data()};
    effect.Process(inputs, outputs, static_cast<int>(left.size()));
    return {outL, outR};
}

std::vector<float> Sine(double frequency, double amplitude, int count)
{
    std::vector<float> samples(static_cast<std::size_t>(count));

    for (int i = 0; i < count; ++i)
    {
        samples[static_cast<std::size_t>(i)] =
            static_cast<float>(amplitude * std::sin(2.0 * kPi * frequency * i / kSampleRate));
    }

    return samples;
}

/// Pink-ish noise (Paul Kellet's economy filter): equal power per octave, close to a
/// guitar's long-term spectrum, and what the cab's loudness model assumes.
std::vector<float> PinkNoise(int count, double amplitude = 0.1)
{
    std::vector<float> samples(static_cast<std::size_t>(count));
    std::uint32_t state = 0x12345678u;
    double b0 = 0.0, b1 = 0.0, b2 = 0.0;

    for (auto& sample : samples)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        const double white = static_cast<double>(state) / 2147483648.0 - 1.0;
        b0 = 0.99765 * b0 + white * 0.0990460;
        b1 = 0.96300 * b1 + white * 0.2965164;
        b2 = 0.57000 * b2 + white * 1.0526913;
        sample = static_cast<float>(amplitude * (b0 + b1 + b2 + white * 0.1848) / 3.0);
    }

    return samples;
}

double PowerDb(const std::vector<float>& samples, std::size_t skip)
{
    double power = 0.0;

    for (std::size_t i = skip; i < samples.size(); ++i)
    {
        power += static_cast<double>(samples[i]) * samples[i];
    }

    return 10.0 * std::log10(std::max(power / static_cast<double>(samples.size() - skip), 1e-30));
}

/// Measured steady-state gain at `frequency`, both channels power-averaged.
double MeasuredGainDb(const Settings& settings, double frequency)
{
    constexpr int kCount = 24000;
    constexpr std::size_t kSkip = 4800;
    const auto input = Sine(frequency, 0.05, kCount);
    const auto [left, right] = Run(settings, input);
    const double out =
        0.5 * (std::pow(10.0, PowerDb(left, kSkip) / 10.0) + std::pow(10.0, PowerDb(right, kSkip) / 10.0));
    return 10.0 * std::log10(out) - PowerDb(input, kSkip);
}

double ModelGainDb(const Settings& settings, double frequency)
{
    SimpleCabEffect effect;
    Apply(effect, settings);
    effect.Prepare(kSampleRate, 512);
    const std::array<double, 1> frequencies = {frequency};
    std::array<double, 1> magnitudes = {};
    Check(effect.GetFrequencyResponse(frequencies, magnitudes), "GetFrequencyResponse answers");
    return magnitudes[0];
}

// ---------------------------------------------------------------------------------------

void TestCabinetVoicing()
{
    const auto gain = [](double hz, double brightness = 0.5) {
        return MeasuredGainDb({{"brightness", brightness}}, hz);
    };
    const double oneKhz = gain(1000.0);
    // The included ENGL 4x12 and Marshall 1960 IRs put 100 Hz about 7-8 dB above 1 kHz and
    // 8 kHz about 22-28 dB below it. Broad bounds leave room for a simple, general voicing
    // while guarding against accidental retuning.
    CheckRange("100 Hz relative to 1 kHz", gain(100.0) - oneKhz, 5.0, 9.0);
    CheckRange("5 kHz relative to 1 kHz", gain(5000.0) - oneKhz, -6.0, 2.0);
    CheckRange("8 kHz relative to 1 kHz", gain(8000.0) - oneKhz, -30.0, -20.0);
    CheckRange("brightness range at 8 kHz", gain(8000.0, 1.0) - gain(8000.0, 0.0), 10.0, 30.0);
}

/// The defaults must still be the original voicing: presets saved before the new controls
/// existed carry only bass, presence, brightness and mix. The reference is the original
/// chain, written out here from its old coefficient formulas.
void TestOriginalVoicingUnchanged()
{
    struct Reference
    {
        std::array<guitarfx::BiquadCoefficients, 6> sections;
        std::array<guitarfx::biquad::State, 6> state;
    };

    for (const auto& [bass, presence, brightness, mix] :
         std::vector<std::array<double, 4>>{{0.5, 0.5, 0.5, 1.0}, {0.0, 1.0, 0.2, 1.0}, {1.0, 0.0, 1.0, 0.6}})
    {
        namespace bq = guitarfx::biquad;
        Reference reference;
        const double lowPass = 4200.0 + brightness * 2100.0;
        reference.sections = {bq::HighPass(90.0 - bass * 50.0, 0.707, kSampleRate),
                              bq::Peaking(140.0, 0.6, 6.0 + bass * 8.0, kSampleRate),
                              bq::Peaking(2000.0 + presence * 1500.0, 1.5, -1.0 + presence * 9.0, kSampleRate),
                              bq::LowPass(lowPass, 0.5176380902, kSampleRate),
                              bq::LowPass(lowPass, 0.7071067812, kSampleRate),
                              bq::LowPass(lowPass, 1.9318516526, kSampleRate)};

        const auto input = PinkNoise(8192, 0.5);
        const auto [left, right] =
            Run({{"bass", bass}, {"presence", presence}, {"brightness", brightness}, {"mix", mix}}, input);
        double worst = 0.0;

        for (std::size_t i = 0; i < input.size(); ++i)
        {
            double wet = input[i];

            for (int s = 0; s < 6; ++s)
            {
                wet = reference.state[s].Process(reference.sections[s], wet);
            }

            const double expected = input[i] * (1.0 - mix) + wet * mix;
            worst = std::max(worst, std::abs(expected - left[i]));
        }

        Check(worst < 1e-5, "original voicing reproduced (worst difference " + std::to_string(worst) + ")");
    }
}

void TestLiveBrightnessTransition()
{
    SimpleCabEffect changing, reference;
    changing.SetParam("brightness", 0.0);
    reference.SetParam("brightness", 0.0);
    changing.Prepare(kSampleRate, 512);
    reference.Prepare(kSampleRate, 512);

    constexpr int sampleCount = 8192;
    constexpr int changeAt = 4096;
    const auto input = Sine(6000.0, 0.5, sampleCount);
    std::vector<float> changed(sampleCount), unchanged(sampleCount);

    auto process = [&](SimpleCabEffect& cabinet, std::vector<float>& output, int start, int count) {
        float* inputs[2] = {const_cast<float*>(input.data()) + start, nullptr};
        float* outputs[2] = {output.data() + start, nullptr};
        cabinet.Process(inputs, outputs, count);
    };
    process(changing, changed, 0, changeAt);
    process(reference, unchanged, 0, changeAt);
    changing.SetParam("brightness", 1.0);
    process(changing, changed, changeAt, sampleCount - changeAt);
    process(reference, unchanged, changeAt, sampleCount - changeAt);

    double immediateDifference = 0.0;

    for (int i = changeAt; i < changeAt + 8; ++i)
    {
        immediateDifference = std::max(immediateDifference, static_cast<double>(std::abs(changed[i] - unchanged[i])));
    }

    const std::vector<float> changedTail(changed.end() - 2048, changed.end());
    const std::vector<float> unchangedTail(unchanged.end() - 2048, unchanged.end());
    Check(immediateDifference < 0.05, "brightness change glides rather than jumps");
    Check(PowerDb(changedTail, 0) - PowerDb(unchangedTail, 0) > 6.0, "brightness change takes effect");
}

/// Every control, at random, every block, at a low and a high sample rate: nothing may
/// blow up however hard it is automated.
void TestRapidAutomationStability()
{
    for (const double sampleRate : {8000.0, 48000.0, 192000.0})
    {
        SimpleCabEffect effect;
        effect.Prepare(sampleRate, 64);
        std::array<float, 64> left = {}, right = {}, outL = {}, outR = {};
        std::uint32_t noise = 0x9e3779b9u;
        const auto next = [&noise]() {
            noise ^= noise << 13;
            noise ^= noise >> 17;
            noise ^= noise << 5;
            return static_cast<double>(noise) / 4294967296.0;
        };
        float* inputs[2] = {left.data(), right.data()};
        float* outputs[2] = {outL.data(), outR.data()};
        bool finite = true;

        for (int block = 0; block < 400 && finite; ++block)
        {
            for (const auto& spec : cab::kParams)
            {
                effect.SetParam(spec.id, spec.minValue + next() * (spec.maxValue - spec.minValue));
            }

            for (std::size_t i = 0; i < left.size(); ++i)
            {
                left[i] = static_cast<float>(next() * 2.0 - 1.0);
                right[i] = static_cast<float>(next() * 2.0 - 1.0);
            }

            effect.Process(inputs, outputs, static_cast<int>(left.size()));

            for (std::size_t i = 0; i < left.size(); ++i)
            {
                finite = finite && guitarfx::IsFinite(outL[i]) && guitarfx::IsFinite(outR[i]) &&
                         std::abs(outL[i]) < 1000.0f && std::abs(outR[i]) < 1000.0f;
            }
        }

        Check(finite, "bounded output under random automation at " + std::to_string(sampleRate) + " Hz");
    }
}

/// The model the UI draws, auto level measures and IR matching searches must be what the
/// audio path does.
void TestModelMatchesProcessing()
{
    const std::vector<Settings> cases = {
        {},
        {{"cabinet", 1}, {"micType", 1}, {"micPosition", 0.1}},
        {{"cabinet", 4}, {"micType", 2}, {"micPosition", 0.9}, {"mids", 0.1}, {"size", 0.9}},
        {{"cabinet", 2}, {"micDistance", 0.7}},
        {{"mic2Blend", 0.4}, {"mic2Distance", 0.5}, {"mic2Type", 2}},
        {{"cabinet", 3}, {"spread", 1.0}, {"autoLevel", 1}, {"outputGain", -6.0}},
        {{"mix", 0.5}},
    };

    for (std::size_t c = 0; c < cases.size(); ++c)
    {
        for (const double hz : {150.0, 700.0, 2500.0})
        {
            const double measured = MeasuredGainDb(cases[c], hz);
            const double model = ModelGainDb(cases[c], hz);
            CheckRange("case " + std::to_string(c) + " at " + std::to_string(static_cast<int>(hz)) +
                           " Hz, model minus measured",
                       model - measured, -0.75, 0.75);
        }
    }
}

double PinkLoudnessDb(const Settings& settings)
{
    const auto input = PinkNoise(96000);
    const auto [left, right] = Run(settings, input);
    return 0.5 * (PowerDb(left, 9600) + PowerDb(right, 9600)) - PowerDb(input, 9600);
}

void TestCabinetsAreLevelMatched()
{
    const double reference = PinkLoudnessDb({});

    for (int cabinet = 1; cabinet < static_cast<int>(cab::Cabinet::Count); ++cabinet)
    {
        CheckRange(std::string("cabinet ") + cab::kCabinetLabels[cabinet] + " loudness against the 4x12",
                   PinkLoudnessDb({{"cabinet", cabinet}}) - reference, -1.5, 1.5);
    }
}

void TestAutoLevel()
{
    const double quiet = PinkLoudnessDb({{"bass", 0.0}});
    const double loud = PinkLoudnessDb({{"bass", 1.0}});
    CheckRange("Bass moves the level with auto level off", loud - quiet, 3.0, 20.0);

    const double reference = PinkLoudnessDb({});

    for (const Settings& settings : std::vector<Settings>{{{"bass", 0.0}},
                                                          {{"bass", 1.0}},
                                                          {{"cabinet", 1}, {"micType", 1}, {"micPosition", 1.0}},
                                                          {{"brightness", 1.0}, {"presence", 1.0}, {"mids", 1.0}}})
    {
        Settings levelled = settings;
        levelled.emplace_back("autoLevel", 1.0);
        CheckRange("auto level holds loudness to the default voicing", PinkLoudnessDb(levelled) - reference, -1.25,
                   1.25);
    }
}

void TestMicDistanceAddsFloorComb()
{
    // How far the response at `distance` swings either way of the close mic's, 500 Hz to
    // 2.5 kHz: above the proximity shelf, so what is left is the floor bounce's comb.
    const auto combRangeDb = [](double distance) {
        std::vector<double> frequencies;

        for (int i = 0; i <= 200; ++i)
        {
            frequencies.push_back(500.0 + i * 10.0);
        }

        const auto response = [&frequencies](double micDistance) {
            SimpleCabEffect effect;
            effect.SetParam("micDistance", micDistance);
            effect.Prepare(kSampleRate, 512);
            std::vector<double> magnitudes(frequencies.size());
            Check(effect.GetFrequencyResponse(frequencies, magnitudes), "GetFrequencyResponse answers");
            return magnitudes;
        };
        const auto close = response(0.0);
        const auto far = response(distance);
        double lowest = 0.0, highest = 0.0;

        for (std::size_t i = 0; i < close.size(); ++i)
        {
            lowest = std::min(lowest, far[i] - close[i]);
            highest = std::max(highest, far[i] - close[i]);
        }

        return highest - lowest;
    };
    CheckRange("a mic almost touching the grille hears no bounce", combRangeDb(0.05), 0.0, 0.5);
    CheckRange("a mic a metre back hears the floor bounce as a comb", combRangeDb(1.0), 6.0, 20.0);
    CheckRange("backing off loses the close-up bass",
               ModelGainDb({{"micDistance", 0.5}}, 150.0) - ModelGainDb({}, 150.0), -7.0, -1.5);
}

void TestSecondMicBlendAndPolarity()
{
    const auto input = PinkNoise(24000, 0.3);
    // Two identical mics, the second inverted, blended equally: they cancel.
    const Settings sameMic = {{"mic2Type", 0}, {"mic2Position", 0.5}, {"mic2Distance", 0.0}, {"mic2Blend", 0.5}};
    Settings inverted = sameMic;
    inverted.emplace_back("mic2Polarity", 1.0);
    const auto [cancelled, unusedRight] = Run(inverted, input);
    CheckRange("identical mics in opposite polarity cancel", PowerDb(cancelled, 2400) - PowerDb(input, 2400), -1000.0,
               -60.0);

    const auto [single, unusedA] = Run({}, input);
    const auto [blended, unusedB] = Run(sameMic, input);
    double worst = 0.0;

    for (std::size_t i = 2400; i < input.size(); ++i)
    {
        worst = std::max(worst, static_cast<double>(std::abs(single[i] - blended[i])));
    }

    Check(worst < 1e-5, "blending in an identical mic changes nothing");
}

void TestStereoSpread()
{
    const auto input = PinkNoise(9600, 0.3);
    SimpleCabEffect effect;
    effect.Prepare(kSampleRate, 512);
    // Spread can turn a mono input stereo, so the type declares it can widen and a graph keeps
    // what follows a cab in stereo; raising Spread is then heard at once.
    Check(effect.CanWiden(), "declared able to widen, since Spread does");

    const auto [monoL, monoR] = Run({}, input);
    Check(monoL == monoR, "without spread both sides are identical");
    const auto [wideL, wideR] = Run({{"spread", 1.0}}, input);
    std::vector<float> side(wideL.size());

    for (std::size_t i = 0; i < side.size(); ++i)
    {
        side[i] = wideL[i] - wideR[i];
    }

    CheckRange("spread's side signal against the left", PowerDb(side, 960) - PowerDb(wideL, 960), -30.0, -3.0);
}

void TestSpeakerDrive()
{
    const auto driven = [](double amplitude, double drive) {
        const auto input = Sine(100.0, amplitude, 48000);
        const auto [left, unused] = Run({{"speakerDrive", drive}}, input);
        return std::make_pair(PowerDb(left, 24000) - PowerDb(input, 24000), left);
    };

    CheckRange("drive is transparent at low level", driven(0.003, 1.0).first - driven(0.003, 0.0).first, -0.5, 0.5);
    CheckRange("drive compresses a loud low note", driven(0.5, 1.0).first - driven(0.5, 0.0).first, -12.0, -1.5);

    // Third harmonic of the loud note, relative to the fundamental.
    const auto harmonicDb = [](const std::vector<float>& samples) {
        const auto magnitude = [&samples](double hz) {
            double re = 0.0, im = 0.0;

            for (std::size_t i = 24000; i < samples.size(); ++i)
            {
                re += samples[i] * std::cos(2.0 * kPi * hz * i / kSampleRate);
                im += samples[i] * std::sin(2.0 * kPi * hz * i / kSampleRate);
            }

            return std::hypot(re, im);
        };
        return 20.0 * std::log10(magnitude(300.0) / magnitude(100.0));
    };
    CheckRange("undriven speaker adds no harmonics", harmonicDb(driven(0.5, 0.0).second), -300.0, -80.0);
    CheckRange("driven speaker grows a third harmonic", harmonicDb(driven(0.5, 1.0).second), -45.0, 0.0);
}

/// An impulse through the cab, analysed and matched back, should land on a voicing whose
/// shape is the original's: that proves the analysis, the search and the model agree.
void TestMatchRecoversItsOwnImpulse()
{
    const Settings original = {{"cabinet", 1}, {"micType", 1}, {"bass", 0.7}, {"presence", 0.3}, {"brightness", 0.8}};
    std::vector<float> impulse(4096, 0.0f);
    impulse[0] = 1.0f;
    const auto [ir, unused] = Run(original, impulse);

    const auto frequencies = cab::MatchFrequencies();
    const auto target = guitarfx::SmoothedMagnitudeDb(ir, kSampleRate, frequencies);
    Check(target.size() == frequencies.size(), "analysis returns one value per frequency");

    const cab::Voicer voicer(kSampleRate);
    const cab::MatchResult match = cab::MatchResponse(target, voicer);
    CheckRange("match error in dB", match.rmsErrorDb, 0.0, 1.0);
}

/// One NaN and one infinity in the input, with every stateful stage in use: the output must
/// stay finite and the cab must keep playing. Before the guard, one bad sample silenced it
/// until the node was rebuilt.
void TestRecoversFromNonFiniteInput()
{
    SimpleCabEffect effect;
    Apply(effect, {{"speakerDrive", 0.8},
                   {"micDistance", 0.6},
                   {"mic2Blend", 0.4},
                   {"mic2Distance", 0.3},
                   {"spread", 0.5},
                   {"mix", 0.8}});
    effect.Prepare(kSampleRate, 256);
    auto left = Sine(220.0, 0.25, 48000);
    auto right = left;
    // strtof, not a constant: the fast-math builds may fold a NaN constant away.
    left[1000] = right[1000] = std::strtof("nan", nullptr);
    left[1001] = right[1001] = std::strtof("inf", nullptr);
    std::vector<float> outL(left.size()), outR(left.size());

    for (std::size_t start = 0; start < left.size(); start += 256)
    {
        float* inputs[2] = {left.data() + start, right.data() + start};
        float* outputs[2] = {outL.data() + start, outR.data() + start};
        effect.Process(inputs, outputs, static_cast<int>(std::min<std::size_t>(256, left.size() - start)));
    }

    bool finite = true;

    for (std::size_t i = 0; i < outL.size(); ++i)
    {
        finite = finite && guitarfx::IsFinite(outL[i]) && guitarfx::IsFinite(outR[i]);
    }

    const std::vector<float> tail(outL.end() - 9600, outL.end());
    Check(finite, "a non-finite input sample never reaches the output");
    CheckRange("the cab still plays after a non-finite sample", PowerDb(tail, 0), -40.0, 20.0);
}

void TestParameterHandling()
{
    SimpleCabEffect effect;
    effect.SetParam("cabinet", 1.4);
    Check(effect.GetParam("cabinet") == 1.0, "an enum snaps to a choice");
    effect.SetParam("outputGain", 99.0);
    Check(effect.GetParam("outputGain") == 24.0, "a value clamps to its range");
    // From strtod rather than a constant: under -ffast-math clang may fold a NaN constant
    // away before it reaches the effect (see FastMathNanTests).
    effect.SetParam("bass", std::strtod("nan", nullptr));
    effect.SetParam("presence", std::strtod("inf", nullptr));
    Check(effect.GetParam("bass") == 0.5 && effect.GetParam("presence") == 0.5, "a non-finite value is ignored");
    effect.SetParam("noSuchParameter", 1.0);
    Check(effect.GetParam("noSuchParameter") == 0.0, "an unknown key is ignored");
}

void TestFactoryPresets()
{
    guitarfx::RegisterSimpleCabEffect();
    const auto info = guitarfx::EffectRegistry::Instance().GetTypeInfo(guitarfx::EffectGuids::kCabSimple);
    Check(info.has_value(), "the simple cab registers");

    if (!info)
    {
        return;
    }

    Check(info->parameters.size() == static_cast<std::size_t>(cab::kParamCount), "every parameter is declared");
    int defaults = 0;

    for (const auto& preset : info->presets)
    {
        defaults += preset.isDefault ? 1 : 0;
        Check(!preset.parameters.contains("outputGain"), preset.id + " leaves Output alone");

        for (const auto& [key, value] : preset.parameters)
        {
            const std::size_t index = guitarfx::FindParamSpec(cab::kParams, key);
            Check(index != cab::kParamCount && guitarfx::NormaliseParamValue(cab::kParams[index], value) == value,
                  preset.id + " sets " + key + " to a value the parameter can hold");

            if (preset.isDefault && index != cab::kParamCount)
            {
                Check(cab::kParams[index].defaultValue == value, "the default preset is the parameter defaults");
            }
        }
    }

    Check(defaults == 1 && info->presets.front().isDefault, "the first preset, and only it, starts new nodes");
}

/// No two factory presets sound alike: every pair's response shapes differ by at least 1.3 dB
/// RMS across 60 Hz to 10 kHz, level removed. A pair that differs in Speaker Drive or Stereo Spread
/// is told apart by that, which a power-averaged response does not show.
void TestFactoryPresetsAreDistinct()
{
    guitarfx::RegisterSimpleCabEffect();
    const auto info = guitarfx::EffectRegistry::Instance().GetTypeInfo(guitarfx::EffectGuids::kCabSimple);

    if (!info)
    {
        return;
    }

    constexpr double kMinimumDifferenceDb = 1.3;
    const cab::Voicer voicer(kSampleRate);
    std::vector<double> frequencies;

    for (double hz = 60.0; hz <= 10000.0; hz *= std::exp2(1.0 / 12.0))
    {
        frequencies.push_back(hz);
    }

    struct Shape
    {
        std::string id;
        double speakerDrive = 0.0;
        double spread = 0.0;
        std::vector<double> db;
    };

    std::vector<Shape> shapes;

    for (const auto& preset : info->presets)
    {
        cab::ParamValues values = cab::kDefaultValues;

        for (const auto& [key, value] : preset.parameters)
        {
            const std::size_t index = guitarfx::FindParamSpec(cab::kParams, key);

            if (index != cab::kParamCount)
            {
                values[index] = value;
            }
        }

        const cab::Design design = voicer.Build(cab::ToSettings(values));
        Shape shape{preset.id, values[cab::kSpeakerDrive], values[cab::kSpread], {}};
        double mean = 0.0;

        for (const double hz : frequencies)
        {
            shape.db.push_back(voicer.MagnitudeDb(design, hz));
            mean += shape.db.back() / static_cast<double>(frequencies.size());
        }

        for (double& db : shape.db)
        {
            db -= mean;
        }

        shapes.push_back(std::move(shape));
    }

    for (std::size_t a = 0; a < shapes.size(); ++a)
    {
        for (std::size_t b = a + 1; b < shapes.size(); ++b)
        {
            if (shapes[a].speakerDrive != shapes[b].speakerDrive || shapes[a].spread != shapes[b].spread)
            {
                continue;
            }

            double sumSquares = 0.0;

            for (std::size_t i = 0; i < frequencies.size(); ++i)
            {
                const double difference = shapes[a].db[i] - shapes[b].db[i];
                sumSquares += difference * difference;
            }

            const double rmsDb = std::sqrt(sumSquares / static_cast<double>(frequencies.size()));
            Check(rmsDb >= kMinimumDifferenceDb,
                  shapes[a].id + " and " + shapes[b].id + " sound alike (" + std::to_string(rmsDb) + " dB apart)");
        }
    }
}
} // namespace

int main()
{
    TestCabinetVoicing();
    TestOriginalVoicingUnchanged();
    TestLiveBrightnessTransition();
    TestRapidAutomationStability();
    TestModelMatchesProcessing();
    TestCabinetsAreLevelMatched();
    TestAutoLevel();
    TestMicDistanceAddsFloorComb();
    TestSecondMicBlendAndPolarity();
    TestStereoSpread();
    TestSpeakerDrive();
    TestMatchRecoversItsOwnImpulse();
    TestRecoversFromNonFiniteInput();
    TestParameterHandling();
    TestFactoryPresets();
    TestFactoryPresetsAreDistinct();

    if (gFailures > 0)
    {
        std::cerr << gFailures << " check(s) failed\n";
        return 1;
    }

    std::cout << "SimpleCabEffectTests passed\n";
    return 0;
}
