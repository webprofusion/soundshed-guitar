#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace guitarfx
{
/**
 * The time offset between the IR Cabinet's two slots: where IR B sits against IR A.
 *
 * Two cabinet IRs blended together sum like two mics on one speaker. Lined up, they reinforce;
 * a fraction of a millisecond apart, they comb-filter, which can be a fault to fix or a tone to
 * choose. The offset is a delay on the input of whichever slot plays later: B when it is
 * positive, A when it is negative. The earlier slot is never delayed, so the effect's latency
 * does not change.
 *
 * It is a delay on the input rather than one baked into the impulse because SetParam can run on
 * the audio thread (docs/fx-library.md, "Where SetParam runs"), and a delay can move there
 * without a rebuild. A new offset is reached by gliding, a one-pole over about 20 ms, so a nudge
 * or an automation move bends pitch for a moment instead of clicking. At 0 the slots read their
 * input untouched, so a node that never sets an offset sounds exactly as it did before there was one.
 *
 * Fractional delays are read with 4-point Lagrange interpolation. Below one sample that would
 * need a sample from the future, so the first sample of delay is linear between the newest two.
 */
class IrSlotAlignment
{
  public:
    static constexpr double kMaxOffsetMs = 10.0;

    void Prepare(double sampleRate, int maxBlockSize)
    {
        mSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
        mMaxBlockSize = std::max(1, maxBlockSize);
        // The glide's time constant, 20 ms, as a per-sample pole.
        mGlidePole = std::exp(-1.0 / (0.02 * mSampleRate));

        const auto maxDelay = static_cast<std::size_t>(std::ceil(kMaxOffsetMs * mSampleRate / 1000.0));
        const std::size_t needed = maxDelay + static_cast<std::size_t>(mMaxBlockSize) + kInterpolationSpan;
        std::size_t size = 1;

        while (size < needed)
        {
            size <<= 1;
        }

        for (int channel = 0; channel < 2; ++channel)
        {
            mHistory[channel].assign(size, 0.0f);
            mSlotInput[0][channel].assign(static_cast<std::size_t>(mMaxBlockSize), 0.0f);
            mSlotInput[1][channel].assign(static_cast<std::size_t>(mMaxBlockSize), 0.0f);
        }

        mHistoryMask = size - 1;
        mWritePosition = 0;
        mOffsetSamples = TargetSamples();
    }

    /// Clears the history and jumps to the offset, with nothing to glide from.
    void Reset()
    {
        for (auto& channel : mHistory)
        {
            std::fill(channel.begin(), channel.end(), 0.0f);
        }

        mWritePosition = 0;
        mOffsetSamples = TargetSamples();
    }

    /// IR B's offset in ms, positive when B plays later. Takes effect over the glide.
    void SetOffsetMs(double offsetMs)
    {
        mOffsetMs = std::clamp(offsetMs, -kMaxOffsetMs, kMaxOffsetMs);
    }

    [[nodiscard]] double GetOffsetMs() const
    {
        return mOffsetMs;
    }

    /// Whether the slots apply the offset, which only means something with both loaded. Set
    /// before Prepare or Reset to start there; set while playing, the slots glide to it.
    void SetBothSlotsLoaded(bool bothLoaded)
    {
        mBothLoaded = bothLoaded;
    }

    /// Records this block's input, then works out what each slot's convolver reads. `numSamples`
    /// must not exceed the block size given to Prepare.
    void Process(const float* inputL, const float* inputR, int numSamples)
    {
        mInput = {inputL, inputR};
        mDelaying = false;

        if (mHistory[0].empty())
        {
            return; // not prepared
        }

        const std::size_t blockStart = mWritePosition;

        for (int i = 0; i < numSamples; ++i)
        {
            mHistory[0][(blockStart + static_cast<std::size_t>(i)) & mHistoryMask] = inputL[i];
            mHistory[1][(blockStart + static_cast<std::size_t>(i)) & mHistoryMask] = inputR[i];
        }

        mWritePosition = (blockStart + static_cast<std::size_t>(numSamples)) & mHistoryMask;
        const double target = TargetSamples();
        mDelaying = mOffsetSamples != 0.0 || target != 0.0;

        if (!mDelaying)
        {
            return;
        }

        for (int i = 0; i < numSamples; ++i)
        {
            mOffsetSamples = target + (mOffsetSamples - target) * mGlidePole;

            // Close enough to stop: a ten-thousandth of a sample is far below anything audible,
            // and landing exactly on 0 is what lets the next block bypass.
            if (std::abs(mOffsetSamples - target) < 1e-4)
            {
                mOffsetSamples = target;
            }

            const std::size_t position = (blockStart + static_cast<std::size_t>(i)) & mHistoryMask;
            const double delayA = std::max(0.0, -mOffsetSamples);
            const double delayB = std::max(0.0, mOffsetSamples);

            for (int channel = 0; channel < 2; ++channel)
            {
                mSlotInput[0][channel][static_cast<std::size_t>(i)] =
                    delayA > 0.0 ? Read(channel, position, delayA) : mHistory[channel][position];
                mSlotInput[1][channel][static_cast<std::size_t>(i)] =
                    delayB > 0.0 ? Read(channel, position, delayB) : mHistory[channel][position];
            }
        }
    }

    /// What slot 0 (A) or 1 (B) convolves on channel 0 (left) or 1 (right) for the block Process
    /// last saw: that block's input when nothing is delayed, else the slot's delayed copy.
    [[nodiscard]] const float* SlotInput(int slot, int channel) const
    {
        return mDelaying ? mSlotInput[slot][channel].data() : mInput[channel];
    }

    /// The offset the slots are playing at this moment, in samples, partway through a glide.
    [[nodiscard]] double CurrentOffsetSamples() const
    {
        return mOffsetSamples;
    }

  private:
    /// The interpolator reads up to two samples behind the delay, and a sample of rounding margin.
    static constexpr std::size_t kInterpolationSpan = 4;

    [[nodiscard]] double TargetSamples() const
    {
        return mBothLoaded ? mOffsetMs * mSampleRate / 1000.0 : 0.0;
    }

    [[nodiscard]] float Read(int channel, std::size_t position, double delay) const
    {
        const auto& history = mHistory[channel];
        const auto whole = static_cast<std::size_t>(delay);
        const double fraction = delay - static_cast<double>(whole);
        const auto at = [&](std::size_t samplesBack) {
            return static_cast<double>(history[(position - samplesBack) & mHistoryMask]);
        };

        if (whole == 0)
        {
            return static_cast<float>(at(0) + fraction * (at(1) - at(0)));
        }

        // Lagrange through the samples at delays whole-1 .. whole+2, read at whole + fraction.
        const double f = fraction;
        const double cNewer = -f * (f - 1.0) * (f - 2.0) / 6.0;
        const double cAt = (f + 1.0) * (f - 1.0) * (f - 2.0) / 2.0;
        const double cOlder = -(f + 1.0) * f * (f - 2.0) / 2.0;
        const double cOldest = (f + 1.0) * f * (f - 1.0) / 6.0;
        return static_cast<float>(cNewer * at(whole - 1) + cAt * at(whole) + cOlder * at(whole + 1) +
                                  cOldest * at(whole + 2));
    }

    double mSampleRate = 48000.0;
    int mMaxBlockSize = 0;
    double mGlidePole = 0.0;
    double mOffsetMs = 0.0;
    bool mBothLoaded = false;
    double mOffsetSamples = 0.0;
    bool mDelaying = false;

    std::array<std::vector<float>, 2> mHistory;
    std::size_t mHistoryMask = 0;
    std::size_t mWritePosition = 0;
    std::array<std::array<std::vector<float>, 2>, 2> mSlotInput;
    std::array<const float*, 2> mInput = {nullptr, nullptr};
};
} // namespace guitarfx
