#pragma once

#define _USE_MATH_DEFINES
#include <cmath>

#ifndef M_PI
    #define M_PI 3.14159265358979323846
#endif

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include <algorithm>
#include <array>

namespace guitarfx
{
/**
 * 4-band parametric equalizer implemented as a serial chain of biquad (second-order IIR)
 * sections using the Direct Form I difference equation.
 *
 * Band layout (fixed topology):
 *   [0] Low shelf      — shelving filter below ~20–500 Hz
 *   [1] Low-mid peak   — peaking bell filter, 100–2000 Hz
 *   [2] High-mid peak  — peaking bell filter, 500–8000 Hz
 *   [3] High shelf     — shelving filter above ~2000–16000 Hz
 *
 * All biquad coefficients are derived from the RBJ Audio EQ Cookbook
 * (Robert Bristow-Johnson, https://www.w3.org/2011/audio/audio-eq-cookbook.html).
 *
 * Q on shelf bands controls the shelf slope / resonance at the corner frequency:
 *   Q = 0.707 (1/sqrt(2)) → maximally-flat Butterworth shelf (no resonant bump)
 *   Q > 0.707              → resonant peak/dip near the corner (Pultec-style)
 *   Q < 0.707              → gentler, more gradual shelf transition
 */
class ParametricEQEffect : public EffectProcessor
{
  public:
    // Stores sample rate and block size, then computes initial biquad coefficients
    // and clears all filter state. Must be called before Process().
    void Prepare(double sampleRate, int maxBlockSize) override
    {
        if (!ValidatePrepare(sampleRate, maxBlockSize))
        {
            return;
        }

        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;
        UpdateCoefficients();
        Reset();
    }

    // Zeros all biquad delay-line state. Called after a discontinuity (e.g. transport
    // stop) to prevent stale state from producing clicks or tails on the next play.
    void Reset() override
    {
        for (auto& band : mBands)
        {
            ResetBandState(band);
        }
    }

    // Runs the 4-band EQ sample-by-sample using Direct Form I biquad sections.
    //
    // Direct Form I difference equation for one biquad:
    //   y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2]
    //                   - a1*y[n-1] - a2*y[n-2]
    //
    // State variable naming convention (matched for both L and R channels):
    //   z1, z2  — input  delay lines: z1 = x[n-1], z2 = x[n-2]
    //   x1, x2  — output delay lines: x1 = y[n-1], x2 = y[n-2]
    // ("x" is reused for output history here to avoid colliding with the
    //  DSP convention of x for input; the code predates that naming.)
    //
    // Stability guard: if the output is non-finite (overflow from extreme
    // parameters or denormals accumulating), the delay lines are zeroed and
    // the dry sample is passed through rather than corrupting the stream.
    void Process(float** inputs, float** outputs, int numSamples) override
    {
        for (int i = 0; i < numSamples; ++i)
        {
            // Sanitised once here, not once per band: after the first band the sample is
            // that band's output, which the finiteness check below has already vetted.
            float sampleL = SanitizeSample(inputs[0] ? inputs[0][i] : 0.0f);
            float sampleR = SanitizeSample(inputs[1] ? inputs[1][i] : 0.0f);

            // Each band is applied serially; the output of one feeds the input of the next.
            for (auto& band : mBands)
            {
                if (!band.enabled)
                {
                    continue;
                }

                // The delay lines hold nothing but finite values already — a reset writes
                // zeros, and every value stored below is either the sanitised input or a
                // checked output — so they are not re-checked here. They used to be, at
                // eight std::isfinite calls per band per sample; MSVC compiles each of
                // those into an _fdtest call, and on a four-band EQ that alone was most of
                // this effect's cost.

                // Left channel — Direct Form I: y = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
                float outL = band.b0 * sampleL + band.b1 * band.z1L + band.b2 * band.z2L - band.a1 * band.x1L -
                             band.a2 * band.x2L;

                if (!std::isfinite(outL))
                {
                    ResetBandState(band); // clear both L and R state on overflow
                    outL = sampleL;
                }

                // Shift the input and output delay lines
                band.z2L = band.z1L; // x[n-2] ← x[n-1]
                band.z1L = sampleL;  // x[n-1] ← x[n]
                band.x2L = band.x1L; // y[n-2] ← y[n-1]
                band.x1L = outL;     // y[n-1] ← y[n]
                sampleL = outL;

                // Right channel — identical DF-I with independent state
                float outR = band.b0 * sampleR + band.b1 * band.z1R + band.b2 * band.z2R - band.a1 * band.x1R -
                             band.a2 * band.x2R;

                if (!std::isfinite(outR))
                {
                    ResetBandState(band);
                    outR = sampleR;
                }

                band.z2R = band.z1R;
                band.z1R = sampleR;
                band.x2R = band.x1R;
                band.x1R = outR;
                sampleR = outR;
            }

            if (outputs[0])
            {
                outputs[0][i] = sampleL;
            }

            if (outputs[1])
            {
                outputs[1][i] = sampleR;
            }
        }
    }

