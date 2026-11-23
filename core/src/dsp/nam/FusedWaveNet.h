#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <nlohmann/json.hpp>

#include "NAM/dsp.h"
#include "NAM/slimmable.h"

/**
 * A fused, frame-vectorised forward pass for the WaveNet shapes NAM models ship in, built by
 * dsp/NamModelCache.cpp in place of the NAM core's model wherever the shape allows. It runs
 * them 2.0-4.8x faster and builds and prewarms in well under a millisecond, with output within
 * float rounding of the NAM core's (100 dB or more on the test assets). See
 * docs/nam-dsp-performance-review-2026-10-16.md, recommendations 1 and 2.
 *
 * It takes any WaveNet whose layers are non-gated Tanh or LeakyReLU, with layer1x1 on and no
 * head1x1, FiLM, grouping, slimmable channels, condition DSP or post-stack head; any number of
 * layer arrays, channel count, kernel size and dilation, and a head of any kernel size. That is
 * the trainer's standard, lite, feather and nano presets, the xSTD and 32-channel variants, and
 * both A2 submodels. Pack() returns nullptr for anything else, and the NAM core runs it.
 *
 * Layout: every activation buffer is channel-major, one contiguous time row per channel, so the
 * SIMD lanes run across 8 frames and any channel count fills them. Every product (dilated conv,
 * 1x1, rechannel, head) goes through one register-tiled kernel. Histories are power-of-two rings
 * with a mirror of their first columns, so every tap's window is contiguous. Blocks are padded
 * to whole 8-frame lanes; the padded lanes compute on stale history and are never output, so
 * every real frame takes the same arithmetic whatever the host block size, and the output is
 * bit-identical across host block patterns (which the NAM core's is not).
 *
 * The packed weights are immutable and shared: every instance and channel built from one file
 * holds the same Weights through a shared_ptr<const>, and only its histories are its own.
 */
namespace guitarfx::fusedwavenet
{
/// Whether this build has a vector kernel for the fused pass: AVX2 with FMA, or SSE on other
/// x86-64 builds, or NEON on arm64 unless built with GUITARFX_FUSED_WAVENET_NEON off. False
/// elsewhere, where Pack() always returns nullptr and every model stays on the NAM core. A plain
/// C++ fallback measured 0.3x the NAM core, so none is compiled.
[[nodiscard]] bool IsAvailable() noexcept;

/// The kernel in use, for the session log: "AVX2", "SSE", "NEON" or "none".
[[nodiscard]] const char* KernelName() noexcept;

enum class Activation : std::uint8_t
{
    Tanh, // nam::activations::fast_tanh: the app always runs the NAM core with fast tanh
    LeakyRelu
};

struct Layer
{
    int kernelSize = 1;
    int dilation = 1; // at the model's own rate; SetTimeScale multiplies it
    Activation activation = Activation::Tanh;
    float slope = 0.01f;             // LeakyReLU only
    std::vector<float> conv;         // [k][i][o]: input-major per tap, so taps and channels flatten into K*C rows
    std::vector<float> convBias;     // [o]
    std::vector<float> mixin;        // [o]
    std::vector<float> layer1x1;     // [i][o]
    std::vector<float> layer1x1Bias; // [o]
};

struct LayerArray
{
    int inputSize = 1;
    int channels = 1;
    int headSize = 1;
    int headKernelSize = 1;
    bool hasHeadBias = false;
    std::vector<float> rechannel; // [i][o], inputSize -> channels, no bias
    std::vector<Layer> layers;
    std::vector<float> head;     // [k][i][o], channels -> headSize
    std::vector<float> headBias; // [o], zeros when the head has no bias
};

/// One WaveNet's weights, packed for the fused pass. Immutable once packed.
struct Weights
{
    std::vector<LayerArray> arrays;
    float headScale = 1.0f;
    std::size_t bytes = 0;
};

/// The metadata nam::create_dsp applies to a model it builds.
struct Levels
{
    std::optional<double> loudness;
    std::optional<double> inputLevel;
    std::optional<double> outputLevel;
};

/// A model file packed for the fused pass: one WaveNet, or every submodel of a slimmable
/// container (each a WaveNet the fused pass takes), in ascending max value.
struct PackedModel
{
    struct Submodel
    {
        double maxValue = 1.0;
        double expectedSampleRate = NAM_UNKNOWN_EXPECTED_SAMPLE_RATE;
        std::shared_ptr<const Weights> weights;
        Levels levels;
    };

    bool isContainer = false;
    double expectedSampleRate = NAM_UNKNOWN_EXPECTED_SAMPLE_RATE;
    Levels levels;
    std::vector<Submodel> submodels;
    std::size_t bytes = 0;
};

/// Packs one WaveNet's config and weights, or returns nullptr when the shape is not one the
/// fused pass takes or the weight count is wrong. Never throws.
[[nodiscard]] std::shared_ptr<const Weights> PackWaveNet(const nlohmann::json& config,
                                                         const std::vector<float>& weights);

/// Packs a parsed model file: a WaveNet, or a SlimmableContainer whose every submodel packs.
/// Returns nullptr for anything else, and for anything the NAM core would refuse to load (an
/// unsupported version, unordered submodels, mismatched sample rates). Never throws.
[[nodiscard]] std::shared_ptr<const PackedModel> Pack(const ::nam::dspData& data);

/// Builds a model from packed weights, with the file's levels applied and prewarmed as
/// nam::create_dsp leaves a model: a FusedWaveNet, or a FusedContainer of them.
[[nodiscard]] std::unique_ptr<::nam::DSP> Build(const PackedModel& packed);

/// One WaveNet instance: its own histories and scratch rows over shared packed weights.
class FusedWaveNet final : public ::nam::DSP
{
  public:
    FusedWaveNet(std::shared_ptr<const Weights> weights, double expectedSampleRate);

