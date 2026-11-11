#include "dsp/effects/BuiltinAmpEffect.h"
#include "dsp/effects/BuiltinAmpOversampling.h"
#include "dsp/effects/BuiltinAmpVoicing.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "helpers/BuiltinAmpLevelCalibration.h"
#include "helpers/BuiltinAmpTestSupport.h"

using namespace builtin_amp_test;

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

double DriveThd(double gain, int stages, double voice = 1.0, double amplitude = 0.10)
{
    return Thd(Render(48000.0, amplitude, 220.0, 256, voice, stages, 0.0, gain), 220.0, 48000.0);
}

// The amp has to span an American rock/metal range: near-clean at the bottom of
// the gain control, a saturated cascade at the top, and a usable spread in
// between. A cascade of bounded saturators reaches a fixed point very easily,
// so these guard the part that is hard to keep: that turning something up still
// buys more distortion.
void TestGainRange()
{
    const double cleanFloor = DriveThd(0.0, 2, 0.0);
    const double cleanCeiling = DriveThd(1.0, 2, 0.0);
    std::cout << "Clean voice THD: " << 100.0 * cleanFloor << "% at gain 0, " << 100.0 * cleanCeiling
              << "% at gain 1\n";
    Check(cleanFloor < 0.015, "clean voice stays clean at the bottom of the gain control");
    Check(cleanCeiling < 0.12, "clean voice stays a clean channel even at full gain");

    const double quarter = DriveThd(0.25, 2);
    const double half = DriveThd(0.5, 2);
    const double threeQuarters = DriveThd(0.75, 2);
    const double full = DriveThd(1.0, 2);
    std::cout << "Drive voice THD by gain: " << 100.0 * quarter << "%, " << 100.0 * half << "%, "
              << 100.0 * threeQuarters << "%, " << 100.0 * full << "%\n";
    Check(full > 0.30, "drive voice reaches high-gain saturation at the top of the control");
    Check(half > quarter * 1.3 && threeQuarters > half * 1.3,
          "the gain control keeps buying distortion across its range");
    Check(full > cleanCeiling * 3.0, "the drive voice is decisively dirtier than the clean voice");

    // Every added preamp stage has to add saturation. It stops doing that if an
    // interstage high-pass is looser than the one before it, because the stage
    // then clips a waveform the previous filter had already tilted into spikes.
    double previous = 0.0;

    for (int stages = 1; stages <= 4; ++stages)
    {
        const double thd = stages == 2 ? full : DriveThd(1.0, stages);
        std::cout << "  stages=" << stages << " THD " << 100.0 * thd << "%\n";
        Check(thd > previous, "each added preamp stage adds saturation");
        previous = thd;
    }

    // A weak pickup has to be able to reach the same place a hot one does.
    Check(DriveThd(1.0, 4, 1.0, 0.02) > 0.30, "a quiet input still reaches full saturation");
}

// The amp's level is Output's alone: a guitar at the nominal level comes out at
// that level, as heard through a cab, whatever Voice, Gain, Preamp Stages,
// Character and Power Drive say. Those are distortion controls, and an amp is a
// compressor, so this has to hold on a real guitar at the nominal level: with
// the makeup measured on quiet sine tones, Gain, stages and Power Drive each
// cost up to 4-5 dB, the Drive voice ran 6 dB over the Clean, and the default
// was 7.5 dB over the input. If this fails after a voicing change, re-measure
// (BuiltinAmpEffectTests --measure-levels).
void TestLevelHoldsAtNominal()
{
    const auto guitar = NominalGuitar("guitar-riff-01.wav");
    Check(!guitar.empty(), "the demo riff loads");

    if (guitar.empty())
    {
        return;
    }

    std::vector<Voicing> voicings;

    for (const double voice : {0.0, 1.0})
    {
        for (const int stages : {1, 2, 4})
        {
            for (const double gain : {0.0, 0.5, 1.0})
            {
                for (const double character : {0.0, 0.5, 1.0})
                {
                    voicings.push_back({gain, voice, character, stages});
                }

                // Character follows Gain here and Bias follows the voice, which reaches
                // each of their ends without multiplying the count. Sag is tried at
                // full Power Drive, where it costs the most.
                for (const double powerDrive : {0.5, 1.0})
                {
                    voicings.push_back({gain, voice, gain, stages, powerDrive, 0.0, voice * 2.0 - 1.0});
                }

                voicings.push_back({gain, voice, gain, stages, 1.0, 1.0, voice * 2.0 - 1.0});
            }
        }
    }

    // Between the table's steps.
    voicings.push_back({0.6, 1.0, 0.3, 3, 0.7, 0.4, 0.5});
    voicings.push_back({0.15, 0.0, 0.85, 3, 0.3, 0.0, -0.5});
    voicings.push_back({0.9, 1.0, 0.1, 2});
    voicings.push_back({0.35, 0.0, 0.65, 4, 1.0, 0.0, 1.0});

    double worst = 0.0, squares = 0.0;
    Voicing worstAt;

    for (const Voicing& v : voicings)
    {
        const double heard = HeardGainDb(v, guitar);
        squares += heard * heard;

        if (std::abs(heard) > std::abs(worst))
        {
            worst = heard;
            worstAt = v;
        }
    }

    const double rms = std::sqrt(squares / static_cast<double>(voicings.size()));
    std::cout << "Heard level against a guitar at the nominal level over " << voicings.size() << " settings: " << rms
              << " dB RMS, worst " << worst << " dB (voice " << worstAt.voice << ", " << worstAt.stages
              << " stages, gain " << worstAt.gain << ", Character " << worstAt.character << ", Power Drive "
              << worstAt.powerDrive << ", Bias " << worstAt.bias << ")\n";
    // The corners (full Power Drive and Bias on the soft knee) sit near 1 dB.
    Check(rms < 0.5 && std::abs(worst) < 1.5,
          "a guitar at the nominal level comes out at that level, whatever the controls say");
}

