/**
 * @file Spatial3DEffectTests.cpp
 * @brief Tests for the Spatial 3D positioner and its motion engine.
 *
 * These tests verify the perceptual cues the effect claims to deliver, rather
 * than just that it produces output:
 *   - interaural time difference sign and magnitude versus azimuth
 *   - interaural level difference monotonicity and head shadow
 *   - front/back spectral tilt
 *   - elevation notch sweep
 *   - inverse-distance level law
 *   - latency-compensated dry blend
 *   - motion trajectory rate, tempo sync, determinism and pitch stability
 *   - every factory preset being complete, in range and well behaved
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/Spatial3DEffect.h"

namespace
{

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 256;
constexpr double kPi = 3.14159265358979323846;

int gFailures = 0;
int gChecks = 0;

void Check(bool condition, const std::string& what, const std::string& detail = "")
{
    ++gChecks;
    if (condition)
    {
        std::cout << "  [PASS] " << what;
    }
    else
    {
        ++gFailures;
        std::cout << "  [FAIL] " << what;
    }
    if (!detail.empty())
    {
        std::cout << "  (" << detail << ")";
    }
    std::cout << std::endl;
}

std::string Num(double v, int precision = 3)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, v);
    return std::string(buffer);
}

std::unique_ptr<guitarfx::EffectProcessor> MakeEffect()
{
    auto effect = guitarfx::EffectRegistry::Instance().Create(guitarfx::EffectGuids::kSpatial3D);
    if (effect)
    {
        effect->Prepare(kSampleRate, kBlockSize);
    }
    return effect;
}

struct Stereo
{
    std::vector<float> left;
    std::vector<float> right;

    explicit Stereo(std::size_t n) : left(n, 0.0f), right(n, 0.0f)
    {
    }

    [[nodiscard]] std::size_t Size() const
    {
        return left.size();
    }
};

Stereo Sine(std::size_t samples, double frequency, double amplitude = 0.5)
{
    Stereo s(samples);
    for (std::size_t i = 0; i < samples; ++i)
    {
        const auto v =
            static_cast<float>(amplitude * std::sin(2.0 * kPi * frequency * static_cast<double>(i) / kSampleRate));
        s.left[i] = v;
        s.right[i] = v;
    }
    return s;
}

Stereo Noise(std::size_t samples, unsigned seed = 12345, double amplitude = 0.3)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    Stereo s(samples);
    for (std::size_t i = 0; i < samples; ++i)
    {
        const float v = dist(rng) * static_cast<float>(amplitude);
        s.left[i] = v;
        s.right[i] = v;
    }
    return s;
}

/**
 * Noise with a roughly guitar-like spectrum: flat to ~1 kHz then falling at about
 * 12 dB/octave, which is what a speaker cabinet impulse response leaves behind.
 *
 * This matters for any test that judges *loudness*. White noise puts roughly 80% of
 * its power above 4.5 kHz at a 48 kHz sample rate, so a high shelf that is barely
 * audible on a real guitar signal shows up as a huge broadband level change. Judging
 * the spatialiser's loudness stability with white noise would make us "fix" a
 * problem that does not exist for the signal this effect actually processes.
 */
Stereo GuitarLikeNoise(std::size_t samples, double amplitude = 0.25)
{
    Stereo s = Noise(samples, amplitude);
    const double cutoff = 1000.0;
    const float a = static_cast<float>(std::exp(-2.0 * 3.14159265358979323846 * cutoff / kSampleRate));
    float z0 = 0.0f;
    float z1 = 0.0f;
    for (std::size_t i = 0; i < samples; ++i)
    {
        z0 = (1.0f - a) * s.left[i] + a * z0;
        z1 = (1.0f - a) * z0 + a * z1;
        s.left[i] = z1 * 3.0f;
        s.right[i] = s.left[i];
    }
    return s;
}

Stereo Run(guitarfx::EffectProcessor& effect, const Stereo& input)
{
    Stereo output(input.Size());
    const int total = static_cast<int>(input.Size());
    for (int offset = 0; offset < total; offset += kBlockSize)
    {
        const int len = std::min(kBlockSize, total - offset);
        float* in[2] = {const_cast<float*>(input.left.data()) + offset,
                        const_cast<float*>(input.right.data()) + offset};
        float* out[2] = {output.left.data() + offset, output.right.data() + offset};
        effect.Process(in, out, len);
    }
    return output;
}

double Rms(const std::vector<float>& v, std::size_t from = 0)
{
    if (from >= v.size())
    {
        return 0.0;
    }
    double sum = 0.0;
    for (std::size_t i = from; i < v.size(); ++i)
    {
        sum += static_cast<double>(v[i]) * static_cast<double>(v[i]);
    }
    return std::sqrt(sum / static_cast<double>(v.size() - from));
}

double Db(double ratio)
{
    return 20.0 * std::log10(std::max(ratio, 1.0e-12));
}

