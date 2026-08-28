/**
 * @file TransposeBenchmark.cpp
 * @brief Offline benchmark harness for the transpose/pitch effect family.
 *
 * For each (effect variant x semitone setting x demo sample) pass this tool:
 *   - renders the demo audio through a fresh effect instance,
 *   - records reported latency (GetLatencySamples) and measured latency
 *     (envelope cross-correlation between input and output),
 *   - records processing cost (total, average and worst block time,
 *     realtime factor),
 *   - writes a latency-compensated WAV render for audible comparison.
 *
 * Results are written to <outputRoot>/<snapshot>/results.json plus WAV files.
 * Use tools/transpose-benchmark/generate_report.py to build an HTML report
 * comparing one or more snapshots (e.g. different git revisions).
 *
 * Usage:
 *   TransposeBenchmark [--all-demo-audio] [outputRoot] [snapshotLabel]
 *     --all-demo-audio  render all demo inputs (default renders first riff only)
 *     outputRoot    default: transpose-benchmark-out (relative to cwd)
 *     snapshotLabel default: snapshot-<UTC timestamp>
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/BuiltinEffects.h"
#include "util/Wav.h"

#ifndef GUITARFX_DEMO_AUDIO_DIR
    #error "GUITARFX_DEMO_AUDIO_DIR must be defined"
#endif

namespace fs = std::filesystem;

namespace
{
constexpr int kBlockSize = 512;
constexpr double kMaxRenderSeconds = 12.0;
constexpr int kEnvelopeDecimation = 32;
constexpr double kMaxMeasuredLatencySeconds = 0.5;

// Pitch-accuracy tracking. Guitar fundamentals sit well below 500 Hz, so the
// tracker runs on a ~8 kHz decimated copy to keep YIN's search space small.
constexpr double kPitchTargetRate = 8000.0;
// 128 ms frame / 64 ms hop. Long enough for a reliable YIN estimate on a low E
// (~5 periods in the correlation window), short enough that a riff still yields
// plenty of steady-pitch frames — a 256 ms frame yielded almost none.
constexpr int kPitchFrameSamples = 1024;
constexpr int kPitchHopSamples = 256;
constexpr double kPitchDryMinHz = 65.0;
constexpr double kPitchDryMaxHz = 500.0;
constexpr double kPitchAbsoluteMinHz = 30.0; // -12 st from a low E is 41 Hz
constexpr double kPitchYinThreshold = 0.20;
// Fallback: when nothing clears the threshold, accept the best CMNDF minimum if
// it is at least this good. Without it a phasey shifter output goes untracked
// and the metric silently reports "no data" for the very cases it should catch.
constexpr double kPitchFallbackMax = 0.60;
constexpr double kPitchSilenceRms = 1.0e-4;
constexpr double kPitchOctaveRejectCents = 600.0;
constexpr int kPitchMinFrames = 10;

struct EffectVariant
{
    std::string alias; // registry alias, e.g. "transpose"
    std::string label; // report label, e.g. "Transpose (STFT, polyphonic)"
    std::vector<std::pair<std::string, double>> extraParams;
};

struct ExternalPluginVariant
{
    std::string pluginId;
    std::string label;
    std::string pluginPath;
    std::string stateConfigKey = "pluginStateBase64";
    std::map<int, std::string> stateBySemitone;
};

const std::vector<EffectVariant> kVariants = {
    {"pitch_shift", "Pitch Shift (Signalsmith)", {}},
    {"transpose", "Transpose (Signalsmith)", {}},
    {"transpose_stft", "Transpose (STFT, low latency)", {{"mode", 0.0}}},
    {"transpose_stft", "Transpose (STFT, polyphonic)", {{"mode", 1.0}}},
    {"transpose_hybrid", "Transpose (Hybrid)", {}},
};

const std::vector<int> kSemitoneSettings = {-12, -7, -5, -3, -2, -1, 0, 2, 5, 7, 12};

const std::vector<std::string> kDemoSamples = {
    "guitar-riff-01.wav",
    "guitar-riff-02.wav",
    "DI_Guitar_L.wav",
};

const fs::path kExternalPluginsCollectionRelPath = fs::path("tools") / "transpose-benchmark" / "external-plugins.json";

struct StereoAudio
{
    double sampleRate = 0.0;
    std::vector<float> left;
    std::vector<float> right;

    [[nodiscard]] size_t frames() const
    {
        return left.size();
    }
};

std::vector<std::uint8_t> ReadBinaryFile(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return {};
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::optional<StereoAudio> LoadDemoSample(const fs::path& path)
{
    const auto bytes = ReadBinaryFile(path);
    if (bytes.empty())
    {
        return std::nullopt;
    }

    const auto decoded = guitarfx::util::DecodePcmWav(bytes);
    if (!decoded || decoded->channelSamples.empty() || decoded->sampleRate <= 0.0)
    {
        return std::nullopt;
    }

    StereoAudio audio;
    audio.sampleRate = decoded->sampleRate;

    const size_t maxFrames = static_cast<size_t>(kMaxRenderSeconds * decoded->sampleRate);
    const auto& srcL = decoded->channelSamples[0];
    const auto& srcR = decoded->channelSamples.size() > 1 ? decoded->channelSamples[1] : decoded->channelSamples[0];
    const size_t frames = std::min(std::min(srcL.size(), srcR.size()), maxFrames);

    audio.left.resize(frames);
    audio.right.resize(frames);
    for (size_t i = 0; i < frames; ++i)
    {
        audio.left[i] = static_cast<float>(srcL[i]);
        audio.right[i] = static_cast<float>(srcR[i]);
    }
    return audio;
}

bool WriteWav16(const fs::path& path, const StereoAudio& audio)
{
    std::ofstream out(path, std::ios::binary);
    if (!out)
    {
        return false;
    }

    const std::uint32_t frames = static_cast<std::uint32_t>(audio.frames());
    const std::uint16_t channels = 2;
    const std::uint16_t bitsPerSample = 16;
    const std::uint32_t sampleRate = static_cast<std::uint32_t>(std::lround(audio.sampleRate));
    const std::uint32_t byteRate = sampleRate * channels * (bitsPerSample / 8);
    const std::uint16_t blockAlign = channels * (bitsPerSample / 8);
    const std::uint32_t dataSize = frames * blockAlign;
    const std::uint32_t riffSize = 36 + dataSize;

    auto write32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto write16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };

    out.write("RIFF", 4);
    write32(riffSize);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    write32(16);
    write16(1); // PCM
    write16(channels);
    write32(sampleRate);
    write32(byteRate);
    write16(blockAlign);
    write16(bitsPerSample);
    out.write("data", 4);
    write32(dataSize);

    for (std::uint32_t i = 0; i < frames; ++i)
    {
        const auto clampToInt16 = [](float v) {
            const float clamped = std::clamp(v, -1.0f, 1.0f);
            return static_cast<std::int16_t>(std::lround(clamped * 32767.0f));
        };
        const std::int16_t l = clampToInt16(audio.left[i]);
        const std::int16_t r = clampToInt16(audio.right[i]);
        out.write(reinterpret_cast<const char*>(&l), 2);
        out.write(reinterpret_cast<const char*>(&r), 2);
    }
    return static_cast<bool>(out);
}

/**
 * Decimated rectified-envelope of a mono mix. Pitch shifting changes the
 * waveform but largely preserves the amplitude envelope, so cross-correlating
 * envelopes gives a robust latency estimate even for shifted material.
 */