// Input Trim is not made up, but Power Drive and Sag are, at any trim: their
// makeup has to feed the power stage as hot as the trim makes it. Worked out for
// the nominal feed, Power Drive and Sag moved the level by 2.2 dB RMS over Input
// Trims of -18 to +12 dB, and up to 6.3 dB. If this fails after a voicing change, re-measure kTrimGainDb
// (BuiltinAmpEffectTests --measure-levels).
void TestPowerDriveHoldsAtAnyTrim()
{
    const auto guitar = NominalGuitar("guitar-riff-01.wav");

    if (guitar.empty())
    {
        return;
    }

    double worst = 0.0, squares = 0.0;
    int count = 0;
    Voicing worstAt;

    for (const double voice : {0.0, 1.0})
    {
        for (const int stages : {1, 4})
        {
            for (const double gain : {0.0, 0.6})
            {
                for (const double trim : {-12.0, 12.0})
                {
                    // The default knee: the Character extremes are TestLevelHoldsAtNominal's.
                    Voicing plain{gain, voice, 0.5, stages};
                    plain.trimDb = trim;
                    const double untouched = HeardGainDb(plain, guitar);
                    Voicing driven = plain;
                    driven.powerDrive = 1.0;
                    driven.sag = 0.5;
                    const double moved = HeardGainDb(driven, guitar) - untouched;
                    squares += moved * moved;
                    ++count;

                    if (std::abs(moved) > std::abs(worst))
                    {
                        worst = moved;
                        worstAt = driven;
                    }
                }
            }
        }
    }

    const double rms = std::sqrt(squares / count);
    std::cout << "Level change from Power Drive and Sag at an Input Trim of 12 dB either way: " << rms
              << " dB RMS, worst " << worst << " dB (voice " << worstAt.voice << ", " << worstAt.stages
              << " stages, gain " << worstAt.gain << ", trim " << worstAt.trimDb << " dB, Power Drive "
              << worstAt.powerDrive << ", Sag " << worstAt.sag << ")\n";
    Check(rms < 0.6 && std::abs(worst) < 1.5, "Power Drive and Sag hold the level at any Input Trim");
}

// Power Drive is level-compensated, but it still has to be a distortion control.
void TestPowerDriveAddsDistortion()
{
    const double none = Thd(RenderVoicing({0.45, 0.0, 0.5, 2, 0.0}, Signal::Sine, 0.10, 48000), 220.0, 48000.0);
    const double full = Thd(RenderVoicing({0.45, 0.0, 0.5, 2, 1.0}, Signal::Sine, 0.10, 48000), 220.0, 48000.0);
    std::cout << "Clean voice THD at Power Drive 0 and 1: " << 100.0 * none << "%, " << 100.0 * full << "%\n";
    Check(full > none * 3.0, "Power Drive adds distortion");
}