/** Lag (in samples) at which left best matches right. Positive = source on the right. */
double CrossCorrelationLag(const std::vector<float>& left, const std::vector<float>& right, int maxLag,
                           std::size_t skip)
{
    double best = -std::numeric_limits<double>::infinity();
    int bestLag = 0;
    for (int lag = -maxLag; lag <= maxLag; ++lag)
    {
        double sum = 0.0;
        for (std::size_t i = skip; i < left.size(); ++i)
        {
            const auto j = static_cast<long long>(i) - lag;
            if (j < 0 || j >= static_cast<long long>(right.size()))
            {
                continue;
            }
            sum += static_cast<double>(left[i]) * static_cast<double>(right[static_cast<std::size_t>(j)]);
        }
        if (sum > best)
        {
            best = sum;
            bestLag = lag;
        }
    }
    return static_cast<double>(bestLag);
}

int ZeroCrossings(const std::vector<float>& v, std::size_t from)
{
    int count = 0;
    for (std::size_t i = from + 1; i < v.size(); ++i)
    {
        if ((v[i - 1] <= 0.0f && v[i] > 0.0f) || (v[i - 1] >= 0.0f && v[i] < 0.0f))
        {
            ++count;
        }
    }
    return count;
}

bool AllFinite(const Stereo& s)
{
    for (std::size_t i = 0; i < s.Size(); ++i)
    {
        if (!std::isfinite(s.left[i]) || !std::isfinite(s.right[i]))
        {
            return false;
        }
    }
    return true;
}

float PeakAbs(const Stereo& s)
{
    float peak = 0.0f;
    for (std::size_t i = 0; i < s.Size(); ++i)
    {
        peak = std::max({peak, std::fabs(s.left[i]), std::fabs(s.right[i])});
    }
    return peak;
}

/** Applies a static position with the motion engine and room disabled. */
void SetStatic(guitarfx::EffectProcessor& effect, double azimuth, double elevation, double distance,
               double roomAmount = 0.0)
{
    effect.SetParam("motionMode", 0.0);
    effect.SetParam("roomAmount", roomAmount);
    effect.SetParam("mix", 1.0);
    effect.SetParam("listenMode", 0.0);
    effect.SetParam("delayMode", 0.0);
    effect.SetParam("azimuth", azimuth);
    effect.SetParam("elevation", elevation);
    effect.SetParam("distance", distance);
    effect.Reset();
}

// ─────────────────────────────────────────────────────────────────────────────

void TestRegistration()
{
    std::cout << "\nRegistration and factory presets" << std::endl;

    auto info = guitarfx::EffectRegistry::Instance().GetTypeInfo(guitarfx::EffectGuids::kSpatial3D);
    Check(info.has_value(), "effect type is registered");
    if (!info.has_value())
    {
        return;
    }

    Check(info->requiresTempo, "declares tempo requirement so sync divisions receive BPM");
    Check(!info->parameters.empty(), "exposes parameters", std::to_string(info->parameters.size()));
    Check(info->presets.size() >= 11, "ships the animated panning presets",
          std::to_string(info->presets.size()) + " presets");

    Check(guitarfx::EffectRegistry::Instance().Resolve("spatial_3d") == guitarfx::EffectGuids::kSpatial3D,
          "legacy alias resolves to the canonical type id");

    // Presets are copied wholesale into a graph node, so a partial preset would
    // silently inherit whatever the node held before. Every preset must be complete.
    bool allComplete = true;
    bool allInRange = true;
    std::string firstProblem;
    for (const auto& preset : info->presets)
    {
        for (const auto& param : info->parameters)
        {
            auto it = preset.parameters.find(param.id);
            if (it == preset.parameters.end())
            {
                allComplete = false;
                if (firstProblem.empty())
                {
                    firstProblem = preset.id + " missing " + param.id;
                }
                continue;
            }
            if (it->second < param.minValue - 1e-9 || it->second > param.maxValue + 1e-9)
            {
                allInRange = false;
                if (firstProblem.empty())
                {
                    firstProblem = preset.id + "." + param.id + " = " + Num(it->second) + " out of range";
                }
            }
        }
        for (const auto& ordered : preset.parameterOrder)
        {
            if (preset.parameters.find(ordered) == preset.parameters.end())
            {
                allComplete = false;
                if (firstProblem.empty())
                {
                    firstProblem = preset.id + " orders unknown parameter " + ordered;
                }
            }
        }
    }
    Check(allComplete, "every preset specifies every parameter", firstProblem);
    Check(allInRange, "every preset value is within its declared range", firstProblem);

    bool hasMotion = false;
    for (const auto& preset : info->presets)
    {
        auto it = preset.parameters.find("motionMode");
        if (it != preset.parameters.end() && it->second > 0.5)
        {
            hasMotion = true;
            break;
        }
    }
    Check(hasMotion, "at least one preset animates the position");
}

