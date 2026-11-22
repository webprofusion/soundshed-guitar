/**
 * @file MultibandCompressorEffectTests.cpp
 * @brief The Multi-band Compressor: the four-band split, the per-band gain curve and the band
 *        meters its node panel draws.
 *
 *   - below every threshold the bands add back to flat, at the default crossovers and moved ones
 *   - a loud tone in one band settles to the static curve; a quiet tone in another keeps its level
 *   - crossovers set out of order are kept apart, and under Nyquist
 *   - Mix 0 is the uncompressed all-pass; Makeup and Output move the level by what they say
 *   - Knee: 0 is a hard knee, a wide one starts compressing below the threshold
 *   - a band turned off passes untouched and meters nothing; Solo leaves only the soloed bands,
 *     and switching either glides rather than clicks
 *   - the band meters report the crossovers in use, the gain reduction held since the last read
 *     and each band's level
 *   - the mono path matches the stereo one; dual mono keeps one side's level from the other
 *   - a non-finite sample does not latch
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "dsp/effects/BuiltinEffects.h"
#include "dsp/effects/MultibandCompressorEffect.h"

namespace
{
using namespace guitarfx;

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 128;
constexpr double kPi = 3.14159265358979323846;
constexpr int kBands = MultibandCompressorEffect::kBands;

int gFailures = 0;
int gChecks = 0;

void Check(bool condition, const std::string& what, const std::string& detail = "")
{
    ++gChecks;
    gFailures += condition ? 0 : 1;
    std::cout << (condition ? "  [PASS] " : "  [FAIL] ") << what;

    if (!detail.empty())
    {
        std::cout << "  (" << detail << ")";
    }

    std::cout << std::endl;
}

std::string Num(double value, int precision = 1)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, value);
    return buffer;
}

double Amplitude(double db)
{
    return std::pow(10.0, db / 20.0);
}

std::vector<float> Tone(double peakDb, double seconds, double frequency)
{
    std::vector<float> signal(static_cast<std::size_t>(kSampleRate * seconds));

    for (std::size_t i = 0; i < signal.size(); ++i)
    {
        signal[i] = static_cast<float>(Amplitude(peakDb) *
                                       std::sin(2.0 * kPi * frequency * static_cast<double>(i) / kSampleRate));
    }

    return signal;
}

std::vector<float> Mix(std::vector<float> a, const std::vector<float>& b)
{
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i)
    {
        a[i] += b[i];
    }

    return a;
}

/// RMS in dB of `signal` from `from` (seconds) to the end.
double RmsDb(const std::vector<float>& signal, double from = 0.0)
{
    const auto start = static_cast<std::size_t>(from * kSampleRate);
    double sum = 0.0;

    for (std::size_t i = start; i < signal.size(); ++i)
    {
        sum += static_cast<double>(signal[i]) * signal[i];
    }

    const double mean = sum / static_cast<double>(std::max<std::size_t>(1, signal.size() - start));
    return 10.0 * std::log10(std::max(mean, 1.0e-30));
}

/// The peak level in dB of the `hz` component of `signal` from `from` (seconds) to the end, by
/// correlation over a whole number of its cycles.
double ToneDb(const std::vector<float>& signal, double hz, double from)
{
    const auto start = static_cast<std::size_t>(from * kSampleRate);
    const auto cycles = static_cast<std::size_t>(std::floor((signal.size() - start) * hz / kSampleRate));
    const auto count = static_cast<std::size_t>(std::round(cycles * kSampleRate / hz));
    double re = 0.0;
    double im = 0.0;

    for (std::size_t i = 0; i < count; ++i)
    {
        const double phase = 2.0 * kPi * hz * static_cast<double>(start + i) / kSampleRate;
        re += signal[start + i] * std::cos(phase);
        im += signal[start + i] * std::sin(phase);
    }

    const double amplitude = 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(std::max<std::size_t>(count, 1));
    return 20.0 * std::log10(std::max(amplitude, 1.0e-15));
}

/// Every band's threshold and ratio at once.
void SetBands(MultibandCompressorEffect& compressor, double thresholdDb, double ratio)
{
    for (int band = 1; band <= kBands; ++band)
    {
        compressor.SetParam("threshold" + std::to_string(band), thresholdDb);
        compressor.SetParam("ratio" + std::to_string(band), ratio);
    }
}

std::unique_ptr<MultibandCompressorEffect> MakeCompressor(double thresholdDb, double ratio,
                                                          double sampleRate = kSampleRate)
{
    auto compressor = std::make_unique<MultibandCompressorEffect>();
    compressor->Prepare(sampleRate, kBlockSize);
    SetBands(*compressor, thresholdDb, ratio);
    return compressor;
}

/// Block by block, `input` on the left and `right` (or `input`) on the right. Returns the left
/// output, and the right in `rightOut` when asked.
std::vector<float> Run(MultibandCompressorEffect& compressor, const std::vector<float>& input, bool mono = false,
                       const std::vector<float>* right = nullptr, std::vector<float>* rightOut = nullptr)
{
    std::vector<float> out(input.size(), 0.0f);
    std::vector<float> inL(kBlockSize);
    std::vector<float> inR(kBlockSize);
    std::vector<float> outL(kBlockSize);
    std::vector<float> outR(kBlockSize);

    if (rightOut)
    {
        rightOut->assign(input.size(), 0.0f);
    }

    for (std::size_t offset = 0; offset < input.size(); offset += kBlockSize)
    {
        const auto count = static_cast<int>(std::min<std::size_t>(kBlockSize, input.size() - offset));
        const auto at = static_cast<std::ptrdiff_t>(offset);
        std::copy_n(input.begin() + at, count, inL.begin());
        std::copy_n((right ? *right : input).begin() + at, count, inR.begin());

        if (mono)
        {
            compressor.ProcessMono(inL.data(), outL.data(), count);
        }
        else
        {
            float* inputs[2] = {inL.data(), inR.data()};
            float* outputs[2] = {outL.data(), outR.data()};
            compressor.Process(inputs, outputs, count);
        }

        std::copy_n(outL.begin(), count, out.begin() + at);

        if (rightOut)
        {
            std::copy_n(outR.begin(), count, rightOut->begin() + at);
        }
    }

    return out;
}

/// The worst level change over tones across the range, through `compressor` as `setup` leaves it.
template <typename Setup> double WorstFlatness(Setup setup, std::string& where)
{
    double worst = 0.0;

    for (const double hz : {40.0, 100.0, 200.0, 350.0, 800.0, 1500.0, 3000.0, 5000.0, 8000.0, 12000.0, 16000.0})
    {
        auto compressor = MakeCompressor(0.0, 4.0);
        setup(*compressor);
        const auto tone = Tone(-30.0, 0.5, hz);
        const double change = RmsDb(Run(*compressor, tone), 0.25) - RmsDb(tone, 0.25);

        if (std::abs(change) > std::abs(worst))
        {
            worst = change;
            where = Num(hz, 0) + " Hz";
        }
    }

    return worst;
}

void TestBelowThresholdIsFlat()
{
    std::cout << "\n-- below every threshold, the bands add back to flat --" << std::endl;

    std::string where;
    double worst = WorstFlatness([](MultibandCompressorEffect&) {}, where);
    Check(std::abs(worst) < 0.1, "default crossovers: 40 Hz to 16 kHz keep their level",
          "worst " + Num(worst, 3) + " dB at " + where);

    worst = WorstFlatness(
        [](MultibandCompressorEffect& compressor) {
            compressor.SetParam("crossover1", 90.0);
            compressor.SetParam("crossover2", 2000.0);
            compressor.SetParam("crossover3", 9000.0);
        },
        where);
    Check(std::abs(worst) < 0.1, "moved crossovers: still flat", "worst " + Num(worst, 3) + " dB at " + where);
}

void TestStaticCurveAndIsolation()
{
    std::cout << "\n-- a loud low tone is compressed; a quiet high one is not --" << std::endl;

    // 60 Hz sits two octaves under the default 200 Hz crossover, so it is all in the low band.
    // Slow times, so the detector's ripple on a low note averages out of the gain reduction.
    auto compressor = MakeCompressor(-30.0, 4.0);
    compressor->SetParam("attack", 50.0);
    compressor->SetParam("release", 500.0);

    const auto low = Tone(-6.0, 3.0, 60.0);
    const auto high = Tone(-40.0, 3.0, 5000.0);
    const auto out = Run(*compressor, Mix(low, high));

    // The band's mean square is the sine's: 3 dB under its peak. 21 dB over at 4:1 takes 15.75.
    const double expected = (-9.0 + 30.0) * (1.0 - 1.0 / 4.0);
    const double lowChange = ToneDb(out, 60.0, 2.0) - ToneDb(low, 60.0, 2.0);
    Check(std::abs(-lowChange - expected) < 0.6, "the low tone settles on the static curve",
          Num(lowChange, 2) + " dB, expected " + Num(-expected, 2));

    const double highChange = ToneDb(out, 5000.0, 2.0) - ToneDb(high, 5000.0, 2.0);
    Check(std::abs(highChange) < 0.3, "the quiet high tone keeps its level", Num(highChange, 2) + " dB");

    std::array<BandMeter, kBands> meters{};
    const int count = compressor->ReadBandMeters(meters);
    Check(count == kBands, "four band meters", std::to_string(count));
    Check(std::abs(meters[0].gainReductionDb - expected) < 1.5, "the low band's meter shows its gain reduction",
          Num(meters[0].gainReductionDb, 2) + " dB");
    Check(meters[3].gainReductionDb < 0.1, "the high band's meter shows none",
          Num(meters[3].gainReductionDb, 2) + " dB");
    Check(std::abs(meters[0].levelDb - -9.0) < 1.5, "the low band's level is the tone's mean square",
          Num(meters[0].levelDb, 2) + " dBFS");
    Check(meters[0].lowHz == 0.0 && meters[0].highHz == 200.0 && meters[1].lowHz == 200.0 &&
              meters[1].highHz == 800.0 && meters[2].highHz == 3000.0 && meters[3].lowHz == 3000.0 &&
              meters[3].highHz == 0.0,
          "the meters carry the crossovers in use");

    compressor->ReadBandMeters(meters);
    Check(meters[0].gainReductionDb == 0.0f, "a read starts the next hold", Num(meters[0].gainReductionDb, 2) + " dB");

    // Reset, the meters read silence again, as before any audio, rather than 0 dBFS.
    compressor->Reset();
    compressor->ReadBandMeters(meters);
    Check(meters[0].levelDb <= -120.0f && meters[3].levelDb <= -120.0f, "a reset meters silence",
          Num(meters[0].levelDb, 2) + " dBFS");
}

void TestMetersSilentBeforeAudio()
{
    std::cout << "\n-- before any audio, every band meters silence --" << std::endl;

    // The UI draws the level strip from these: 0 dBFS would show every band far over threshold.
    auto compressor = MakeCompressor(-24.0, 3.0);
    std::array<BandMeter, kBands> meters{};
    compressor->ReadBandMeters(meters);
    bool silent = true;

    for (const auto& meter : meters)
    {
        silent = silent && meter.levelDb <= -120.0f && meter.gainReductionDb == 0.0f;
    }

    Check(silent, "a prepared compressor meters -120 dBFS and no reduction", Num(meters[0].levelDb, 2) + " dBFS");
}

void TestRatioOneAndThresholdPerBand()
{
    std::cout << "\n-- each band has its own threshold and ratio --" << std::endl;

    auto compressor = MakeCompressor(-30.0, 4.0);
    compressor->SetParam("ratio1", 1.0);
    const auto low = Tone(-6.0, 1.5, 60.0);
    const double change = RmsDb(Run(*compressor, low), 1.0) - RmsDb(low, 1.0);
    Check(std::abs(change) < 0.1, "a band at 1:1 is not compressed", Num(change, 3) + " dB");

    compressor = MakeCompressor(-30.0, 4.0);
    compressor->SetParam("threshold1", 0.0);
    const double raised = RmsDb(Run(*compressor, low), 1.0) - RmsDb(low, 1.0);
    Check(std::abs(raised) < 0.1, "nor one whose threshold is above the tone", Num(raised, 3) + " dB");
}

void TestCrossoverOrdering()
{
    std::cout << "\n-- crossovers out of order are kept apart --" << std::endl;

    auto compressor = MakeCompressor(0.0, 1.0);
    compressor->SetParam("crossover1", 1000.0);
    compressor->SetParam("crossover2", 150.0);
    compressor->SetParam("crossover3", 600.0);
    Run(*compressor, Tone(-30.0, 0.01, 440.0));
    const auto splits = compressor->SplitsInUse();
    const double ratio = multiband_compressor::kMinSplitRatio;
    Check(splits[0] == 1000.0, "the lowest is where it was set", Num(splits[0]));
    Check(std::abs(splits[1] - 1000.0 * ratio) < 0.01 && std::abs(splits[2] - 1000.0 * ratio * ratio) < 0.01,
          "the next two are pushed a third of an octave apart", Num(splits[1]) + ", " + Num(splits[2]));

    // 22.05 kHz: the top crossover cannot sit at 12 kHz.
    compressor = MakeCompressor(0.0, 1.0, 22050.0);
    compressor->SetParam("crossover2", 5000.0);
    compressor->SetParam("crossover3", 12000.0);
    Run(*compressor, Tone(-30.0, 0.01, 440.0));
    const auto low = compressor->SplitsInUse();
    Check(low[2] <= 0.45 * 22050.0 + 0.01 && low[1] <= low[2] / ratio + 0.01,
          "at a low rate the top ones are pulled under Nyquist", Num(low[1]) + ", " + Num(low[2]));
}

void TestMixMakeupAndOutput()
{
    std::cout << "\n-- Mix, Makeup and Output --" << std::endl;

    const auto low = Tone(-6.0, 1.5, 60.0);
    const double in = RmsDb(low, 1.0);

    auto compressor = MakeCompressor(-40.0, 20.0);
    compressor->SetParam("mix", 0.0);
    Check(std::abs(RmsDb(Run(*compressor, low), 1.0) - in) < 0.1, "Mix 0 leaves the level as it was");

    compressor = MakeCompressor(0.0, 1.0);
    compressor->SetParam("gain1", 6.0);
    const double makeup = RmsDb(Run(*compressor, low), 1.0) - in;
    Check(std::abs(makeup - 6.0) < 0.1, "Makeup raises its band by what it says", Num(makeup, 3) + " dB");

    compressor = MakeCompressor(0.0, 1.0);
    compressor->SetParam("gain4", 6.0);
    const double other = RmsDb(Run(*compressor, low), 1.0) - in;
    Check(std::abs(other) < 0.1, "and not another band", Num(other, 3) + " dB");

    compressor = MakeCompressor(0.0, 1.0);
    compressor->SetParam("output", -6.0);
    const double output = RmsDb(Run(*compressor, low), 1.0) - in;
    Check(std::abs(output + 6.0) < 0.1, "Output trims the whole", Num(output, 3) + " dB");

    // Half Mix of a band cut by about 15.75 dB: the sum of half of each, in phase.
    compressor = MakeCompressor(-30.0, 4.0);
    compressor->SetParam("attack", 50.0);
    compressor->SetParam("release", 500.0);
    compressor->SetParam("mix", 0.5);
    const auto longLow = Tone(-6.0, 3.0, 60.0);
    const double half = ToneDb(Run(*compressor, longLow), 60.0, 2.0) - ToneDb(longLow, 60.0, 2.0);
    const double expected = 20.0 * std::log10(0.5 * Amplitude(-15.75) + 0.5);
    Check(std::abs(half - expected) < 0.5, "half Mix blends in phase, no comb",
          Num(half, 2) + " dB, expected " + Num(expected, 2));
}

void TestKneeCurve()
{
    std::cout << "\n-- the knee curve --" << std::endl;

    using multiband_compressor::GainReductionDb;
    const double slope = 1.0 - 1.0 / 4.0;
    Check(GainReductionDb(0.0, 4.0, 0.0) == 0.0 && std::abs(GainReductionDb(2.0, 4.0, 0.0) - 2.0 * slope) < 1.0e-12,
          "knee 0 is a hard knee", Num(GainReductionDb(2.0, 4.0, 0.0), 3) + " dB at 2 dB over");
    Check(std::abs(GainReductionDb(0.0, 4.0, 12.0) - slope * 1.5) < 1.0e-12,
          "a 12 dB knee takes a quarter of its half-width at the threshold", Num(GainReductionDb(0.0, 4.0, 12.0), 3));
    Check(GainReductionDb(-6.0, 4.0, 12.0) == 0.0 && std::abs(GainReductionDb(6.0, 4.0, 12.0) - 6.0 * slope) < 1.0e-12,
          "and meets the straight lines at its edges");
    Check(GainReductionDb(3.0, 4.0) == GainReductionDb(3.0, 4.0, 6.0), "6 dB is the default");
}

void TestKnee()
{
    std::cout << "\n-- Knee in the effect --" << std::endl;

    // The low tone's mean square sits right on the threshold.
    const auto low = Tone(-6.0, 3.0, 60.0);
    const auto changeAt = [&low](double kneeDb) {
        auto compressor = MakeCompressor(-9.0, 4.0);
        compressor->SetParam("attack", 50.0);
        compressor->SetParam("release", 500.0);
        compressor->SetParam("knee1", kneeDb);
        return ToneDb(Run(*compressor, low), 60.0, 2.0) - ToneDb(low, 60.0, 2.0);
    };

    const double hard = changeAt(0.0);
    Check(hard > -0.6, "a hard knee leaves a tone on the threshold nearly alone", Num(hard, 2) + " dB");
    // 12 dB half-width at 4:1: 0.75 * 12^2 / 48 = 2.25 dB.
    const double wide = changeAt(24.0);
    Check(std::abs(wide + 2.25) < 0.6, "a 24 dB knee is already compressing it", Num(wide, 2) + " dB, expected -2.25");
}

void TestOnAndSolo()
{
    std::cout << "\n-- On and Solo --" << std::endl;

    const auto low = Tone(-6.0, 1.5, 60.0);
    // Well clear of the 3 kHz crossover, so all of it is in the high band.
    const auto high = Tone(-20.0, 1.5, 10000.0);
    const auto both = Mix(low, high);

    auto compressor = MakeCompressor(-40.0, 20.0);
    compressor->SetParam("gain1", 6.0);
    compressor->SetParam("on1", 0.0);
    const auto off = Run(*compressor, low);
    const double offChange = ToneDb(off, 60.0, 1.0) - ToneDb(low, 60.0, 1.0);
    Check(std::abs(offChange) < 0.1, "a band turned off passes untouched, makeup and all", Num(offChange, 3) + " dB");
    std::array<BandMeter, kBands> meters{};
    compressor->ReadBandMeters(meters);
    Check(meters[0].gainReductionDb == 0.0f, "and meters no reduction", Num(meters[0].gainReductionDb, 2) + " dB");

    compressor = MakeCompressor(0.0, 1.0);
    compressor->SetParam("solo4", 1.0);
    auto out = Run(*compressor, both);
    const double lowGone = ToneDb(out, 60.0, 1.0) - ToneDb(low, 60.0, 1.0);
    const double highKept = ToneDb(out, 10000.0, 1.0) - ToneDb(high, 10000.0, 1.0);
    Check(lowGone < -60.0, "soloing the high band silences the low one", Num(lowGone, 1) + " dB");
    Check(std::abs(highKept) < 0.2, "and keeps the high one", Num(highKept, 3) + " dB");

    compressor->SetParam("solo1", 1.0);
    out = Run(*compressor, both);
    const double bothKept = std::max(std::abs(ToneDb(out, 60.0, 1.0) - ToneDb(low, 60.0, 1.0)),
                                     std::abs(ToneDb(out, 10000.0, 1.0) - ToneDb(high, 10000.0, 1.0)));
    Check(bothKept < 0.2, "two soloed bands are both heard", "worst " + Num(bothKept, 3) + " dB");

    compressor->SetParam("solo1", 0.0);
    compressor->SetParam("solo4", 0.0);
    out = Run(*compressor, both);
    const double flat = RmsDb(out, 1.0) - RmsDb(both, 1.0);
    Check(std::abs(flat) < 0.1, "with no solo, all of it again", Num(flat, 3) + " dB");
}

/// The largest step between neighbouring samples of `signal`.
double LargestStep(const std::vector<float>& signal)
{
    double largest = 0.0;

    for (std::size_t i = 1; i < signal.size(); ++i)
    {
        largest = std::max(largest, static_cast<double>(std::abs(signal[i] - signal[i - 1])));
    }

    return largest;
}

void TestSwitchingGlides()
{
    std::cout << "\n-- switching a band glides --" << std::endl;

    // A 60 Hz sine at -6 dB moves at most 0.004 a sample; a hard mute would step by up to 0.5.
    const auto low = Tone(-6.0, 1.0, 60.0);
    const std::vector<float> first(low.begin(), low.begin() + low.size() / 2);
    const std::vector<float> second(low.begin() + static_cast<std::ptrdiff_t>(low.size() / 2), low.end());

    for (const auto& [key, value] : {std::pair{"solo4", 1.0}, std::pair{"on1", 0.0}})
    {
        auto compressor = MakeCompressor(-40.0, 20.0);
        compressor->SetParam("gain1", 12.0);
        auto out = Run(*compressor, first);
        compressor->SetParam(key, value);
        const auto after = Run(*compressor, second);
        out.insert(out.end(), after.begin(), after.end());
        const double step = LargestStep(out);
        Check(step < 0.02, std::string(key) + " does not click", "largest step " + Num(step, 4));
    }
}

void TestMonoMatchesStereo()
{
    std::cout << "\n-- mono and stereo agree --" << std::endl;

    const auto signal = Mix(Tone(-6.0, 0.5, 60.0), Tone(-12.0, 0.5, 2000.0));
    auto stereo = MakeCompressor(-30.0, 4.0);
    auto mono = MakeCompressor(-30.0, 4.0);
    const auto a = Run(*stereo, signal);
    const auto b = Run(*mono, signal, true);
    double worst = 0.0;

    for (std::size_t i = 0; i < a.size(); ++i)
    {
        worst = std::max(worst, static_cast<double>(std::abs(a[i] - b[i])));
    }

    Check(worst < 1.0e-6, "the mono path matches the stereo one", Num(worst, 9));
}

void TestDualMono()
{
    std::cout << "\n-- linked and dual mono --" << std::endl;

    const auto loud = Tone(-6.0, 1.5, 60.0);
    const auto quiet = Tone(-40.0, 1.5, 60.0);
    std::vector<float> right;

    auto linked = MakeCompressor(-30.0, 4.0);
    Run(*linked, loud, false, &quiet, &right);
    const double linkedChange = RmsDb(right, 1.0) - RmsDb(quiet, 1.0);
    Check(linkedChange < -10.0, "linked, the quiet side follows the loud one", Num(linkedChange, 2) + " dB");

    auto dual = MakeCompressor(-30.0, 4.0);
    dual->SetDualMono(true);
    Run(*dual, loud, false, &quiet, &right);
    const double dualChange = RmsDb(right, 1.0) - RmsDb(quiet, 1.0);
    Check(std::abs(dualChange) < 0.1, "dual mono, the quiet side is left alone", Num(dualChange, 3) + " dB");
}

void TestNonFiniteDoesNotLatch()
{
    std::cout << "\n-- a non-finite sample does not latch --" << std::endl;

    auto compressor = MakeCompressor(-30.0, 4.0);
    auto poisoned = Tone(-6.0, 0.2, 60.0);
    poisoned[100] = std::numeric_limits<float>::quiet_NaN();
    Run(*compressor, poisoned);
    const auto after = Run(*compressor, Tone(-6.0, 0.5, 60.0));
    const bool finite = std::all_of(after.begin(), after.end(), [](float value) { return std::isfinite(value); });
    Check(finite && RmsDb(after, 0.3) > -40.0, "the next notes come out", Num(RmsDb(after, 0.3), 1) + " dB");
}
} // namespace

int main()
{
    std::cout << "=== MultibandCompressorEffectTests ===" << std::endl;
    RegisterAllEffects();

    TestBelowThresholdIsFlat();
    TestMetersSilentBeforeAudio();
    TestStaticCurveAndIsolation();
    TestRatioOneAndThresholdPerBand();
    TestCrossoverOrdering();
    TestMixMakeupAndOutput();
    TestKneeCurve();
    TestKnee();
    TestOnAndSolo();
    TestSwitchingGlides();
    TestMonoMatchesStereo();
    TestDualMono();
    TestNonFiniteDoesNotLatch();

    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}
