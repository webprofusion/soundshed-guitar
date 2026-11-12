/**
 * EffectEdgeCaseSweepTests - every built-in effect, run where hosts actually take it.
 *
 * EffectProcessorTests runs each registered effect once: 48 kHz, 512-sample blocks, default
 * parameters. Hosts are less tidy than that. This sweep runs every effect that needs no
 * resource through:
 *
 *  - each common sample rate, 44.1 kHz to 192 kHz;
 *  - block sizes from 1 sample to 4096, each Prepared for exactly that size;
 *  - blocks that change size on every call, below the prepared maximum, as FL Studio and
 *    offline renders do;
 *  - every parameter at its minimum and at its maximum on its own, then all of them at once;
 *  - one NaN and one infinity on the input, after which the output must come back finite.
 *
 * Each case checks the output is finite and below kMaxPeak, the bound EffectProcessorTests uses,
 * except all parameters at once, which only has to stay finite: there every gain the effect has
 * can be at its top together, and stacking them is what was asked for. The Graphic EQ's ten
 * bands all at 20 kHz and +18 dB add to 180 dB there, and that is a setting, not a fault.
 * Nothing here measures what an effect sounds like: that is each effect's own tests' job.
 */

#ifndef NOMINMAX
    #define NOMINMAX
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/effects/BuiltinEffects.h"
#include "NonFiniteInputs.h"

namespace
{
constexpr double kPi = 3.14159265358979323846;

/// The bound EffectProcessorTests holds every effect to with a -6 dBFS sine in.
constexpr float kMaxPeak = 100.0f;

constexpr double kSampleRates[] = {44100.0, 48000.0, 88200.0, 96000.0, 192000.0};
constexpr int kBlockSizes[] = {1, 7, 32, 64, 128, 256, 512, 1024, 2048, 4096};

/// Long enough for every effect's latency and first envelope, short enough that ~50 effects
/// through every case stay a few seconds of a Debug run.
constexpr double kRunSeconds = 0.15;
/// After a non-finite input the output has this long to come back finite...
constexpr double kRecoverySeconds = 1.0;
/// ...and must then stay finite for this long.
constexpr double kSettledSeconds = 0.25;

struct Stats
{
    bool finite = true;
    float peak = 0.0f;

    void Add(const float* samples, int count)
    {
        for (int i = 0; i < count; ++i)
        {
            const float value = samples[i];

            // guitarfx::IsFinite, not std::isfinite: tests build with the core's flags, and under
            // Release's -ffast-math std::isfinite folds to true and this check would pass vacuously.
            if (!guitarfx::IsFinite(value))
            {
                finite = false;
                continue;
            }

            peak = std::max(peak, std::fabs(value));
        }
    }

    [[nodiscard]] bool Ok() const
    {
        return finite && peak <= kMaxPeak;
    }

    [[nodiscard]] std::string Describe() const
    {
        std::ostringstream text;

        if (!finite)
        {
            text << "non-finite output";
        }
        else
        {
            text << "peak " << peak;
        }

        return text.str();
    }
};

/// A plucked-string stand-in: a low E with its partials, decaying and re-picked every 75 ms,
/// so dynamics and envelope effects see attacks and pitch effects see a pitch. Stereo, with the
/// right side slightly different so a linked-stereo shortcut cannot hide a bug.
class GuitarSignal
{
  public:
    explicit GuitarSignal(double sampleRate) : mSampleRate(sampleRate)
    {
    }

    void Fill(float* left, float* right, int count)
    {
        const int pickPeriod = std::max(1, static_cast<int>(0.075 * mSampleRate));

        for (int i = 0; i < count; ++i, ++mPosition)
        {
            const double t = static_cast<double>(mPosition % pickPeriod) / mSampleRate;
            const double envelope = std::exp(-t * 18.0);
            const double phase = 2.0 * kPi * 82.41 * static_cast<double>(mPosition) / mSampleRate;
            const double tone = std::sin(phase) + 0.5 * std::sin(2.0 * phase) + 0.25 * std::sin(3.0 * phase);
            const auto sample = static_cast<float>(0.3 * envelope * tone);
            left[i] = sample;
            right[i] = 0.9f * sample;
        }
    }

  private:
    double mSampleRate;
    std::int64_t mPosition = 0;
};

/// A small fixed-seed generator, so a varying-block failure reproduces exactly.
class Lcg
{
  public:
    int Next(int lowest, int highest)
    {
        mState = mState * 6364136223846793005ull + 1442695040888963407ull;
        const auto span = static_cast<std::uint64_t>(highest - lowest + 1);
        return lowest + static_cast<int>((mState >> 33) % span);
    }