std::vector<double> ComputeEnvelope(const std::vector<float>& left, const std::vector<float>& right, double sampleRate)
{
    const double smoothingSeconds = 0.005;
    const double alpha = 1.0 - std::exp(-1.0 / (smoothingSeconds * sampleRate));

    std::vector<double> envelope;
    envelope.reserve(left.size() / kEnvelopeDecimation + 1);
    double state = 0.0;
    for (size_t i = 0; i < left.size(); ++i)
    {
        const double mono = 0.5 * (std::abs(static_cast<double>(left[i])) + std::abs(static_cast<double>(right[i])));
        state += alpha * (mono - state);
        if (i % kEnvelopeDecimation == 0)
        {
            envelope.push_back(state);
        }
    }
    return envelope;
}

/**
 * Estimate output delay (in frames) via normalized cross-correlation of the
 * decimated envelopes. Resolution is +/- kEnvelopeDecimation frames.
 * Returns -1 when correlation is too weak to be trusted.
 */
int MeasureLatencyFrames(const StereoAudio& input, const StereoAudio& output)
{
    const auto inputEnv = ComputeEnvelope(input.left, input.right, input.sampleRate);
    const auto outputEnv = ComputeEnvelope(output.left, output.right, output.sampleRate);
    if (inputEnv.size() < 16 || outputEnv.size() < 16)
    {
        return -1;
    }

    const int maxLag = static_cast<int>(kMaxMeasuredLatencySeconds * input.sampleRate) / kEnvelopeDecimation;
    const int usableLag = std::min<int>(maxLag, static_cast<int>(outputEnv.size()) - 8);
    if (usableLag <= 0)
    {
        return -1;
    }

    double inputEnergy = 0.0;
    for (const double v : inputEnv)
    {
        inputEnergy += v * v;
    }
    if (inputEnergy <= 1.0e-12)
    {
        return -1;
    }

    int bestLag = 0;
    double bestScore = -1.0;
    for (int lag = 0; lag <= usableLag; ++lag)
    {
        double dot = 0.0;
        double outEnergy = 0.0;
        const size_t count = std::min(inputEnv.size(), outputEnv.size() - static_cast<size_t>(lag));
        for (size_t i = 0; i < count; ++i)
        {
            const double o = outputEnv[i + static_cast<size_t>(lag)];
            dot += inputEnv[i] * o;
            outEnergy += o * o;
        }
        if (outEnergy <= 1.0e-12)
        {
            continue;
        }
        const double score = dot / std::sqrt(inputEnergy * outEnergy);
        if (score > bestScore)
        {
            bestScore = score;
            bestLag = lag;
        }
    }

    if (bestScore < 0.5)
    {
        return -1;
    }
    return bestLag * kEnvelopeDecimation;
}