// The factory presets have to be what their names say. The drive law's quartic
// top leaves the Drive voice a crunch below a Gain of about 0.6, however many
// stages are in, and the hard knee hardly distorts until it is driven: Tight
// Djent once sat at Gain 0.5 and came out less saturated than Classic Crunch.
// So each preset is held to the distortion its name promises on a quiet tone
// (0.05 peak, a soft pick), where a crunch has mostly cleaned up and a
// high-gain preset has not. Re-check these after a voicing change.
void TestFactoryPresetsReachTheirGain()
{
    constexpr double sampleRate = 48000.0;
    constexpr int frames = 48128;
    // Least and most THD each preset may show on the quiet tone, as a fraction.
    const std::map<std::string, std::pair<double, double>> bounds = {
        {"clean-channel", {0.0, 0.02}},
        {"edge-of-breakup", {0.02, 0.10}},
        {"classic-crunch", {0.10, 0.30}},
        {"tight-modern-rhythm", {0.35, 1.0}},
        {"tight-djent", {0.45, 1.0}},
        {"scooped-thrash", {0.30, 1.0}},
        {"vintage-high-gain", {0.30, 1.0}},
        {"singing-lead", {0.35, 1.0}},
        // Sepia's eight.
        {"glass-clean", {0.0, 0.02}},
        {"warm-jazz", {0.0, 0.02}},
        {"chime-combo", {0.015, 0.10}},
        {"tweed-breakup", {0.04, 0.20}},
        {"british-crunch", {0.10, 0.30}},
        {"hot-rodded-stack", {0.30, 1.0}},
        {"doom-wall", {0.25, 1.0}},
        {"liquid-lead", {0.30, 1.0}},
    };

    const auto presets =
        guitarfx::builtin_amp::FactoryPresets(guitarfx::BuildParameterDefs(guitarfx::builtin_amp::kParams));

    for (const auto& preset : presets)
    {
        guitarfx::BuiltinAmpEffect amp;
        amp.Prepare(sampleRate, 256);

        for (const auto& key : preset.parameterOrder)
        {
            amp.SetParam(key, preset.parameters.at(key));
        }

        amp.Reset();
        std::vector<float> input(frames), output(frames);

        for (int i = 0; i < frames; ++i)
        {
            input[i] = static_cast<float>(0.05 * std::sin(2.0 * kPi * 220.0 * i / sampleRate));
        }

        for (int start = 0; start < frames; start += 256)
        {
            amp.ProcessMono(input.data() + start, output.data() + start, std::min(256, frames - start));
        }

        const double thd = Thd(output, 220.0, sampleRate);
        std::cout << "  " << preset.displayName << ": THD " << 100.0 * thd << "% on a quiet tone\n";
        const auto bound = bounds.find(preset.id);
        Check(bound != bounds.end(), "every factory preset has a saturation bound");

        if (bound != bounds.end())
        {
            Check(thd >= bound->second.first && thd <= bound->second.second,
                  (preset.displayName + " distorts as its name says").c_str());
        }
    }
}

// One amp, classic fuzz to modern high gain. At a high-gain setting Character
// has to move every property a player would name: the vintage end is even-order
// and wooly, with low strings blooming into each other; the modern end is
// odd-order, tight and forward in the upper mids. And it has to do that without
// becoming a loudness control.
void TestCharacterRange()
{
    double previousEvenOdd = 1.0e9, previousRumble = 1.0e9, previousBite = -1.0e9;
    double evenOddEnds[2] = {}, rumbleEnds[2] = {}, biteEnds[2] = {}, levelEnds[2] = {};

    for (const double character : {0.0, 0.5, 1.0})
    {
        const Voicing v{0.8, 1.0, character, 2};

        const auto sine = RenderVoicing(v, Signal::Sine, 0.10, 48000);
        double even = 0.0, odd = 0.0;

        for (int h = 2; h <= 15; ++h)
        {
            const double magnitude = ToneMagnitude(sine, 12000, 220.0 * h, 48000.0);
            (h % 2 ? odd : even) += magnitude * magnitude;
        }

        const double evenOdd = std::sqrt(even / odd);

        const auto chord = RenderVoicing(v, Signal::PowerChord, 0.10, 48000);
        const double rumble = 20.0 * std::log10(ToneMagnitude(chord, 12000, 41.06, 48000.0) /
                                                ToneMagnitude(chord, 12000, 82.41, 48000.0));

        const double bite = BandShareDb(RenderVoicing(v, Signal::String, 0.10, 48000), 220.0, 1500.0, 4500.0, 12000.0);

        std::cout << "Character " << character << ": even/odd " << evenOdd << ", 41 Hz power-chord rumble " << rumble
                  << " dB, 1.5-4.5 kHz bite " << bite << " dB\n";
        Check(evenOdd < previousEvenOdd, "Character moves the harmonics from even-order towards odd-order");
        Check(rumble < previousRumble, "Character tightens the low end");
        Check(bite > previousBite, "Character brings the upper mids forward");
        previousEvenOdd = evenOdd;
        previousRumble = rumble;
        previousBite = bite;

        if (character != 0.5)
        {
            const int end = character == 0.0 ? 0 : 1;
            evenOddEnds[end] = evenOdd;
            rumbleEnds[end] = rumble;
            biteEnds[end] = bite;
            levelEnds[end] = HeardLevel(v);
        }
    }

    Check(evenOddEnds[0] > 5.0 * evenOddEnds[1], "the fuzz end is decisively more even-order than the modern end");
    Check(rumbleEnds[0] - rumbleEnds[1] > 4.0, "the fuzz end is audibly looser than the modern end");
    Check(biteEnds[1] - biteEnds[0] > 3.0, "the modern end is audibly sharper than the fuzz end");
    Check(std::abs(levelEnds[1] - levelEnds[0]) < 2.0, "Character changes the voicing, not the loudness");
}

