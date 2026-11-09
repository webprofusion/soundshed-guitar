#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <memory>
#include <vector>

namespace guitarfx
{
class SimdFFT;

/**
 * How a window of audio's energy falls across the twelve pitch classes (a chroma vector), for
 * the Scale Guide's chord LISTEN (docs/plans/scale-guide.md): a strummed chord reads as its
 * tones holding most of the twelve.
 *
 * A Hann-windowed FFT of the window; each spectral peak between kMinHz and kMaxHz adds its
 * magnitude to the pitch class it is nearest, less the further it sits from that note's centre,
 * so leakage between two notes counts for little. A peak that is the 3rd, 5th, 6th or 7th harmonic
 * of a comparably strong lower peak is left out: those land on notes the player did not fret, and
 * without this a well played chord's own tones typically hold under 80% of the total. The result
 * sums to 1, or is all zero for silence. Not for the audio thread: it runs on the tuner's worker.
 */
class ChromaAnalyzer
{
  public:
    static constexpr double kMinHz = 70.0;
    static constexpr double kMaxHz = 2000.0;

    using Chroma = std::array<float, 12>;

    /// `size` is the window length, a power of two.
    explicit ChromaAnalyzer(std::size_t size);
    ~ChromaAnalyzer();
    ChromaAnalyzer(const ChromaAnalyzer&) = delete;
    ChromaAnalyzer& operator=(const ChromaAnalyzer&) = delete;

    [[nodiscard]] std::size_t Size() const noexcept
    {
        return mSize;
    }

    /// The chroma of `samples` (Size() of them), pitch class 0 = C, against A = `referenceHz`.
    [[nodiscard]] Chroma Analyze(const float* samples, double sampleRate, double referenceHz = 440.0);

    /// The window length for a sample rate: the power of two nearest 170 ms, so a low E's
    /// harmonics stand apart.
    [[nodiscard]] static std::size_t WindowFor(double sampleRate);

  private:
    struct Peak
    {
        double frequency;
        float magnitude;
    };

    /// Whether mPeaks[index], lowest first, is a foreign harmonic of a peak below it.
    [[nodiscard]] bool IsOvertone(std::size_t index) const;

    std::size_t mSize;
    std::unique_ptr<SimdFFT> mFft;
    std::vector<float> mWindow;
    std::vector<std::complex<float>> mInput;
    std::vector<std::complex<float>> mSpectrum;
    std::vector<float> mMagnitude;
    std::vector<Peak> mPeaks;
};
} // namespace guitarfx