void TestInterauralTimeDifference()
{
    std::cout << "\nInteraural time difference" << std::endl;

    const std::size_t samples = static_cast<std::size_t>(kSampleRate * 0.5);
    const Stereo input = Sine(samples, 300.0);

    // Woodworth model at full lateral displacement.
    const double expectedFullItd =
        (guitarfx::spatial3d::kHeadRadiusM / guitarfx::spatial3d::kSpeedOfSoundMs) * (kPi * 0.5 + 1.0) * kSampleRate;

    auto effect = MakeEffect();
    Check(effect != nullptr, "effect instantiates");
    if (!effect)
    {
        return;
    }

    SetStatic(*effect, 90.0, 0.0, 1.5);
    const Stereo right = Run(*effect, input);
    const double lagRight = CrossCorrelationLag(right.left, right.right, 64, samples / 4);

    SetStatic(*effect, -90.0, 0.0, 1.5);
    const Stereo left = Run(*effect, input);
    const double lagLeft = CrossCorrelationLag(left.left, left.right, 64, samples / 4);

    SetStatic(*effect, 0.0, 0.0, 1.5);
    const Stereo centre = Run(*effect, input);
    const double lagCentre = CrossCorrelationLag(centre.left, centre.right, 64, samples / 4);

    Check(std::fabs(lagCentre) <= 1.0, "centred source has no interaural delay", Num(lagCentre, 1) + " samples");
    Check(lagRight > 0.0, "source on the right reaches the right ear first", Num(lagRight, 1) + " samples");
    Check(lagLeft < 0.0, "source on the left reaches the left ear first", Num(lagLeft, 1) + " samples");
    Check(std::fabs(lagRight - expectedFullItd) <= 3.0, "ITD magnitude matches the Woodworth model",
          "measured " + Num(lagRight, 1) + " vs expected " + Num(expectedFullItd, 1));
    Check(std::fabs(lagRight + lagLeft) <= 2.0, "ITD is symmetric about the median plane",
          Num(lagRight, 1) + " vs " + Num(lagLeft, 1));

    // Elevation collapses the lateral component; directly overhead there is no ITD.
    SetStatic(*effect, 90.0, 90.0, 1.5);
    const Stereo overhead = Run(*effect, input);
    const double lagOverhead = CrossCorrelationLag(overhead.left, overhead.right, 64, samples / 4);
    Check(std::fabs(lagOverhead) <= 1.0, "overhead source has no lateral delay", Num(lagOverhead, 1) + " samples");

    // Speaker mode removes ITD, which does not survive crosstalk or mono fold-down.
    SetStatic(*effect, 90.0, 0.0, 1.5);
    effect->SetParam("listenMode", 1.0);
    effect->Reset();
    const Stereo speakers = Run(*effect, input);
    const double lagSpeakers = CrossCorrelationLag(speakers.left, speakers.right, 64, samples / 4);
    Check(std::fabs(lagSpeakers) <= 1.0, "speaker mode removes interaural delay", Num(lagSpeakers, 1) + " samples");
}

void TestInterauralLevelDifference()
{
    std::cout << "\nInteraural level difference and head shadow" << std::endl;

    const std::size_t samples = static_cast<std::size_t>(kSampleRate * 0.5);
    const Stereo input = Noise(samples);

    auto effect = MakeEffect();
    if (!effect)
    {
        return;
    }

    std::vector<double> ilds;
    for (double azimuth : {0.0, 30.0, 60.0, 90.0})
    {
        SetStatic(*effect, azimuth, 0.0, 1.5);
        const Stereo out = Run(*effect, input);
        ilds.push_back(Db(Rms(out.right, samples / 4) / std::max(Rms(out.left, samples / 4), 1e-12)));
    }

    Check(std::fabs(ilds[0]) < 0.5, "centred source is level-balanced", Num(ilds[0], 2) + " dB");
    Check(ilds[1] > ilds[0] && ilds[2] > ilds[1] && ilds[3] > ilds[2],
          "level difference increases monotonically with azimuth",
          Num(ilds[0], 1) + " / " + Num(ilds[1], 1) + " / " + Num(ilds[2], 1) + " / " + Num(ilds[3], 1) + " dB");
    Check(ilds[3] >= 8.0, "hard right strongly favours the right ear", Num(ilds[3], 1) + " dB");

    // Rotating the source must not change its loudness, otherwise an orbit preset
    // would pump in level as it travels. Judged on a guitar-like spectrum, not white
    // noise — see GuitarLikeNoise for why that distinction decides the result.
    const Stereo musical = GuitarLikeNoise(samples);
    double minRms = std::numeric_limits<double>::max();
    double maxRms = 0.0;
    for (double azimuth = -180.0; azimuth <= 180.0; azimuth += 30.0)
    {
        SetStatic(*effect, azimuth, 0.0, 1.5);
        const Stereo out = Run(*effect, musical);
        const double power = std::sqrt(Rms(out.left, samples / 4) * Rms(out.left, samples / 4) +
                                       Rms(out.right, samples / 4) * Rms(out.right, samples / 4));
        minRms = std::min(minRms, power);
        maxRms = std::max(maxRms, power);
    }
    Check(Db(maxRms / minRms) < 4.0, "loudness stays stable as the source rotates",
          Num(Db(maxRms / minRms), 2) + " dB spread");
}

