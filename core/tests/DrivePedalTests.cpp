/**
 * @file DrivePedalTests.cpp
 * @brief Tests for the Overdrive, Distortion and Fuzz pedals and the oversampler they share.
 *
 * These measure what the pedals claim, model by model:
 *   - registration: the Model and Clipping switches, and the parameters old presets use
 *   - Level is calibrated: a guitar phrase at the nominal level comes out as loud as bypass
 *     at any drive (the LPB-1, a boost, comes out boosted)
 *   - no DC leaves any pedal, however asymmetric its clipping, and silence stays silent
 *   - the clipping is antialiased: a high note at full drive keeps its aliases far down
 *   - the overdrives respond to dynamics: clean when played softly, driven when hard
 *   - the fuzz Bias knob gates when cold, and moves the Big Muff's harmonics
 *   - Model switches fade instead of clicking, and knob moves glide
 *   - the mono path matches the stereo one sample for sample
 *   - loudness holds from 44.1 to 192 kHz, and the output stays finite down to 8 kHz
 *   - a NaN or infinity in the input is shrugged off
 *   - the half-band oversampler is flat through the audio band and rejects images
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <iostream>
#include <string>
#include <vector>

#include "DriveTestSupport.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/HalfBandIir.h"
#include "dsp/effects/DistortionEffect.h"
#include "dsp/effects/DriveStages.h"
#include "dsp/effects/FuzzEffect.h"
#include "dsp/effects/OverdriveEffect.h"

namespace
{
using namespace drive_test;

void TestRegistration()
{
    std::cout << "\nRegistration" << std::endl;
    auto& registry = guitarfx::EffectRegistry::Instance();

    for (const auto& pedal : Pedals())
    {
        const auto info = registry.GetTypeInfo(pedal.type);
        const std::string name = pedal.name;

        if (!info)
        {
            Check(false, name + " is registered");
            continue;
        }

        Check(info->category == "drive" && registry.Resolve(pedal.alias) == pedal.type,
              name + " is a drive effect and its legacy alias resolves");

        bool legacyKeys = true;

        for (const char* key : {"drive", "tone", "level", "mix"})
        {
            legacyKeys = legacyKeys && std::any_of(info->parameters.begin(), info->parameters.end(),
                                                   [key](const auto& def) { return def.id == key; });
        }

        Check(legacyKeys, name + " keeps the drive, tone, level and mix keys old presets store");

        const auto model = std::find_if(info->parameters.begin(), info->parameters.end(),
                                        [](const auto& def) { return def.id == "model"; });
        Check(model != info->parameters.end() && model->unit == "enum" && model->step == 1.0 &&
                  model->labels.size() == pedal.models && model->maxValue == static_cast<double>(pedal.models - 1),
              name + " Model switch lists its " + std::to_string(pedal.models) + " circuits");

        auto effect = registry.Create(pedal.type);
        const double defaultDrive = effect->GetParam("drive");
        effect->SetParam("model", 99.0);
        effect->SetParam("drive", RuntimeDouble(0x7ff8000000000000ull));
        Check(effect->GetParam("model") == static_cast<double>(pedal.models - 1) &&
                  effect->GetParam("drive") == defaultDrive,
              name + " clamps an out-of-range model and ignores a NaN");
    }
}

void TestLoudnessMatchesBypass()
{
    std::cout << "\nLevel calibration (guitar phrase at the nominal level, K-weighted)" << std::endl;
    const auto phrase = GuitarPhrase();
    const double bypass = KWeightedDb(phrase);

    for (const auto& pedal : Pedals())
    {
        for (std::size_t model = 0; model < pedal.models; ++model)
        {
            double worst = 0.0;
            double boostAtHalf = 0.0;

            for (double drive : {0.0, 0.5, 1.0})
            {
                if (IsBoost(pedal, model) && drive == 1.0)
                {
                    continue; // nothing below reads the boost at full drive
                }

                auto effect = Make(pedal, model, {{"drive", drive}});
                const double difference = KWeightedDb(Render(*effect, phrase)) - bypass;
                worst = std::max(worst, std::fabs(IsBoost(pedal, model) && drive > 0.0 ? 0.0 : difference));
                boostAtHalf = drive == 0.5 ? difference : boostAtHalf;
            }

            const std::string label = std::string(pedal.name) + " " + pedal.labels[model];

            if (IsBoost(pedal, model))
            {
                // +12 dB of gain, less what its 9 V transistor gives up to a hot guitar's peaks.
                Check(worst < 1.0 && boostAtHalf > 8.0 && boostAtHalf < 13.0,
                      label + " is unity at drive 0 and boosts at half", "+" + Num(boostAtHalf, 1) + " dB");
            }
            else
            {
                Check(worst < 1.0, label + " is as loud as bypass at drive 0, 0.5 and 1",
                      "worst " + Num(worst, 2) + " dB");
            }
        }
    }
}

void TestNoDcAndSilence()
{
    std::cout << "\nDC and silence" << std::endl;
    // 120 Hz has a period of exactly 400 samples, so the mean over whole periods is the DC. The
    // LPB-1 gets a quieter tone: at full boost the loud one would be clipped by the output
    // limiter, after the pedal's own DC blocking.
    const auto tone = Sine(120.0, -12.0, 48000);
    const auto quietTone = Sine(120.0, -36.0, 48000);

    for (const auto& pedal : Pedals())
    {
        double worstDb = -300.0;
        bool silent = true;

        for (std::size_t model = 0; model < pedal.models; ++model)
        {
            for (const Params& params :
                 {Params{{"drive", 1.0}, {"clipping", 2.0}, {"bias", 0.0}}, Params{{"drive", 1.0}, {"bias", 1.0}}})
            {
                auto effect = Make(pedal, model, params);
                const auto out = Render(*effect, IsBoost(pedal, model) ? quietTone : tone);
                double sum = 0.0;
                double squares = 0.0;

                for (std::size_t i = 24000; i < out.size(); ++i)
                {
                    sum += out[i];
                    squares += static_cast<double>(out[i]) * out[i];
                }

                const double count = static_cast<double>(out.size() - 24000);
                const double ratio = std::fabs(sum / count) / std::sqrt(squares / count);
                worstDb = std::max(worstDb, 20.0 * std::log10(ratio + 1.0e-15));
            }

            auto fresh = Make(pedal, model, {{"bias", 0.2}});
            const auto quiet = Render(*fresh, std::vector<float>(4096, 0.0f));
            silent = silent && std::all_of(quiet.begin(), quiet.end(), [](float s) { return s == 0.0f; });
        }

        Check(worstDb < -60.0, std::string(pedal.name) + ": no DC at full drive and extreme bias or clipping",
              "worst " + Num(worstDb, 1) + " dB re RMS");
        Check(silent, std::string(pedal.name) + ": silence in, exact silence out, biased stages included");
    }
}

void TestAntialiasing()
{
    std::cout << "\nAliasing: a 1318.5 Hz note (the 24th-fret E) at full drive" << std::endl;

    for (const auto& pedal : Pedals())
    {
        for (std::size_t model = 0; model < pedal.models; ++model)
        {
            // The LPB-1 at full boost would run the output limiter, which is not the pedal.
            const double level = IsBoost(pedal, model) ? -36.0 : kNominalRmsDb;
            auto effect = Make(pedal, model, {{"drive", 1.0}});
            const double alias = Analyse(Render(*effect, Sine(1318.5, level, 24576)), 1318.5).aliasDb;
            Check(alias < -55.0, std::string(pedal.name) + " " + pedal.labels[model] + " keeps aliases down",
                  Num(alias, 1) + " dB");
        }
    }
}

void TestOverdriveDynamics()
{
    std::cout << "\nOverdrives clean up when played softly" << std::endl;
    const Pedal& overdrive = Pedals()[0];

    for (std::size_t model = 0; model < overdrive.models; ++model)
    {
        if (IsBoost(overdrive, model))
        {
            continue;
        }

        auto soft = Make(overdrive, model);
        auto hard = Make(overdrive, model);
        const double softThd = Analyse(Render(*soft, Sine(220.0, -40.0, 24576)), 220.0).thdDb;
        const double hardThd = Analyse(Render(*hard, Sine(220.0, -18.0, 24576)), 220.0).thdDb;
        Check(softThd < -18.0 && softThd < hardThd - 8.0,
              std::string("Overdrive ") + overdrive.labels[model] + " is cleaner at -40 dBFS than at -18",
              "THD " + Num(softThd, 1) + " vs " + Num(hardThd, 1) + " dB");
    }
}

void TestFuzzBias()
{
    std::cout << "\nFuzz Bias" << std::endl;
    const Pedal& fuzz = Pedals()[2];
    const auto faceModel = static_cast<std::size_t>(guitarfx::fuzz::Model::FuzzFace);
    const auto quiet = Sine(220.0, -52.0, 24000);

    auto stock = Make(fuzz, faceModel);
    auto cold = Make(fuzz, faceModel, {{"bias", 0.0}});
    const double stockDb = RmsDb(Render(*stock, quiet), 12000);
    const double coldDb = RmsDb(Render(*cold, quiet), 12000);
    Check(coldDb < stockDb - 20.0, "a cold-biased Fuzz Face gates a quiet note that the stock one passes",
          Num(coldDb, 1) + " vs " + Num(stockDb, 1) + " dBFS");

    auto fuzzTone = Make(fuzz, static_cast<std::size_t>(guitarfx::fuzz::Model::FuzzTone));
    const double tailDb = RmsDb(Render(*fuzzTone, Sine(220.0, -62.0, 24000)), 12000);
    auto fuzzToneLoud = Make(fuzz, static_cast<std::size_t>(guitarfx::fuzz::Model::FuzzTone));
    const double bodyDb = RmsDb(Render(*fuzzToneLoud, Sine(220.0, -36.0, 24000)), 12000);
    Check(tailDb < -90.0 && bodyDb > -30.0, "the stock Fuzz-Tone sputters out as a note decays",
          Num(tailDb, 1) + " dBFS at -62, " + Num(bodyDb, 1) + " at -36");

    const auto muff = static_cast<std::size_t>(guitarfx::fuzz::Model::BigMuff);
    auto symmetric = Make(fuzz, muff);
    auto leaning = Make(fuzz, muff, {{"bias", 0.0}});
    const auto tone = Sine(220.0, kNominalRmsDb, 24576);
    const double symmetricEven = Analyse(Render(*symmetric, tone), 220.0).evenOverOddDb;
    const double leaningEven = Analyse(Render(*leaning, tone), 220.0).evenOverOddDb;
    Check(leaningEven > symmetricEven + 40.0, "Bias moves the Big Muff from odd harmonics to even ones",
          "even/odd " + Num(symmetricEven, 1) + " -> " + Num(leaningEven, 1) + " dB");
}

void TestSwitchesAndGlides()
{
    std::cout << "\nSwitching and knob moves" << std::endl;
    const Pedal& overdrive = Pedals()[0];
    auto effect = Make(overdrive, 0);
    const auto tone = Sine(220.0, kNominalRmsDb, 48000);
    std::vector<float> out(tone.size());
    std::vector<float> scratch(tone.size());

    for (std::size_t start = 0; start < tone.size(); start += kBlockSize)
    {
        if (start == 19200)
        {
            effect->SetParam("model", static_cast<double>(guitarfx::overdrive::Model::Timmy));
            effect->SetParam("clipping", 4.0);
        }

        std::vector<float> left(tone.begin() + static_cast<std::ptrdiff_t>(start),
                                tone.begin() + static_cast<std::ptrdiff_t>(start + kBlockSize));
        std::vector<float> right(left);
        float* inputs[2] = {left.data(), right.data()};
        float* outputs[2] = {out.data() + start, scratch.data() + start};
        effect->Process(inputs, outputs, kBlockSize);
    }

    // The largest sample-to-sample step either circuit makes on its own, played steadily.
    const double steadyStep = std::max(LargestStep(out, 9600, 19200), LargestStep(out, 30000, 48000));
    const double switchStep = LargestStep(out, 19200, 24000);
    double quietest = 1.0;

    for (std::size_t i = 19200; i < 22000; i += 64)
    {
        double peak = 0.0;

        for (std::size_t j = i; j < i + 64; ++j)
        {
            peak = std::max(peak, static_cast<double>(std::fabs(out[j])));
        }

        quietest = std::min(quietest, peak);
    }

    Check(switchStep < steadyStep * 1.5 && quietest < 0.02,
          "a Model and Clipping change fades out and in instead of jumping",
          "largest step " + Num(switchStep, 4) + " vs " + Num(steadyStep, 4) + " steady, dips to " + Num(quietest, 4));
    Check(RmsDb(out, 40000) > -30.0, "and the new circuit is playing afterwards");

    auto levelled = Make(overdrive, 0, {{"level", -24.0}, {"drive", 0.0}});
    const auto quietTone = Sine(1000.0, -40.0, 24000);
    std::vector<float> glide(quietTone.size());

    for (std::size_t start = 0; start < quietTone.size(); start += kBlockSize)
    {
        if (start == 9600)
        {
            levelled->SetParam("level", 12.0);
        }

        std::vector<float> left(quietTone.begin() + static_cast<std::ptrdiff_t>(start),
                                quietTone.begin() + static_cast<std::ptrdiff_t>(start + kBlockSize));
        std::vector<float> right(left);
        float* inputs[2] = {left.data(), right.data()};
        float* outputs[2] = {glide.data() + start, scratch.data() + start};
        levelled->Process(inputs, outputs, kBlockSize);
    }

    double firstMs = 0.0;
    double settled = 0.0;

    for (std::size_t i = 9600; i < 9648; ++i)
    {
        firstMs = std::max(firstMs, static_cast<double>(std::fabs(glide[i])));
    }

    for (std::size_t i = 20000; i < 24000; ++i)
    {
        settled = std::max(settled, static_cast<double>(std::fabs(glide[i])));
    }

    Check(firstMs < 0.1 * settled, "a 36 dB Level change glides rather than jumps",
          "first ms peaks at " + Num(100.0 * firstMs / settled, 1) + "% of the settled level");
}

void TestMonoMatchesStereo()
{
    std::cout << "\nMono path" << std::endl;
    const auto phrase = GuitarPhrase();

    for (const auto& pedal : Pedals())
    {
        bool identical = true;

        for (std::size_t model = 0; model < pedal.models; ++model)
        {
            auto stereo = Make(pedal, model, {{"mix", 0.7}});
            auto mono = Make(pedal, model, {{"mix", 0.7}});
            const auto expected = Render(*stereo, phrase, true);
            std::vector<float> input(phrase);
            std::vector<float> actual(phrase.size());

            for (std::size_t start = 0; start < input.size(); start += kBlockSize)
            {
                const int count = static_cast<int>(std::min<std::size_t>(kBlockSize, input.size() - start));
                mono->ProcessMono(input.data() + start, actual.data() + start, count);
            }

            identical = identical && mono->SupportsMonoProcessing() && actual == expected;
        }

        Check(identical, std::string(pedal.name) + ": mono output matches stereo sample for sample, every model");
    }
}

void TestSampleRates()
{
    std::cout << "\nSample rates" << std::endl;

    for (const auto& pedal : Pedals())
    {
        double worst = 0.0;
        bool finite = true;

        for (std::size_t model = 0; model < pedal.models; ++model)
        {
            auto reference = Make(pedal, model);
            const double referenceDb = RmsDb(Render(*reference, Sine(220.0, kNominalRmsDb, 24000)), 12000);

            for (double rate : {44100.0, 96000.0, 192000.0, 11025.0, 8000.0})
            {
                auto effect = Make(pedal, model, {}, rate);
                const auto length = static_cast<std::size_t>(rate * 0.5);
                const auto out = Render(*effect, Sine(220.0, kNominalRmsDb, length, rate));
                finite = finite && std::all_of(out.begin(), out.end(), [](float s) { return guitarfx::IsFinite(s); });

                if (rate >= 44100.0)
                {
                    worst = std::max(worst, std::fabs(RmsDb(out, length / 2) - referenceDb));
                }
            }
        }

        Check(worst < 0.5, std::string(pedal.name) + ": every model is as loud at 44.1, 96 and 192 kHz as at 48",
              "worst " + Num(worst, 2) + " dB");
        Check(finite, std::string(pedal.name) + ": finite down to 8 kHz");
    }
}

void TestNonFiniteRecovery()
{
    std::cout << "\nNon-finite input" << std::endl;

    for (const auto& pedal : Pedals())
    {
        bool recovered = true;

        for (std::size_t model = 0; model < pedal.models; ++model)
        {
            auto effect = Make(pedal, model, {{"mix", 0.5}});
            auto tone = Sine(440.0, kNominalRmsDb, 24000);
            tone[1000] = RuntimeFloat(0x7fc00000u);
            tone[1001] = RuntimeFloat(0x7f800000u);
            const auto out = Render(*effect, tone);
            recovered = recovered &&
                        std::all_of(out.begin(), out.end(), [](float s) { return guitarfx::IsFinite(s); }) &&
                        RmsDb(out, 12000) > -40.0;
        }

        Check(recovered, std::string(pedal.name) + ": a NaN and an infinity pass without trace, every model");
    }
}

void TestHalfBand()
{
    std::cout << "\nHalf-band oversampler" << std::endl;

    for (int factor : {2, 4, 8})
    {
        double worstRipple = 0.0;

        for (double hz : {100.0, 1000.0, 10000.0, 18000.0})
        {
            guitarfx::halfband::Oversampler oversampler;
            oversampler.Prepare(factor);
            const auto tone = Sine(hz, -6.0, 24000);
            double in = 0.0;
            double out = 0.0;

            for (std::size_t i = 0; i < tone.size(); ++i)
            {
                double lanes[guitarfx::halfband::Oversampler::kMaxFactor];
                oversampler.Up(tone[i], lanes);
                const double y = oversampler.Down(lanes);

                if (i >= 12000)
                {
                    in += static_cast<double>(tone[i]) * tone[i];
                    out += y * y;
                }
            }

            worstRipple = std::max(worstRipple, std::fabs(10.0 * std::log10(out / in)));
        }

        Check(worstRipple < 0.01, "a " + std::to_string(factor) + "x round trip is flat to 18 kHz",
              Num(worstRipple, 4) + " dB");
    }

    // An 18 kHz tone upsampled 2x: its image at 30 kHz is what the first stage must remove.
    guitarfx::halfband::Oversampler oversampler;
    oversampler.Prepare(2);
    std::complex<double> tone = 0.0;
    std::complex<double> image = 0.0;

    for (int i = 0; i < 48000; ++i)
    {
        double lanes[2];
        oversampler.Up(std::sin(2.0 * kPi * 18000.0 * i / kSampleRate), lanes);

        for (int lane = 0; lane < 2; ++lane)
        {
            const double t = (2.0 * i + lane) / (2.0 * kSampleRate);
            const double window = 0.5 - 0.5 * std::cos(2.0 * kPi * (2.0 * i + lane) / 96000.0);
            tone += lanes[lane] * window * std::polar(1.0, -2.0 * kPi * 18000.0 * t);
            image += lanes[lane] * window * std::polar(1.0, -2.0 * kPi * 30000.0 * t);
        }
    }

    const double rejection = 20.0 * std::log10(std::abs(image) / std::abs(tone));
    Check(rejection < -90.0, "the image of an 18 kHz tone is rejected", Num(rejection, 1) + " dB");
}

/// `DrivePedalTests --calibrate` prints what a voicing change needs: per model, the correction
/// to add to each of its nine `trimDb` entries, and for the overdrives and distortions the
/// least-squares fit for `clipLevelExponent` (see ClipLevelCompensation). Add the corrections,
/// rebuild, and run it again until they are near zero; the fit converges in one pass.
void PrintCalibration()
{
    const auto phrase = GuitarPhrase();
    const double bypass = KWeightedDb(phrase);

    for (const auto& pedal : Pedals())
    {
        for (std::size_t model = 0; model < pedal.models; ++model)
        {
            std::cout << pedal.name << " " << pedal.labels[model] << "\n  trimDb corrections:";

            for (int step = 0; step <= 8; ++step)
            {
                auto effect = Make(pedal, model, {{"drive", step / 8.0}});
                std::cout << " " << Num(bypass - KWeightedDb(Render(*effect, phrase)), 1);
            }

            std::cout << std::endl;

            const bool hasClipping = std::string(pedal.type) != guitarfx::EffectGuids::kFuzz;

            if (!hasClipping)
            {
                continue;
            }

            const bool isOverdrive = std::string(pedal.type) == guitarfx::EffectGuids::kOverdrive;
            const auto& stock = isOverdrive ? guitarfx::overdrive::kVoicings[model].stockClip
                                            : guitarfx::distortion::kVoicings[model].stockClip;
            const double exponent = isOverdrive ? guitarfx::overdrive::kVoicings[model].clipLevelExponent
                                                : guitarfx::distortion::kVoicings[model].clipLevelExponent;
            auto reference = Make(pedal, model);
            const double stockDb = KWeightedDb(Render(*reference, phrase));
            double numerator = 0.0;
            double denominator = 0.0;

            for (int choice = 1; choice < static_cast<int>(guitarfx::drive::ClipChoice::Count); ++choice)
            {
                auto effect = Make(pedal, model, {{"clipping", static_cast<double>(choice)}});
                const double residual = KWeightedDb(Render(*effect, phrase)) - stockDb;
                const auto chosen = guitarfx::drive::ClipFor(static_cast<guitarfx::drive::ClipChoice>(choice), stock);
                const double ratioDb =
                    20.0 * std::log10((chosen.positive + chosen.negative) / (stock.positive + stock.negative));
                numerator += (residual + exponent * ratioDb) * ratioDb;
                denominator += ratioDb * ratioDb;
            }

            std::cout << "  clipLevelExponent fit: " << Num(numerator / denominator, 2) << " (now " << Num(exponent, 2)
                      << ")" << std::endl;
        }
    }
}

/// `DrivePedalTests --cost` runs the checks, then times each model. Build it Release to read it.
void ReportCost()
{
    std::cout << "\nCost per 64-sample block at 48 kHz, mono (for information)" << std::endl;
    const auto tone = Sine(220.0, kNominalRmsDb, 48000);
    std::vector<float> out(tone.size());

    for (const auto& pedal : Pedals())
    {
        for (std::size_t model = 0; model < pedal.models; ++model)
        {
            auto effect = Make(pedal, model);
            std::vector<float> input(tone);
            const auto begin = std::chrono::steady_clock::now();

            for (std::size_t start = 0; start < input.size(); start += kBlockSize)
            {
                effect->ProcessMono(input.data() + start, out.data() + start, kBlockSize);
            }

            const std::chrono::duration<double, std::micro> elapsed = std::chrono::steady_clock::now() - begin;
            std::cout << "  " << pedal.name << " " << pedal.labels[model] << ": "
                      << Num(elapsed.count() / (static_cast<double>(input.size()) / kBlockSize), 2) << " us"
                      << std::endl;
        }
    }
}
} // namespace

int main(int argc, char** argv)
{
    std::cout << "=== DrivePedalTests ===" << std::endl;
    guitarfx::RegisterOverdriveEffect();
    guitarfx::RegisterDistortionEffect();
    guitarfx::RegisterFuzzEffect();

    if (argc > 1 && std::string(argv[1]) == "--calibrate")
    {
        PrintCalibration();
        return 0;
    }

    TestRegistration();
    TestLoudnessMatchesBypass();
    TestNoDcAndSilence();
    TestAntialiasing();
    TestOverdriveDynamics();
    TestFuzzBias();
    TestSwitchesAndGlides();
    TestMonoMatchesStereo();
    TestSampleRates();
    TestNonFiniteRecovery();
    TestHalfBand();

    // Timings only, and meaningless in the Debug build ctest runs, so on request.
    if (argc > 1 && std::string(argv[1]) == "--cost")
    {
        ReportCost();
    }

    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}