// Sag is the supply giving way under a hard-driven power stage. Its envelope
// has to pull the stage's ceiling down, so the same playing clips harder and
// loud notes are squeezed more than soft ones: more harmonics and less dynamic
// range, not just less level. It once scaled the signal and the ceiling
// together, which left the clipping exactly where it was: the same spectrum at
// every setting, only quieter. The level it costs is made up at the nominal
// level (TestLevelHoldsAtNominal); these quieter tones only have to stay close.
void TestSagLowersPowerCeiling()
{
    for (const double voice : {0.0, 1.0})
    {
        const Voicing steady{0.45, voice, 0.5, 2, 1.0, 0.0};
        Voicing sagging = steady;
        sagging.sag = 1.0;

        const double steadyThd = Thd(RenderVoicing(steady, Signal::Sine, 0.10, 48000), 220.0, 48000.0);
        const double saggingThd = Thd(RenderVoicing(sagging, Signal::Sine, 0.10, 48000), 220.0, 48000.0);
        const double steadySpan =
            HeardDb(RenderVoicing(steady, Signal::Sine)) - HeardDb(RenderVoicing(steady, Signal::Sine, 0.02));
        const double saggingSpan =
            HeardDb(RenderVoicing(sagging, Signal::Sine)) - HeardDb(RenderVoicing(sagging, Signal::Sine, 0.02));
        const double levelDrop = HeardLevel(steady) - HeardLevel(sagging);
        std::cout << "Sag at full Power Drive, voice " << voice << ": THD " << 100.0 * steadyThd << "% to "
                  << 100.0 * saggingThd << "%, 0.10/0.02 sine span " << steadySpan << " to " << saggingSpan
                  << " dB, heard level " << -levelDrop << " dB\n";
        Check(saggingThd > steadyThd * 1.08, "sag clips the power stage harder, not just more quietly");
        Check(saggingSpan < steadySpan - 0.6, "sag squeezes loud playing more than soft");
        Check(std::abs(levelDrop) < 2.0, "full sag is a feel control, not a volume control");
    }

    // With no Power Drive nothing clips there, so there is no ceiling to pull down.
    const auto withoutSag = RenderVoicing({0.45, 1.0, 0.5, 2, 0.0, 0.0}, Signal::PowerChord);
    const auto withSag = RenderVoicing({0.45, 1.0, 0.5, 2, 0.0, 1.0}, Signal::PowerChord);
    Check(withoutSag == withSag, "sag leaves a power stage that is not driven alone");
}

// Character moves filter corners, clipper knees and bias points while a note
// rings. Any of those stepping instead of gliding shows up as a spike in the
// second difference of the output. This has to listen to a nearly clean tone:
// a saturated one has sharp edges of its own every cycle that bury a step.
void TestCharacterSweepIsSmooth()
{
    constexpr double sampleRate = 48000.0;
    constexpr int frames = 48000, change = 24000;
    guitarfx::BuiltinAmpEffect amp;
    amp.Prepare(sampleRate, 64);
    amp.SetParam("gain", 0.0);
    amp.SetParam("voice", 0.0);
    amp.SetParam("character", 0.0);
    amp.Reset();

    std::vector<float> input(frames), output(frames);

    for (int i = 0; i < frames; ++i)
    {
        input[i] = static_cast<float>(0.1 * std::sin(2.0 * kPi * 220.0 * i / sampleRate));
    }

    for (int start = 0; start < frames; start += 64)
    {
        if (start == change)
        {
            amp.SetParam("character", 1.0);
        }

        float* in[2] = {input.data() + start, input.data() + start};
        float* out[2] = {output.data() + start, nullptr};
        amp.Process(in, out, 64);
    }

    auto worstCurvature = [&](int from, int to) {
        double worst = 0.0;

        for (int i = from + 1; i < to - 1; ++i)
        {
            worst = std::max(worst, std::abs(static_cast<double>(output[i + 1]) - 2.0 * output[i] + output[i - 1]));
        }

        return worst;
    };
    const double steady = std::max(worstCurvature(12000, change), worstCurvature(frames - 8000, frames));
    const double sweep = worstCurvature(change, change + 4800);
    std::cout << "Character sweep: worst curvature " << sweep << " against " << steady << " at rest\n";
    Check(sweep < steady * 1.5, "a Character change glides instead of clicking");
}