/**
 * Mono, band-limited, decimated copy for pitch tracking. A box average over the
 * decimation factor is the anti-alias filter — crude, but everything above
 * ~4 kHz is irrelevant to a fundamental estimate.
 */
std::vector<double> DecimateForPitch(const std::vector<float>& left, const std::vector<float>& right,
                                     size_t offsetFrames, size_t frameCount, double sampleRate, double& outRate)
{
    const size_t factor = static_cast<size_t>(std::max(1L, std::lround(sampleRate / kPitchTargetRate)));
    outRate = sampleRate / static_cast<double>(factor);

    std::vector<double> out;
    out.reserve(frameCount / factor + 1);
    for (size_t i = 0; i + factor <= frameCount; i += factor)
    {
        double acc = 0.0;
        size_t taken = 0;
        for (size_t k = 0; k < factor; ++k)
        {
            const size_t idx = offsetFrames + i + k;
            if (idx >= left.size() || idx >= right.size())
            {
                break;
            }
            acc += 0.5 * (static_cast<double>(left[idx]) + static_cast<double>(right[idx]));
            ++taken;
        }
        if (taken == 0)
        {
            break;
        }
        out.push_back(acc / static_cast<double>(taken));
    }
    return out;
}

/**
 * YIN fundamental estimate (cumulative mean normalized difference function with
 * parabolic refinement). Returns 0 when no confident pitch is found, so unvoiced
 * frames drop out of the comparison rather than contributing noise.
 */
double EstimateF0(const std::vector<double>& x, size_t start, size_t count, double sampleRate, double minHz,
                  double maxHz)
{
    if (count < 256 || start + count > x.size())
    {
        return 0.0;
    }

    const int window = static_cast<int>(count) / 2;
    const int tauMax = std::min(window, static_cast<int>(sampleRate / std::max(minHz, 1.0)));
    const int tauMin = std::max(2, static_cast<int>(sampleRate / std::max(maxHz, 1.0)));
    if (tauMax <= tauMin + 2)
    {
        return 0.0;
    }

    double mean = 0.0;
    for (size_t i = 0; i < count; ++i)
    {
        mean += x[start + i];
    }
    mean /= static_cast<double>(count);

    double energy = 0.0;
    for (size_t i = 0; i < count; ++i)
    {
        const double centered = x[start + i] - mean;
        energy += centered * centered;
    }
    if (std::sqrt(energy / static_cast<double>(count)) < kPitchSilenceRms)
    {
        return 0.0;
    }

    std::vector<double> diff(static_cast<size_t>(tauMax) + 1, 0.0);
    for (int tau = 1; tau <= tauMax; ++tau)
    {
        double sum = 0.0;
        for (int i = 0; i < window; ++i)
        {
            const double a = x[start + static_cast<size_t>(i)] - mean;
            const double b = x[start + static_cast<size_t>(i + tau)] - mean;
            sum += (a - b) * (a - b);
        }
        diff[static_cast<size_t>(tau)] = sum;
    }

    std::vector<double> normalized(diff.size(), 1.0);
    double running = 0.0;
    for (int tau = 1; tau <= tauMax; ++tau)
    {
        running += diff[static_cast<size_t>(tau)];
        normalized[static_cast<size_t>(tau)] = running > 1.0e-12 ? diff[static_cast<size_t>(tau)] * tau / running : 1.0;
    }

    int tau = -1;
    for (int t = tauMin; t <= tauMax; ++t)
    {
        if (normalized[static_cast<size_t>(t)] >= kPitchYinThreshold)
        {
            continue;
        }
        // Descend to the local minimum so we lock the true period, not its onset.
        while (t + 1 <= tauMax && normalized[static_cast<size_t>(t + 1)] < normalized[static_cast<size_t>(t)])
        {
            ++t;
        }
        tau = t;
        break;
    }
    if (tau < 1)
    {
        int best = tauMin;
        for (int t = tauMin; t <= tauMax; ++t)
        {
            if (normalized[static_cast<size_t>(t)] < normalized[static_cast<size_t>(best)])
            {
                best = t;
            }
        }
        if (normalized[static_cast<size_t>(best)] > kPitchFallbackMax)
        {
            return 0.0;
        }
        tau = best;
    }

    double refined = static_cast<double>(tau);
    if (tau > 1 && tau < tauMax)
    {
        const double a = normalized[static_cast<size_t>(tau) - 1];
        const double b = normalized[static_cast<size_t>(tau)];
        const double c = normalized[static_cast<size_t>(tau) + 1];
        const double denom = a - 2.0 * b + c;
        if (std::abs(denom) > 1.0e-12)
        {
            refined = tau + 0.5 * (a - c) / denom;
        }
    }
    return refined > 0.0 ? sampleRate / refined : 0.0;
}

struct PitchAccuracy
{
    double medianCents = 0.0;
    double jitterCents = 0.0;
    int frames = 0;
    bool valid = false;
};