    void SetParam(const std::string& key, double value) override
    {
        if (key == "lowGain")
        {
            mBands[0].gainDb = value;
        }
        else if (key == "lowFreq")
        {
            mBands[0].freq = value;
        }
        else if (key == "lowQ")
        {
            mBands[0].q = value;
        }
        else if (key == "lowMidGain")
        {
            mBands[1].gainDb = value;
        }
        else if (key == "lowMidFreq")
        {
            mBands[1].freq = value;
        }
        else if (key == "lowMidQ")
        {
            mBands[1].q = value;
        }
        else if (key == "highMidGain")
        {
            mBands[2].gainDb = value;
        }
        else if (key == "highMidFreq")
        {
            mBands[2].freq = value;
        }
        else if (key == "highMidQ")
        {
            mBands[2].q = value;
        }
        else if (key == "highGain")
        {
            mBands[3].gainDb = value;
        }
        else if (key == "highFreq")
        {
            mBands[3].freq = value;
        }
        else if (key == "highQ")
        {
            mBands[3].q = value;
        }

        ClampBandParams();
        UpdateCoefficients();
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "lowGain")
        {
            return mBands[0].gainDb;
        }

        if (key == "lowFreq")
        {
            return mBands[0].freq;
        }

        if (key == "lowQ")
        {
            return mBands[0].q;
        }

        if (key == "lowMidGain")
        {
            return mBands[1].gainDb;
        }

        if (key == "lowMidFreq")
        {
            return mBands[1].freq;
        }

        if (key == "lowMidQ")
        {
            return mBands[1].q;
        }

        if (key == "highMidGain")
        {
            return mBands[2].gainDb;
        }

        if (key == "highMidFreq")
        {
            return mBands[2].freq;
        }

        if (key == "highMidQ")
        {
            return mBands[2].q;
        }

        if (key == "highGain")
        {
            return mBands[3].gainDb;
        }

        if (key == "highFreq")
        {
            return mBands[3].freq;
        }

        if (key == "highQ")
        {
            return mBands[3].q;
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "eq_parametric";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "eq";
    }

  private:
    // Holds all parameters, computed biquad coefficients, and delay-line state
    // for a single second-order IIR section.
    struct Band
    {
        bool enabled = true;  // when false the band is bypassed entirely
        double gainDb = 0.0;  // boost/cut in dB (±12 dB range)
        double freq = 1000.0; // corner / centre frequency in Hz
        double q = 1.0;       // Q factor:  bandwidth for peaking bands,
                              //            slope/resonance for shelf bands
                              //            (0.707 = maximally-flat Butterworth shelf)
        bool isShelf = false; // true → shelf filter, false → peaking bell

        // Normalised biquad coefficients (denominator leading coefficient a0 = 1).
        // Transfer function:  H(z) = (b0 + b1*z^-1 + b2*z^-2)
        //                           / (1  + a1*z^-1 + a2*z^-2)
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f; // feedforward (numerator)
        float a1 = 0.0f, a2 = 0.0f;            // feedback    (denominator, sign convention: subtracted)

        // Direct Form I delay lines — stored as floats for cache efficiency.
        // z1/z2 = input history (x[n-1], x[n-2])
        // x1/x2 = output history (y[n-1], y[n-2])
        float z1L = 0.0f, z2L = 0.0f; // left  channel input  delay
        float z1R = 0.0f, z2R = 0.0f; // right channel input  delay
        float x1L = 0.0f, x2L = 0.0f; // left  channel output delay
        float x1R = 0.0f, x2R = 0.0f; // right channel output delay
    };