// Preamp Stages adds or removes clip stages and their coupling filters, which
// moves the phase and shape of a ringing note at once: switched, that is a
// click. It has to fade, both ways, on the same nearly clean tone the Character
// sweep listens to.
void TestStageChangeIsSmooth()
{
    constexpr double sampleRate = 48000.0;
    constexpr int frames = 72000, up = 24000, down = 48000;
    guitarfx::BuiltinAmpEffect amp;
    amp.Prepare(sampleRate, 64);
    amp.SetParam("gain", 0.0);
    amp.SetParam("voice", 0.0);
    amp.SetParam("stageCount", 1);
    amp.Reset();

    std::vector<float> input(frames), output(frames);

    for (int i = 0; i < frames; ++i)
    {
        input[i] = static_cast<float>(0.1 * std::sin(2.0 * kPi * 220.0 * i / sampleRate));
    }

    for (int start = 0; start < frames; start += 64)
    {
        if (start == up || start == down)
        {
            amp.SetParam("stageCount", start == up ? 4 : 1);
        }

        amp.ProcessMono(input.data() + start, output.data() + start, 64);
    }

    const double steady = std::max({WorstCurvature(output, 12000, up), WorstCurvature(output, down - 12000, down),
                                    WorstCurvature(output, frames - 12000, frames)});
    const double adding = WorstCurvature(output, up, up + 4800);
    const double removing = WorstCurvature(output, down, down + 4800);
    std::cout << "Preamp Stages 1 to 4 and back: worst curvature " << adding << " and " << removing << " against "
              << steady << " at rest\n";
    Check(adding < steady * 1.5, "adding preamp stages fades them in instead of clicking");
    Check(removing < steady * 1.5, "removing preamp stages fades them out instead of clicking");
}

// A stage switched back in must not pick up where its coupling filter was left
// the last time it played, a different note ago: that is a transient of the
// old note in the new one. Two amps hear the same playing; one had stages 3
// and 4 in at the start and the other never did. Once both are back at four
// stages they must agree from the first sample of the fade.
void TestReturningStageStartsFresh()
{
    constexpr double sampleRate = 48000.0;
    constexpr int block = 64, frames = 96000, dropped = 12032, restored = 48000;
    guitarfx::BuiltinAmpEffect former, fresh;

    for (auto* amp : {&former, &fresh})
    {
        amp->SetParam("gain", 0.8);
        amp->SetParam("voice", 1.0);
        amp->SetParam("stageCount", amp == &former ? 4 : 2);
        amp->Prepare(sampleRate, block);
    }

    // A loud low chord while stages 3 and 4 are in, then a quieter high note.
    std::vector<float> input(frames);

    for (int i = 0; i < frames; ++i)
    {
        const double t = i / sampleRate;
        input[i] = static_cast<float>(i < dropped + 4800
                                          ? 0.3 * (std::sin(2.0 * kPi * 82.41 * t) + std::sin(2.0 * kPi * 123.47 * t))
                                          : 0.05 * std::sin(2.0 * kPi * 659.3 * t));
    }

    std::vector<float> formerOut(frames), freshOut(frames);

    for (int start = 0; start < frames; start += block)
    {
        if (start == dropped)
        {
            former.SetParam("stageCount", 2);
        }

        if (start == restored)
        {
            former.SetParam("stageCount", 4);
            fresh.SetParam("stageCount", 4);
        }

        former.ProcessMono(input.data() + start, formerOut.data() + start, block);
        fresh.ProcessMono(input.data() + start, freshOut.data() + start, block);
    }

    double before = 0.0, after = 0.0;

    for (int i = restored - 2400; i < frames; ++i)
    {
        double& worst = i < restored ? before : after;
        worst = std::max(worst, std::abs(static_cast<double>(formerOut[i]) - freshOut[i]));
    }

    std::cout << "Stages 3 and 4 back in: worst difference from an amp that never had them " << after << " (" << before
              << " just before)\n";
    Check(after < 1.0e-5, "a stage switched back in starts fresh, not where it was left");
}

// Voice picks a channel. It is a switch like Bright, so anything that sends it
// a value between the two (a host's automation lane) lands on one of them.
void TestVoiceIsASwitch()
{
    guitarfx::BuiltinAmpEffect amp;
    amp.SetParam("voice", 0.3);
    const bool low = amp.GetParam("voice") == 0.0;
    amp.SetParam("voice", 0.7);
    Check(low && amp.GetParam("voice") == 1.0, "Voice lands on Clean or Drive, never between");
    Check(RenderVoicing({0.45, 0.7}, Signal::Sine) == RenderVoicing({0.45, 1.0}, Signal::Sine),
          "a Voice between the two sounds exactly like the nearer channel");
}