/**
 * Compares the output fundamental against the dry fundamental scaled by the
 * requested interval. This catches an engine that reports plausible latency and
 * CPU but is not actually transposing — the STFT path measured 80-200 cents off
 * at shallow settings while looking healthy on every other metric.
 *
 * Only frames whose dry pitch is stable across the frame are used, so vibrato,
 * bends and note transitions do not masquerade as engine error. The wet search
 * range is shifted by the requested interval to avoid octave confusion.
 */
PitchAccuracy MeasurePitchAccuracy(const StereoAudio& dry, const StereoAudio& wet, size_t wetOffsetFrames,
                                   int semitones)
{
    PitchAccuracy result;
    if (dry.frames() == 0 || wet.left.size() <= wetOffsetFrames)
    {
        return result;
    }

    const size_t frameCount = std::min(dry.frames(), wet.left.size() - wetOffsetFrames);

    double rate = 0.0;
    double wetRate = 0.0;
    const std::vector<double> dryMono = DecimateForPitch(dry.left, dry.right, 0, frameCount, dry.sampleRate, rate);
    const std::vector<double> wetMono =
        DecimateForPitch(wet.left, wet.right, wetOffsetFrames, frameCount, wet.sampleRate, wetRate);
    if (rate <= 0.0 || dryMono.size() < static_cast<size_t>(kPitchFrameSamples))
    {
        return result;
    }

    const double factor = std::pow(2.0, static_cast<double>(semitones) / 12.0);
    const double wetMinHz = std::max(kPitchAbsoluteMinHz, kPitchDryMinHz * factor * 0.7);
    const double wetMaxHz = kPitchDryMaxHz * factor * 1.4;

    const size_t usable = std::min(dryMono.size(), wetMono.size());
    const size_t frameSamples = static_cast<size_t>(kPitchFrameSamples);
    const size_t hop = static_cast<size_t>(kPitchHopSamples);

    // Track both signals on a common grid first, then decide which frames to trust.
    std::vector<double> dryTrack;
    std::vector<double> wetTrack;
    for (size_t start = 0; start + frameSamples <= usable; start += hop)
    {
        dryTrack.push_back(EstimateF0(dryMono, start, frameSamples, rate, kPitchDryMinHz, kPitchDryMaxHz));
        wetTrack.push_back(EstimateF0(wetMono, start, frameSamples, rate, wetMinHz, wetMaxHz));
    }

    std::vector<double> cents;
    for (size_t i = 0; i < dryTrack.size(); ++i)
    {
        const double here = dryTrack[i];
        if (here <= 0.0)
        {
            continue;
        }

        const double wetF0 = wetTrack[i];
        if (wetF0 <= 0.0)
        {
            continue;
        }

        const double expected = here * factor;
        if (expected <= 0.0)
        {
            continue;
        }

        const double error = 1200.0 * std::log2(wetF0 / expected);
        // Reject octave-tracking artifacts; a genuine engine error of >600 cents
        // would show up as a collapse in frame count instead.
        if (std::abs(error) >= kPitchOctaveRejectCents)
        {
            continue;
        }
        cents.push_back(error);
    }

    result.frames = static_cast<int>(cents.size());
    if (result.frames < kPitchMinFrames)
    {
        return result;
    }

    const auto median = [](std::vector<double> values) {
        std::sort(values.begin(), values.end());
        const size_t mid = values.size() / 2;
        return values.size() % 2 == 0 ? 0.5 * (values[mid - 1] + values[mid]) : values[mid];
    };

    result.medianCents = median(cents);

    // Robust spread (MAD scaled to a normal-equivalent sigma). The YIN fallback
    // admits a few weak frames on purpose to keep coverage up, and a plain stddev
    // would let those outliers dominate the number.
    std::vector<double> deviations;
    deviations.reserve(cents.size());
    for (const double c : cents)
    {
        deviations.push_back(std::abs(c - result.medianCents));
    }
    result.jitterCents = 1.4826 * median(std::move(deviations));
    result.valid = true;
    return result;
}

