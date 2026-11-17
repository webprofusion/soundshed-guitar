/**
 * @file IRCabMonoInputTests.cpp
 * @brief IR Cab on a mono input with a mono IR convolves once for both sides.
 *
 * Told its input is mono (SetInputLayout), the cab lets each slot's right convolver sit out
 * while a mono impulse would only repeat the left's work, and copies the left's result over.
 * None of that may be heard, so every check here compares a cab told mono with one left in
 * stereo, fed the same thing, sample for sample and bit for bit:
 *  - a mono signal, with pan, speaker drive and mic emulation on, short and long IRs, one slot
 *    or both;
 *  - the input turning stereo, which needs the right convolvers' history caught up exactly;
 *  - an IR loaded while the input is mono, whose crossfade runs the outgoing pair in full;
 *  - a stereo IR, which never takes the shortcut.
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "dsp/ChannelLayout.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetTypes.h"

using namespace guitarfx;

namespace
{
namespace fs = std::filesystem;

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 64;

int gFailures = 0;

void Check(bool condition, const std::string& what)
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << what << "\n";

    if (!condition)
    {
        ++gFailures;
    }
}

const fs::path& TempDir()
{
    static const fs::path dir =
        fs::temp_directory_path() / ("guitarfx_ir_cab_mono_input_tests_" + std::to_string(std::random_device{}()));
    return dir;
}

/// A 32-bit float WAV at 48 kHz: mono, or stereo when `right` is given.
fs::path WriteWav(const std::string& name, const std::vector<float>& left, const std::vector<float>* right = nullptr)
{
    fs::create_directories(TempDir());
    const auto path = TempDir() / name;
    std::vector<float> interleaved;

    for (std::size_t n = 0; n < left.size(); ++n)
    {
        interleaved.push_back(left[n]);

        if (right)
        {
            interleaved.push_back((*right)[n]);
        }
    }

    const std::uint16_t channels = right ? 2 : 1;
    const auto dataSize = static_cast<std::uint32_t>(interleaved.size() * sizeof(float));
    const std::uint32_t riffSize = 36 + dataSize;
    const std::uint32_t fmtSize = 16;
    const std::uint16_t format = 3; // IEEE float
    const auto rate = static_cast<std::uint32_t>(kSampleRate);
    const auto blockAlign = static_cast<std::uint16_t>(sizeof(float) * channels);
    const std::uint32_t byteRate = rate * blockAlign;
    const std::uint16_t bitsPerSample = 32;

    std::ofstream file(path, std::ios::binary);
    file.write("RIFF", 4);
    file.write(reinterpret_cast<const char*>(&riffSize), 4);
    file.write("WAVE", 4);
    file.write("fmt ", 4);
    file.write(reinterpret_cast<const char*>(&fmtSize), 4);
    file.write(reinterpret_cast<const char*>(&format), 2);
    file.write(reinterpret_cast<const char*>(&channels), 2);
    file.write(reinterpret_cast<const char*>(&rate), 4);
    file.write(reinterpret_cast<const char*>(&byteRate), 4);
    file.write(reinterpret_cast<const char*>(&blockAlign), 2);
    file.write(reinterpret_cast<const char*>(&bitsPerSample), 2);
    file.write("data", 4);
    file.write(reinterpret_cast<const char*>(&dataSize), 4);
    file.write(reinterpret_cast<const char*>(interleaved.data()), dataSize);
    return path;
}

std::vector<float> Noise(std::size_t length, unsigned seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    std::vector<float> samples(length);

    for (auto& sample : samples)
    {
        sample = noise(rng);
    }

    return samples;
}

/// Decaying noise, distinct per seed. A short one runs on the convolver's direct FIR alone, a
/// long one on its partitioned stages too.
std::vector<float> DecayingNoiseIR(unsigned seed, std::size_t length)
{
    auto ir = Noise(length, seed);
    const double decay = static_cast<double>(length) / 5.0;

    for (std::size_t n = 0; n < ir.size(); ++n)
    {
        ir[n] *= static_cast<float>(std::exp(-static_cast<double>(n) / decay));
    }

    return ir;
}

void Load(EffectProcessor& cab, const fs::path& a, const fs::path& b = {})
{
    std::vector<ResourceRef> refs;
    std::vector<fs::path> paths;

    for (const auto& [path, slot] : {std::pair{a, 0}, std::pair{b, 1}})
    {
        if (path.empty())
        {
            continue;
        }

        ResourceRef ref;
        ref.resourceType = "ir";
        ref.filePath = path;
        ref.metadata["resourceSlotIndex"] = std::to_string(slot);
        refs.push_back(ref);
        paths.push_back(path);
    }

    cab.LoadResources(refs, paths);
}

using Params = std::vector<std::pair<std::string, double>>;

std::unique_ptr<EffectProcessor> MakeCab(const fs::path& a, const fs::path& b, const Params& params)
{
    auto cab = EffectRegistry::Instance().Create("cab_ir");

    for (const auto& [key, value] : params)
    {
        cab->SetParam(key, value);
    }

    Load(*cab, a, b);
    cab->Prepare(kSampleRate, kBlockSize);
    return cab;
}

/// The cab told mono and the cab left in stereo, side by side.
struct Pair
{
    std::unique_ptr<EffectProcessor> mono;
    std::unique_ptr<EffectProcessor> stereo;
    std::vector<float> inL = std::vector<float>(kBlockSize);
    std::vector<float> inR = std::vector<float>(kBlockSize);
    std::vector<float> outs[4] = {std::vector<float>(kBlockSize), std::vector<float>(kBlockSize),
                                  std::vector<float>(kBlockSize), std::vector<float>(kBlockSize)};
    std::mt19937 rng{7};

    /// Runs `blocks` blocks of noise, the same on both sides when `monoSignal`; false at the first
    /// sample where the two cabs differ at all.
    bool Run(int blocks, bool monoSignal)
    {
        std::uniform_real_distribution<float> noise(-0.5f, 0.5f);
        bool same = true;

        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < kBlockSize; ++i)
            {
                inL[static_cast<size_t>(i)] = noise(rng);
                inR[static_cast<size_t>(i)] = monoSignal ? inL[static_cast<size_t>(i)] : noise(rng);
            }

            float* ins[2] = {inL.data(), inR.data()};
            float* monoOut[2] = {outs[0].data(), outs[1].data()};
            float* stereoOut[2] = {outs[2].data(), outs[3].data()};
            mono->Process(ins, monoOut, kBlockSize);
            stereo->Process(ins, stereoOut, kBlockSize);

            same = same && std::memcmp(outs[0].data(), outs[2].data(), kBlockSize * sizeof(float)) == 0 &&
                   std::memcmp(outs[1].data(), outs[3].data(), kBlockSize * sizeof(float)) == 0;
        }

        return same;
    }
};

Pair MakePair(const fs::path& a, const fs::path& b, const Params& params)
{
    Pair pair;
    pair.mono = MakeCab(a, b, params);
    pair.stereo = MakeCab(a, b, params);
    pair.mono->SetInputLayout(ChannelLayout::Mono);
    pair.stereo->SetInputLayout(ChannelLayout::Stereo);
    return pair;
}

void RunCase(const std::string& name, const fs::path& a, const fs::path& b, const fs::path& next, const Params& params)
{
    auto pair = MakePair(a, b, params);
    Check(pair.Run(200, true), name + ": a mono signal comes out the same, bit for bit");

    // The input turns stereo: the right convolvers catch up on the left's history first.
    pair.mono->SetInputLayout(ChannelLayout::Stereo);
    Check(pair.Run(200, false), name + ": then a stereo signal, the same from its first sample");

    // And back to mono, then a new IR while it is: the outgoing pair runs in full for the fade.
    pair.mono->SetInputLayout(ChannelLayout::Mono);
    Check(pair.Run(100, true), name + ": mono again");
    Load(*pair.mono, next, b);
    Load(*pair.stereo, next, b);
    Check(pair.Run(100, true), name + ": an IR loaded while mono crossfades the same");
    pair.mono->SetInputLayout(ChannelLayout::Stereo);
    Check(pair.Run(100, false), name + ": and stereo after it");
}

/// The point of it: on a long IR, the cab told mono spends clearly less time than the one left in
/// stereo. Timed alternately, best of several rounds, so load on the machine weighs on both alike.
void CheckItIsCheaper(const fs::path& ir)
{
    auto pair = MakePair(ir, {}, {{"quality", 3.0}});
    pair.Run(50, true); // past the convergence wait
    std::vector<float> in(kBlockSize, 0.1f);
    std::vector<float> outL(kBlockSize), outR(kBlockSize);
    float* ins[2] = {in.data(), in.data()};
    float* outs[2] = {outL.data(), outR.data()};
    double best[2] = {1.0e9, 1.0e9};

    for (int round = 0; round < 5; ++round)
    {
        for (int which = 0; which < 2; ++which)
        {
            auto& cab = which == 0 ? *pair.mono : *pair.stereo;
            const auto start = std::chrono::steady_clock::now();

            for (int b = 0; b < 400; ++b)
            {
                cab.Process(ins, outs, kBlockSize);
            }

            best[which] =
                std::min(best[which], std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        }
    }

    const double ratio = best[0] / best[1];
    std::cout << "mono-told cab takes " << ratio << " of the stereo-told one's time\n";
    Check(ratio < 0.8, "on a long mono IR, the cab told mono costs clearly less");
}
} // namespace

int main()
{
    RegisterAllEffects();

    const auto shortA = WriteWav("short-a.wav", DecayingNoiseIR(1, 400));
    const auto shortB = WriteWav("short-b.wav", DecayingNoiseIR(2, 400));
    const auto shortC = WriteWav("short-c.wav", DecayingNoiseIR(3, 400));
    const auto longA = WriteWav("long-a.wav", DecayingNoiseIR(4, 24000));
    const auto longB = WriteWav("long-b.wav", DecayingNoiseIR(5, 24000));
    const auto longC = WriteWav("long-c.wav", DecayingNoiseIR(6, 24000));
    const auto stereoLeft = DecayingNoiseIR(7, 2000);
    const auto stereoRight = DecayingNoiseIR(8, 2000);
    const auto stereo = WriteWav("stereo.wav", stereoLeft, &stereoRight);

    const Params full = {{"quality", 3.0}};
    const Params coloured = {{"quality", 3.0},      {"slotAPan", -0.4},    {"slotBPan", 0.6},   {"irBlend", 0.4},
                             {"speakerDrive", 0.5}, {"micEmulation", 1.0}, {"micRadialA", 0.6}, {"micProximityA", 0.4},
                             {"air", 0.5},          {"mix", 0.8},          {"lowCutHz", 90.0}};

    RunCase("short IR, slot A", shortA, {}, shortC, full);
    RunCase("long IR, slot A", longA, {}, longC, full);
    RunCase("long IRs in both slots, panned and driven", longA, longB, longC, coloured);
    RunCase("short IRs in both slots, panned and driven", shortA, shortB, shortC, coloured);
    RunCase("a stereo IR beside a mono one", stereo, longB, longA, coloured);
    CheckItIsCheaper(longA);

    std::error_code ec;
    fs::remove_all(TempDir(), ec);
    std::cout << (gFailures == 0 ? "IRCabMonoInputTests PASSED\n" : "IRCabMonoInputTests FAILED\n");
    return gFailures == 0 ? 0 : 1;
}