void TestFrontBackAndElevation()
{
    std::cout << "\nFront/back and elevation spectral cues" << std::endl;

    const std::size_t samples = static_cast<std::size_t>(kSampleRate * 0.3);
    auto effect = MakeEffect();
    if (!effect)
    {
        return;
    }

    auto levelAt = [&](double azimuth, double elevation, double frequency) {
        SetStatic(*effect, azimuth, elevation, 1.5);
        const Stereo out = Run(*effect, Sine(samples, frequency));
        return Rms(out.left, samples / 3);
    };

    const double frontHigh = levelAt(0.0, 0.0, 8000.0);
    const double backHigh = levelAt(180.0, 0.0, 8000.0);
    const double frontLow = levelAt(0.0, 0.0, 200.0);
    const double backLow = levelAt(180.0, 0.0, 200.0);

    Check(Db(frontHigh / backHigh) >= 2.0, "a source behind loses high frequencies",
          Num(Db(frontHigh / backHigh), 2) + " dB");
    Check(std::fabs(Db(frontLow / backLow)) < 2.0, "low frequencies are largely unaffected by front/back",
          Num(Db(frontLow / backLow), 2) + " dB");

    // The pinna notch should sweep upward with elevation, so the low notch
    // frequency is attenuated most when the source is below the listener.
    const double lowNotchBelow = levelAt(0.0, -90.0, 6000.0);
    const double lowNotchAbove = levelAt(0.0, 90.0, 6000.0);
    const double highNotchBelow = levelAt(0.0, -90.0, 11000.0);
    const double highNotchAbove = levelAt(0.0, 90.0, 11000.0);

    Check(lowNotchBelow < lowNotchAbove, "6 kHz is notched more when the source is below",
          Num(Db(lowNotchAbove / lowNotchBelow), 2) + " dB");
    Check(highNotchAbove < highNotchBelow, "11 kHz is notched more when the source is above",
          Num(Db(highNotchBelow / highNotchAbove), 2) + " dB");
}

void TestDistance()
{
    std::cout << "\nDistance" << std::endl;

    const std::size_t samples = static_cast<std::size_t>(kSampleRate * 0.3);
    const Stereo input = Sine(samples, 200.0);
    auto effect = MakeEffect();
    if (!effect)
    {
        return;
    }

    SetStatic(*effect, 0.0, 0.0, 1.5);
    const double near = Rms(Run(*effect, input).left, samples / 3);
    SetStatic(*effect, 0.0, 0.0, 3.0);
    const double far = Rms(Run(*effect, input).left, samples / 3);
    SetStatic(*effect, 0.0, 0.0, 6.0);
    const double further = Rms(Run(*effect, input).left, samples / 3);

    Check(std::fabs(Db(near / far) - 6.02) < 1.0, "doubling the distance halves the level",
          Num(Db(near / far), 2) + " dB");
    Check(std::fabs(Db(far / further) - 6.02) < 1.0, "the inverse-distance law holds further out",
          Num(Db(far / further), 2) + " dB");

    // Distant sources lose high frequencies to air absorption.
    const Stereo bright = Sine(samples, 12000.0);
    SetStatic(*effect, 0.0, 0.0, 1.5);
    const double nearHigh = Rms(Run(*effect, bright).left, samples / 3);
    SetStatic(*effect, 0.0, 0.0, 10.0);
    const double farHigh = Rms(Run(*effect, bright).left, samples / 3);
    SetStatic(*effect, 0.0, 0.0, 1.5);
    const double nearLow = Rms(Run(*effect, input).left, samples / 3);
    SetStatic(*effect, 0.0, 0.0, 10.0);
    const double farLow = Rms(Run(*effect, input).left, samples / 3);

    Check(Db(nearHigh / farHigh) > Db(nearLow / farLow) + 2.0, "distance attenuates high frequencies faster than low",
          "HF " + Num(Db(nearHigh / farHigh), 1) + " dB vs LF " + Num(Db(nearLow / farLow), 1) + " dB");
}