  private:
    std::uint64_t mState = 0x5eed5eedull;
};

/// One effect instance and the buffers to drive it with.
class Harness
{
  public:
    Harness(const guitarfx::EffectTypeInfo& info, double sampleRate, int maxBlockSize)
        : mSampleRate(sampleRate), mMaxBlockSize(maxBlockSize), mSignal(sampleRate),
          mInL(static_cast<std::size_t>(maxBlockSize)), mInR(static_cast<std::size_t>(maxBlockSize)),
          mOutL(static_cast<std::size_t>(maxBlockSize)), mOutR(static_cast<std::size_t>(maxBlockSize))
    {
        mEffect = guitarfx::EffectRegistry::Instance().Create(info.type);

        if (mEffect)
        {
            mEffect->Prepare(sampleRate, maxBlockSize);
            mEffect->Reset();

            if (info.requiresTempo)
            {
                mEffect->SetParam("bpm", 120.0);
            }
        }
    }

    [[nodiscard]] bool Created() const
    {
        return mEffect != nullptr;
    }

    [[nodiscard]] guitarfx::EffectProcessor& Effect()
    {
        return *mEffect;
    }

    [[nodiscard]] int SamplesFor(double seconds) const
    {
        return std::max(1, static_cast<int>(seconds * mSampleRate));
    }

    /// Runs `blockSize` blocks of the signal (the prepared maximum when 0) until `samples`.
    Stats Run(int samples, int blockSize = 0)
    {
        Stats stats;
        const int block = blockSize > 0 ? blockSize : mMaxBlockSize;

        for (int done = 0; done < samples;)
        {
            const int count = std::min(block, samples - done);
            ProcessBlock(count, stats);
            done += count;
        }

        return stats;
    }

    /// Like Run, with every block a different size between 1 and the prepared maximum.
    Stats RunVarying(int samples, Lcg& random)
    {
        Stats stats;

        for (int done = 0; done < samples;)
        {
            const int count = std::min(random.Next(1, mMaxBlockSize), samples - done);
            ProcessBlock(count, stats);
            done += count;
        }

        return stats;
    }

    /// One block of the signal with `bad` written over one sample of each side.
    void RunPoisoned(float bad)
    {
        const int count = mMaxBlockSize;
        mSignal.Fill(mInL.data(), mInR.data(), count);
        mInL[static_cast<std::size_t>(count / 2)] = bad;
        mInR[static_cast<std::size_t>(count / 2)] = bad;
        Process(count);
    }

  private:
    void ProcessBlock(int count, Stats& stats)
    {
        mSignal.Fill(mInL.data(), mInR.data(), count);
        Process(count);
        stats.Add(mOutL.data(), count);
        stats.Add(mOutR.data(), count);
    }

    void Process(int count)
    {
        // Stale output from the last block must not pass for this one's.
        std::fill_n(mOutL.begin(), count, 0.0f);
        std::fill_n(mOutR.begin(), count, 0.0f);
        float* inputs[2] = {mInL.data(), mInR.data()};
        float* outputs[2] = {mOutL.data(), mOutR.data()};
        mEffect->Process(inputs, outputs, count);
    }

    double mSampleRate;
    int mMaxBlockSize;
    GuitarSignal mSignal;
    std::unique_ptr<guitarfx::EffectProcessor> mEffect;
    std::vector<float> mInL;
    std::vector<float> mInR;
    std::vector<float> mOutL;
    std::vector<float> mOutR;
};

class Sweep
{
  public:
    void Check(const guitarfx::EffectTypeInfo& info, const std::string& scenario, const Stats& stats,
               bool finiteOnly = false)
    {
        ++mChecks;

        if (finiteOnly ? stats.finite : stats.Ok())
        {
            return;
        }

        ++mFailures;
        std::cout << "  FAIL " << info.displayName << " (" << info.type << "): " << scenario << ": " << stats.Describe()
                  << "\n";
    }

    void Fail(const guitarfx::EffectTypeInfo& info, const std::string& scenario, const std::string& why)
    {
        ++mChecks;
        ++mFailures;
        std::cout << "  FAIL " << info.displayName << " (" << info.type << "): " << scenario << ": " << why << "\n";
    }

    void SampleRates(const guitarfx::EffectTypeInfo& info)
    {
        for (const double rate : kSampleRates)
        {
            Harness harness(info, rate, 128);

            if (!Created(info, harness))
            {
                return;
            }

            Check(info, Label("sample rate", rate), harness.Run(harness.SamplesFor(kRunSeconds)));
        }
    }

    void BlockSizes(const guitarfx::EffectTypeInfo& info)
    {
        for (const int size : kBlockSizes)
        {
            Harness harness(info, 48000.0, size);

            if (!Created(info, harness))
            {
                return;
            }

            Check(info, Label("block size", size), harness.Run(harness.SamplesFor(kRunSeconds)));
        }
    }