/** Human-readable pitch-accuracy suffix for the per-pass console line. */
std::string FormatPitchSummary(const nlohmann::json& stats)
{
    if (!stats.contains("pitchErrorCents") || stats["pitchErrorCents"].is_null())
    {
        return ", pitch n/a";
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << ", pitch " << stats["pitchErrorCents"].get<double>() << " +/-"
        << stats["pitchJitterCents"].get<double>() << " cents";
    return out.str();
}

struct PassResult
{
    nlohmann::json stats;
    StereoAudio render;
};

std::optional<PassResult> RunConfiguredPass(const std::string& alias, const std::string& label,
                                            const StereoAudio& input, int semitones,
                                            const std::function<void(guitarfx::EffectProcessor&)>& configure)
{
    auto& registry = guitarfx::EffectRegistry::Instance();
    const std::string resolvedType = registry.Resolve(alias);
    auto effect = registry.Create(resolvedType);
    if (!effect)
    {
        std::cerr << "  SKIP: cannot create effect '" << label << "' (alias='" << alias << "')\n";
        return std::nullopt;
    }

    configure(*effect);
    effect->Prepare(input.sampleRate, kBlockSize);
    effect->Reset();

    const int reportedLatency = effect->GetLatencySamples();
    const size_t inputFrames = input.frames();
    const size_t flushFrames = static_cast<size_t>(std::max(reportedLatency, 0)) + 8192;
    const size_t totalFrames = inputFrames + flushFrames;

    StereoAudio raw;
    raw.sampleRate = input.sampleRate;
    raw.left.assign(totalFrames, 0.0f);
    raw.right.assign(totalFrames, 0.0f);

    std::vector<float> inL(kBlockSize, 0.0f);
    std::vector<float> inR(kBlockSize, 0.0f);
    std::vector<float> outL(kBlockSize, 0.0f);
    std::vector<float> outR(kBlockSize, 0.0f);
    float* inputs[2] = {inL.data(), inR.data()};
    float* outputs[2] = {outL.data(), outR.data()};

    using Clock = std::chrono::steady_clock;
    std::chrono::nanoseconds totalNs{0};
    std::chrono::nanoseconds maxBlockNs{0};
    size_t timedBlocks = 0;

    for (size_t pos = 0; pos < totalFrames; pos += kBlockSize)
    {
        const int blockFrames = static_cast<int>(std::min<size_t>(kBlockSize, totalFrames - pos));
        for (int i = 0; i < blockFrames; ++i)
        {
            const size_t idx = pos + static_cast<size_t>(i);
            inL[static_cast<size_t>(i)] = idx < inputFrames ? input.left[idx] : 0.0f;
            inR[static_cast<size_t>(i)] = idx < inputFrames ? input.right[idx] : 0.0f;
        }

        const auto start = Clock::now();
        effect->Process(inputs, outputs, blockFrames);
        const auto elapsed = Clock::now() - start;

        if (timedBlocks > 0)
        {
            maxBlockNs = std::max(maxBlockNs, std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed));
        }
        totalNs += std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed);
        ++timedBlocks;

        for (int i = 0; i < blockFrames; ++i)
        {
            raw.left[pos + static_cast<size_t>(i)] = outL[static_cast<size_t>(i)];
            raw.right[pos + static_cast<size_t>(i)] = outR[static_cast<size_t>(i)];
        }
    }

    const int measuredLatency = MeasureLatencyFrames(input, raw);

    StereoAudio render;
    render.sampleRate = input.sampleRate;
    render.left.assign(inputFrames, 0.0f);
    render.right.assign(inputFrames, 0.0f);
    const size_t offset = static_cast<size_t>(std::max(reportedLatency, 0));
    for (size_t i = 0; i < inputFrames && (i + offset) < totalFrames; ++i)
    {
        render.left[i] = raw.left[i + offset];
        render.right[i] = raw.right[i + offset];
    }

    double peak = 0.0;
    double sumSquares = 0.0;
    for (size_t i = 0; i < inputFrames; ++i)
    {
        const double l = std::abs(static_cast<double>(render.left[i]));
        const double r = std::abs(static_cast<double>(render.right[i]));
        peak = std::max({peak, l, r});
        sumSquares += 0.5 * (l * l + r * r);
    }
    const double rms = inputFrames > 0 ? std::sqrt(sumSquares / static_cast<double>(inputFrames)) : 0.0;

    const double audioMs = 1000.0 * static_cast<double>(totalFrames) / input.sampleRate;
    const double processMs = std::chrono::duration<double, std::milli>(totalNs).count();
    const double avgBlockUs =
        timedBlocks > 0 ? std::chrono::duration<double, std::micro>(totalNs).count() / static_cast<double>(timedBlocks)
                        : 0.0;

    const auto toDb = [](double v) { return v > 1.0e-9 ? 20.0 * std::log10(v) : -180.0; };
    const auto toMs = [&](int samples) { return 1000.0 * static_cast<double>(samples) / input.sampleRate; };

    nlohmann::json stats;
    stats["reportedLatencySamples"] = reportedLatency;
    stats["reportedLatencyMs"] = toMs(reportedLatency);
    stats["measuredLatencySamples"] = measuredLatency;
    stats["measuredLatencyMs"] = measuredLatency >= 0 ? toMs(measuredLatency) : -1.0;
    if (measuredLatency >= 0)
    {
        stats["latencyDeltaSamples"] = measuredLatency - reportedLatency;
    }
    else
    {
        stats["latencyDeltaSamples"] = nullptr;
    }
    stats["processMs"] = processMs;
    stats["audioMs"] = audioMs;
    stats["realtimeFactor"] = processMs > 0.0 ? audioMs / processMs : 0.0;
    stats["avgBlockUs"] = avgBlockUs;
    stats["maxBlockUs"] = std::chrono::duration<double, std::micro>(maxBlockNs).count();
    stats["peakDb"] = toDb(peak);
    stats["rmsDb"] = toDb(rms);

    // Align on measured latency when the correlation was trustworthy; the whole
    // point of this metric is to stay honest when the reported value is not.
    const size_t pitchOffset =
        static_cast<size_t>(std::max(measuredLatency >= 0 ? measuredLatency : reportedLatency, 0));
    const PitchAccuracy pitch = MeasurePitchAccuracy(input, raw, pitchOffset, semitones);
    if (pitch.valid)
    {
        stats["pitchErrorCents"] = pitch.medianCents;
        stats["pitchJitterCents"] = pitch.jitterCents;
    }
    else
    {
        stats["pitchErrorCents"] = nullptr;
        stats["pitchJitterCents"] = nullptr;
    }
    stats["pitchFrames"] = pitch.frames;

    PassResult result;
    result.stats = std::move(stats);
    result.render = std::move(render);
    return result;
}