void TestLatencyAndDryBlend()
{
    std::cout << "\nLatency and dry blend" << std::endl;

    auto effect = MakeEffect();
    if (!effect)
    {
        return;
    }

    const int latency = effect->GetLatencySamples();
    Check(latency > 0, "reports the constant interaural bias as latency", std::to_string(latency) + " samples");

    SetStatic(*effect, 75.0, 30.0, 4.0, 0.8);
    effect->SetParam("mix", 0.0);
    effect->Reset();

    const std::size_t samples = static_cast<std::size_t>(kSampleRate * 0.1);
    const Stereo input = Noise(samples, 999);
    const Stereo out = Run(*effect, input);

    double worst = 0.0;
    for (std::size_t i = static_cast<std::size_t>(latency); i < samples; ++i)
    {
        worst = std::max(
            worst, static_cast<double>(std::fabs(out.left[i] - input.left[i - static_cast<std::size_t>(latency)])));
        worst = std::max(
            worst, static_cast<double>(std::fabs(out.right[i] - input.right[i - static_cast<std::size_t>(latency)])));
    }
    Check(worst == 0.0, "at zero mix the output is the input delayed by exactly the reported latency",
          "max deviation " + Num(worst, 9));

    // Latency must not move when parameters change, or a moving source would
    // shift the whole mix against other chain elements.
    effect->SetParam("distance", 10.0);
    effect->SetParam("motionMode", 1.0);
    effect->SetParam("mix", 1.0);
    Check(effect->GetLatencySamples() == latency, "latency is independent of parameter values");
}

void TestMotionOffMatchesStatic()
{
    std::cout << "\nMotion engine: neutral settings" << std::endl;

    const std::size_t samples = static_cast<std::size_t>(kSampleRate * 0.25);
    const Stereo input = Noise(samples, 4242);

    auto staticEffect = MakeEffect();
    auto zeroDepth = MakeEffect();
    if (!staticEffect || !zeroDepth)
    {
        return;
    }

    SetStatic(*staticEffect, 25.0, 10.0, 2.0, 0.3);
    const Stereo staticOut = Run(*staticEffect, input);

    SetStatic(*zeroDepth, 25.0, 10.0, 2.0, 0.3);
    zeroDepth->SetParam("motionMode", 2.0); // Arc
    zeroDepth->SetParam("motionRate", 1.0);
    zeroDepth->SetParam("motionDepth", 0.0);
    zeroDepth->SetParam("motionElevDepth", 0.0);
    zeroDepth->SetParam("motionDistDepth", 0.0);
    zeroDepth->Reset();
    const Stereo zeroOut = Run(*zeroDepth, input);

    double worst = 0.0;
    for (std::size_t i = 0; i < samples; ++i)
    {
        worst = std::max(worst, static_cast<double>(std::fabs(staticOut.left[i] - zeroOut.left[i])));
        worst = std::max(worst, static_cast<double>(std::fabs(staticOut.right[i] - zeroOut.right[i])));
    }
    Check(worst < 1.0e-6, "zero-depth motion is identical to a static position", "max deviation " + Num(worst, 9));
}

void TestMotionRateAndSync()
{
    std::cout << "\nMotion engine: rate, sync and direction" << std::endl;

    auto effect = MakeEffect();
    if (!effect)
    {
        return;
    }

    auto measureTravelDegrees = [&](double seconds) {
        const auto samples = static_cast<std::size_t>(kSampleRate * seconds);
        const Stereo input = Noise(samples, 77);
        double previous = effect->GetParam("currentAzimuth");
        double travel = 0.0;
        const int total = static_cast<int>(samples);
        for (int offset = 0; offset < total; offset += kBlockSize)
        {
            const int len = std::min(kBlockSize, total - offset);
            std::vector<float> outL(static_cast<std::size_t>(len));
            std::vector<float> outR(static_cast<std::size_t>(len));
            float* in[2] = {const_cast<float*>(input.left.data()) + offset,
                            const_cast<float*>(input.right.data()) + offset};
            float* out[2] = {outL.data(), outR.data()};
            effect->Process(in, out, len);

            const double current = effect->GetParam("currentAzimuth");
            double delta = current - previous;
            while (delta > 180.0)
            {
                delta -= 360.0;
            }
            while (delta < -180.0)
            {
                delta += 360.0;
            }
            travel += delta;
            previous = current;
        }
        return travel;
    };

    SetStatic(*effect, 0.0, 0.0, 1.5);
    effect->SetParam("motionMode", 1.0); // Orbit
    effect->SetParam("motionRate", 1.0);
    effect->SetParam("motionSmooth", 0.0);
    effect->Reset();
    const double travelForward = measureTravelDegrees(2.0);
    Check(std::fabs(travelForward - 720.0) < 40.0, "orbit completes one revolution per 1/rate seconds",
          Num(travelForward, 1) + " deg over 2 s at 1 Hz");

    effect->SetParam("motionDirection", 1.0);
    effect->Reset();
    const double travelReverse = measureTravelDegrees(2.0);
    Check(travelReverse < -600.0, "reverse direction orbits the other way", Num(travelReverse, 1) + " deg");

    effect->SetParam("motionDirection", 0.0);
    effect->SetParam("syncMode", 1.0);
    effect->SetParam("syncDivision", 0.0); // 1/1 = one bar of four beats
    effect->SetParam("bpm", 120.0);
    Check(std::fabs(effect->GetParam("effectiveRate") - 0.5) < 1.0e-6, "one bar at 120 BPM is a two second revolution",
          Num(effect->GetParam("effectiveRate"), 4) + " Hz");
    effect->SetParam("bpm", 240.0);
    Check(std::fabs(effect->GetParam("effectiveRate") - 1.0) < 1.0e-6, "doubling the tempo doubles the motion rate",
          Num(effect->GetParam("effectiveRate"), 4) + " Hz");
}

