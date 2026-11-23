#include "dsp/nam/FusedWaveNet.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

#include "dsp/nam/FusedWaveNetSimd.h"

namespace guitarfx::fusedwavenet
{
namespace
{
using namespace simd;

int RoundUp8(int n)
{
    return (n + 7) & ~7;
}

// A short copy without a call: vectors, then single floats.
GUITARFX_FWN_INLINE void CopyFloats(float* d, const float* s, int n)
{
    int f = 0;

    for (; f + 8 <= n; f += 8)
    {
        Store(d + f, Load(s + f));
    }

    for (; f < n; ++f)
    {
        d[f] = s[f];
    }
}

// A GEMM's input rows: for each tap, one row per channel, channelStride floats apart. Rows are
// taken tap-major, row t * channels + c, which is the packed weights' input order. tapBase is the
// caller's row buffer, with room for taps * channels pointers: Gemm lists a single tap's rows in it.
struct Rows
{
    const float** tapBase;
    int taps = 1;
    int channels = 1;
    std::ptrdiff_t channelStride = 0;
};

// A tap of only a few channels makes too short an inner loop, so a GEMM over fewer channels than
// this takes every row as a tap of its own.
constexpr int kMinTapChannels = 4;

// How many rows of each tap a caller lists: all of them when they become taps of their own.
int ListedChannels(int channels)
{
    return channels < kMinTapChannels ? channels : 1;
}

// The Rows for taps listed with ListedChannels(channels) rows each.
Rows ListedRows(const float** rows, int taps, int channels, std::ptrdiff_t channelStride)
{
    if (channels < kMinTapChannels)
    {
        return Rows{rows, taps * channels, 1, 0};
    }

    return Rows{rows, taps, channels, channelStride};
}

int NextPow2(int v)
{
    int p = 1;

    while (p < v)
    {
        p <<= 1;
    }

    return p;
}

// out[o][f] (+)= bias[o] + sum_i W[i][o] * in[i][f], for one tile of OT outputs x FT 8-frame
// vectors. W is input-major: the nout weights for input i are contiguous. Each output sums its
// inputs in the same order whatever the tile, so every tile gives the same bits.
#if defined(GUITARFX_FWN_NEON)
// The NEON tile names its accumulators, one row of up to three vectors per output. Held in an
// array, as below, MSVC's ARM64 back end keeps them in registers but also stores every one back to
// the stack on every input (12 stores per 24 multiply-adds); clang, and MSVC on x64, do not. A
// 4-output tile takes its four weights in one register (FmaLane). The arithmetic and its order are
// the array version's, so the bits are the same.
template <int FT>
GUITARFX_FWN_INLINE void RowInit(V8& a0, V8& a1, V8& a2, const float* bias, int o, const float* outRow, bool accumulate)
{
    const V8 b = bias != nullptr ? Set1(bias[o]) : Set1(0.0f);
    a0 = accumulate ? Add(Load(outRow), b) : b;

    if constexpr (FT > 1)
    {
        a1 = accumulate ? Add(Load(outRow + 8), b) : b;
    }

    if constexpr (FT > 2)
    {
        a2 = accumulate ? Add(Load(outRow + 16), b) : b;
    }
}

// One output's row += its weight, lane L of w, times the inputs.
template <int FT, int L> GUITARFX_FWN_INLINE void RowFma(V8& a0, V8& a1, V8& a2, float32x4_t w, V8 x0, V8 x1, V8 x2)
{
    a0 = FmaLane<L>(w, x0, a0);

    if constexpr (FT > 1)
    {
        a1 = FmaLane<L>(w, x1, a1);
    }

    if constexpr (FT > 2)
    {
        a2 = FmaLane<L>(w, x2, a2);
    }
}

template <int FT> GUITARFX_FWN_INLINE void RowStore(float* outRow, V8 a0, V8 a1, V8 a2)
{
    Store(outRow, a0);

    if constexpr (FT > 1)
    {
        Store(outRow + 8, a1);
    }

    if constexpr (FT > 2)
    {
        Store(outRow + 16, a2);
    }
}

template <int OT, int FT, bool Flat>
GUITARFX_FWN_INLINE void GemmTile(const float* weights, int nout, const float* bias, const float* const* tapBase,
                                  int taps, int channels, std::ptrdiff_t channelStride, float* out, int outStride,
                                  int o0, int f0, bool accumulate)
{
    static_assert((OT == 1 || OT == 2 || OT == 4) && FT >= 1 && FT <= 3, "the named rows cover 4 x 3");
    float* const tile = out + o0 * outStride + f0;
    V8 a00, a01, a02, a10, a11, a12, a20, a21, a22, a30, a31, a32;

    RowInit<FT>(a00, a01, a02, bias, o0, tile, accumulate);

    if constexpr (OT > 1)
    {
        RowInit<FT>(a10, a11, a12, bias, o0 + 1, tile + outStride, accumulate);
    }

    if constexpr (OT > 2)
    {
        RowInit<FT>(a20, a21, a22, bias, o0 + 2, tile + 2 * outStride, accumulate);
        RowInit<FT>(a30, a31, a32, bias, o0 + 3, tile + 3 * outStride, accumulate);
    }

    const float* w = weights + o0;

    if constexpr (Flat)
    {
        channels = 1;
    }

    for (int t = 0; t < taps; ++t)
    {
        const float* x = tapBase[t] + f0;

        for (int c = 0; c < channels; ++c, x += channelStride, w += nout)
        {
            const V8 x0 = Load(x);
            V8 x1 = x0;
            V8 x2 = x0;

            if constexpr (FT > 1)
            {
                x1 = Load(x + 8);
            }

            if constexpr (FT > 2)
            {
                x2 = Load(x + 16);
            }

            if constexpr (OT == 4)
            {
                const float32x4_t w4 = vld1q_f32(w);
                RowFma<FT, 0>(a00, a01, a02, w4, x0, x1, x2);
                RowFma<FT, 1>(a10, a11, a12, w4, x0, x1, x2);
                RowFma<FT, 2>(a20, a21, a22, w4, x0, x1, x2);
                RowFma<FT, 3>(a30, a31, a32, w4, x0, x1, x2);
            }
            else
            {
                RowFma<FT, 0>(a00, a01, a02, vld1q_dup_f32(w), x0, x1, x2);

                if constexpr (OT > 1)
                {
                    RowFma<FT, 0>(a10, a11, a12, vld1q_dup_f32(w + 1), x0, x1, x2);
                }
            }
        }
    }

    RowStore<FT>(tile, a00, a01, a02);

    if constexpr (OT > 1)
    {
        RowStore<FT>(tile + outStride, a10, a11, a12);
    }

    if constexpr (OT > 2)
    {
        RowStore<FT>(tile + 2 * outStride, a20, a21, a22);
        RowStore<FT>(tile + 3 * outStride, a30, a31, a32);
    }
}
#else
template <int OT, int FT, bool Flat>
GUITARFX_FWN_INLINE void GemmTile(const float* weights, int nout, const float* bias, const float* const* tapBase,
                                  int taps, int channels, std::ptrdiff_t channelStride, float* out, int outStride,
                                  int o0, int f0, bool accumulate)
{
    V8 acc[OT][FT];

    for (int oo = 0; oo < OT; ++oo)
    {
        const V8 b = bias != nullptr ? Set1(bias[o0 + oo]) : Set1(0.0f);

        for (int ft = 0; ft < FT; ++ft)
        {
            acc[oo][ft] = accumulate ? Add(Load(out + (o0 + oo) * outStride + f0 + 8 * ft), b) : b;
        }
    }

    const float* w = weights + o0;

    if constexpr (Flat)
    {
        channels = 1;
    }

    for (int t = 0; t < taps; ++t)
    {
        const float* x = tapBase[t] + f0;

        for (int c = 0; c < channels; ++c, x += channelStride, w += nout)
        {
            V8 xv[FT];

            for (int ft = 0; ft < FT; ++ft)
            {
                xv[ft] = Load(x + 8 * ft);
            }

            for (int oo = 0; oo < OT; ++oo)
            {
                const V8 wv = Broadcast(w + oo);

                for (int ft = 0; ft < FT; ++ft)
                {
                    acc[oo][ft] = Fma(wv, xv[ft], acc[oo][ft]);
                }
            }
        }
    }

    for (int oo = 0; oo < OT; ++oo)
    {
        for (int ft = 0; ft < FT; ++ft)
        {
            Store(out + (o0 + oo) * outStride + f0 + 8 * ft, acc[oo][ft]);
        }
    }
}
#endif

template <int FT, bool Flat>
GUITARFX_FWN_INLINE void GemmFrames(const float* weights, int nout, const float* bias, const float* const* tapBase,
                                    int taps, int channels, std::ptrdiff_t channelStride, float* out, int outStride,
                                    int f0, bool accumulate)
{
    int o0 = 0;

    for (; o0 + kTileOutputs <= nout; o0 += kTileOutputs)
    {
        GemmTile<kTileOutputs, FT, Flat>(weights, nout, bias, tapBase, taps, channels, channelStride, out, outStride,
                                         o0, f0, accumulate);
    }

    for (; o0 + 2 <= nout; o0 += 2)
    {
        GemmTile<2, FT, Flat>(weights, nout, bias, tapBase, taps, channels, channelStride, out, outStride, o0, f0,
                              accumulate);
    }

    for (; o0 < nout; ++o0)
    {
        GemmTile<1, FT, Flat>(weights, nout, bias, tapBase, taps, channels, channelStride, out, outStride, o0, f0,
                              accumulate);
    }
}

template <bool Flat>
void GemmOver(const float* weights, int nout, const float* bias, const float* const* tapBase, int taps, int channels,
              std::ptrdiff_t channelStride, float* out, int outStride, int padded, bool accumulate)
{
    int f0 = 0;
    int wide = padded / (8 * kTileVectors);

    // Never end on a lone 8-frame vector: its tile has too few independent accumulators to cover
    // the multiply-add latency, and ran at about half speed. 24 + 8 becomes 16 + 16.
    if (kTileVectors > 2 && wide > 0 && padded - wide * 8 * kTileVectors == 8)
    {
        --wide;
    }

    for (; f0 < wide * 8 * kTileVectors; f0 += 8 * kTileVectors)
    {
        GemmFrames<kTileVectors, Flat>(weights, nout, bias, tapBase, taps, channels, channelStride, out, outStride, f0,
                                       accumulate);
    }

    if constexpr (kTileVectors > 2)
    {
        for (; f0 + 16 <= padded; f0 += 16)
        {
            GemmFrames<2, Flat>(weights, nout, bias, tapBase, taps, channels, channelStride, out, outStride, f0,
                                accumulate);
        }
    }

    for (; f0 < padded; f0 += 8)
    {
        GemmFrames<1, Flat>(weights, nout, bias, tapBase, taps, channels, channelStride, out, outStride, f0,
                            accumulate);
    }
}

// A matrix times input rows (see Rows), over padded frames (a multiple of 8). A dilated conv
// passes its taps' rows in a ring, from Ring::TapBases.
void Gemm(const float* weights, int nout, const float* bias, Rows in, float* out, int outStride, int padded,
          bool accumulate)
{
    // A single tap of a few channels (see kMinTapChannels): list its rows in place.
    if (in.taps == 1 && in.channels > 1 && in.channels < kMinTapChannels)
    {
        const float* const base = in.tapBase[0];

        for (int c = 1; c < in.channels; ++c)
        {
            in.tapBase[c] = base + c * in.channelStride;
        }

        in = ListedRows(in.tapBase, 1, in.channels, in.channelStride);
    }

    if (in.channels == 1)
    {
        GemmOver<true>(weights, nout, bias, in.tapBase, in.taps, 1, 0, out, outStride, padded, accumulate);
    }
    else
    {
        GemmOver<false>(weights, nout, bias, in.tapBase, in.taps, in.channels, in.channelStride, out, outStride, padded,
                        accumulate);
    }
}
} // namespace

bool IsAvailable() noexcept
{
#if defined(GUITARFX_FWN_AVX2) || defined(GUITARFX_FWN_SSE)
    return true;
#elif defined(GUITARFX_FWN_NEON) && defined(GUITARFX_FWN_NEON_ENABLED)
    // GUITARFX_FUSED_WAVENET_NEON in CMake, on by default.
    return true;
#else
    return false;
#endif
}

const char* KernelName() noexcept
{
#if defined(GUITARFX_FWN_AVX2)
    return "AVX2";
#elif defined(GUITARFX_FWN_SSE)
    return "SSE";
#elif defined(GUITARFX_FWN_NEON) && defined(GUITARFX_FWN_NEON_ENABLED)
    return "NEON";
#else
    return "none";
#endif
}

// ---------------------------------------------------------------- rings
void FusedWaveNet::Ring::Allocate(int rows, int lookback, int paddedMax)
{
    size = NextPow2(lookback + paddedMax);
    mask = size - 1;
    rowStride = size + paddedMax;
    history.assign(static_cast<std::size_t>(rows) * static_cast<std::size_t>(rowStride), 0.0f);
    writePos = 0;
}

void FusedWaveNet::Ring::Write(const float* source, int sourceStride, int rows, int frames, int paddedMax)
{
    // All padded frames in one straight run of vector copies from writePos; the row's mirror tail
    // makes room (rowStride = size + paddedMax). The columns past `frames` are the oldest history,
    // which nothing reads again before the next write replaces it: a tap that reaches them is
    // feeding a padded lane. Then the two copies are squared for the real frames only: frames that
    // ran past size into the mirror go back to the front, and frames written in the first
    // paddedMax columns go to the mirror.
    const int padded = RoundUp8(frames);
    const int overflow = writePos + frames - size;
    const int mirrored = std::min(writePos + frames, paddedMax) - writePos;

    for (int c = 0; c < rows; ++c)
    {
        float* row = history.data() + static_cast<std::size_t>(c) * rowStride;
        const float* s = source + static_cast<std::size_t>(c) * sourceStride;
        float* d = row + writePos;

        for (int f = 0; f < padded; f += 8)
        {
            Store(d + f, Load(s + f));
        }

        if (overflow > 0)
        {
            CopyFloats(row, row + size, overflow);
        }

        if (mirrored > 0)
        {
            CopyFloats(row + size + writePos, s, mirrored);
        }
    }
}

// The first `channels` rows of each tap of a causal K-tap conv at this dilation, tap-major: tap k
// reads (K - 1 - k) * dilation frames back. With channels = 1 that is one base per tap, and the
// GEMM finds channel c rowStride * c further on.
void FusedWaveNet::Ring::TapBases(const float** bases, int kernelSize, int dilation, int channels) const
{
    for (int k = 0; k < kernelSize; ++k)
    {
        const float* base = history.data() + ((writePos - (kernelSize - 1 - k) * dilation) & mask);

        for (int c = 0; c < channels; ++c, base += rowStride)
        {
            *bases++ = base;
        }
    }
}

// Every column of each row set to that row's first source frame: the history of a constant input.
void FusedWaveNet::Ring::Fill(const float* source, int sourceStride, int rows)
{
    for (int c = 0; c < rows; ++c)
    {
        std::fill_n(history.data() + static_cast<std::size_t>(c) * rowStride, rowStride,
                    source[static_cast<std::size_t>(c) * sourceStride]);
    }
}

// ---------------------------------------------------------------- FusedWaveNet
FusedWaveNet::FusedWaveNet(std::shared_ptr<const Weights> weights, double expectedSampleRate)
    : ::nam::DSP(1, 1, expectedSampleRate), mWeights(std::move(weights))
{
    if (!mWeights || mWeights->arrays.empty())
    {
        throw std::invalid_argument("FusedWaveNet: no weights");
    }

    mArrays.resize(mWeights->arrays.size());

    for (std::size_t a = 0; a < mArrays.size(); ++a)
    {
        mArrays[a].layers.resize(mWeights->arrays[a].layers.size());
    }

    UpdateDilations();
}

void FusedWaveNet::UpdateDilations()
{
    // As the NAM core's WaveNet counts it: one sample, plus every layer's and head's lookback.
    mPrewarmSamples = 1;

    for (std::size_t a = 0; a < mArrays.size(); ++a)
    {
        const LayerArray& weights = mWeights->arrays[a];

        for (std::size_t l = 0; l < weights.layers.size(); ++l)
        {
            LayerState& state = mArrays[a].layers[l];
            state.dilation = weights.layers[l].dilation * mTimeScale;
            state.lookback = (weights.layers[l].kernelSize - 1) * state.dilation;
            mPrewarmSamples += state.lookback;
        }

        mPrewarmSamples += (weights.headKernelSize - 1) * mTimeScale;
    }
}

void FusedWaveNet::SetTimeScale(const int scale)
{
    mTimeScale = std::max(1, scale);
    UpdateDilations();

    if (mMaxFrames > 0)
    {
        SetMaxBufferSize(GetMaxBufferSize());
    }
}

void FusedWaveNet::SetMaxBufferSize(const int maxBufferSize)
{
    ::nam::DSP::SetMaxBufferSize(maxBufferSize);
    mMaxFrames = std::clamp(maxBufferSize, 1, kChunkFrames);
    mPaddedMax = RoundUp8(mMaxFrames);
    const auto stride = static_cast<std::size_t>(mPaddedMax);
    mCondition.assign(stride, 0.0f);
    std::size_t maxRows = 1;

    for (std::size_t a = 0; a < mArrays.size(); ++a)
    {
        const LayerArray& weights = mWeights->arrays[a];
        ArrayState& state = mArrays[a];
        const auto channels = static_cast<std::size_t>(weights.channels);
        state.current.assign(channels * stride, 0.0f);
        state.headSum.assign(channels * stride, 0.0f);
        state.conv.assign(channels * stride, 0.0f);
        state.headOut.assign(static_cast<std::size_t>(weights.headSize) * stride, 0.0f);

        for (std::size_t l = 0; l < weights.layers.size(); ++l)
        {
            state.layers[l].ring.Allocate(weights.channels, state.layers[l].lookback, mPaddedMax);
            maxRows = std::max(maxRows, static_cast<std::size_t>(weights.layers[l].kernelSize) * channels);
        }

        if (weights.headKernelSize > 1)
        {
            state.headRing.Allocate(weights.channels, (weights.headKernelSize - 1) * mTimeScale, mPaddedMax);
        }

        maxRows = std::max(maxRows, static_cast<std::size_t>(weights.headKernelSize) * channels);
        maxRows = std::max(maxRows, std::max(channels, static_cast<std::size_t>(weights.inputSize)));
    }

    mRows.assign(maxRows, nullptr);
}

void FusedWaveNet::process(NAM_SAMPLE** input, NAM_SAMPLE** output, const int numFrames)
{
    if (mMaxFrames <= 0)
    {
        // Never prepared. Build() prewarms every model it returns, so this is a caller's bug;
        // silence beats allocating here.
        std::fill_n(output[0], std::max(0, numFrames), static_cast<NAM_SAMPLE>(0));
        return;
    }

    for (int done = 0; done < numFrames;)
    {
        const int frames = std::min(numFrames - done, mMaxFrames);
        ProcessChunk(input[0] + done, output[0] + done, frames, false);
        done += frames;
    }
}

void FusedWaveNet::prewarm()
{
    if (mMaxFrames == 0)
    {
        SetMaxBufferSize(64);
    }

    NAM_SAMPLE zeros[8] = {};
    NAM_SAMPLE sink[8];
    ProcessChunk(zeros, sink, std::min(8, mMaxFrames), true);
}

void FusedWaveNet::PrewarmByRunning()
{
    SetMaxBufferSize(mMaxFrames == 0 ? 64 : GetMaxBufferSize());
    const int block = std::max(GetMaxBufferSize(), 1);
    std::vector<NAM_SAMPLE> zeros(static_cast<std::size_t>(block), static_cast<NAM_SAMPLE>(0));
    std::vector<NAM_SAMPLE> sink(static_cast<std::size_t>(block));
    NAM_SAMPLE* in[1] = {zeros.data()};
    NAM_SAMPLE* out[1] = {sink.data()};

    for (int done = 0; done < mPrewarmSamples; done += block)
    {
        process(in, out, block);
    }
}

std::unique_ptr<::nam::DSP> FusedWaveNet::CloneForPhase() const
{
    auto clone = std::make_unique<FusedWaveNet>(mWeights, mExpectedSampleRate);
    clone->SetTimeScale(mTimeScale);
    return clone;
}

void FusedWaveNet::ProcessChunk(const NAM_SAMPLE* input, NAM_SAMPLE* output, const int frames, const bool prime)
{
    const int padded = RoundUp8(frames);
    const int stride = mPaddedMax;
    const auto rowBytes = static_cast<std::size_t>(stride);

    for (int f = 0; f < frames; ++f)
    {
        mCondition[static_cast<std::size_t>(f)] = static_cast<float>(input[f]);
    }

    std::fill(mCondition.begin() + frames, mCondition.begin() + padded, 0.0f);
    const float* condition = mCondition.data();
    const float** rows = mRows.data();
    const ArrayState* previous = nullptr;

    for (std::size_t a = 0; a < mArrays.size(); ++a)
    {
        const LayerArray& weights = mWeights->arrays[a];
        ArrayState& state = mArrays[a];
        const int channels = weights.channels;

        // Rechannel the input, or the previous array's layer output, into the residual stream.
        if (previous == nullptr)
        {
            rows[0] = condition;
        }
        else
        {
            rows[0] = previous->current.data();
        }

        Gemm(weights.rechannel.data(), channels, nullptr, Rows{rows, 1, weights.inputSize, stride},
             state.current.data(), stride, padded, false);

        // The head accumulator starts at zero, or at the previous array's head output.
        if (previous == nullptr)
        {
            std::fill_n(state.headSum.begin(), static_cast<std::size_t>(channels) * rowBytes, 0.0f);
        }
        else
        {
            std::memcpy(state.headSum.data(), previous->headOut.data(),
                        sizeof(float) * static_cast<std::size_t>(channels) * rowBytes);
        }

        for (std::size_t l = 0; l < weights.layers.size(); ++l)
        {
            const Layer& layer = weights.layers[l];
            LayerState& layerState = state.layers[l];

            if (prime)
            {
                layerState.ring.Fill(state.current.data(), stride, channels);
            }

            layerState.ring.Write(state.current.data(), stride, channels, frames, stride);
            layerState.ring.TapBases(rows, layer.kernelSize, layerState.dilation, ListedChannels(channels));
            float* conv = state.conv.data();
            Gemm(layer.conv.data(), channels, layer.convBias.data(),
                 ListedRows(rows, layer.kernelSize, channels, layerState.ring.rowStride), conv, stride, padded, false);

            // Input mixin, activation and head sum, in one pass.
            const V8 slope = Set1(layer.slope);

            for (int o = 0; o < channels; ++o)
            {
                float* z = conv + static_cast<std::size_t>(o) * rowBytes;
                float* h = state.headSum.data() + static_cast<std::size_t>(o) * rowBytes;
                const V8 mix = Set1(layer.mixin[static_cast<std::size_t>(o)]);

                if (layer.activation == Activation::Tanh)
                {
                    for (int f = 0; f < padded; f += 8)
                    {
                        const V8 t = FastTanh(Fma(mix, Load(condition + f), Load(z + f)));
                        Store(z + f, t);
                        Store(h + f, Add(Load(h + f), t));
                    }
                }
                else
                {
                    for (int f = 0; f < padded; f += 8)
                    {
                        const V8 t = LeakyRelu(Fma(mix, Load(condition + f), Load(z + f)), slope);
                        Store(z + f, t);
                        Store(h + f, Add(Load(h + f), t));
                    }
                }
            }

            // layer1x1 residual, in place on the residual stream.
            rows[0] = conv;
            Gemm(layer.layer1x1.data(), channels, layer.layer1x1Bias.data(), Rows{rows, 1, channels, stride},
                 state.current.data(), stride, padded, true);
            layerState.ring.Advance(frames);
        }

        // Head rechannel, channels -> head size: a causal conv when the head has more than one tap.
        if (weights.headKernelSize > 1)
        {
            if (prime)
            {
                state.headRing.Fill(state.headSum.data(), stride, channels);
            }

            state.headRing.Write(state.headSum.data(), stride, channels, frames, stride);
            state.headRing.TapBases(rows, weights.headKernelSize, mTimeScale, ListedChannels(channels));
        }
        else
        {
            rows[0] = state.headSum.data();
        }

        const Rows headRows = weights.headKernelSize > 1
                                  ? ListedRows(rows, weights.headKernelSize, channels, state.headRing.rowStride)
                                  : Rows{rows, 1, channels, stride};
        Gemm(weights.head.data(), weights.headSize, weights.hasHeadBias ? weights.headBias.data() : nullptr, headRows,
             state.headOut.data(), stride, padded, false);

        if (weights.headKernelSize > 1)
        {
            state.headRing.Advance(frames);
        }

        previous = &state;
    }

    const float* y = previous->headOut.data();
    const float headScale = mWeights->headScale;

    for (int f = 0; f < frames; ++f)
    {
        output[f] = static_cast<NAM_SAMPLE>(headScale * y[f]);
    }
}

// ---------------------------------------------------------------- FusedContainer
FusedContainer::FusedContainer(std::vector<Submodel> submodels, const double expectedSampleRate)
    : ::nam::DSP(1, 1, expectedSampleRate), mSubmodels(std::move(submodels))
{
    if (mSubmodels.empty())
    {
        throw std::invalid_argument("FusedContainer: no submodels");
    }

    // Full size, the last submodel, until SetSlimmableSize says otherwise.
    mActiveIndex.store(mSubmodels.size() - 1, std::memory_order_release);
}

void FusedContainer::process(NAM_SAMPLE** input, NAM_SAMPLE** output, const int numFrames)
{
    mSubmodels[mActiveIndex.load(std::memory_order_acquire)].model->process(input, output, numFrames);
}

void FusedContainer::prewarm()
{
    mSubmodels[mActiveIndex.load(std::memory_order_acquire)].model->prewarm();
}

void FusedContainer::SetMaxBufferSize(const int maxBufferSize)
{
    ::nam::DSP::SetMaxBufferSize(maxBufferSize);
    mSubmodels[mActiveIndex.load(std::memory_order_acquire)].model->SetMaxBufferSize(maxBufferSize);
}

void FusedContainer::Reset(const double sampleRate, const int maxBufferSize)
{
    std::lock_guard<std::mutex> lock(mMutex);
    // As the NAM core's container: not through DSP::Reset(), which would prewarm the active
    // submodel before the submodel has these settings.
    mExternalSampleRate = sampleRate;
    mHaveExternalSampleRate = true;
    ::nam::DSP::SetMaxBufferSize(maxBufferSize);
    mSubmodels[mActiveIndex.load(std::memory_order_acquire)].model->Reset(sampleRate, maxBufferSize);
}

void FusedContainer::SetTimeScale(const int scale)
{
    std::lock_guard<std::mutex> lock(mMutex);
    mTimeScale = std::max(1, scale);

    for (auto& submodel : mSubmodels)
    {
        submodel.model->SetTimeScale(mTimeScale);
    }
}

void FusedContainer::SetSlimmableSize(const double value)
{
    std::size_t index = mSubmodels.size() - 1;

    for (std::size_t i = 0; i < mSubmodels.size(); ++i)
    {
        if (value < mSubmodels[i].maxValue)
        {
            index = i;
            break;
        }
    }

    if (index == mActiveIndex.load(std::memory_order_acquire))
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mMutex);

    if (index == mActiveIndex.load(std::memory_order_acquire))
    {
        return;
    }

    // Ready the submodel before it goes on the audio path.
    const double sampleRate = mHaveExternalSampleRate ? mExternalSampleRate : mExpectedSampleRate;
    mSubmodels[index].model->SetTimeScale(mTimeScale);
    mSubmodels[index].model->Reset(sampleRate, GetMaxBufferSize());
    mActiveIndex.store(index, std::memory_order_release);
}

std::unique_ptr<::nam::DSP> FusedContainer::CloneForPhase() const
{
    return mSubmodels[mActiveIndex.load(std::memory_order_acquire)].model->CloneForPhase();
}
} // namespace guitarfx::fusedwavenet