std::optional<PassResult> RunPass(const EffectVariant& variant, int semitones, const StereoAudio& input)
{
    return RunConfiguredPass(variant.alias, variant.label, input, semitones, [&](guitarfx::EffectProcessor& effect) {
        // Set params before Prepare so preloaded values are picked up (see repo
        // memory: TransposeEffect only applies preloaded semitones during Prepare).
        effect.SetParam("semitones", static_cast<double>(semitones));
        effect.SetParam("mix", 1.0);
        for (const auto& [key, value] : variant.extraParams)
        {
            effect.SetParam(key, value);
        }
    });
}

std::string DefaultSnapshotLabel()
{
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "snapshot-%Y%m%d-%H%M%S", &utc);
    return buffer;
}

std::string SanitizeForFilename(std::string value)
{
    for (char& c : value)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_')
        {
            c = '_';
        }
    }
    return value;
}

std::optional<fs::path> FindExternalPluginCollectionPath()
{
    fs::path current = fs::current_path();
    for (int i = 0; i < 6; ++i)
    {
        const fs::path candidate = current / kExternalPluginsCollectionRelPath;
        if (fs::exists(candidate))
        {
            return candidate;
        }

        const fs::path parent = current.parent_path();
        if (parent == current)
        {
            break;
        }
        current = parent;
    }
    return std::nullopt;
}

nlohmann::json LoadExternalPluginCollection(const fs::path& path)
{
    std::ifstream input(path);
    if (!input)
    {
        return nlohmann::json::array();
    }

    nlohmann::json parsed;
    try
    {
        input >> parsed;
    }
    catch (...)
    {
        return nlohmann::json::array();
    }

    const auto pluginsIt = parsed.find("plugins");
    if (pluginsIt == parsed.end() || !pluginsIt->is_array())
    {
        return nlohmann::json::array();
    }

    nlohmann::json out = nlohmann::json::array();
    for (const auto& row : *pluginsIt)
    {
        if (!row.is_object())
        {
            continue;
        }
        const auto idIt = row.find("pluginId");
        const auto labelIt = row.find("pluginLabel");
        if (idIt == row.end() || labelIt == row.end() || !idIt->is_string() || !labelIt->is_string())
        {
            continue;
        }

        nlohmann::json plugin;
        plugin["pluginId"] = *idIt;
        plugin["pluginLabel"] = *labelIt;
        const auto pathIt = row.find("pluginPath");
        if (pathIt != row.end() && pathIt->is_string())
        {
            plugin["pluginPath"] = *pathIt;
        }
        out.push_back(std::move(plugin));
    }
    return out;
}

std::vector<ExternalPluginVariant> ParseExternalPluginVariants(const nlohmann::json& pluginsJson)
{
    std::vector<ExternalPluginVariant> out;
    if (!pluginsJson.is_array())
    {
        return out;
    }

    for (const auto& row : pluginsJson)
    {
        if (!row.is_object())
        {
            continue;
        }

        const auto pluginIdIt = row.find("pluginId");
        const auto labelIt = row.find("pluginLabel");
        const auto pathIt = row.find("pluginPath");
        if (pluginIdIt == row.end() || labelIt == row.end() || pathIt == row.end() || !pluginIdIt->is_string() ||
            !labelIt->is_string() || !pathIt->is_string())
        {
            continue;
        }

        ExternalPluginVariant variant;
        variant.pluginId = pluginIdIt->get<std::string>();
        variant.label = labelIt->get<std::string>();
        variant.pluginPath = pathIt->get<std::string>();

        const auto stateKeyIt = row.find("stateConfigKey");
        if (stateKeyIt != row.end() && stateKeyIt->is_string() && !stateKeyIt->get<std::string>().empty())
        {
            variant.stateConfigKey = stateKeyIt->get<std::string>();
        }

        const auto stateMapIt = row.find("stateBySemitone");
        if (stateMapIt != row.end() && stateMapIt->is_object())
        {
            for (auto it = stateMapIt->begin(); it != stateMapIt->end(); ++it)
            {
                if (!it.value().is_string())
                {
                    continue;
                }
                try
                {
                    const int semitone = std::stoi(it.key());
                    variant.stateBySemitone[semitone] = it.value().get<std::string>();
                }
                catch (...)
                {
                }
            }
        }

        out.push_back(std::move(variant));
    }

    return out;
}
} // namespace

