/**
 * @file IRReverbQualityTests.cpp
 * @brief Guards the IR reverb "quality" parameter against blocking the DSP lock.
 *
 * Changing quality rebuilds the convolvers. The controller holds mDSPMutex for anything it
 * calls into the chain, and the audio thread only try_locks that mutex and outputs silence
 * when it fails, so a rebuild under the lock silences the entire plugin -- not just the
 * reverb. SetParam() only records the change, and the message thread builds it off the lock
 * and swaps it in under it (DeferredRebuild). These tests measure both halves and verify the
 * effect is producing wet output at the new quality afterwards.
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectProcessor.h"
#include "dsp/effects/BuiltinEffects.h"

namespace
{
namespace fs = std::filesystem;

constexpr double kHostSampleRate = 48000.0;
constexpr int kBlockSize = 512;
constexpr double kPi = 3.14159265358979323846;

// Budget for building a quality change off the DSP lock, on the message thread. The resampled
// IR is cached, so a change only re-truncates it; resampling it again would take seconds.
constexpr double kRebuildBudgetMs = 1000.0;

// What the quality changes hold the DSP lock for, as a share of what building them costs off it.
// The audio thread outputs silence for as long as the lock is held. Taking the work and swapping
// it in take microseconds; rebuilding under the lock, as SetParam used to, holds it for the
// whole build. A share rather than a time, so a busy machine stalling the thread for a moment
// cannot fail it.
constexpr double kLockedShareBudget = 0.1;

std::vector<fs::path> gTempFiles;

/// Writes a stereo 32-bit float WAV.
fs::path WriteStereoWav(const std::vector<float>& left, const std::vector<float>& right, double sampleRate,
                        const std::string& name)
{
    const auto dir = fs::temp_directory_path() / "guitarfx_ir_reverb_tests";
    fs::create_directories(dir);
    const auto path = dir / name;
    gTempFiles.push_back(path);

    const std::size_t frames = std::min(left.size(), right.size());
    std::vector<float> interleaved(frames * 2);

    for (std::size_t i = 0; i < frames; ++i)
    {
        interleaved[i * 2] = left[i];
        interleaved[i * 2 + 1] = right[i];
    }

    const uint32_t dataSize = static_cast<uint32_t>(interleaved.size() * sizeof(float));
    const uint32_t riffSize = 36 + dataSize;
    const uint16_t audioFormat = 3; // IEEE float
    const uint16_t numChannels = 2;
    const uint32_t rate = static_cast<uint32_t>(sampleRate);
    const uint32_t byteRate = rate * numChannels * sizeof(float);
    const uint16_t blockAlign = static_cast<uint16_t>(numChannels * sizeof(float));
    const uint16_t bitsPerSample = 32;
    const uint32_t fmtChunkSize = 16;

    std::ofstream file(path, std::ios::binary);
    file.write("RIFF", 4);
    file.write(reinterpret_cast<const char*>(&riffSize), 4);
    file.write("WAVE", 4);
    file.write("fmt ", 4);
    file.write(reinterpret_cast<const char*>(&fmtChunkSize), 4);
    file.write(reinterpret_cast<const char*>(&audioFormat), 2);
    file.write(reinterpret_cast<const char*>(&numChannels), 2);
    file.write(reinterpret_cast<const char*>(&rate), 4);
    file.write(reinterpret_cast<const char*>(&byteRate), 4);
    file.write(reinterpret_cast<const char*>(&blockAlign), 2);
    file.write(reinterpret_cast<const char*>(&bitsPerSample), 2);
    file.write("data", 4);
    file.write(reinterpret_cast<const char*>(&dataSize), 4);
    file.write(reinterpret_cast<const char*>(interleaved.data()), dataSize);
    return path;
}

/// Exponentially decaying noise: the shape of a real hall/plate IR, long enough that
/// the quality tiers (1.5s / 3s / 6s / unlimited) actually differ.
fs::path MakeReverbIR(double seconds, double sampleRate, const std::string& name)
{
    const std::size_t frames = static_cast<std::size_t>(seconds * sampleRate);
    std::vector<float> left(frames), right(frames);

    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    const double decay = seconds / 4.0; // ~RT60 of the requested length

    for (std::size_t i = 0; i < frames; ++i)
    {
        const double t = static_cast<double>(i) / sampleRate;
        const double envelope = std::exp(-t / decay);
        left[i] = static_cast<float>(noise(rng) * envelope);
        right[i] = static_cast<float>(noise(rng) * envelope);
    }

    // Direct impulse at the head so the IR has a well-defined onset.
    left[0] = 1.0f;
    right[0] = 1.0f;

    return WriteStereoWav(left, right, sampleRate, name);
}

struct RenderResult
{
    double rms = 0.0;
    bool hasNonFinite = false;
    bool wet = false; // differs from the input, which a bypassed reverb copies to its output
};

RenderResult RenderBlocks(guitarfx::EffectProcessor& effect, int blocks)
{
    std::vector<float> inL(kBlockSize), inR(kBlockSize);
    std::vector<float> outL(kBlockSize, 0.0f), outR(kBlockSize, 0.0f);

    RenderResult result;
    double sumSquares = 0.0;
    std::size_t count = 0;

    for (int block = 0; block < blocks; ++block)
    {
        for (int i = 0; i < kBlockSize; ++i)
        {
            const double t = static_cast<double>(block * kBlockSize + i) / kHostSampleRate;
            const float s = static_cast<float>(0.25 * std::sin(2.0 * kPi * 220.0 * t));
            inL[static_cast<std::size_t>(i)] = s;
            inR[static_cast<std::size_t>(i)] = s;
        }

        float* inputs[2] = {inL.data(), inR.data()};
        float* outputs[2] = {outL.data(), outR.data()};
        effect.Process(inputs, outputs, kBlockSize);

        for (int i = 0; i < kBlockSize; ++i)
        {
            const float v = outL[static_cast<std::size_t>(i)];

            if (!std::isfinite(v))
            {
                result.hasNonFinite = true;
            }

            if (std::abs(v - inL[static_cast<std::size_t>(i)]) > 1e-4f)
            {
                result.wet = true;
            }

            sumSquares += static_cast<double>(v) * v;
            ++count;
        }
    }

    result.rms = count > 0 ? std::sqrt(sumSquares / static_cast<double>(count)) : 0.0;
    return result;
}

std::unique_ptr<guitarfx::EffectProcessor> MakeReverb(const fs::path& irPath)
{
    auto effect = guitarfx::EffectRegistry::Instance().Create(guitarfx::EffectGuids::kReverbIr);

    if (!effect)
    {
        return nullptr;
    }

    effect->Prepare(kHostSampleRate, kBlockSize);
    effect->SetParam("mix", 1.0); // wet only, so a bypassed reverb is obvious

    if (!effect->LoadResource(irPath))
    {
        return nullptr;
    }

    return effect;
}

struct QualityChange
{
    bool rebuilt = false;  // the change waited for the message thread, which built it
    double lockedMs = 0.0; // taking the work and committing it: both hold the DSP lock
    double buildMs = 0.0;  // off the lock
};

double MsSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

/// A quality change as the controller makes it: SetParam, then the message thread's part
/// (PluginController::ApplyDeferredNodeRebuilds), with no lock since nothing else runs here.
QualityChange ChangeQuality(guitarfx::EffectProcessor& effect, double quality)
{
    QualityChange change;
    effect.SetParam("quality", quality);

    auto start = std::chrono::steady_clock::now();
    auto work = effect.TakeDeferredRebuild();
    change.lockedMs = MsSince(start);

    if (!work)
    {
        return change;
    }

    start = std::chrono::steady_clock::now();
    work->Build();
    change.buildMs = MsSince(start);

    start = std::chrono::steady_clock::now();
    effect.CommitDeferredRebuild(*work);
    change.lockedMs += MsSince(start);
    change.rebuilt = true;
    return change;
}

// Each quality change holds the DSP lock only to take the work and swap the result in.
bool TestQualityChangeIsNotBlocking()
{
    std::cout << "Test: quality change does not stall the DSP lock... ";

    // 44.1 kHz source against a 48 kHz host forces the resampling path, as a real
    // downloaded reverb IR usually would.
    const auto irPath = MakeReverbIR(8.0, 44100.0, "reverb_8s_44k.wav");
    const auto loadStart = std::chrono::steady_clock::now();
    auto effect = MakeReverb(irPath);
    const double loadMs = MsSince(loadStart);

    if (!effect)
    {
        std::cout << "FAILED (could not create ir_reverb effect)\n";
        return false;
    }

    RenderBlocks(*effect, 4);

    bool ok = true;
    double lockedMs = 0.0;
    double buildMs = 0.0;
    double worstLockedMs = 0.0;
    double worstBuildMs = 0.0;
    std::string worstLabel;
    const std::pair<const char*, double> tiers[] = {
        {"Economy", 0.0}, {"High", 2.0}, {"Standard", 1.0}, {"Full", 3.0}, {"Economy", 0.0}};

    for (const auto& [label, value] : tiers)
    {
        const QualityChange change = ChangeQuality(*effect, value);
        ok = ok && change.rebuilt && change.buildMs <= kRebuildBudgetMs;
        lockedMs += change.lockedMs;
        buildMs += change.buildMs;
        worstLockedMs = std::max(worstLockedMs, change.lockedMs);

        if (change.buildMs > worstBuildMs)
        {
            worstBuildMs = change.buildMs;
            worstLabel = label;
        }
    }

    ok = ok && lockedMs <= buildMs * kLockedShareBudget;
    std::cout << (ok ? "OK" : "FAILED") << " (under the lock: " << std::fixed << std::setprecision(3) << lockedMs
              << " ms in all, worst=" << worstLockedMs << " ms, budget=" << std::setprecision(0)
              << kLockedShareBudget * 100.0 << "% of the " << std::setprecision(1) << buildMs
              << " ms built off it; build: worst=" << worstBuildMs << " ms on " << worstLabel
              << ", budget=" << kRebuildBudgetMs << " ms; load=" << loadMs << " ms)\n";
    return ok;
}

// A rebuild that fails or is never installed leaves the reverb at the old quality, or with no
// convolvers at all, which bypasses it and reads as "the reverb stopped working".
bool TestReverbStillWetAfterQualityChanges()
{
    std::cout << "Test: reverb still produces wet output after quality changes... ";

    const auto irPath = MakeReverbIR(4.0, 48000.0, "reverb_4s_48k.wav");
    auto effect = MakeReverb(irPath);

    if (!effect)
    {
        std::cout << "FAILED (could not create ir_reverb effect)\n";
        return false;
    }

    const RenderResult before = RenderBlocks(*effect, 8);

    for (const double quality : {0.0, 3.0, 1.0, 2.0})
    {
        const bool rebuilt = ChangeQuality(*effect, quality).rebuilt;

        if (!rebuilt || effect->GetParam("quality") != quality || effect->TakeDeferredRebuild())
        {
            std::cout << "FAILED (quality " << quality << " was not built in)\n";
            return false;
        }

        const RenderResult after = RenderBlocks(*effect, 8);

        if (after.hasNonFinite)
        {
            std::cout << "FAILED (non-finite output at quality " << quality << ")\n";
            return false;
        }

        // Convolving a sine with a dense noise IR should not collapse the level, and a reverb
        // left without convolvers would pass its input straight through.
        if (after.rms < before.rms * 0.1 || !after.wet)
        {
            std::cout << "FAILED (output " << (after.wet ? "collapsed" : "dry") << " at quality " << quality << ": rms "
                      << before.rms << " -> " << after.rms << ")\n";
            return false;
        }
    }

    std::cout << "OK\n";
    return true;
}

// A write that does not actually change the quality must not rebuild, or preset re-applies
// and automation resending a held value would restart the reverb's tail.
bool TestRedundantQualityWriteIsIgnored()
{
    std::cout << "Test: redundant quality write does not rebuild... ";

    const auto irPath = MakeReverbIR(4.0, 44100.0, "reverb_4s_44k.wav");
    auto effect = MakeReverb(irPath);

    if (!effect)
    {
        std::cout << "FAILED (could not create ir_reverb effect)\n";
        return false;
    }

    const bool changed = ChangeQuality(*effect, 2.0).rebuilt;
    RenderBlocks(*effect, 4);

    // The same value again asks for nothing; a different one does, and changing back to the
    // built value before the message thread gets to it cancels it.
    effect->SetParam("quality", 2.0);
    const bool redundantIgnored = !effect->TakeDeferredRebuild();
    effect->SetParam("quality", 1.0);
    const bool realWaits = effect->TakeDeferredRebuild() != nullptr;
    effect->SetParam("quality", 2.0);
    const bool revertCancels = !effect->TakeDeferredRebuild();

    const bool ok = changed && redundantIgnored && realWaits && revertCancels;
    std::cout << (ok ? "OK" : "FAILED") << " (change rebuilt=" << changed << ", redundant ignored=" << redundantIgnored
              << ", real change waits=" << realWaits << ", revert cancels=" << revertCancels << ")\n";
    return ok;
}

// Hosts do not always honour the block size we were prepared with. Clamping without
// filling the rest of the buffer leaves stale audio in the tail, which clicks.
bool TestOversizedBlockFillsWholeBuffer()
{
    std::cout << "Test: oversized block writes the whole output buffer... ";

    const auto irPath = MakeReverbIR(2.0, 48000.0, "reverb_2s_48k.wav");
    auto effect = guitarfx::EffectRegistry::Instance().Create(guitarfx::EffectGuids::kReverbIr);

    if (!effect)
    {
        std::cout << "FAILED (could not create ir_reverb effect)\n";
        return false;
    }

    const int prepared = kBlockSize;
    const int oversized = kBlockSize * 2;
    effect->Prepare(kHostSampleRate, prepared);
    effect->SetParam("mix", 1.0);

    if (!effect->LoadResource(irPath))
    {
        std::cout << "FAILED (LoadResource failed)\n";
        return false;
    }

    std::vector<float> inL(static_cast<std::size_t>(oversized));
    std::vector<float> inR(static_cast<std::size_t>(oversized));
    constexpr float kPoison = -999.0f;
    std::vector<float> outL(static_cast<std::size_t>(oversized), kPoison);
    std::vector<float> outR(static_cast<std::size_t>(oversized), kPoison);

    for (int i = 0; i < oversized; ++i)
    {
        const float s = static_cast<float>(0.25 * std::sin(2.0 * kPi * 220.0 * i / kHostSampleRate));
        inL[static_cast<std::size_t>(i)] = s;
        inR[static_cast<std::size_t>(i)] = s;
    }

    float* inputs[2] = {inL.data(), inR.data()};
    float* outputs[2] = {outL.data(), outR.data()};
    effect->Process(inputs, outputs, oversized);

    int untouched = 0;

    for (int i = 0; i < oversized; ++i)
    {
        if (outL[static_cast<std::size_t>(i)] == kPoison || outR[static_cast<std::size_t>(i)] == kPoison)
        {
            ++untouched;
        }
    }

    if (untouched > 0)
    {
        std::cout << "FAILED (" << untouched << "/" << oversized << " output samples left stale)\n";
        return false;
    }

    std::cout << "OK\n";
    return true;
}

void CleanupTempFiles()
{
    for (const auto& path : gTempFiles)
    {
        std::error_code ec;
        fs::remove(path, ec);
    }
}
} // namespace

int main()
{
    std::cout << std::unitbuf;
    std::cout << "IR Reverb Quality Tests\n";
    std::cout << "=======================\n\n";

    guitarfx::RegisterAllEffects();

    int passed = 0;
    int failed = 0;

    for (auto* test : {&TestQualityChangeIsNotBlocking, &TestReverbStillWetAfterQualityChanges,
                       &TestRedundantQualityWriteIsIgnored, &TestOversizedBlockFillsWholeBuffer})
    {
        if ((*test)())
        {
            ++passed;
        }
        else
        {
            ++failed;
        }
    }

    CleanupTempFiles();

    std::cout << "\nResults: " << passed << "/" << (passed + failed) << " tests passed.\n";
    return failed == 0 ? 0 : 1;
}
