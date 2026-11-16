#pragma once

#include "dsp/BiquadDesign.h"

#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace guitarfx
{
class RealFFT;

/**
 * The spectrum a polyphonic note detector reads: the level at every third of a semitone from
 * A#1 up to 4.7 kHz, seen through four windows at once, about 187 times a second.
 *
 * One window cannot serve chords. Telling the notes of a low chord apart takes a long window
 * (their partials sit 20 to 40 Hz apart), and a long window sees a new note late and smears where it
 * began. So each frame carries the same grid over the last 85, 43, 21 and 11 ms. What reads them
 * decides which to believe where.
 *
 * Each window gives two grids. The plain one is the spectrum as transformed: through the 21 ms
 * window a lone low E covers 18 semitones of it. The phase-placed one puts each transform bin's
 * energy at the frequency its phase gives instead of at the bin's centre (reassignment: the
 * transform taken through the window's slope, over the plain one, has the offset as its imaginary
 * part). Every bin of one partial's lobe agrees where the partial is, so its energy lands on one
 * third of a semitone even through a window far too short to resolve it, and the same low E covers
 * three bins. Two partials inside one lobe still land together, between the two.
 *
 * The grid is in notes, not hertz: bin 3 (m - kLowestMidi) is note m exactly at A = 440 Hz and its
 * neighbours are a third of a semitone either side, so a note's harmonics sit a fixed number of
 * bins above it whatever the note (HarmonicOffset()) and the same detector reads every note.
 *
 * The input is brought down to about 12 kHz first (11 to 12.6 kHz, by a whole factor), and the
 * windows and the hop are set in time, so the frames mean the same at every sample rate.
 *
 * Values are log10 of the amplitude, 0 for a full-scale sine, floored at kFloor. A phase-placed
 * bin holds the whole of a partial's lobe, so a lone partial reads its amplitude there.
 *
 * Prepare() allocates; Push() does not. The work is in PolySpectrum.cpp, which is built optimised
 * in Debug too (cmake/PolyNotes).
 */
class PolySpectrum
{
  public:
    static constexpr int kWindows = 4;
    /// The plain grids, longest window first, then the phase-placed ones in the same order.
    static constexpr int kChannels = 2 * kWindows;
    static constexpr int kBinsPerSemitone = 3;
    static constexpr int kLowestMidi = 34;
    static constexpr int kHighestMidi = 110;
    static constexpr int kBins = (kHighestMidi - kLowestMidi) * kBinsPerSemitone + 1;
    static constexpr int kFrameSize = kChannels * kBins;
    static constexpr float kFloor = -5.0f;
    static constexpr double kHopSeconds = 64.0 / 12000.0;
    /// The four windows, longest first.
    static constexpr std::array<double, kWindows> kWindowSeconds = {1024.0 / 12000.0, 512.0 / 12000.0, 256.0 / 12000.0,
                                                                    128.0 / 12000.0};

    PolySpectrum();
    ~PolySpectrum();
    PolySpectrum(const PolySpectrum&) = delete;
    PolySpectrum& operator=(const PolySpectrum&) = delete;

    /// How many bins above a note its `ratio`-th partial sits (negative for a ratio under 1).
    [[nodiscard]] static int HarmonicOffset(double ratio)
    {
        return static_cast<int>(std::lround(12.0 * kBinsPerSemitone * std::log2(ratio)));
    }

    [[nodiscard]] static double BinMidi(int bin)
    {
        return kLowestMidi + static_cast<double>(bin) / kBinsPerSemitone;
    }

    void Prepare(double sampleRate);
    void Reset();

    /// Takes one input sample; true when it completed a frame, which Frame() then holds.
    bool Push(float sample);

    /// The newest frame: kChannels grids of kBins.
    [[nodiscard]] const float* Frame() const
    {
        return mFrame.data();
    }

    [[nodiscard]] std::uint64_t FrameCount() const
    {
        return mFrames;
    }

    /// Window `w`'s phase-placed energy of the newest frame, linear and before the log, per grid
    /// bin (kBins of them)...
    [[nodiscard]] const float* PlacedEnergy(int w) const
    {
        return mWindows[static_cast<std::size_t>(w)].placed.data();
    }

    /// ...and the same energy times the exact grid position (in bins, fractional) the phase gave
    /// it. Their sums over a few bins, divided, put a partial between grid bins: where a pitch
    /// tracker reads a note's bend from.
    [[nodiscard]] const float* PlacedMoment(int w) const
    {
        return mWindows[static_cast<std::size_t>(w)].moment.data();
    }

    /// Input samples between frames.
    [[nodiscard]] int HopSamples() const
    {
        return mHop * mDecimation;
    }

    [[nodiscard]] double InternalRate() const
    {
        return mRate;
    }

  private:
    static constexpr double kTargetRate = 12000.0;
    static constexpr double kLowPassRatio = 0.4;
    static constexpr int kSections = 4;

    struct Map
    {
        int first = 0;       ///< the FFT bin a narrow grid bin interpolates from, or the first of a wide one's
        int last = 0;        ///< the last FFT bin of a wide grid bin; under `first` for a narrow one
        float weight = 0.0f; ///< how far from `first` to `first + 1` a narrow bin sits
    };

    struct Window
    {
        int length = 0;
        std::size_t size = 0; ///< of the transform: the window, zero-padded
        std::unique_ptr<RealFFT> fft;
        std::vector<float> shape;
        std::vector<float> slope; ///< the shape's slope per sample
        std::vector<float> buffer;
        std::vector<float> slopeBuffer;
        std::vector<std::complex<float>> spectrum;
        std::vector<std::complex<float>> slopeSpectrum;
        std::vector<float> power;
        std::vector<float> placed;
        std::vector<float> moment; ///< placed, times each part's exact grid position
        float lobeScale = 1.0f;
        double binHz = 1.0;
        std::array<Map, kBins> map{};
    };

    void BuildMap(Window& window) const;
    void Analyse();

    double mSampleRate = 48000.0;
    int mDecimation = 4;
    double mRate = 12000.0;
    int mHop = 64;
    float mLowestHz = 0.0f;
    float mHighestHz = 0.0f;

    std::array<BiquadCoefficients, kSections> mLowPass{};
    std::array<biquad::State, kSections> mFilter{};
    std::array<Window, kWindows> mWindows{};
    std::vector<float> mRing;
    std::vector<float> mFrame;
    std::size_t mWrite = 0;
    int mDecimationCount = 0;
    int mSinceFrame = 0;
    std::uint64_t mFrames = 0;
};
} // namespace guitarfx