void TestDriftDeterminism()
{
    std::cout << "\nMotion engine: drift determinism" << std::endl;

    const std::size_t samples = static_cast<std::size_t>(kSampleRate * 0.5);
    const Stereo input = Noise(samples, 31337);

    auto make = [&](int seed) {
        auto effect = MakeEffect();
        SetStatic(*effect, 0.0, 0.0, 2.0, 0.2);
        effect->SetParam("motionMode", 5.0); // Drift
        effect->SetParam("motionRate", 0.5);
        effect->SetParam("motionDepth", 0.8);
        effect->SetParam("motionElevDepth", 0.5);
        effect->SetParam("motionSeed", static_cast<double>(seed));
        effect->Reset();
        return Run(*effect, input);
    };

    const Stereo a = make(7);
    const Stereo b = make(7);
    const Stereo c = make(8);

    double sameWorst = 0.0;
    double diffWorst = 0.0;
    for (std::size_t i = 0; i < samples; ++i)
    {
        sameWorst = std::max(sameWorst, static_cast<double>(std::fabs(a.left[i] - b.left[i])));
        diffWorst = std::max(diffWorst, static_cast<double>(std::fabs(a.left[i] - c.left[i])));
    }
    Check(sameWorst == 0.0, "the same drift seed reproduces the same trajectory exactly",
          "max deviation " + Num(sameWorst, 9));
    Check(diffWorst > 1.0e-5, "a different drift seed produces a different trajectory",
          "max deviation " + Num(diffWorst, 6));
}

void TestPitchStabilityDuringMotion()
{
    std::cout << "\nMotion engine: pitch stability" << std::endl;

    // Positioning a source with a delay line means that moving it changes phase over
    // time, and a changing phase *is* a frequency shift. That cannot be designed away,
    // so the honest questions are: (a) is the shift negligible at the motion rates the
    // factory presets actually use, and (b) does Doppler mode, which also tracks the
    // propagation delay to the source, bend pitch markedly more?
    //
    // Counting zero crossings over whole orbit cycles cannot answer either: the shift
    // is sinusoidal, so the sharp and flat halves cancel. Measure the peak
    // *instantaneous* frequency instead, from sub-sample-interpolated rising zero
    // crossings averaged over a short sliding window.
    const double toneHz = 1000.0;
    const std::size_t samples = static_cast<std::size_t>(kSampleRate * 4.0);
    const Stereo input = Sine(samples, toneHz);

    auto peakFrequencyDeviation = [&](int delayMode, double rateHz, double distanceDepth) {
        auto effect = MakeEffect();
        if (!effect)
        {
            return -1.0;
        }
        SetStatic(*effect, 0.0, 0.0, 1.5);
        effect->SetParam("motionMode", 1.0); // Orbit
        effect->SetParam("motionRate", rateHz);
        effect->SetParam("motionDistDepth", distanceDepth);
        effect->SetParam("motionElevDepth", 0.0);
        effect->SetParam("motionSmooth", 0.0);
        effect->SetParam("delayMode", static_cast<double>(delayMode));
        effect->Reset();
        const Stereo out = Run(*effect, input);

        // Sub-sample rising zero crossings, skipping the smoothing ramp-in.
        std::vector<double> crossings;
        const std::size_t from = static_cast<std::size_t>(kSampleRate * 0.5);
        for (std::size_t i = from + 1; i < out.left.size(); ++i)
        {
            const float previous = out.left[i - 1];
            const float current = out.left[i];
            if (previous <= 0.0f && current > 0.0f && current != previous)
            {
                crossings.push_back(static_cast<double>(i - 1) +
                                    static_cast<double>(-previous) / static_cast<double>(current - previous));
            }
        }

        // Average over 20 periods: long enough to reject interpolation noise, short
        // enough to still resolve the peak of a sub-hertz modulation.
        const std::size_t window = 20;
        if (crossings.size() <= window + 1)
        {
            return -1.0;
        }

        double worst = 0.0;
        for (std::size_t i = 0; i + window < crossings.size(); ++i)
        {
            const double span = crossings[i + window] - crossings[i];
            if (span <= 0.0)
            {
                continue;
            }
            const double frequency = static_cast<double>(window) * kSampleRate / span;
            worst = std::max(worst, std::fabs(frequency / toneHz - 1.0));
        }
        return worst;
    };

    // 0.12 Hz is around the fastest orbit any factory preset uses.
    const double musicalRate = peakFrequencyDeviation(0, 0.12, 0.0);
    Check(musicalRate >= 0.0, "the tone survives motion well enough to measure its pitch");

    // One cent is 2^(1/1200) - 1 = 0.0578%.
    const double oneCent = 0.000578;
    Check(musicalRate >= 0.0 && musicalRate < oneCent,
          "at factory preset motion rates the pitch shift stays under one cent",
          Num(musicalRate * 100.0, 4) + "% peak deviation (one cent = " + Num(oneCent * 100.0, 4) + "%)");

    // With the source moving towards and away from the listener, Doppler mode must
    // bend pitch substantially more than Smooth mode, which holds propagation fixed.
    const double smoothApproach = peakFrequencyDeviation(0, 0.5, 1.0);
    const double dopplerApproach = peakFrequencyDeviation(1, 0.5, 1.0);
    Check(dopplerApproach > smoothApproach * 5.0, "doppler mode bends pitch as the source approaches and recedes",
          Num(dopplerApproach * 100.0, 4) + "% vs " + Num(smoothApproach * 100.0, 4) + "% in smooth mode");
}