    // Recomputes all four biquad coefficient sets from the current band parameters.
    // Called once during Prepare() and again whenever SetParam() changes a value.
    // Filter delay-line state is deliberately preserved across coefficient updates so
    // live parameter changes transition smoothly without an audible click (see note
    // at the end of this function).
    void UpdateCoefficients()
    {
        ClampBandParams();

        // Band 0: low shelving filter (boosts/cuts everything below `freq`)
        mBands[0].isShelf = true;
        CalculateLowShelf(mBands[0]);

        // Bands 1–2: peaking bell filters (boost/cut centred at `freq`)
        mBands[1].isShelf = false;
        CalculatePeaking(mBands[1]);

        mBands[2].isShelf = false;
        CalculatePeaking(mBands[2]);

        // Band 3: high shelving filter (boosts/cuts everything above `freq`)
        mBands[3].isShelf = true;
        CalculateHighShelf(mBands[3]);

        // IMPORTANT: do NOT zero the delay lines here.
        //
        // Direct Form I state holds past input/output samples (x[n-1..2], y[n-1..2]),
        // which remain dimensionally valid regardless of the coefficient values. All
        // four band types here (peaking + RBJ shelves) are unconditionally stable, so
        // swapping coefficients while preserving state lets the filter transition
        // smoothly and continuously — the standard technique for click-free parameter
        // automation. Zeroing the state instead discards the recent signal history and
        // forces an abrupt output discontinuity, which is audible as a click/shear when
        // the user drags an EQ control. State is only cleared on genuine discontinuities
        // (Prepare()/Reset()) or on numerical overflow inside Process().
        for (auto& band : mBands)
        {
            SanitizeBandCoefficients(band); // replace non-finite coefficients with identity
        }
    }

    // Validates and constrains all band parameters to numerically safe ranges before
    // coefficient computation.  Each frequency ceiling is also guarded against the
    // Nyquist limit (sampleRate * 0.49) so the biquad formulae stay well-conditioned
    // at non-standard sample rates (e.g. 88.2 kHz, 96 kHz).
    void ClampBandParams()
    {
        // Hard cap slightly below Nyquist to avoid cos/sin degeneracy at w0 = pi.
        const double maxFreq = std::max(20.0, mSampleRate * 0.49);

        // Band 0 — low shelf: 20 Hz–500 Hz corner, ±12 dB, Q default 0.707 (Butterworth)
        mBands[0].gainDb = ClampFinite(mBands[0].gainDb, -12.0, 12.0, 0.0);
        mBands[0].freq = ClampFinite(mBands[0].freq, 20.0, std::min(500.0, maxFreq), 100.0);
        mBands[0].q = ClampFinite(mBands[0].q, 0.1, 10.0, 0.707);

        // Band 1 — low-mid peak: 100 Hz–2 kHz centre, ±12 dB, Q default 1.0
        mBands[1].gainDb = ClampFinite(mBands[1].gainDb, -12.0, 12.0, 0.0);
        mBands[1].freq = ClampFinite(mBands[1].freq, 100.0, std::min(2000.0, maxFreq), 400.0);
        mBands[1].q = ClampFinite(mBands[1].q, 0.1, 10.0, 1.0);

        // Band 2 — high-mid peak: 500 Hz–8 kHz centre, ±12 dB, Q default 1.0
        mBands[2].gainDb = ClampFinite(mBands[2].gainDb, -12.0, 12.0, 0.0);
        mBands[2].freq = ClampFinite(mBands[2].freq, 500.0, std::min(8000.0, maxFreq), 2000.0);
        mBands[2].q = ClampFinite(mBands[2].q, 0.1, 10.0, 1.0);

        // Band 3 — high shelf: 2 kHz–16 kHz corner, ±12 dB, Q default 0.707 (Butterworth)
        mBands[3].gainDb = ClampFinite(mBands[3].gainDb, -12.0, 12.0, 0.0);
        mBands[3].freq = ClampFinite(mBands[3].freq, 2000.0, std::min(16000.0, maxFreq), 8000.0);
        mBands[3].q = ClampFinite(mBands[3].q, 0.1, 10.0, 0.707);
    }

    // Returns `fallback` if `value` is NaN or ±infinity; otherwise clamps to [minimum, maximum].
    static double ClampFinite(double value, double minimum, double maximum, double fallback)
    {
        if (!std::isfinite(value))
        {
            return fallback;
        }

        return std::clamp(value, minimum, maximum);
    }