// The amp is voiced for a guitar at -18 dBFS. With the nominal operating level
// set somewhere else, a guitar there has to drive it exactly as hard, and come
// out at that level: the same output, 6 dB up.
void TestNominalLevelIsFollowed()
{
    const Voicing v{0.6, 1.0, 0.5, 3, 0.5};
    const auto atDefault = RenderVoicing(v, Signal::PowerChord);
    guitarfx::SetNominalOperatingLevelDbfs(-12.0);
    const double up = guitarfx::DbToLinearGain(6.0);
    const auto raised = RenderVoicing(v, Signal::PowerChord, 0.10 * up);
    guitarfx::SetNominalOperatingLevelDbfs(guitarfx::kDefaultNominalOperatingLevelDbfs);

    double worst = 0.0, peak = 0.0;

    for (std::size_t i = 0; i < raised.size(); ++i)
    {
        worst = std::max(worst, std::abs(raised[i] - up * atDefault[i]));
        peak = std::max(peak, std::abs(up * atDefault[i]));
    }

    std::cout << "Nominal level at -12 dBFS: worst difference from the -18 dBFS render, 6 dB up, " << worst / peak
              << " of its peak\n";
    Check(worst < 1.0e-4 * peak, "a guitar at the nominal level drives the amp the same, wherever that level is");
}

// A NaN gain once survived the range clamp and indexed the level table with it,
// which crashed the audio thread in a build with strict floating point. Every
// control has to ignore a value that is not a number.
void TestNonFiniteValuesIgnored()
{
    // Made at run time: clang's -ffast-math (the Android build's) folds a NaN or
    // infinity constant away before it reaches the amp.
    volatile std::uint64_t nanBits = 0x7ff8000000000000ull, infinityBits = 0x7ff0000000000000ull;
    const auto nan = std::bit_cast<double>(static_cast<std::uint64_t>(nanBits));
    const auto infinity = std::bit_cast<double>(static_cast<std::uint64_t>(infinityBits));
    bool kept = true, finite = true;

    for (const auto& spec : guitarfx::builtin_amp::kParams)
    {
        for (const double value : {nan, infinity, -infinity})
        {
            guitarfx::BuiltinAmpEffect amp;
            amp.Prepare(48000.0, 64);
            const double before = amp.GetParam(spec.id);
            amp.SetParam(spec.id, value);
            kept = kept && amp.GetParam(spec.id) == before;

            std::vector<float> block(64, 0.05f), out(64);

            for (int b = 0; b < 8; ++b)
            {
                amp.ProcessMono(block.data(), out.data(), 64);
                finite = finite && std::all_of(out.begin(), out.end(), [](float y) { return guitarfx::IsFinite(y); });
            }
        }
    }

    Check(kept, "a control ignores NaN and infinity");
    Check(finite, "the output stays finite after a non-finite control value");
}