void TestEdgeCases()
{
    std::cout << "\nEdge cases" << std::endl;

    // Invalid Prepare arguments must leave the effect in a safe passthrough state.
    auto unprepared = guitarfx::EffectRegistry::Instance().Create(guitarfx::EffectGuids::kSpatial3D);
    if (unprepared)
    {
        unprepared->Prepare(0.0, 0);
        const std::size_t samples = 512;
        const Stereo input = Noise(samples, 5);
        const Stereo out = Run(*unprepared, input);
        bool passthrough = true;
        for (std::size_t i = 0; i < samples; ++i)
        {
            if (out.left[i] != input.left[i] || out.right[i] != input.right[i])
            {
                passthrough = false;
            }
        }
        Check(passthrough, "an invalid Prepare leaves the effect passing audio through unchanged");
        Check(unprepared->GetLatencySamples() == 0, "an unprepared effect reports no latency");
    }

    auto effect = MakeEffect();
    if (!effect)
    {
        return;
    }

    // Null channel pointers, as a mono host buffer would supply.
    {
        std::vector<float> mono(static_cast<std::size_t>(kBlockSize), 0.25f);
        std::vector<float> outL(static_cast<std::size_t>(kBlockSize), 0.0f);
        std::vector<float> outR(static_cast<std::size_t>(kBlockSize), 0.0f);
        float* in[2] = {mono.data(), nullptr};
        float* out[2] = {outL.data(), outR.data()};
        effect->Process(in, out, kBlockSize);
        bool finite = true;
        for (int i = 0; i < kBlockSize; ++i)
        {
            finite = finite && std::isfinite(outL[i]) && std::isfinite(outR[i]);
        }
        Check(finite, "a missing right input channel is handled without producing garbage");
    }

    // Out-of-range parameters must clamp rather than corrupt the geometry.
    effect->SetParam("azimuth", 5000.0);
    effect->SetParam("elevation", -400.0);
    effect->SetParam("distance", -3.0);
    Check(effect->GetParam("azimuth") == 180.0 && effect->GetParam("elevation") == -90.0 &&
              std::fabs(effect->GetParam("distance") - guitarfx::spatial3d::kMinDistanceM) < 1e-9,
          "out-of-range positions clamp to the supported sphere");

    effect->SetParam("motionMode", 99.0);
    Check(effect->GetParam("motionMode") == guitarfx::spatial3d::kMotionModeCount - 1,
          "an unknown motion mode clamps to a valid one");

    effect->SetParam("unknownParameter", 1.0);
    Check(effect->GetParam("unknownParameter") == 0.0, "unknown parameters are ignored safely");

    // Reset must clear the delay lines and filter state.
    SetStatic(*effect, 60.0, 20.0, 3.0, 0.6);
    Run(*effect, Noise(static_cast<std::size_t>(kSampleRate * 0.2), 17, 0.9));
    effect->Reset();
    const Stereo silence(static_cast<std::size_t>(kSampleRate * 0.2));
    const Stereo tail = Run(*effect, silence);
    Check(PeakAbs(tail) == 0.0f, "Reset clears every delay line and filter", "residual peak " + Num(PeakAbs(tail), 9));
}