int main(int argc, char** argv)
{
    bool renderAllDemoAudio = false;
    std::vector<std::string> positionalArgs;
    positionalArgs.reserve(static_cast<size_t>(std::max(0, argc - 1)));

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--all-demo-audio")
        {
            renderAllDemoAudio = true;
            continue;
        }
        if (arg == "-h" || arg == "--help")
        {
            std::cout << "Usage: " << argv[0] << " [--all-demo-audio] [outputRoot] [snapshotLabel]\n"
                      << "  --all-demo-audio  render all demo inputs (default renders first riff only)\n"
                      << "  outputRoot        default: transpose-benchmark-out\n"
                      << "  snapshotLabel     default: snapshot-<UTC timestamp>\n";
            return 0;
        }
        if (!arg.empty() && arg[0] == '-')
        {
            std::cerr << "ERROR: unknown option: " << arg << '\n';
            return 1;
        }
        positionalArgs.push_back(arg);
    }

    if (positionalArgs.size() > 2)
    {
        std::cerr << "ERROR: too many positional arguments\n";
        return 1;
    }

    const fs::path outputRoot =
        positionalArgs.size() > 0 ? fs::path(positionalArgs[0]) : fs::path("transpose-benchmark-out");
    const std::string snapshot = positionalArgs.size() > 1 ? positionalArgs[1] : DefaultSnapshotLabel();
    const fs::path snapshotDir = outputRoot / snapshot;

    std::error_code ec;
    fs::create_directories(snapshotDir, ec);
    if (ec)
    {
        std::cerr << "ERROR: cannot create output directory " << snapshotDir << ": " << ec.message() << '\n';
        return 1;
    }

    guitarfx::RegisterAllEffects();
    auto& registry = guitarfx::EffectRegistry::Instance();

    nlohmann::json report;
    report["snapshot"] = snapshot;
    report["blockSize"] = kBlockSize;
    report["maxRenderSeconds"] = kMaxRenderSeconds;
    report["latencyMeasurementResolutionFrames"] = kEnvelopeDecimation;
    {
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm utc{};
#if defined(_WIN32)
        gmtime_s(&utc, &now);
#else
        gmtime_r(&now, &utc);
#endif
        char buffer[32];
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
        report["generatedAt"] = buffer;
    }
    report["references"] = nlohmann::json::array();
    report["passes"] = nlohmann::json::array();

    if (const auto collectionPath = FindExternalPluginCollectionPath())
    {
        report["externalPlugins"] = LoadExternalPluginCollection(*collectionPath);
        report["externalPluginsSource"] = collectionPath->generic_string();
    }
    else
    {
        report["externalPlugins"] = nlohmann::json::array();
        std::cerr << "WARN: external plugin collection not found at tools/transpose-benchmark/external-plugins.json\n";
    }

    const std::vector<ExternalPluginVariant> externalVariants = ParseExternalPluginVariants(report["externalPlugins"]);

    const std::string pluginHostAlias = [&]() -> std::string {
        const std::string resolved = registry.Resolve("plugin_host");
        if (!resolved.empty())
        {
            return "plugin_host";
        }
        const std::string resolvedJuce = registry.Resolve("juce_plugin_host");
        if (!resolvedJuce.empty())
        {
            return "juce_plugin_host";
        }
        return {};
    }();

    if (!externalVariants.empty() && pluginHostAlias.empty())
    {
        std::cerr << "WARN: external plugin variants are configured but plugin host effect is not registered; "
                     "skipping inline external renders\n";
    }

    const fs::path demoDir(GUITARFX_DEMO_AUDIO_DIR);
    int failures = 0;

    std::vector<std::string> demoSamplesToRender;
    if (renderAllDemoAudio)
    {
        demoSamplesToRender = kDemoSamples;
    }
    else if (!kDemoSamples.empty())
    {
        demoSamplesToRender.push_back(kDemoSamples.front());
    }

    if (demoSamplesToRender.empty())
    {
        std::cerr << "ERROR: no demo samples configured\n";
        return 1;
    }

    for (const auto& sampleName : demoSamplesToRender)
    {
        const fs::path samplePath = demoDir / sampleName;
        const auto audio = LoadDemoSample(samplePath);
        if (!audio)
        {
            std::cerr << "WARN: skipping demo sample (missing or undecodable): " << samplePath << '\n';
            continue;
        }

        const std::string sampleStem = SanitizeForFilename(fs::path(sampleName).stem().string());

        // Dry reference for the report.
        const std::string referenceFile = "reference_" + sampleStem + ".wav";
        if (!WriteWav16(snapshotDir / referenceFile, *audio))
        {
            std::cerr << "ERROR: cannot write reference wav for " << sampleName << '\n';
            ++failures;
            continue;
        }
        nlohmann::json reference;
        reference["sample"] = sampleName;
        reference["wav"] = referenceFile;
        reference["sampleRate"] = audio->sampleRate;
        reference["frames"] = audio->frames();
        report["references"].push_back(reference);

        std::cout << "Sample: " << sampleName << " (" << audio->sampleRate << " Hz, " << audio->frames()
                  << " frames)\n";

        for (const auto& variant : kVariants)
        {
            const auto info = registry.GetTypeInfo(registry.Resolve(variant.alias));
            double minSemitones = -12.0;
            double maxSemitones = 12.0;
            if (info)
            {
                for (const auto& param : info->parameters)
                {
                    if (param.id == "semitones")
                    {
                        minSemitones = param.minValue;
                        maxSemitones = param.maxValue;
                    }
                }
            }

            for (const int semitones : kSemitoneSettings)
            {
                if (semitones < minSemitones || semitones > maxSemitones)
                {
                    continue;
                }

                std::cout << "  " << variant.label << " @ " << semitones << " st ... " << std::flush;
                const auto pass = RunPass(variant, semitones, *audio);
                if (!pass)
                {
                    ++failures;
                    continue;
                }

                const std::string wavFile = SanitizeForFilename(variant.label) + "_" + (semitones < 0 ? "m" : "p") +
                                            std::to_string(std::abs(semitones)) + "st_" + sampleStem + ".wav";
                if (!WriteWav16(snapshotDir / wavFile, pass->render))
                {
                    std::cerr << "ERROR: cannot write " << wavFile << '\n';
                    ++failures;
                    continue;
                }

                nlohmann::json entry = pass->stats;
                entry["effect"] = variant.alias;
                entry["effectLabel"] = variant.label;
                entry["sample"] = sampleName;
                entry["semitones"] = semitones;
                entry["sampleRate"] = audio->sampleRate;
                entry["wav"] = wavFile;
                report["passes"].push_back(entry);

                std::cout << "latency " << pass->stats["reportedLatencySamples"].get<int>() << " rep / "
                          << pass->stats["measuredLatencySamples"].get<int>() << " meas samples, rtf "
                          << pass->stats["realtimeFactor"].get<double>() << FormatPitchSummary(pass->stats) << '\n';
            }
        }

        if (!pluginHostAlias.empty())
        {
            for (const auto& plugin : externalVariants)
            {
                std::vector<int> semitonesToRun;
                if (!plugin.stateBySemitone.empty())
                {
                    for (const int semitone : kSemitoneSettings)
                    {
                        if (plugin.stateBySemitone.contains(semitone))
                        {
                            semitonesToRun.push_back(semitone);
                        }
                    }
                }
                else
                {
                    semitonesToRun.push_back(0);
                }

                if (semitonesToRun.empty())
                {
                    std::cerr << "  WARN: skipping " << plugin.label
                              << " (no usable semitone states in stateBySemitone)\n";
                    continue;
                }

                for (const int semitones : semitonesToRun)
                {
                    std::cout << "  " << plugin.label << " @ " << semitones << " st ... " << std::flush;

                    const std::optional<PassResult> pass = RunConfiguredPass(
                        pluginHostAlias, plugin.label, *audio, semitones, [&](guitarfx::EffectProcessor& effect) {
                            effect.SetParam("mix", 1.0);
                            effect.SetConfig("pluginPath", plugin.pluginPath);
                            if (const auto stateIt = plugin.stateBySemitone.find(semitones);
                                stateIt != plugin.stateBySemitone.end())
                            {
                                effect.SetConfig(plugin.stateConfigKey, stateIt->second);
                            }
                        });
                    if (!pass)
                    {
                        ++failures;
                        continue;
                    }

                    const std::string wavFile = SanitizeForFilename(plugin.label) + "_" + (semitones < 0 ? "m" : "p") +
                                                std::to_string(std::abs(semitones)) + "st_" + sampleStem + ".wav";
                    if (!WriteWav16(snapshotDir / wavFile, pass->render))
                    {
                        std::cerr << "ERROR: cannot write " << wavFile << '\n';
                        ++failures;
                        continue;
                    }

                    nlohmann::json entry = pass->stats;
                    entry["effect"] = plugin.pluginId;
                    entry["effectLabel"] = plugin.label;
                    entry["sourcePluginPath"] = plugin.pluginPath;
                    entry["sample"] = sampleName;
                    entry["semitones"] = semitones;
                    entry["sampleRate"] = audio->sampleRate;
                    entry["wav"] = wavFile;
                    report["passes"].push_back(entry);

                    std::cout << "latency " << pass->stats["reportedLatencySamples"].get<int>() << " rep / "
                              << pass->stats["measuredLatencySamples"].get<int>() << " meas samples, rtf "
                              << pass->stats["realtimeFactor"].get<double>() << FormatPitchSummary(pass->stats) << '\n';
                }
            }
        }
    }

    if (report["passes"].empty())
    {
        std::cerr << "ERROR: no benchmark passes were produced\n";
        return 1;
    }

    const fs::path resultsPath = snapshotDir / "results.json";
    std::ofstream results(resultsPath);
    if (!results)
    {
        std::cerr << "ERROR: cannot write " << resultsPath << '\n';
        return 1;
    }
    results << report.dump(2) << '\n';

    std::cout << "\nWrote " << report["passes"].size() << " passes to " << snapshotDir << '\n';
    std::cout << "Generate the HTML report with:\n"
              << "  python tools/transpose-benchmark/generate_report.py " << outputRoot << '\n';
    return failures == 0 ? 0 : 1;
}
