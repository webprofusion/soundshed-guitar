#pragma once

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "dsp/DeferredRebuild.h"
#include "presets/PresetTypes.h"

namespace guitarfx
{
struct NoteBlock;

/// The span a parameter is driven across, and the step it lands on (0 = continuous).
struct ParamRange
{
    double minValue = 0.0;
    double maxValue = 1.0;
    double step = 0.0;
};

/**
 * Base interface for all effect processors.
 * Each effect type implements this interface.
 */
class EffectProcessor
{
  public:
    using RuntimeConfigChangedCallback = std::function<void(const std::string&, const std::string&)>;

    virtual ~EffectProcessor() = default;

    // Lifecycle
    virtual void Prepare(double sampleRate, int maxBlockSize) = 0;
    virtual void Reset() = 0;

    // Processing (stereo in/out)
    virtual void Process(float** inputs, float** outputs, int numSamples) = 0;

    // Optional mono processing fast path. Effects should override both methods
    // when they can process a single channel without running stereo code.
    [[nodiscard]] virtual bool SupportsMonoProcessing() const
    {
        return false;
    }

    // Returns true when this effect instance will produce distinct L and R output
    // from a mono (identical L=R) input — e.g. due to pan, stereo widening, etc.
    // The executor uses this to prevent downstream nodes collapsing the stereo field.
    [[nodiscard]] virtual bool ProducesStereoOutput() const
    {
        return false;
    }

    virtual void ProcessMono(float* input, float* output, int numSamples)
    {
        if (!output || numSamples <= 0)
        {
            return;
        }

        if (!input)
        {
            std::fill_n(output, numSamples, 0.0f);
            return;
        }

        float* inPtrs[2] = {input, input};
        float* outPtrs[2] = {output, output};
        Process(inPtrs, outPtrs, numSamples);
    }

    // Parameters
    virtual void SetParam(const std::string& key, double value) = 0;
    virtual void SetConfig(const std::string& key, const std::string& value) = 0;
    [[nodiscard]] virtual double GetParam(const std::string& key) const = 0;

    [[nodiscard]] virtual std::string GetConfig(const std::string& /*key*/) const
    {
        return "";
    }

    /// Lets an instance's own settings narrow the range automation drives one of its
    /// parameters across: a pitch shift held to the interval an expression pedal should sweep.
    /// Returns false to use the range the effect type declares. Called on the audio thread,
    /// so it must not allocate.
    [[nodiscard]] virtual bool GetAutomationRange(const std::string& /*key*/, ParamRange& /*range*/) const
    {
        return false;
    }

    virtual void SetRuntimeConfigChangedCallback(RuntimeConfigChangedCallback /*callback*/)
    {
    }

    /// A parameter change SetParam recorded but could not build where it ran (see
    /// DeferredRebuild). Under the DSP lock, message thread: the work, with what it will read
    /// copied into it, or nullptr when nothing is waiting.
    [[nodiscard]] virtual std::unique_ptr<DeferredRebuild> TakeDeferredRebuild()
    {
        return nullptr;
    }

    /// Under the DSP lock, message thread: installs `work`, which this processor's
    /// TakeDeferredRebuild() returned and which has since been built, in O(1). Drops it if the
    /// effect was rebuilt in between, and moves what it replaces into `work` to be freed later.
    virtual void CommitDeferredRebuild(DeferredRebuild& /*work*/)
    {
    }

    // Resource loading (for effects that need external files)
    virtual bool LoadResource(const std::filesystem::path& /*path*/)
    {
        return true;
    }

    virtual bool LoadResources(const std::vector<ResourceRef>& /*refs*/,
                               const std::vector<std::filesystem::path>& paths)
    {
        if (!paths.empty())
        {
            return LoadResource(paths.front());
        }

        return false;
    }

    [[nodiscard]] virtual bool RequiresResource() const
    {
        return false;
    }

    [[nodiscard]] virtual bool HasResource() const
    {
        return true;
    }

