#pragma once

#include <filesystem>
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
    virtual ~EffectProcessor() = default;

    // Lifecycle
    virtual void Prepare(double sampleRate, int maxBlockSize) = 0;
    virtual void Reset() = 0;

    // Processing (stereo in/out)
    virtual void Process(float **inputs, float **outputs, int numSamples) = 0;

    // Parameters
    virtual void SetParam(const std::string &key, double value) = 0;
    virtual void SetConfig(const std::string &key, const std::string &value) = 0;
    [[nodiscard]] virtual double GetParam(const std::string &key) const = 0;
    [[nodiscard]] virtual std::string GetConfig(const std::string &key) const { return ""; }

    // Resource loading (for effects that need external files)
    virtual bool LoadResource(const std::filesystem::path &path) { return true; }
    virtual bool LoadResources(const std::vector<ResourceRef> &refs,
                               const std::vector<std::filesystem::path> &paths)
    {
      if (!paths.empty())
        return LoadResource(paths.front());
      return false;
    }
    [[nodiscard]] virtual bool RequiresResource() const { return false; }
    [[nodiscard]] virtual bool HasResource() const { return true; }
    [[nodiscard]] virtual std::filesystem::path GetResourcePath() const { return {}; }

    // Bypass
    void SetEnabled(bool enabled) { mEnabled = enabled; }
    [[nodiscard]] bool IsEnabled() const { return mEnabled; }

    // Type info
    [[nodiscard]] virtual std::string GetType() const = 0;
    [[nodiscard]] virtual std::string GetCategory() const = 0;

  protected:
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
      mSampleRate = sampleRate;
      mMaxBlockSize = maxBlockSize;
    }

    void Reset() override {}

    void Process(float **inputs, float **outputs, int numSamples) override
    {
      if (inputs && outputs)
      {
        for (int ch = 0; ch < 2; ++ch)
        {
          if (inputs[ch] && outputs[ch])
          {
            for (int i = 0; i < numSamples; ++i)
            {
              outputs[ch][i] = inputs[ch][i];
            }
          }
        }
      }
    }

    void SetParam(const std::string &, double) override {}
    void SetConfig(const std::string &, const std::string &) override {}
    [[nodiscard]] double GetParam(const std::string &) const override { return 0.0; }

    [[nodiscard]] std::string GetType() const override { return "passthrough"; }
    [[nodiscard]] std::string GetCategory() const override { return "utility"; }
  };

} // namespace guitarfx
