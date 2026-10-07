/**
 * IrAlignmentTests.cpp — the IR Cabinet's IR B offset, and the analysis that finds where B lines up.
 *
 * IrSlotAlignment delays whichever slot plays later: whole-sample offsets exactly, fractional ones
 * by interpolation, a change by gliding rather than jumping, and nothing at all at 0 (each slot then
 * reads the input it was given). The cab with B moved by an offset must sound the same as the cab
 * with B's file padded with that much silence, and moving A must work the same way.
 *
 * ir_alignment::Analyse must find a known offset to a fraction of a sample, report a polarity
 * flip, keep working when B is a duller "mic" of the same cab, and not be pulled a period off by
 * a strong cabinet resonance.
 *
 * The analyzeIrAlignment message answers a pair with the alignment, and a single IR with only
 * that IR's waveform, which the UI draws as its response.
 */

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "IPluginHost.h"
#include "PluginController.h"
#include "dsp/BiquadDesign.h"
#include "dsp/EffectRegistry.h"
#include "dsp/IrAlignment.h"
#include "dsp/IrSlotAlignment.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetTypes.h"
#include "util/PathEncoding.h"

using namespace guitarfx;

namespace
{
namespace fs = std::filesystem;

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 64;
constexpr double kPi = 3.14159265358979323846;

bool Expect(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << std::endl;
    }

    return condition;
}

/// This run's own folder, so two runs at once cannot delete each other's IRs.
const fs::path& TempDir()
{
    static const fs::path dir =
        fs::temp_directory_path() / ("guitarfx_ir_alignment_tests_" + std::to_string(std::random_device{}()));
    return dir;
}

fs::path WriteMonoWav(const std::string& name, const std::vector<float>& samples)
{
    fs::create_directories(TempDir());
    const auto path = TempDir() / name;
    const auto dataSize = static_cast<std::uint32_t>(samples.size() * sizeof(float));
    const std::uint32_t riffSize = 36 + dataSize;
    const std::uint32_t fmtSize = 16;
    const std::uint16_t format = 3; // IEEE float
    const std::uint16_t channels = 1;
    const auto rate = static_cast<std::uint32_t>(kSampleRate);
    const std::uint32_t byteRate = rate * sizeof(float);
    const std::uint16_t blockAlign = sizeof(float);
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
    file.write(reinterpret_cast<const char*>(samples.data()), dataSize);
    return path;
}

/// A cabinet-like IR: a dozen decaying partials between 150 Hz and 5 kHz, all starting from 0 at
/// `onsetSamples` (which may be fractional, so a delayed copy is exact), plus an optional strong
/// resonance at `resonanceHz`.
std::vector<float> CabLikeIR(double onsetSamples, std::size_t length, double gain = 1.0, double resonanceHz = 0.0)
{
    std::mt19937 rng(1234);
    std::uniform_real_distribution<double> frequency(150.0, 5000.0);
    std::uniform_real_distribution<double> amplitude(0.2, 1.0);
    std::uniform_real_distribution<double> decayMs(1.0, 8.0);

    struct Partial
    {
        double hz, amplitude, decaySeconds;
    };

    std::vector<Partial> partials;

    for (int k = 0; k < 12; ++k)
    {
        partials.push_back({frequency(rng), amplitude(rng), decayMs(rng) / 1000.0});
    }

    if (resonanceHz > 0.0)
    {
        partials.push_back({resonanceHz, 4.0, 0.02});
    }

    std::vector<float> ir(length, 0.0f);

    for (std::size_t n = 0; n < length; ++n)
    {
        const double t = (static_cast<double>(n) - onsetSamples) / kSampleRate;

        if (t < 0.0)
        {
            continue;
        }

        double sum = 0.0;

        for (const auto& partial : partials)
        {
            sum += partial.amplitude * std::exp(-t / partial.decaySeconds) * std::sin(2.0 * kPi * partial.hz * t);
        }

        ir[n] = static_cast<float>(gain * sum);
    }

    return ir;
}