    // Returns true if LoadResources must be called on the main/message thread.
    // Override in effects that use platform APIs with thread-affinity requirements
    // (e.g. JUCE plugin hosts that call MessageManager::callSync internally).
    [[nodiscard]] virtual bool RequiresMainThreadLoad() const noexcept
    {
        return false;
    }

    [[nodiscard]] virtual std::filesystem::path GetResourcePath() const
    {
        return {};
    }

    // Latency: effects with algorithmic latency (IR convolution, pitch shift) must override.
    [[nodiscard]] virtual int GetLatencySamples() const
    {
        return 0;
    }

    /// The small-signal magnitude response, in dB, at each of `frequenciesHz`, for effects
    /// that are linear and time-invariant at low level (a cabinet voicing, a fixed filter).
    /// It comes from the effect's current parameters, not its audio state, so the UI can
    /// draw the curve the engine will apply without keeping a second copy of the design,
    /// and an impulse through the effect is a faithful IR of it. Returns false when the
    /// effect has no such response, or the spans differ in length.
    [[nodiscard]] virtual bool GetFrequencyResponse(std::span<const double> /*frequenciesHz*/,
                                                    std::span<double> /*magnitudesDb*/) const
    {
        return false;
    }

    // Notes (dsp/NoteEvents.h). A node that makes notes from its input exposes them here, and
    // the executor hands them to every node downstream of it that plays notes (see
    // SignalGraphExecutor, "Note routing"). Everything else ignores both.

    /// The notes this node made in the block it last processed, or nullptr for a node that makes
    /// none. The block is the node's own and must stay at this address. A source the executor
    /// skipped for a block (bypassed, or cut off from the input) is Reset() before it next runs,
    /// on the audio thread, so its Reset() must not allocate.
    [[nodiscard]] virtual const NoteBlock* GetNoteOutput() const
    {
        return nullptr;
    }

    /// True for a node that plays notes: the executor then calls SetNoteInput() before every
    /// Process() it makes.
    [[nodiscard]] virtual bool AcceptsNoteInput() const
    {
        return false;
    }

    /// This block's notes from the sources upstream that ran this block, which may be none. The
    /// span and the blocks are valid only until Process() returns.
    virtual void SetNoteInput(std::span<const NoteBlock* const> /*sources*/)
    {
    }

    // Bypass
    void SetEnabled(bool enabled)
    {
        mEnabled = enabled;
    }

    [[nodiscard]] bool IsEnabled() const
    {
        return mEnabled;
    }

    // Type info
    [[nodiscard]] virtual std::string GetType() const = 0;
    [[nodiscard]] virtual std::string GetCategory() const = 0;

  protected:
    /**
     * Guards against invalid Prepare() arguments (zero/negative sample rate or block size).
     * Effects must call this at the top of their Prepare() override and return early if false.
     */
    [[nodiscard]] static bool ValidatePrepare(double sampleRate, int maxBlockSize)
    {
        return sampleRate > 0.0 && maxBlockSize > 0;
    }

    /**
     * Copies stereo input to output for bypass/passthrough paths.
     * Null channels are skipped so callers can handle sparse mono/stereo buffers safely.
     */
    static void CopyStereoInputToOutput(float* const* inputs, float** outputs, int numSamples)
    {
        if (!inputs || !outputs || numSamples <= 0)
        {
            return;
        }

        for (int ch = 0; ch < 2; ++ch)
        {
            if (inputs[ch] && outputs[ch])
            {
                std::copy_n(inputs[ch], numSamples, outputs[ch]);
            }
        }
    }

    bool mEnabled = true;
    double mSampleRate = 44100.0;
    int mMaxBlockSize = 512;
};

/**
 * Passthrough processor for unknown effect types or bypassed nodes.
 */
class PassthroughProcessor : public EffectProcessor
{
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
        if (!ValidatePrepare(sampleRate, maxBlockSize))
        {
            return;
        }

        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;
    }

    void Reset() override
    {
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        CopyStereoInputToOutput(inputs, outputs, numSamples);
    }

    void SetParam(const std::string&, double) override
    {
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string&) const override
    {
        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "passthrough";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "utility";
    }
};
} // namespace guitarfx