void TestFactoryPresetsBehaviour()
{
    std::cout << "\nFactory preset playback" << std::endl;

    auto info = guitarfx::EffectRegistry::Instance().GetTypeInfo(guitarfx::EffectGuids::kSpatial3D);
    if (!info.has_value())
    {
        return;
    }

    const std::size_t samples = static_cast<std::size_t>(kSampleRate * 2.0);
    const Stereo input = Noise(samples, 20240, 0.4);
    const double inputRms = Rms(input.left);

    bool allFinite = true;
    bool allAudible = true;
    bool allSane = true;
    bool allMoved = true;
    std::string problem;

    for (const auto& preset : info->presets)
    {
        auto effect = MakeEffect();
        if (!effect)
        {
            return;
        }
        for (const auto& [key, value] : preset.parameters)
        {
            effect->SetParam(key, value);
        }
        effect->Reset();

        // Sample the rendered position while processing so that presets which claim
        // to animate are checked for actually animating.
        double minAz = 1e9;
        double maxAz = -1e9;
        double travel = 0.0;
        double previousAz = effect->GetParam("currentAzimuth");
        Stereo output(samples);
        const int total = static_cast<int>(samples);
        for (int offset = 0; offset < total; offset += kBlockSize)
        {
            const int len = std::min(kBlockSize, total - offset);
            float* in[2] = {const_cast<float*>(input.left.data()) + offset,
                            const_cast<float*>(input.right.data()) + offset};
            float* out[2] = {output.left.data() + offset, output.right.data() + offset};
            effect->Process(in, out, len);

            const double az = effect->GetParam("currentAzimuth");
            minAz = std::min(minAz, az);
            maxAz = std::max(maxAz, az);
            double delta = az - previousAz;
            while (delta > 180.0)
            {
                delta -= 360.0;
            }
            while (delta < -180.0)
            {
                delta += 360.0;
            }
            travel += std::fabs(delta);
            previousAz = az;
        }

        if (!AllFinite(output))
        {
            allFinite = false;
            problem = preset.id + " produced non-finite samples";
        }
        const double outputRms = Rms(output.left, samples / 4);
        if (outputRms < inputRms * 0.05)
        {
            allAudible = false;
            if (problem.empty())
            {
                problem = preset.id + " is effectively silent";
            }
        }
        if (PeakAbs(output) > 4.0f)
        {
            allSane = false;
            if (problem.empty())
            {
                problem = preset.id + " peaks at " + Num(PeakAbs(output), 2);
            }
        }

        const auto motionIt = preset.parameters.find("motionMode");
        const bool animated = motionIt != preset.parameters.end() && motionIt->second > 0.5;
        if (animated)
        {
            // An animated preset must move the source somewhere over two seconds,
            // either horizontally or vertically.
            const bool movedHorizontally = travel > 5.0;
            const auto elevIt = preset.parameters.find("motionElevDepth");
            const bool couldMoveVertically = elevIt != preset.parameters.end() && elevIt->second > 0.01;
            if (!movedHorizontally && !couldMoveVertically)
            {
                allMoved = false;
                if (problem.empty())
                {
                    problem = preset.id + " claims motion but the source barely moves";
                }
            }
        }
    }

    Check(allFinite, "every preset produces finite audio", problem);
    Check(allAudible, "no preset silences the signal", problem);
    Check(allSane, "no preset produces a runaway level", problem);
    Check(allMoved, "every animated preset actually moves the source", problem);
}

void TestStereoPreservationContract()
{
    std::cout << "\nSignal graph contract" << std::endl;

    auto effect = MakeEffect();
    if (!effect)
    {
        return;
    }

    Check(effect->ProducesStereoOutput(), "declares stereo output so the graph will not collapse the image downstream");
    Check(!effect->SupportsMonoProcessing(), "does not offer a mono fast path");
    Check(effect->GetCategory() == "modulation", "reports a category the UI already understands");

    // A mono (identical L and R) input must come out with a genuinely different
    // left and right, otherwise the positioning is not audible at all.
    const std::size_t samples = static_cast<std::size_t>(kSampleRate * 0.2);
    SetStatic(*effect, 60.0, 0.0, 1.5);
    const Stereo out = Run(*effect, Noise(samples, 606));
    double difference = 0.0;
    for (std::size_t i = samples / 4; i < samples; ++i)
    {
        difference += std::fabs(static_cast<double>(out.left[i] - out.right[i]));
    }
    Check(difference > 1.0, "a mono input becomes a genuinely stereo image", Num(difference, 1));
}

} // namespace

int main()
{
    std::cout << "Spatial 3D effect tests" << std::endl;
    std::cout << "=======================" << std::endl;

    guitarfx::RegisterSpatial3DEffect();

    TestRegistration();
    TestStereoPreservationContract();
    TestInterauralTimeDifference();
    TestInterauralLevelDifference();
    TestFrontBackAndElevation();
    TestDistance();
    TestLatencyAndDryBlend();
    TestMotionOffMatchesStatic();
    TestMotionRateAndSync();
    TestDriftDeterminism();
    TestPitchStabilityDuringMotion();
    TestEdgeCases();
    TestFactoryPresetsBehaviour();

    std::cout << "\n=======================" << std::endl;
    std::cout << (gChecks - gFailures) << " / " << gChecks << " checks passed" << std::endl;
    if (gFailures > 0)
    {
        std::cout << gFailures << " FAILED" << std::endl;
    }
    return gFailures == 0 ? 0 : 1;
}
