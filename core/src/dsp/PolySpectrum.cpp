#include "dsp/PolySpectrum.h"

#include "dsp/FiniteCheck.h"
#include "dsp/RealFFT.h"

#include <algorithm>

namespace guitarfx
{
PolySpectrum::PolySpectrum() = default;
PolySpectrum::~PolySpectrum() = default;

void PolySpectrum::Prepare(double sampleRate)
{
    mSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    mDecimation = std::max(1, static_cast<int>(std::lround(mSampleRate / kTargetRate)));
    mRate = mSampleRate / mDecimation;
    mHop = std::max(1, static_cast<int>(std::lround(kHopSeconds * mRate)));

    for (int section = 0; section < kSections; ++section)
    {
        mLowPass[static_cast<std::size_t>(section)] =
            biquad::LowPass(kLowPassRatio * mRate, biquad::ButterworthSectionQ(2 * kSections, section), mSampleRate);
    }

    std::size_t longest = 0;

    for (int w = 0; w < kWindows; ++w)
    {
        Window& window = mWindows[static_cast<std::size_t>(w)];
        window.length = std::max(8, static_cast<int>(std::lround(kWindowSeconds[static_cast<std::size_t>(w)] * mRate)));
        std::size_t size = 16;

        // Zero-padded to nearly twice the window or more, so the grid reads between transform bins.
        while (size < 2 * static_cast<std::size_t>(window.length) - static_cast<std::size_t>(window.length) / 4)
        {
            size *= 2;
        }

        window.size = size;
        window.fft = std::make_unique<RealFFT>(size);
        window.shape.assign(static_cast<std::size_t>(window.length), 0.0f);
        window.slope.assign(static_cast<std::size_t>(window.length), 0.0f);
        double sum = 0.0;
        double squares = 0.0;

        for (int n = 0; n < window.length; ++n)
        {
            const double phase = 2.0 * biquad::kPi * (n + 0.5) / window.length;
            const double hann = 0.5 - 0.5 * std::cos(phase);
            window.shape[static_cast<std::size_t>(n)] = static_cast<float>(hann);
            window.slope[static_cast<std::size_t>(n)] =
                static_cast<float>(biquad::kPi / window.length * std::sin(phase));
            sum += hann;
            squares += hann * hann;
        }

        // A full-scale sine then reads 1 at its peak...
        const auto scale = static_cast<float>(2.0 / sum);

        for (std::size_t n = 0; n < window.shape.size(); ++n)
        {
            window.shape[n] *= scale;
            window.slope[n] *= scale;
        }

        // ...and its whole lobe, summed, comes to 1 too once multiplied by this.
        window.lobeScale = static_cast<float>(4.0 / (static_cast<double>(size) * squares * scale * scale));
        window.binHz = mRate / static_cast<double>(size);
        window.buffer.assign(size, 0.0f);
        window.slopeBuffer.assign(size, 0.0f);
        window.spectrum.assign(size / 2 + 1, {});
        window.slopeSpectrum.assign(size / 2 + 1, {});
        window.power.assign(size / 2 + 1, 0.0f);
        window.placed.assign(static_cast<std::size_t>(kBins) + 1, 0.0f);
        window.moment.assign(static_cast<std::size_t>(kBins) + 1, 0.0f);
        BuildMap(window);
        longest = std::max(longest, static_cast<std::size_t>(window.length));
    }

    mLowestHz = static_cast<float>(440.0 * std::pow(2.0, (kLowestMidi - 0.5 - 69.0) / 12.0));
    mHighestHz = static_cast<float>(440.0 * std::pow(2.0, (kHighestMidi + 0.5 - 69.0) / 12.0));
    mRing.assign(longest, 0.0f);
    mFrame.assign(static_cast<std::size_t>(kFrameSize), kFloor);
    Reset();
}

void PolySpectrum::Reset()
{
    for (auto& state : mFilter)
    {
        state.Reset();
    }

    std::fill(mRing.begin(), mRing.end(), 0.0f);
    std::fill(mFrame.begin(), mFrame.end(), kFloor);
    mWrite = 0;
    mDecimationCount = 0;
    mSinceFrame = 0;
    mFrames = 0;
}

bool PolySpectrum::Push(float sample)
{
    double y = IsFinite(sample) ? sample : 0.0f;

    for (int section = 0; section < kSections; ++section)
    {
        y = mFilter[static_cast<std::size_t>(section)].Process(mLowPass[static_cast<std::size_t>(section)], y);
    }

    if (++mDecimationCount < mDecimation)
    {
        return false;
    }

    mDecimationCount = 0;
    mRing[mWrite] = static_cast<float>(y);
    mWrite = mWrite + 1 == mRing.size() ? 0 : mWrite + 1;

    if (++mSinceFrame < mHop)
    {
        return false;
    }

    mSinceFrame = 0;
    Analyse();
    ++mFrames;
    return true;
}

void PolySpectrum::BuildMap(Window& window) const
{
    const double binHz = mRate / static_cast<double>(window.size);
    const int top = static_cast<int>(window.size / 2) - 1;

    for (int k = 0; k < kBins; ++k)
    {
        const double hz = 440.0 * std::pow(2.0, (BinMidi(k) - 69.0) / 12.0);
        const double halfWidth = hz * (std::pow(2.0, 1.0 / (24.0 * kBinsPerSemitone)) - 1.0) / binHz;
        const double position = hz / binHz;
        Map& map = window.map[static_cast<std::size_t>(k)];

        if (halfWidth < 0.75)
        {
            map.first = std::clamp(static_cast<int>(std::floor(position)), 1, top - 1);
            map.last = map.first - 1;
            map.weight = static_cast<float>(std::clamp(position - map.first, 0.0, 1.0));
        }
        else
        {
            map.first = std::clamp(static_cast<int>(std::lround(position - halfWidth)), 1, top);
            map.last = std::clamp(static_cast<int>(std::lround(position + halfWidth)), map.first, top);
        }
    }
}

void PolySpectrum::Analyse()
{
    const std::size_t ring = mRing.size();
    const float* samples = mRing.data();

    for (int w = 0; w < kWindows; ++w)
    {
        Window& window = mWindows[static_cast<std::size_t>(w)];
        const auto length = static_cast<std::size_t>(window.length);
        std::size_t read = (mWrite + ring - length) % ring;
        float* buffer = window.buffer.data();
        float* slopeBuffer = window.slopeBuffer.data();
        const float* shape = window.shape.data();
        const float* slope = window.slope.data();

        for (std::size_t n = 0; n < length; ++n)
        {
            const float x = samples[read];
            buffer[n] = x * shape[n];
            slopeBuffer[n] = x * slope[n];
            read = read + 1 == ring ? 0 : read + 1;
        }

        window.fft->Forward(window.spectrum.data(), buffer, buffer + window.size / 2);
        window.fft->Forward(window.slopeSpectrum.data(), slopeBuffer, slopeBuffer + window.size / 2);
        const std::size_t bins = window.size / 2;
        const std::complex<float>* spectrum = window.spectrum.data();
        const std::complex<float>* slopeSpectrum = window.slopeSpectrum.data();
        float* power = window.power.data();
        float* placed = window.placed.data();
        float* moment = window.moment.data();
        power[0] = 0.0f;
        std::fill(window.placed.begin(), window.placed.end(), 0.0f);
        std::fill(window.moment.begin(), window.moment.end(), 0.0f);

        // Each bin's energy belongs to a partial somewhere inside the window's main lobe round it.
        // Where, the phase says: see the class comment. An offset further than the lobe reaches is
        // noise, and is left out.
        const auto perHz = static_cast<float>(mRate / (2.0 * biquad::kPi));
        const float reach = 2.5f * static_cast<float>(mRate / window.length);

        for (std::size_t b = 1; b < bins; ++b)
        {
            const std::complex<float> x = spectrum[b];
            const float energy = std::norm(x);
            power[b] = energy;

            if (energy < 1.0e-12f)
            {
                continue;
            }

            const std::complex<float> d = slopeSpectrum[b];
            const float offsetHz = -perHz * (d.imag() * x.real() - d.real() * x.imag()) / energy;

            if (std::abs(offsetHz) > reach)
            {
                continue;
            }

            const float hz = static_cast<float>(static_cast<double>(b) * window.binHz) + offsetHz;

            if (hz < mLowestHz || hz > mHighestHz)
            {
                continue;
            }

            const float position = kBinsPerSemitone * (12.0f * std::log2(hz * (1.0f / 440.0f)) + 69.0f - kLowestMidi);
            const float clamped = std::clamp(position, 0.0f, static_cast<float>(kBins - 1));
            const auto k = static_cast<std::size_t>(clamped);
            const float t = clamped - static_cast<float>(k);
            placed[k] += energy * (1.0f - t);
            placed[k + 1] += energy * t;
            moment[k] += energy * (1.0f - t) * clamped;
            moment[k + 1] += energy * t * clamped;
        }

        float* placedOut = mFrame.data() + static_cast<std::size_t>(kWindows + w) * kBins;

        for (int k = 0; k < kBins; ++k)
        {
            placedOut[k] = std::max(kFloor, 0.5f * std::log10(window.lobeScale * placed[k] + 1.0e-12f));
        }

        float* plainOut = mFrame.data() + static_cast<std::size_t>(w) * kBins;
        const Map* maps = window.map.data();

        for (int k = 0; k < kBins; ++k)
        {
            const Map& map = maps[k];
            float energy;

            if (map.last < map.first)
            {
                const float a = power[map.first];
                const float b = power[map.first + 1];
                energy = a + map.weight * (b - a);
            }
            else
            {
                energy = 0.0f;

                for (int b = map.first; b <= map.last; ++b)
                {
                    energy = std::max(energy, power[b]);
                }
            }

            plainOut[k] = std::max(kFloor, 0.5f * std::log10(energy + 1.0e-12f));
        }
    }
}
} // namespace guitarfx