    // Replaces non-finite audio samples (NaN, ±inf, denormals produce 0 here) with silence.
    static float SanitizeSample(float sample)
    {
        return std::isfinite(sample) ? sample : 0.0f;
    }

    // Zeros all four delay lines for both channels. Used after coefficient changes
    // and on transport reset to prevent stale history from bleeding into new audio.
    static void ResetBandState(Band& band)
    {
        band.z1L = band.z2L = 0.0f;
        band.z1R = band.z2R = 0.0f;
        band.x1L = band.x2L = 0.0f;
        band.x1R = band.x2R = 0.0f;
    }

    // Sets the biquad to a unity-gain all-pass: H(z) = 1.  Used when gain is
    // effectively zero or when coefficient calculation would be degenerate.
    static void SetIdentity(Band& band)
    {
        band.b0 = 1.0f;
        band.b1 = band.b2 = band.a1 = band.a2 = 0.0f;
    }

    // Guards against NaN/inf coefficients that could arise from extreme parameter
    // combinations (e.g. freq at Nyquist, very low Q).  Falls back to identity.
    static void SanitizeBandCoefficients(Band& band)
    {
        if (!std::isfinite(band.b0) || !std::isfinite(band.b1) || !std::isfinite(band.b2) || !std::isfinite(band.a1) ||
            !std::isfinite(band.a2))
        {
            SetIdentity(band);
        }
    }

    // Computes biquad coefficients for a peaking (bell) EQ band.
    //
    // RBJ Cookbook — "peakingEQ" formula:
    //
    //   A     = 10^(dB/40)          — linear amplitude factor; dB/40 because the
    //                                  filter applies A in both numerator and denominator,
    //                                  so the gain at w0 is A/（1/A) = A^2 = 10^(dB/20).
    //   w0    = 2π * freq / Fs      — normalised angular frequency (radians/sample)
    //   alpha = sin(w0) / (2*Q)     — bandwidth control; larger Q → narrower bell
    //
    // Unnormalised coefficients (a0 used only for normalisation, not stored):
    //   b0 =  1 + alpha*A
    //   b1 = -2*cos(w0)              (same for numerator and denominator)
    //   b2 =  1 - alpha*A
    //   a0 =  1 + alpha/A            (normalisation divisor)
    //   a1 = -2*cos(w0)
    //   a2 =  1 - alpha/A
    //
    // All stored coefficients are divided by a0 so the difference equation needs
    // no additional division at run-time.
    void CalculatePeaking(Band& band)
    {
        // Gain below 0.001 dB is inaudible; use identity to save CPU.
        if (std::abs(band.gainDb) < 0.001)
        {
            SetIdentity(band);
            return;
        }

        // A = sqrt(linear gain) — appears as A and 1/A in the RBJ formula.
        const double A = std::pow(10.0, band.gainDb / 40.0);
        // w0: angular frequency in radians per sample.
        const double w0 = 2.0 * M_PI * band.freq / mSampleRate;
        const double cosw0 = std::cos(w0);
        const double sinw0 = std::sin(w0);
        // alpha relates to the -3 dB bandwidth: BW = arcsin(alpha) / (π/Fs).
        // Higher Q → smaller alpha → narrower peak.
        const double alpha = sinw0 / (2.0 * band.q);

        // a0 is the normalisation factor; guard against near-zero to avoid division explosion.
        const double a0 = 1.0 + alpha / A;

        if (!std::isfinite(a0) || std::abs(a0) < 1.0e-9)
        {
            SetIdentity(band);
            return;
        }

        // Store pre-divided (normalised) coefficients.
        band.b0 = static_cast<float>((1.0 + alpha * A) / a0);
        band.b1 = static_cast<float>((-2.0 * cosw0) / a0);
        band.b2 = static_cast<float>((1.0 - alpha * A) / a0);
        band.a1 = static_cast<float>((-2.0 * cosw0) / a0);
        band.a2 = static_cast<float>((1.0 - alpha / A) / a0);
    }