// The executor runs the amp on one channel whenever the guitar signal is mono,
// and may flip between that and stereo block by block as the input changes.
// One channel has to sound exactly like the left of two, and coming back to
// stereo the right channel has to pick up exactly where the left is, because
// its input was the left's the whole time it sat still.
void TestMonoPath()
{
    constexpr double sampleRate = 48000.0;
    constexpr int block = 64;
    std::vector<float> note(block * 400);

    for (std::size_t i = 0; i < note.size(); ++i)
    {
        const double t = static_cast<double>(i) / sampleRate;
        note[i] = static_cast<float>(0.1 * (std::sin(2.0 * kPi * 110.0 * t) + 0.4 * std::sin(2.0 * kPi * 330.0 * t)) *
                                     (1.0 + 0.5 * std::sin(2.0 * kPi * 3.0 * t)));
    }

    // Sag, power drive, four stages, a Character between two knees and every
    // tone control off centre, so every piece of per-channel state carries
    // something. A filter at a flat setting is an identity whose state stays
    // at zero, and would hide a gap in the copy.
    auto configure = [&](guitarfx::BuiltinAmpEffect& amp) {
        amp.SetParam("gain", 0.8);
        amp.SetParam("voice", 1.0);
        amp.SetParam("stageCount", 4);
        amp.SetParam("character", 0.3);
        amp.SetParam("sag", 0.6);
        amp.SetParam("powerDrive", 0.5);
        amp.SetParam("bass", 0.7);
        amp.SetParam("middle", 0.3);
        amp.SetParam("treble", 0.65);
        amp.SetParam("contour", 0.4);
        amp.SetParam("presence", 0.7);
        amp.SetParam("bright", 1.0);
        amp.Prepare(sampleRate, block);
    };

    guitarfx::BuiltinAmpEffect mono, stereo;
    configure(mono);
    configure(stereo);
    Check(mono.SupportsMonoProcessing(), "the amp offers the executor its mono path");

    std::vector<float> monoOut(block), left(block), right(block);
    double worstMismatch = 0.0;

    for (int b = 0; b < 200; ++b)
    {
        float* in = note.data() + b * block;
        mono.ProcessMono(in, monoOut.data(), block);
        float* ins[2] = {in, in};
        float* outs[2] = {left.data(), right.data()};
        stereo.Process(ins, outs, block);

        for (int i = 0; i < block; ++i)
        {
            worstMismatch = std::max(worstMismatch, std::abs(static_cast<double>(monoOut[i] - left[i])));
        }
    }

    Check(worstMismatch == 0.0, "one channel sounds exactly like the left of two");

    // Stereo, then a mono stretch, then stereo again with the input still
    // identical on both sides: the right channel must match the left at once.
    guitarfx::BuiltinAmpEffect flipping;
    configure(flipping);
    double worstSplit = 0.0;

    for (int b = 0; b < 400; ++b)
    {
        float* in = note.data() + b * block;

        if (b >= 100 && b < 250)
        {
            flipping.ProcessMono(in, monoOut.data(), block);
            continue;
        }

        float* ins[2] = {in, in};
        float* outs[2] = {left.data(), right.data()};
        flipping.Process(ins, outs, block);

        if (b >= 250)
        {
            for (int i = 0; i < block; ++i)
            {
                worstSplit = std::max(worstSplit, std::abs(static_cast<double>(left[i] - right[i])));
            }
        }
    }

    std::cout << "Mono path: worst difference from stereo left " << worstMismatch
              << ", worst left/right split after a mono stretch " << worstSplit << "\n";
    Check(worstSplit == 0.0, "the right channel comes back from a mono stretch in step with the left");
}

// An impulse up to 4x and back down, `block` host samples at a time (at most half a stage's
// kMaxBlock, as the amp's chunks are: the inner stage takes twice as many).
std::vector<float> HalfbandImpulseResponse(int block)
{
    guitarfx::BuiltinAmpHalfband2x upFirst, upSecond, downSecond, downFirst;
    upFirst.Prepare();
    upSecond.Prepare();
    downSecond.Prepare();
    downFirst.Prepare();

    std::vector<float> input(128, 0.0f), output(128);
    input[0] = 1.0f;
    std::vector<float> twice(2 * block), fourTimes(4 * block);

    for (int at = 0; at < static_cast<int>(output.size()); at += block)
    {
        const int n = std::min(block, static_cast<int>(output.size()) - at);
        upFirst.UpsampleBlock(input.data() + at, n, twice.data());
        upSecond.UpsampleBlock(twice.data(), 2 * n, fourTimes.data());
        downSecond.DownsampleBlock(fourTimes.data(), 2 * n, twice.data());
        downFirst.DownsampleBlock(twice.data(), n, output.data() + at);
    }

    return output;
}

void TestHalfbandLatency()
{
    const auto output = HalfbandImpulseResponse(64);
    bool sameInAnyBlocks = true;

    for (const int block : {1, 7, 17, 47})
    {
        sameInAnyBlocks = sameInAnyBlocks && HalfbandImpulseResponse(block) == output;
    }

    Check(sameInAnyBlocks, "the halfband stages give the same output in blocks of any size");

    const auto peak = std::max_element(output.begin(), output.end());
    Check(std::distance(output.begin(), peak) == 48, "4x halfband latency is 48 host samples");
    double impulseSum = 0.0;

    for (const float sample : output)
    {
        impulseSum += sample;
    }

    Check(std::abs(impulseSum - 1.0) < 0.002, "4x halfband has unity DC gain");

    guitarfx::BuiltinAmpEffect amp;
    amp.Prepare(48000.0, 256);
    Check(amp.GetLatencySamples() == 48, "48 kHz processing reports 4x latency");
    amp.Prepare(96000.0, 256);
    Check(amp.GetLatencySamples() == 32, "96 kHz processing reports 2x latency");
    amp.Prepare(192000.0, 256);
    Check(amp.GetLatencySamples() == 0, "192 kHz processing reports no resampling latency");
}

