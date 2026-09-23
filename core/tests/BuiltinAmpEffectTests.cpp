#include "dsp/effects/BuiltinAmpEffect.h"
#include "dsp/effects/BuiltinAmpOversampling.h"
#include "dsp/effects/BuiltinAmpVoicing.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace
{
constexpr double kPi = 3.14159265358979323846;
int failures = 0;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::vector<float> Render(double sampleRate, double amplitude, double frequency, int blockSize, double voice,
                          int stages, double powerDrive = 0.0, double gain = 0.45)
{
    constexpr int frames = 48000;
    guitarfx::BuiltinAmpEffect amp;
    amp.SetParam("voice", voice);
    amp.SetParam("stageCount", stages);
    amp.SetParam("powerDrive", powerDrive);
    amp.SetParam("gain", gain);
    amp.Prepare(sampleRate, blockSize);

    std::vector<float> input(frames), output(frames);
    for (int i = 0; i < frames; ++i)
    {
        input[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * frequency * i / sampleRate));
    }
    for (int start = 0; start < frames; start += blockSize)
    {
        const int count = std::min(blockSize, frames - start);
        // The left of a stereo render bit for bit (TestMonoPath), without running the right.
        amp.ProcessMono(input.data() + start, output.data() + start, count);
    }
    return output;
}

double Rms(const std::vector<float>& signal, int start)
{
    double power = 0.0;
    for (std::size_t i = static_cast<std::size_t>(start); i < signal.size(); ++i)
    {
        power += static_cast<double>(signal[i]) * signal[i];
    }
    return std::sqrt(power / static_cast<double>(signal.size() - static_cast<std::size_t>(start)));
}

double ToneMagnitude(const std::vector<float>& signal, int start, double frequency, double sampleRate)
{
    double real = 0.0, imaginary = 0.0;
    for (std::size_t i = static_cast<std::size_t>(start); i < signal.size(); ++i)
    {
        const double phase = 2.0 * kPi * frequency * static_cast<double>(i) / sampleRate;
        real += signal[i] * std::cos(phase);
        imaginary += signal[i] * std::sin(phase);
    }
    return 2.0 * std::hypot(real, imaginary) / static_cast<double>(signal.size() - start);
}

// Harmonic distortion relative to the fundamental, as a fraction.
double Thd(const std::vector<float>& signal, double fundamental, double sampleRate)
{
    const int start = 12000;
    const double first = ToneMagnitude(signal, start, fundamental, sampleRate);
    double harmonics = 0.0;
    for (int h = 2; h <= 12 && fundamental * h < sampleRate * 0.45; ++h)
    {
        const double magnitude = ToneMagnitude(signal, start, fundamental * h, sampleRate);
        harmonics += magnitude * magnitude;
    }
    return std::sqrt(harmonics) / std::max(first, 1.0e-12);
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

enum class Signal
{
    Sine,        // A3, 220 Hz
    String,      // A3 with harmonics falling at ~9 dB/octave, like a pickup
    PowerChord,  // E2 + B2
    Lead         // E5, 659.3 Hz
};

struct Voicing
{
    double gain = 0.45;
    double voice = 1.0;
    double character = 0.5;
    int stages = 2;
};

std::vector<float> RenderVoicing(const Voicing& v, Signal signal, double level = 0.10, int frames = 36000)
{
    constexpr double sampleRate = 48000.0;
    guitarfx::BuiltinAmpEffect amp;
    amp.Prepare(sampleRate, 256);
    amp.SetParam("gain", v.gain);
    amp.SetParam("voice", v.voice);
    amp.SetParam("character", v.character);
    amp.SetParam("stageCount", v.stages);
    amp.Reset();

    std::vector<float> input(frames), output(frames);
    for (int i = 0; i < frames; ++i)
    {
        const double t = i / sampleRate;
        double x = 0.0;
        switch (signal)
        {
        case Signal::Sine:
            x = std::sin(2.0 * kPi * 220.0 * t);
            break;
        case Signal::String:
            for (int h = 1; h <= 30; ++h)
            {
                x += std::sin(2.0 * kPi * 220.0 * h * t) / std::pow(h, 1.5);
            }
            x /= 1.9;
            break;
        case Signal::PowerChord:
            x = 0.5 * (std::sin(2.0 * kPi * 82.41 * t) + std::sin(2.0 * kPi * 123.47 * t));
            break;
        case Signal::Lead:
            x = std::sin(2.0 * kPi * 659.3 * t);
            break;
        }
        input[i] = static_cast<float>(level * x);
    }
    for (int start = 0; start < frames; start += 256)
    {
        const int count = std::min(256, frames - start);
        // The left of a stereo render bit for bit (TestMonoPath), without running the right.
        amp.ProcessMono(input.data() + start, output.data() + start, count);
    }
    return output;
}

struct Biquad
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0, s1 = 0.0, s2 = 0.0;
    double Run(double x)
    {
        const double y = b0 * x + s1;
        s1 = b1 * x - a1 * y + s2;
        s2 = b2 * x - a2 * y;
        return y;
    }
};

