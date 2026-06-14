#pragma once

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "presets/PresetTypes.h"

namespace guitarfx
{
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
    virtual void Process(float **inputs, float **outputs, int numSamples) = 0;

    // Optional mono processing fast path. Effects should override both methods
    // when they can process a single channel without running stereo code.
    [[nodiscard]] virtual bool SupportsMonoProcessing() const { return false; }

    // Returns true when this effect instance will produce distinct L and R output
    // from a mono (identical L=R) input — e.g. due to pan, stereo widening, etc.
    // The executor uses this to prevent downstream nodes collapsing the stereo field.
    [[nodiscard]] virtual bool ProducesStereoOutput() const { return false; }
    virtual void ProcessMono(float *input, float *output, int numSamples)
    {
      if (!output || numSamples <= 0)
        return;
      if (!input)
      {
        std::fill_n(output, numSamples, 0.0f);
        return;
      }

      float *inPtrs[2] = {input, input};
      float *outPtrs[2] = {output, output};
      Process(inPtrs, outPtrs, numSamples);
    }

    // Parameters
    virtual void SetParam(const std::string &key, double value) = 0;
    virtual void SetConfig(const std::string &key, const std::string &value) = 0;
    [[nodiscard]] virtual double GetParam(const std::string &key) const = 0;
    [[nodiscard]] virtual std::string GetConfig(const std::string & /*key*/) const { return ""; }
    virtual void SetRuntimeConfigChangedCallback(RuntimeConfigChangedCallback /*callback*/) {}

    // Resource loading (for effects that need external files)
    virtual bool LoadResource(const std::filesystem::path & /*path*/) { return true; }
    virtual bool LoadResources(const std::vector<ResourceRef> & /*refs*/,
                               const std::vector<std::filesystem::path> &paths)
    {
      if (!paths.empty())
        return LoadResource(paths.front());
      return false;
    }
    [[nodiscard]] virtual bool RequiresResource() const { return false; }
    [[nodiscard]] virtual bool HasResource() const { return true; }
    [[nodiscard]] virtual std::filesystem::path GetResourcePath() const { return {}; }

    // Latency: effects with algorithmic latency (IR convolution, pitch shift) must override.
    [[nodiscard]] virtual int GetLatencySamples() const { return 0; }

    // Bypass
    void SetEnabled(bool enabled) { mEnabled = enabled; }
    [[nodiscard]] bool IsEnabled() const { return mEnabled; }

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
    static void CopyStereoInputToOutput(float *const *inputs, float **outputs, int numSamples)
    {
      if (!inputs || !outputs || numSamples <= 0)
        return;

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
        return;
      mSampleRate = sampleRate;
      mMaxBlockSize = maxBlockSize;
    }

    void Reset() override {}

    void Process(float **inputs, float **outputs, int numSamples) override
    {
      CopyStereoInputToOutput(inputs, outputs, numSamples);
    }

    void SetParam(const std::string &, double) override {}
    void SetConfig(const std::string &, const std::string &) override {}
    [[nodiscard]] double GetParam(const std::string &) const override { return 0.0; }

    [[nodiscard]] std::string GetType() const override { return "passthrough"; }
    [[nodiscard]] std::string GetCategory() const override { return "utility"; }
  };

} // namespace guitarfx