void TestDynamicsAndBlocks()
{
    const auto cleanQuiet = Render(48000.0, 0.02, 440.0, 127, 0.0, 2);
    const auto cleanLoud = Render(48000.0, 0.10, 440.0, 127, 0.0, 2);
    const auto driveQuiet = Render(48000.0, 0.02, 440.0, 127, 1.0, 2);
    const auto driveLoud = Render(48000.0, 0.10, 440.0, 127, 1.0, 2);
    const double cleanRatio = Rms(cleanLoud, 4096) / Rms(cleanQuiet, 4096);
    const double driveRatio = Rms(driveLoud, 4096) / Rms(driveQuiet, 4096);
    std::cout << "Clean RMS at 0.10 input: " << Rms(cleanLoud, 4096) << ", drive RMS: " << Rms(driveLoud, 4096) << '\n';
    std::cout << "Clean 0.10/0.02 RMS ratio: " << cleanRatio << ", drive ratio: " << driveRatio << '\n';
    Check(cleanRatio > 2.0, "clean path retains useful input dynamics");
    Check(driveRatio > 1.1, "drive path retains useful input dynamics");
    Check(Rms(driveQuiet, 4096) > Rms(cleanQuiet, 4096) * 1.25, "drive voice increases saturation and level");

    const auto oneBlock = Render(48000.0, 0.08, 1234.0, 48000, 1.0, 4, 0.5);
    const auto manyBlocks = Render(48000.0, 0.08, 1234.0, 127, 1.0, 4, 0.5);
    double maximumDifference = 0.0;

    for (std::size_t i = 0; i < oneBlock.size(); ++i)
    {
        maximumDifference = std::max(maximumDifference, std::abs(static_cast<double>(oneBlock[i] - manyBlocks[i])));
    }

    Check(maximumDifference < 1.0e-6, "block partition does not change the output");
}

void TestAliasing()
{
    const auto brightDrive = Render(48000.0, 0.15, 7000.0, 256, 1.0, 4);
    const double fundamental = ToneMagnitude(brightDrive, 24000, 7000.0, 48000.0);
    const double alias = ToneMagnitude(brightDrive, 24000, 13000.0, 48000.0);
    const double aliasDb = 20.0 * std::log10(std::max(alias, 1.0e-12) / std::max(fundamental, 1.0e-12));
    std::cout << "7 kHz drive fundamental: " << fundamental << ", 13 kHz folded harmonic: " << aliasDb << " dBc\n";
    Check(fundamental > 1.0e-4, "alias test retains a measurable fundamental");
    Check(aliasDb < -35.0, "high-gain folded fifth harmonic is attenuated");

    // A 192 kHz render uses the same amplifier without oversampling. Taking
    // every fourth sample without a decimation filter shows the fold that the
    // 48 kHz half-band path must reject.
    const auto highRate = Render(192000.0, 0.15, 7000.0, 256, 1.0, 4);
    std::vector<float> unfiltered(highRate.size() / 4);

    for (std::size_t i = 0; i < unfiltered.size(); ++i)
    {
        unfiltered[i] = highRate[4 * i];
    }

    const double unfilteredFundamental = ToneMagnitude(unfiltered, 6000, 7000.0, 48000.0);
    const double unfilteredAlias = ToneMagnitude(unfiltered, 6000, 13000.0, 48000.0);
    const double unfilteredDb =
        20.0 * std::log10(std::max(unfilteredAlias, 1.0e-12) / std::max(unfilteredFundamental, 1.0e-12));
    std::cout << "Same model without decimation filtering: " << unfilteredDb << " dBc\n";
    Check(aliasDb < unfilteredDb - 20.0, "half-band path suppresses aliasing by at least 20 dB");
}
} // namespace

int main(int argc, char** argv)
{
    if (argc > 1 && std::string(argv[1]) == "--measure-levels")
    {
        const auto guitar = MeasuringGuitar();

        if (std::any_of(guitar.begin(), guitar.end(), [](const auto& take) { return take.empty(); }))
        {
            std::cerr << "The demo guitar did not load from " << GUITARFX_DEMO_AUDIO_DIR << '\n';
            return 1;
        }

        MeasureLevelTable(guitar);
        MeasureTrimTable(guitar);
        FitPowerFeed(guitar);
        return 0;
    }

    TestHalfbandLatency();
    TestGainRange();
    TestLevelHoldsAtNominal();
    TestPowerDriveHoldsAtAnyTrim();
    TestPowerDriveAddsDistortion();
    TestFactoryPresetsReachTheirGain();
    TestCharacterRange();
    TestSagLowersPowerCeiling();
    TestCharacterSweepIsSmooth();
    TestStageChangeIsSmooth();
    TestReturningStageStartsFresh();
    TestVoiceIsASwitch();
    TestNominalLevelIsFollowed();
    TestNonFiniteValuesIgnored();
    TestMonoPath();
    TestDynamicsAndBlocks();
    TestAliasing();

    if (failures)
    {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "BuiltinAmpEffectTests passed\n";
    return 0;
}