    // Computes biquad coefficients for a low-shelving filter.
    //
    // RBJ Cookbook — "lowShelf" formula (Q-parameterised variant):
    //
    //   A     = 10^(dB/40)            — same amplitude factor as peaking
    //   w0    = 2π * freq / Fs        — corner frequency in radians/sample
    //   alpha = sin(w0) / (2*Q)       — controls shelf slope and resonance:
    //                                    Q=0.707 → maximally-flat (no bump),
    //                                    Q>0.707 → resonant peak at corner
    //   sqrtA = sqrt(A) = 10^(dB/80)  — appears in cross-terms; keeps the
    //                                    mid-band level continuous across A values
    //
    // Unnormalised RBJ low-shelf coefficients:
    //   b0 =  A * [ (A+1) - (A-1)*cos(w0) + 2*sqrt(A)*alpha ]
    //   b1 =  2*A * [ (A-1) - (A+1)*cos(w0)                 ]
    //   b2 =  A * [ (A+1) - (A-1)*cos(w0) - 2*sqrt(A)*alpha ]
    //   a0 =        (A+1) + (A-1)*cos(w0) + 2*sqrt(A)*alpha   (normalisation divisor)
    //   a1 = -2 * [ (A-1) + (A+1)*cos(w0)                   ]
    //   a2 =        (A+1) + (A-1)*cos(w0) - 2*sqrt(A)*alpha
    //
    // Interpretation of (A±1) / cos(w0) terms:
    //   (A+1) and (A-1) set the passband and stopband asymptotes.
    //   The cos(w0) terms position the corner in frequency.
    //   The 2*sqrt(A)*alpha term controls the Q/slope of the transition.
    void CalculateLowShelf(Band& band)
    {
        if (std::abs(band.gainDb) < 0.001)
        {
            SetIdentity(band);
            return;
        }

        const double A = std::pow(10.0, band.gainDb / 40.0); // linear amplitude factor
        const double w0 = 2.0 * M_PI * band.freq / mSampleRate;
        const double cosw0 = std::cos(w0);
        const double sinw0 = std::sin(w0);
        // alpha encodes shelf slope; sinw0/(2*Q) mirrors the peaking definition so
        // the Q control feels consistent across all four bands.
        const double alpha = sinw0 / (2.0 * band.q);
        // sqrtA = A^0.5 = 10^(dB/80); used in cross-terms to interpolate smoothly
        // between the passband (gain = A^2 = 10^(dB/20)) and the stopband (gain = 1).
        const double sqrtA = std::sqrt(A);

        const double a0 = (A + 1.0) + (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha;

        if (!std::isfinite(a0) || std::abs(a0) < 1.0e-9)
        {
            SetIdentity(band);
            return;
        }

        band.b0 = static_cast<float>(A * ((A + 1.0) - (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha) / a0);
        band.b1 = static_cast<float>(2.0 * A * ((A - 1.0) - (A + 1.0) * cosw0) / a0);
        band.b2 = static_cast<float>(A * ((A + 1.0) - (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0);
        band.a1 = static_cast<float>(-2.0 * ((A - 1.0) + (A + 1.0) * cosw0) / a0);
        band.a2 = static_cast<float>(((A + 1.0) + (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0);
    }

    // Computes biquad coefficients for a high-shelving filter.
    //
    // RBJ Cookbook — "highShelf" formula (Q-parameterised variant):
    //
    // The high-shelf is the frequency-dual of the low-shelf: the same (A±1)/cos
    // structure applies, but the signs on the cos(w0) terms are flipped so that
    // the gain asymptotes swap sides (passband above w0, stopband below).
    //
    // Unnormalised RBJ high-shelf coefficients:
    //   b0 =  A * [ (A+1) + (A-1)*cos(w0) + 2*sqrt(A)*alpha ]
    //   b1 = -2*A * [ (A-1) + (A+1)*cos(w0)                 ]    ← sign flipped vs low-shelf
    //   b2 =  A * [ (A+1) + (A-1)*cos(w0) - 2*sqrt(A)*alpha ]
    //   a0 =        (A+1) - (A-1)*cos(w0) + 2*sqrt(A)*alpha       ← sign flipped vs low-shelf
    //   a1 =  2 * [ (A-1) - (A+1)*cos(w0)                   ]    ← sign flipped vs low-shelf
    //   a2 =        (A+1) - (A-1)*cos(w0) - 2*sqrt(A)*alpha
    //
    // Key sign differences from CalculateLowShelf():
    //   a0: +(A+1) − (A-1)*cos  vs  +(A+1) + (A-1)*cos  in low-shelf
    //   b0: (A+1) + (A-1)*cos   vs  (A+1) − (A-1)*cos
    //   b1: −2A*(...)            vs  +2A*(...)
    //   a1: +2*(...)             vs  −2*(...)
    // These inversions mirror the filter around π/2 in the z-plane.
    void CalculateHighShelf(Band& band)
    {
        if (std::abs(band.gainDb) < 0.001)
        {
            SetIdentity(band);
            return;
        }

        const double A = std::pow(10.0, band.gainDb / 40.0);
        const double w0 = 2.0 * M_PI * band.freq / mSampleRate;
        const double cosw0 = std::cos(w0);
        const double sinw0 = std::sin(w0);
        const double alpha = sinw0 / (2.0 * band.q);
        const double sqrtA = std::sqrt(A);

        const double a0 = (A + 1.0) - (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha;

        if (!std::isfinite(a0) || std::abs(a0) < 1.0e-9)
        {
            SetIdentity(band);
            return;
        }

        band.b0 = static_cast<float>(A * ((A + 1.0) + (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha) / a0);
        band.b1 = static_cast<float>(-2.0 * A * ((A - 1.0) + (A + 1.0) * cosw0) / a0);
        band.b2 = static_cast<float>(A * ((A + 1.0) + (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0);
        band.a1 = static_cast<float>(2.0 * ((A - 1.0) - (A + 1.0) * cosw0) / a0);
        band.a2 = static_cast<float>(((A + 1.0) - (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0);
    }

    // Fixed four-band topology. Defaults match the parameter registration in
    // RegisterParametricEQEffect() so a freshly-created instance is ready to play
    // without an explicit SetParam() call.
    //   {enabled, gainDb, freq(Hz), Q,     isShelf}
    std::array<Band, 4> mBands = {{
        {true, 0.0, 100.0, 0.707, true}, // [0] Low shelf   — Q 0.707 = Butterworth
        {true, 0.0, 400.0, 1.0, false},  // [1] Low-mid peak
        {true, 0.0, 2000.0, 1.0, false}, // [2] High-mid peak
        {true, 0.0, 8000.0, 0.707, true} // [3] High shelf  — Q 0.707 = Butterworth
    }};
};

inline void RegisterParametricEQEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kEqParametric;
    info.aliases = {"eq_parametric"};
    info.displayName = "Parametric EQ";
    info.category = "eq";
    info.description = "4-band parametric equalizer";
    info.requiresResource = false;
    info.parameters = {{"lowGain", "Low Gain", 0.0, -12.0, 12.0, "dB", "Low", false, 0.0, {}},
                       {"lowFreq", "Low Freq", 100.0, 20.0, 500.0, "Hz", "Low", false, 0.0, {}},
                       {"lowQ", "Low Q", 0.707, 0.1, 10.0, "amount", "Low", false, 0.0, {}},
                       {"lowMidGain", "Low-Mid Gain", 0.0, -12.0, 12.0, "dB", "Low Mid", false, 0.0, {}},
                       {"lowMidFreq", "Low-Mid Freq", 400.0, 100.0, 2000.0, "Hz", "Low Mid", false, 0.0, {}},
                       {"lowMidQ", "Low-Mid Q", 1.0, 0.1, 10.0, "amount", "Low Mid", false, 0.0, {}},
                       {"highMidGain", "High-Mid Gain", 0.0, -12.0, 12.0, "dB", "High Mid", false, 0.0, {}},
                       {"highMidFreq", "High-Mid Freq", 2000.0, 500.0, 8000.0, "Hz", "High Mid", false, 0.0, {}},
                       {"highMidQ", "High-Mid Q", 1.0, 0.1, 10.0, "amount", "High Mid", false, 0.0, {}},
                       {"highGain", "High Gain", 0.0, -12.0, 12.0, "dB", "High", false, 0.0, {}},
                       {"highFreq", "High Freq", 8000.0, 2000.0, 16000.0, "Hz", "High", false, 0.0, {}},
                       {"highQ", "High Q", 0.707, 0.1, 10.0, "amount", "High", false, 0.0, {}}};

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<ParametricEQEffect>(); });
}
} // namespace guitarfx