// Level as the player hears it, the way the amp's makeup table was measured:
// a generic cab band (2nd-order 90 Hz high pass, 4.5 kHz low pass) and the
// BS.1770 K-weighting shelf, in dB.
double HeardDb(const std::vector<float>& signal, int start = 12000)
{
    constexpr double sampleRate = 48000.0;
    auto pass = [&](double frequency, bool high) {
        const double w = 2.0 * kPi * frequency / sampleRate, c = std::cos(w), a = std::sin(w) / std::sqrt(2.0);
        const double a0 = 1.0 + a, edge = high ? (1.0 + c) / 2.0 : (1.0 - c) / 2.0;
        return Biquad{edge / a0, (high ? -2.0 : 2.0) * edge / a0, edge / a0, -2.0 * c / a0, (1.0 - a) / a0};
    };
    const double A = std::pow(10.0, 4.0 / 40.0), w = 2.0 * kPi * 1681.0 / sampleRate;
    const double c = std::cos(w), al = std::sin(w) / std::sqrt(2.0), sA = std::sqrt(A);
    const double a0 = (A + 1.0) - (A - 1.0) * c + 2.0 * sA * al;
    Biquad shelf{A * ((A + 1.0) + (A - 1.0) * c + 2.0 * sA * al) / a0, -2.0 * A * ((A - 1.0) + (A + 1.0) * c) / a0,
                 A * ((A + 1.0) + (A - 1.0) * c - 2.0 * sA * al) / a0, 2.0 * ((A - 1.0) - (A + 1.0) * c) / a0,
                 ((A + 1.0) - (A - 1.0) * c - 2.0 * sA * al) / a0};
    Biquad highPass = pass(90.0, true), lowPass = pass(4500.0, false);

    double power = 0.0;
    for (std::size_t i = 0; i < signal.size(); ++i)
    {
        const double y = shelf.Run(lowPass.Run(highPass.Run(signal[i])));
        if (static_cast<int>(i) >= start)
        {
            power += y * y;
        }
    }
    return 10.0 * std::log10(power / static_cast<double>(signal.size() - start) + 1.0e-30);
}

double HeardLevel(const Voicing& v)
{
    return (HeardDb(RenderVoicing(v, Signal::Sine)) + HeardDb(RenderVoicing(v, Signal::PowerChord)) +
            HeardDb(RenderVoicing(v, Signal::Lead))) /
           3.0;
}

// Gain and stage count are distortion controls. Driving a cascade harder
// raises its small-signal gain long before it saturates, so without the makeup
// the top of the gain control was 7-17 dB louder than the bottom and every
// comparison was really a loudness comparison. If this fails after a voicing
// change, re-measure kHeardLevelDb.
void TestLevelTracksGain()
{
    for (const double voice : {0.0, 1.0})
    {
        double atHalfGain[5] = {}; // by stage count
        for (const int stages : {1, 2, 4})
        {
            double lowest = 1.0e9, highest = -1.0e9;
            for (const double gain : {0.0, 0.5, 1.0})
            {
                const double level = HeardLevel({gain, voice, 0.5, stages});
                lowest = std::min(lowest, level);
                highest = std::max(highest, level);
                if (gain == 0.5)
                {
                    atHalfGain[stages] = level;
                }
            }
            std::cout << "Heard level span across gain, voice " << voice << ", " << stages
                      << " stages: " << highest - lowest << " dB\n";
            Check(highest - lowest < 1.5, "the gain control changes the distortion, not the loudness");
        }

        // Adding stages is also a distortion control, not a volume control.
        for (const int stages : {1, 4})
        {
            Check(std::abs(atHalfGain[stages] - atHalfGain[2]) < 1.5,
                  "the stage count changes the distortion, not the loudness");
        }
    }

    // The Character extremes are not in the table; they only have to stay close.
    for (const double character : {0.0, 1.0})
    {
        const double low = HeardLevel({0.0, 1.0, character, 2});
        const double high = HeardLevel({1.0, 1.0, character, 2});
        Check(std::abs(high - low) < 3.0, "the gain control stays level-neutral at the Character extremes");
    }
}