    void process(NAM_SAMPLE** input, NAM_SAMPLE** output, int numFrames) override;

    /// Silence in, for longer than the receptive field, leaves every history holding one
    /// constant column: the network has no feedback. So each history is filled with it, layer
    /// by layer, in one 8-frame pass instead of PrewarmSamples() frames. Every frame takes the
    /// same arithmetic, so the state matches a real prewarm bit for bit.
    void prewarm() override;

    /// Oversampled rendering: every temporal dilation, the heads' included, scales with the rate.
    void SetTimeScale(int scale) override;

    [[nodiscard]] std::unique_ptr<::nam::DSP> CloneForPhase() const override;

    /// Allocates the histories and scratch rows for blocks of up to maxBufferSize frames (bigger
    /// host blocks run in chunks of at most kChunkFrames), all zero. Public so FusedContainer
    /// can forward it.
    void SetMaxBufferSize(int maxBufferSize) override;

    /// The long way round, for tests: zero the histories, then run PrewarmSamples() frames of
    /// silence through process(), as nam::DSP::prewarm() does.
    void PrewarmByRunning();

    [[nodiscard]] const std::shared_ptr<const Weights>& SharedWeights() const noexcept
    {
        return mWeights;
    }

    /// Host blocks past this run in chunks, so the working rows stay in L1. Exact: the chunking
    /// changes no arithmetic.
    static constexpr int kChunkFrames = 128;

  protected:
    int PrewarmSamples() override
    {
        return mPrewarmSamples;
    }

  private:
    /// A channel-major power-of-two ring with a mirror of its first columns, so any window of
    /// up to the padded chunk size is contiguous.
    struct Ring
    {
        std::vector<float> history;
        int size = 0;
        int mask = 0;
        int rowStride = 0;
        int writePos = 0;

        void Allocate(int rows, int lookback, int paddedMax);
        void Write(const float* source, int sourceStride, int rows, int frames, int paddedMax);
        void TapBases(const float** bases, int kernelSize, int dilation, int channels) const;
        void Fill(const float* source, int sourceStride, int rows);

        void Advance(int frames)
        {
            writePos = (writePos + frames) & mask;
        }
    };

    struct LayerState
    {
        int dilation = 1;
        int lookback = 0;
        Ring ring;
    };

    struct ArrayState
    {
        std::vector<LayerState> layers;
        // Only when the head has more than one tap.
        Ring headRing;
        // Rows of mPaddedMax frames: the residual stream, the head sum, the conv output and the
        // head output.
        std::vector<float> current, headSum, conv, headOut;
    };

    void UpdateDilations();
    void ProcessChunk(const NAM_SAMPLE* input, NAM_SAMPLE* output, int frames, bool prime);

    std::shared_ptr<const Weights> mWeights;
    std::vector<ArrayState> mArrays;
    std::vector<float> mCondition;
    std::vector<const float*> mRows;
    int mMaxFrames = 0;
    int mPaddedMax = 0;
    int mPrewarmSamples = 0;
    int mTimeScale = 1;
};

/// A slimmable container of fused submodels, behaving as nam::container::ContainerModel does:
/// the active submodel processes, and switching size resets the one being switched to.
class FusedContainer final : public ::nam::DSP, public ::nam::SlimmableModel
{
  public:
    struct Submodel
    {
        double maxValue = 1.0;
        std::unique_ptr<FusedWaveNet> model;
    };

    FusedContainer(std::vector<Submodel> submodels, double expectedSampleRate);

    void process(NAM_SAMPLE** input, NAM_SAMPLE** output, int numFrames) override;
    void prewarm() override;
    void Reset(double sampleRate, int maxBufferSize) override;
    void SetTimeScale(int scale) override;

    /// Thread-safe, not real-time safe, as the NAM core's container.
    void SetSlimmableSize(double value) override;

    [[nodiscard]] std::unique_ptr<::nam::DSP> CloneForPhase() const override;

    [[nodiscard]] std::size_t ActiveIndex() const noexcept
    {
        return mActiveIndex.load(std::memory_order_acquire);
    }

    [[nodiscard]] FusedWaveNet& SubmodelAt(std::size_t index) const
    {
        return *mSubmodels.at(index).model;
    }

  protected:
    /// None, as the NAM core's container: MultiModelNAMAmpEffect then uses its warm-up floor.
    int PrewarmSamples() override
    {
        return 0;
    }

    /// Forwards to the active submodel.
    void SetMaxBufferSize(int maxBufferSize) override;

  private:
    std::vector<Submodel> mSubmodels;
    std::atomic<std::size_t> mActiveIndex{0};
    std::mutex mMutex;
    int mTimeScale = 1;
};
} // namespace guitarfx::fusedwavenet