double SamplesToMs(double samples)
{
    return samples * 1000.0 / kSampleRate;
}

// ── IrSlotAlignment ──────────────────────────────────────────────────────

/// Runs `input` (the same on both channels) through the slot delay block by block and keeps what
/// slot `slot` reads on the left. `changeAt` (a block index) moves the offset to `changedMs`.
std::vector<float> RunSlot(IrSlotAlignment& alignment, const std::vector<float>& input, int slot, int changeAt = -1,
                           double changedMs = 0.0)
{
    std::vector<float> out;
    out.reserve(input.size());

    for (std::size_t start = 0, block = 0; start < input.size(); start += kBlockSize, ++block)
    {
        if (static_cast<int>(block) == changeAt)
        {
            alignment.SetOffsetMs(changedMs);
        }

        const int count = static_cast<int>(std::min<std::size_t>(kBlockSize, input.size() - start));
        alignment.Process(input.data() + start, input.data() + start, count);
        const float* read = alignment.SlotInput(slot, 0);
        out.insert(out.end(), read, read + count);
    }

    return out;
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

std::vector<float> Sine(double hz, std::size_t length)
{
    std::vector<float> samples(length);

    for (std::size_t n = 0; n < length; ++n)
    {
        samples[n] = static_cast<float>(std::sin(2.0 * kPi * hz * static_cast<double>(n) / kSampleRate));
    }

    return samples;
}

bool TestZeroOffsetReadsInputUntouched()
{
    IrSlotAlignment alignment;
    alignment.SetBothSlotsLoaded(true);
    alignment.Prepare(kSampleRate, kBlockSize);
    const auto input = Noise(kBlockSize, 1);
    alignment.Process(input.data(), input.data(), kBlockSize);

    bool ok = Expect(alignment.SlotInput(0, 0) == input.data(), "at 0, slot A reads the input itself");
    ok &= Expect(alignment.SlotInput(1, 1) == input.data(), "at 0, slot B reads the input itself");
    return ok;
}

bool TestWholeSampleOffsetIsExact()
{
    bool ok = true;
    const auto input = Noise(4096, 2);

    for (const double offsetMs : {1.0, -1.0})
    {
        IrSlotAlignment alignment;
        alignment.SetOffsetMs(offsetMs);
        alignment.SetBothSlotsLoaded(true);
        alignment.Prepare(kSampleRate, kBlockSize);
        const int delayedSlot = offsetMs > 0.0 ? 1 : 0;
        const auto delayed = RunSlot(alignment, input, delayedSlot);
        alignment.Reset();
        const auto early = RunSlot(alignment, input, 1 - delayedSlot);
        float worstDelayed = 0.0f;
        float worstEarly = 0.0f;

        for (std::size_t n = 0; n < input.size(); ++n)
        {
            const float expected = n >= 48 ? input[n - 48] : 0.0f;
            worstDelayed = std::max(worstDelayed, std::abs(delayed[n] - expected));
            worstEarly = std::max(worstEarly, std::abs(early[n] - input[n]));
        }

        const std::string label = offsetMs > 0.0 ? "B" : "A";
        ok &= Expect(worstDelayed == 0.0f, label + " played 1 ms later is the input 48 samples late (worst " +
                                               std::to_string(worstDelayed) + ")");
        ok &= Expect(worstEarly == 0.0f, "the other slot is the input untouched");
    }

    return ok;
}

bool TestFractionalOffsetInterpolates()
{
    bool ok = true;
    constexpr double kHz = 1000.0;
    const auto input = Sine(kHz, 4096);

    // 2.5 samples is read by the cubic, 0.4 by the linear first sample.
    for (const auto& [samples, tolerance] : {std::pair{2.5, 1e-4}, std::pair{0.4, 5e-3}})
    {
        IrSlotAlignment alignment;
        alignment.SetOffsetMs(SamplesToMs(samples));
        alignment.SetBothSlotsLoaded(true);
        alignment.Prepare(kSampleRate, kBlockSize);
        const auto delayed = RunSlot(alignment, input, 1);
        double worst = 0.0;

        for (std::size_t n = 64; n < input.size(); ++n)
        {
            const double expected = std::sin(2.0 * kPi * kHz * (static_cast<double>(n) - samples) / kSampleRate);
            worst = std::max(worst, std::abs(delayed[n] - expected));
        }

        ok &= Expect(worst < tolerance, "a " + std::to_string(samples) + "-sample delay of a 1 kHz sine is within " +
                                            std::to_string(tolerance) + " (worst " + std::to_string(worst) + ")");
    }

    return ok;
}

bool TestChangeGlidesThenBypasses()
{
    bool ok = true;
    const auto input = Sine(200.0, static_cast<std::size_t>(kSampleRate)); // a second
    IrSlotAlignment alignment;
    alignment.SetBothSlotsLoaded(true);
    alignment.Prepare(kSampleRate, kBlockSize);

    // 2 ms later at block 10, which a jump would hear as a step of up to 2 * sin(pi * 200 * 0.002).
    const auto out = RunSlot(alignment, input, 1, 10, 2.0);
    float largestInputStep = 0.0f;
    float largestOutputStep = 0.0f;

    for (std::size_t n = 1; n < input.size(); ++n)
    {
        largestInputStep = std::max(largestInputStep, std::abs(input[n] - input[n - 1]));
        largestOutputStep = std::max(largestOutputStep, std::abs(out[n] - out[n - 1]));
    }

    // The glide bends pitch by up to 10% (2 ms over a 20 ms time constant), so steps grow by as much.
    ok &= Expect(largestOutputStep < largestInputStep * 1.15f,
                 "moving the offset glides: largest step " + std::to_string(largestOutputStep) + " vs the input's " +
                     std::to_string(largestInputStep));
    ok &= Expect(std::abs(alignment.CurrentOffsetSamples() - 96.0) < 1e-9, "after the glide, B is 96 samples late");

    // Back to 0: once there, the slots read the input itself again.
    RunSlot(alignment, input, 1, 0, 0.0);
    const auto block = Noise(kBlockSize, 3);
    alignment.Process(block.data(), block.data(), kBlockSize);
    ok &= Expect(alignment.CurrentOffsetSamples() == 0.0 && alignment.SlotInput(1, 0) == block.data(),
                 "back at 0, slot B reads the input itself");
    return ok;
}

bool TestOffsetNeedsBothSlots()
{
    IrSlotAlignment alignment;
    alignment.SetOffsetMs(-2.0);
    alignment.SetBothSlotsLoaded(false);
    alignment.Prepare(kSampleRate, kBlockSize);
    const auto input = Noise(kBlockSize, 4);
    alignment.Process(input.data(), input.data(), kBlockSize);
    return Expect(alignment.SlotInput(0, 0) == input.data(), "with one IR, A is never delayed");
}

// ── ir_alignment::Analyse ─────────────────────────────────────────────────

bool TestFindsKnownOffset()
{
    bool ok = true;
    constexpr double kDelay = 37.3; // samples
    constexpr std::size_t kLength = 4800;

    // B 37.3 samples late (and quieter) wants moving earlier by as much; A late wants B later.
    {
        const auto a = CabLikeIR(10.0, kLength);
        const auto b = CabLikeIR(10.0 + kDelay, kLength, 0.5);
        const auto result = ir_alignment::Analyse(a, b, kSampleRate);
        ok &= Expect(result.valid, "a pair of IRs analyses");
        ok &= Expect(std::abs(result.alignedOffsetMs - SamplesToMs(-kDelay)) < SamplesToMs(0.25),
                     "B late by 37.3 samples aligns at " + std::to_string(SamplesToMs(-kDelay)) + " ms, got " +
                         std::to_string(result.alignedOffsetMs));
        ok &= Expect(!result.invertB, "same polarity is not reported inverted");
        ok &= Expect(result.match > 0.95, "an exact copy matches (" + std::to_string(result.match) + ")");
    }
    {
        const auto a = CabLikeIR(10.0 + kDelay, kLength);
        const auto b = CabLikeIR(10.0, kLength);
        const auto result = ir_alignment::Analyse(a, b, kSampleRate);
        ok &= Expect(std::abs(result.alignedOffsetMs - SamplesToMs(kDelay)) < SamplesToMs(0.25),
                     "A late aligns with B moved later, got " + std::to_string(result.alignedOffsetMs));
    }

    return ok;
}

bool TestReportsInvertedPolarity()
{
    const auto a = CabLikeIR(10.0, 4800);
    const auto b = CabLikeIR(30.0, 4800, -0.8);
    const auto result = ir_alignment::Analyse(a, b, kSampleRate);
    bool ok = Expect(result.invertB, "an upside-down copy is reported inverted");
    ok &= Expect(std::abs(result.alignedOffsetMs - SamplesToMs(-20.0)) < SamplesToMs(0.25),
                 "and still lines up, got " + std::to_string(result.alignedOffsetMs));
    return ok;
}

bool TestDullerMicOfSameCab()
{
    // B: the same cab through a darker mic (a 2.5 kHz low-pass), 2 ms further away. The low-pass
    // adds a little delay of its own, which is part of what aligning it has to absorb.
    const auto a = CabLikeIR(10.0, 4800);
    auto b = CabLikeIR(10.0 + 96.0, 4800);
    const auto lowPass = biquad::LowPass(2500.0, biquad::kButterworthQ, kSampleRate);
    biquad::State state;

    for (auto& sample : b)
    {
        sample = static_cast<float>(state.Process(lowPass, sample));
    }

    const auto result = ir_alignment::Analyse(a, b, kSampleRate);
    bool ok = Expect(std::abs(result.alignedOffsetMs + 2.0) < 0.15,
                     "a duller mic 2 ms back aligns near -2 ms, got " + std::to_string(result.alignedOffsetMs));
    ok &= Expect(!result.invertB, "and is not reported inverted");
    return ok;
}

bool TestResonanceStillAligns()
{
    // A 400 Hz resonance gives the correlation side peaks every 2.5 ms, nearly as tall as the
    // main one; B is 3 ms late.
    const auto a = CabLikeIR(10.0, 4800, 1.0, 400.0);
    const auto b = CabLikeIR(10.0 + 144.0, 4800, 1.0, 400.0);
    const auto result = ir_alignment::Analyse(a, b, kSampleRate);
    return Expect(std::abs(result.alignedOffsetMs + 3.0) < SamplesToMs(0.5),
                  "a strong resonance still aligns at -3 ms, got " + std::to_string(result.alignedOffsetMs));
}

bool TestAlignsArrivalsNotALouderReflection()
{
    // B is a far mic: the cab arrives 3 ms late at 0.3, and a reflection 4 ms after that at full
    // level. The reflection correlates best, but the arrivals are what line up. The reflection's
    // own overlap leans on the peak a little, so "lines up" is to within a couple of samples.
    const auto a = CabLikeIR(10.0, 4800);
    const auto direct = CabLikeIR(10.0 + 144.0, 4800, 0.3);
    const auto reflection = CabLikeIR(10.0 + 144.0 + 192.0, 4800, 1.0);
    std::vector<float> b(4800);

    for (std::size_t n = 0; n < b.size(); ++n)
    {
        b[n] = direct[n] + reflection[n];
    }

    const auto result = ir_alignment::Analyse(a, b, kSampleRate);
    const auto& curve = result.matchByOffset;
    const int maxOffset = result.maxOffsetSamples;
    bool ok = Expect(std::abs(curve[static_cast<std::size_t>(maxOffset - 336)]) >
                         std::abs(curve[static_cast<std::size_t>(maxOffset - 144)]),
                     "the reflection correlates best (else this test proves nothing)");
    ok &= Expect(std::abs(result.alignedOffsetMs + 3.0) < SamplesToMs(2.0),
                 "the arrival lines up at -3 ms, got " + std::to_string(result.alignedOffsetMs));
    return ok;
}

bool TestMatchCurveCoversRange()
{
    const auto a = CabLikeIR(10.0, 4800);
    const auto b = CabLikeIR(58.0, 4800);
    const auto result = ir_alignment::Analyse(a, b, kSampleRate, 10.0);
    bool ok = Expect(result.maxOffsetSamples == 480, "10 ms at 48 kHz is 480 samples either way");
    ok &= Expect(result.matchByOffset.size() == 961, "one entry per whole-sample offset");
    ok &=
        Expect(std::abs(result.matchByOffset[480 - 48] - result.match) < 0.01, "the curve peaks at the aligned offset");
    return ok;
}

bool TestSilenceIsNotValid()
{
    const std::vector<float> silent(480, 0.0f);
    const auto a = CabLikeIR(10.0, 4800);
    bool ok = Expect(!ir_alignment::Analyse(a, silent, kSampleRate).valid, "a silent IR does not analyse");
    ok &= Expect(!ir_alignment::Analyse({}, a, kSampleRate).valid, "an empty IR does not analyse");
    return ok;
}

// ── The cab ───────────────────────────────────────────────────────────────

ResourceRef IRRef(const fs::path& path, std::size_t slot)
{
    ResourceRef ir;
    ir.resourceType = "ir";
    ir.filePath = path;
    ir.metadata["resourceSlotIndex"] = std::to_string(slot);
    return ir;
}

std::unique_ptr<EffectProcessor> MakeCab(const fs::path& irA, const fs::path& irB, double offsetMs)
{
    auto cab = EffectRegistry::Instance().Create("cab_ir");
    cab->SetParam("irBlend", 0.5);

    if (offsetMs != 0.0)
    {
        cab->SetParam("slotBOffset", offsetMs);
    }

    std::vector<ResourceRef> refs{IRRef(irA, 0)};
    std::vector<fs::path> paths{irA};

    if (!irB.empty())
    {
        refs.push_back(IRRef(irB, 1));
        paths.push_back(irB);
    }

    cab->LoadResources(refs, paths);
    cab->Prepare(kSampleRate, kBlockSize);
    return cab;
}

std::vector<float> Render(EffectProcessor& cab, const std::vector<float>& input)
{
    std::vector<float> out(input.size());
    std::vector<float> left(kBlockSize);
    std::vector<float> right(kBlockSize);

    for (std::size_t start = 0; start + kBlockSize <= input.size(); start += kBlockSize)
    {
        std::vector<float> in(input.begin() + static_cast<std::ptrdiff_t>(start),
                              input.begin() + static_cast<std::ptrdiff_t>(start + kBlockSize));
        float* inputs[2] = {in.data(), in.data()};
        float* outputs[2] = {left.data(), right.data()};
        cab.Process(inputs, outputs, kBlockSize);
        std::copy(left.begin(), left.end(), out.begin() + static_cast<std::ptrdiff_t>(start));
    }

    return out;
}

float WorstDifference(const std::vector<float>& x, const std::vector<float>& y)
{
    float worst = 0.0f;

    for (std::size_t n = 0; n < x.size() && n < y.size(); ++n)
    {
        worst = std::max(worst, std::abs(x[n] - y[n]));
    }

    return worst;
}

bool TestCabOffsetEqualsPaddedIR()
{
    bool ok = true;
    const auto ir = CabLikeIR(0.0, 1600); // under Standard quality's 43 ms, padded or not
    std::vector<float> padded(48, 0.0f);
    padded.insert(padded.end(), ir.begin(), ir.end());
    const auto plainPath = WriteMonoWav("plain.wav", ir);
    const auto paddedPath = WriteMonoWav("padded.wav", padded);
    const auto input = Noise(64 * 200, 5);

    // B 1 ms later is B's file with 1 ms of silence in front; A 1 ms later is A's.
    auto movedB = MakeCab(plainPath, plainPath, 1.0);
    auto paddedB = MakeCab(plainPath, paddedPath, 0.0);
    const float worstB = WorstDifference(Render(*movedB, input), Render(*paddedB, input));
    ok &=
        Expect(worstB < 1e-5f, "B offset by +1 ms sounds like B padded by 1 ms (worst " + std::to_string(worstB) + ")");

    auto movedA = MakeCab(plainPath, plainPath, -1.0);
    auto paddedA = MakeCab(paddedPath, plainPath, 0.0);
    const float worstA = WorstDifference(Render(*movedA, input), Render(*paddedA, input));
    ok &=
        Expect(worstA < 1e-5f, "B offset by -1 ms sounds like A padded by 1 ms (worst " + std::to_string(worstA) + ")");

    ok &= Expect(movedA->GetLatencySamples() == MakeCab(plainPath, plainPath, 0.0)->GetLatencySamples(),
                 "an offset adds no latency");
    return ok;
}

bool TestCabOffsetNeedsIRB()
{
    const auto ir = CabLikeIR(0.0, 1600);
    const auto path = WriteMonoWav("only-a.wav", ir);
    const auto input = Noise(64 * 50, 6);
    auto withOffset = MakeCab(path, {}, -2.0);
    auto without = MakeCab(path, {}, 0.0);
    return Expect(WorstDifference(Render(*withOffset, input), Render(*without, input)) == 0.0f,
                  "with only IR A loaded, an offset changes nothing");
}

bool TestCabParamRange()
{
    auto cab = EffectRegistry::Instance().Create("cab_ir");
    bool ok = Expect(cab->GetParam("slotBOffset") == 0.0, "the offset starts at 0");
    cab->SetParam("slotBOffset", 25.0);
    ok &= Expect(cab->GetParam("slotBOffset") == 10.0, "the offset is held to +10 ms");
    cab->SetParam("slotBOffset", -0.42);
    ok &= Expect(std::abs(cab->GetParam("slotBOffset") + 0.42) < 1e-12, "the offset reads back as set");
    return ok;
}

// ── The analyzeIrAlignment message ────────────────────────────────────────

class CapturingHost final : public IPluginHost
{
  public:
    explicit CapturingHost(fs::path userDataPath) : mUserDataPath(std::move(userDataPath))
    {
    }

    void SendMessageToUI(const std::string& message) override
    {
        messages.push_back(message);
    }

    void BrowseFileAsync(BrowseFileType, const std::string&,
                         std::function<void(const BrowseFileResult&)> callback) override
    {
        callback(BrowseFileResult{});
    }

    void SaveFileAsync(BrowseFileType, const std::string&, const std::string&,
                       std::function<void(const BrowseFileResult&)> callback) override
    {
        callback(BrowseFileResult{});
    }

    void RunOnMainThread(std::function<void()> fn) override
    {
        fn();
    }

    [[nodiscard]] fs::path GetUserDataPath() const override
    {
        return mUserDataPath;
    }

    [[nodiscard]] fs::path GetBundledAssetsPath() const override
    {
        return mUserDataPath;
    }

    [[nodiscard]] double GetSampleRate() const override
    {
        return kSampleRate;
    }

    [[nodiscard]] int GetBlockSize() const override
    {
        return kBlockSize;
    }

    std::vector<std::string> messages;

  private:
    fs::path mUserDataPath;
};

/// The irAlignment reply to the request with this id, or null.
nlohmann::json ReplyTo(const CapturingHost& host, const std::string& requestId)
{
    for (const auto& text : host.messages)
    {
        auto message = nlohmann::json::parse(text, nullptr, false);

        if (message.is_object() && message.value("type", "") == "irAlignment" &&
            message.value("requestId", "") == requestId)
        {
            return message;
        }
    }

    return nullptr;
}

bool TestAnalyzeMessage()
{
    const fs::path profile = TempDir() / "profile";
    fs::create_directories(profile);
#ifdef _WIN32
    _putenv_s("APPDATA", profile.string().c_str());
#else
    setenv("HOME", profile.string().c_str(), 1);
#endif
    const auto pathA = WriteMonoWav("message-a.wav", CabLikeIR(10.0, 4800));
    const auto pathB = WriteMonoWav("message-b.wav", CabLikeIR(10.0 + 37.3, 4800, 0.5));
    const auto ref = [](const fs::path& path) {
        return nlohmann::json{{"resourceType", "ir"}, {"filePath", util::PathToUtf8(path)}};
    };

    bool ok = true;
    CapturingHost host(profile);
    {
        PluginController controller(host);
        controller.Initialize();
        const auto ask = [&](const std::string& id, nlohmann::json request) {
            request["type"] = "analyzeIrAlignment";
            request["requestId"] = id;
            controller.HandleUIMessage(request.dump());
            return ReplyTo(host, id);
        };

        const auto one = ask("one", {{"irA", ref(pathA)}});
        ok &= Expect(one.is_object() && one.contains("waveformA") && one.contains("normalizeGainA"),
                     "one IR comes back with its waveform and Normalize gain");
        ok &= Expect(one.is_object() && !one.contains("waveformB") && !one.contains("alignedOffsetMs") &&
                         !one.contains("error"),
                     "one IR comes back with nothing to align");

        const auto onlyB = ask("only-b", {{"irB", ref(pathB)}});
        ok &= Expect(onlyB.is_object() && onlyB.contains("waveformB") && !onlyB.contains("waveformA"),
                     "IR B alone comes back as B");

        const auto both = ask("both", {{"irA", ref(pathA)}, {"irB", ref(pathB)}});
        ok &= Expect(both.is_object() && both.contains("waveformA") && both.contains("waveformB"),
                     "a pair comes back with both waveforms");
        ok &= Expect(both.is_object() &&
                         std::abs(both.value("alignedOffsetMs", 99.0) - SamplesToMs(-37.3)) < SamplesToMs(0.25),
                     "a pair comes back aligned");

        const auto missing = ask("missing", {{"irA", ref(pathA)}, {"irB", ref(TempDir() / "no-such.wav")}});
        ok &= Expect(missing.is_object() && missing.value("error", "") == "IR B could not be read",
                     "a missing IR B is reported");

        const auto none = ask("none", nlohmann::json::object());
        ok &= Expect(none.is_object() && none.value("error", "") == "No IR to analyse", "no IR is reported");
    }

    return ok;
}
} // namespace

int main()
{
    RegisterAllEffects();

    bool ok = true;
    ok &= TestZeroOffsetReadsInputUntouched();
    ok &= TestWholeSampleOffsetIsExact();
    ok &= TestFractionalOffsetInterpolates();
    ok &= TestChangeGlidesThenBypasses();
    ok &= TestOffsetNeedsBothSlots();
    ok &= TestFindsKnownOffset();
    ok &= TestReportsInvertedPolarity();
    ok &= TestDullerMicOfSameCab();
    ok &= TestResonanceStillAligns();
    ok &= TestAlignsArrivalsNotALouderReflection();
    ok &= TestMatchCurveCoversRange();
    ok &= TestSilenceIsNotValid();
    ok &= TestCabOffsetEqualsPaddedIR();
    ok &= TestCabOffsetNeedsIRB();
    ok &= TestCabParamRange();
    ok &= TestAnalyzeMessage();

    std::error_code ignored;
    fs::remove_all(TempDir(), ignored);

    if (!ok)
    {
        return 1;
    }

    std::cout << "IrAlignmentTests passed" << std::endl;
    return 0;
}