    void VaryingBlocks(const guitarfx::EffectTypeInfo& info)
    {
        Harness harness(info, 48000.0, 512);

        if (!Created(info, harness))
        {
            return;
        }

        Lcg random;
        Check(info, "varying block sizes", harness.RunVarying(harness.SamplesFor(kRunSeconds * 2.0), random));
    }

    void ParameterExtremes(const guitarfx::EffectTypeInfo& info)
    {
        const auto run = [&](const std::string& scenario, auto&& setParams, bool finiteOnly = false) {
            Harness harness(info, 48000.0, 256);

            if (!Created(info, harness))
            {
                return false;
            }

            setParams(harness.Effect());
            Check(info, scenario, harness.Run(harness.SamplesFor(kRunSeconds)), finiteOnly);
            return true;
        };

        for (const auto& param : info.parameters)
        {
            for (const bool high : {false, true})
            {
                const double value = high ? param.maxValue : param.minValue;
                const std::string scenario = param.id + (high ? " at max " : " at min ") + Number(value);

                if (!run(scenario, [&](guitarfx::EffectProcessor& effect) { effect.SetParam(param.id, value); }))
                {
                    return;
                }
            }
        }

        if (info.parameters.empty())
        {
            return;
        }

        for (const bool high : {false, true})
        {
            run(
                high ? "every parameter at max" : "every parameter at min",
                [&](guitarfx::EffectProcessor& effect) {
                    for (const auto& param : info.parameters)
                    {
                        effect.SetParam(param.id, high ? param.maxValue : param.minValue);
                    }
                },
                true);
        }
    }

    void NonFiniteRecovery(const guitarfx::EffectTypeInfo& info)
    {
        const float poisons[] = {test_support::RuntimeNaNf(), test_support::RuntimeInfinityf()};
        const char* names[] = {"NaN", "infinity"};

        for (std::size_t p = 0; p < 2; ++p)
        {
            Harness harness(info, 48000.0, 256);

            if (!Created(info, harness))
            {
                return;
            }

            harness.Run(harness.SamplesFor(0.05));
            harness.RunPoisoned(poisons[p]);
            harness.Run(harness.SamplesFor(kRecoverySeconds));
            Check(info, std::string("finite again after a ") + names[p] + " input",
                  harness.Run(harness.SamplesFor(kSettledSeconds)));
        }
    }

    [[nodiscard]] int Checks() const
    {
        return mChecks;
    }

    [[nodiscard]] int Failures() const
    {
        return mFailures;
    }

  private:
    bool Created(const guitarfx::EffectTypeInfo& info, const Harness& harness)
    {
        if (harness.Created())
        {
            return true;
        }

        Fail(info, "create", "the registry returned no effect");
        return false;
    }

    template <typename T> [[nodiscard]] static std::string Label(const char* what, T value)
    {
        return std::string(what) + " " + Number(static_cast<double>(value));
    }

    [[nodiscard]] static std::string Number(double value)
    {
        std::ostringstream text;
        text << value;
        return text.str();
    }

    int mChecks = 0;
    int mFailures = 0;
};
} // namespace

int main()
{
    std::cout << "========================================\n";
    std::cout << "Effect Edge Case Sweep Tests\n";
    std::cout << "========================================\n\n";

    guitarfx::RegisterAllEffects();
    auto types = guitarfx::EffectRegistry::Instance().GetAllTypes();
    std::sort(types.begin(), types.end(), [](const auto& a, const auto& b) { return a.displayName < b.displayName; });

    Sweep sweep;
    int swept = 0;

    for (const auto& info : types)
    {
        // A resource-backed effect does nothing until it has its model or IR, and its own tests
        // load one; without, this would only measure its bypass.
        if (info.requiresResource)
        {
            continue;
        }

        const int failuresBefore = sweep.Failures();
        sweep.SampleRates(info);
        sweep.BlockSizes(info);
        sweep.VaryingBlocks(info);
        sweep.ParameterExtremes(info);
        sweep.NonFiniteRecovery(info);
        ++swept;

        std::cout << std::left << std::setw(32) << info.displayName
                  << (sweep.Failures() == failuresBefore ? "PASS" : "FAIL") << "\n";
    }

    std::cout << "\n========================================\n";
    std::cout << "Swept " << swept << " effects, " << sweep.Checks() << " checks, " << sweep.Failures() << " failed\n";
    std::cout << "========================================\n";

    if (swept == 0)
    {
        std::cout << "ERROR: no effects were swept\n";
        return 1;
    }

    return sweep.Failures() == 0 ? 0 : 1;
}