double BandShareDb(const std::vector<float>& signal, double fundamental, double low, double high, double top)
{
    double band = 0.0, all = 0.0;
    for (int h = 1; fundamental * h < top; ++h)
    {
        const double magnitude = ToneMagnitude(signal, 12000, fundamental * h, 48000.0);
        all += magnitude * magnitude;
        if (fundamental * h >= low && fundamental * h < high)
        {
            band += magnitude * magnitude;
        }
    }
    return 10.0 * std::log10(band / all + 1.0e-30);
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

void TestHalfbandLatency()
{
    guitarfx::BuiltinAmpHalfband2x upFirst, upSecond, downSecond, downFirst;
    upFirst.Prepare();
    upSecond.Prepare();
    downSecond.Prepare();
    downFirst.Prepare();

    std::vector<float> output(128);
    for (int i = 0; i < static_cast<int>(output.size()); ++i)
    {
        float a = 0.0f, b = 0.0f, c = 0.0f, d = 0.0f;
        upFirst.Upsample(i == 0 ? 1.0f : 0.0f, a, b);
        upSecond.Upsample(a, c, d);
        const float first = downSecond.Downsample(c, d);
        upSecond.Upsample(b, c, d);
        const float second = downSecond.Downsample(c, d);
        output[i] = downFirst.Downsample(first, second);
    }

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
    std::cout << "Clean RMS at 0.10 input: " << Rms(cleanLoud, 4096)
              << ", drive RMS: " << Rms(driveLoud, 4096) << '\n';
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

void TestAliasingAndPower()
{
    const auto brightDrive = Render(48000.0, 0.15, 7000.0, 256, 1.0, 4);
    const double fundamental = ToneMagnitude(brightDrive, 24000, 7000.0, 48000.0);
    const double alias = ToneMagnitude(brightDrive, 24000, 13000.0, 48000.0);
    const double aliasDb = 20.0 * std::log10(std::max(alias, 1.0e-12) / std::max(fundamental, 1.0e-12));
    std::cout << "7 kHz drive fundamental: " << fundamental << ", 13 kHz folded harmonic: " << aliasDb
              << " dBc\n";
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
    const double unfilteredDb = 20.0 * std::log10(std::max(unfilteredAlias, 1.0e-12) /
                                                 std::max(unfilteredFundamental, 1.0e-12));
    std::cout << "Same model without decimation filtering: " << unfilteredDb << " dBc\n";
    Check(aliasDb < unfilteredDb - 20.0, "half-band path suppresses aliasing by at least 20 dB");

    const auto cleanPower = Render(48000.0, 0.10, 440.0, 256, 0.0, 2, 0.0);
    const auto drivenPower = Render(48000.0, 0.10, 440.0, 256, 0.0, 2, 1.0);
    Check(std::abs(Rms(cleanPower, 4096) - Rms(drivenPower, 4096)) > 0.02,
          "power drive changes the nonlinear output");
}
} // namespace

// Prints kHeardLevelDb for BuiltinAmpVoicing.h. The table has to describe the
// amp's own voicing, so measure it with the makeup taken back out: each cell
// is the heard level divided by the makeup the amp applied there.
void MeasureLevelTable()
{
    std::cout << "inline constexpr float kHeardLevelDb[2][kMaxStages][5] = {\n";
    for (int voice = 0; voice <= 1; ++voice)
    {
        std::cout << "        {";
        for (int stages = 1; stages <= 4; ++stages)
        {
            std::cout << (stages == 1 ? "{" : "         {");
            for (int step = 0; step <= 4; ++step)
            {
                const double gain = step * 0.25;
                double sum = 0.0;
                const float makeupDb =
                    guitarfx::builtin_amp::LevelMakeupDb(static_cast<float>(gain), static_cast<float>(voice), stages);
                for (const double character : {0.0, 0.25, 0.5, 0.75, 1.0})
                {
                    sum += HeardLevel({gain, static_cast<double>(voice), character, stages}) - makeupDb;
                }
                std::cout << std::fixed << std::setprecision(2) << sum / 5.0 << "f" << (step < 4 ? ", " : "");
            }
            std::cout << (stages < 4 ? "},\n" : (voice == 0 ? "}},\n" : "}}};\n"));
        }
    }
}

int main(int argc, char** argv)
{
    if (argc > 1 && std::string(argv[1]) == "--measure-levels")
    {
        MeasureLevelTable();
        return 0;
    }

    TestHalfbandLatency();
    TestGainRange();
    TestLevelTracksGain();
    TestCharacterRange();
    TestCharacterSweepIsSmooth();
    TestMonoPath();
    TestDynamicsAndBlocks();
    TestAliasingAndPower();
    if (failures)
    {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "BuiltinAmpEffectTests passed\n";
    return 0;
}
