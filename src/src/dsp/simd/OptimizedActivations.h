#pragma once

/**
 * Optimized activation functions for NAM neural network inference.
 * Drop-in replacements for nam::activations with SIMD acceleration.
 */

#include "SimdMath.h"
#include <Eigen/Dense>
#include <string>
#include <unordered_map>
#include <memory>

namespace guitarfx
{
namespace activations
{

/**
 * Base class for optimized activation functions.
 * Matches the NAM Activation interface but uses SIMD under the hood.
 */
class Activation
{
public:
  virtual ~Activation() = default;

  // Apply activation to raw float array
  virtual void Apply(float* data, long size) = 0;

  // Apply to Eigen matrix (column-major)
  virtual void Apply(Eigen::MatrixXf& matrix)
  {
    Apply(matrix.data(), matrix.rows() * matrix.cols());
  }

  // Apply to Eigen block
  virtual void Apply(Eigen::Block<Eigen::MatrixXf> block)
  {
    Apply(block.data(), block.rows() * block.cols());
  }

  // Factory method
  static Activation* Get(const std::string& name);

  // Get all registered activation names
  static std::vector<std::string> GetAvailableActivations();
};

// ============================================================================
// Concrete Activation Implementations
// ============================================================================

class TanhActivation : public Activation
{
public:
  void Apply(float* data, long size) override
  {
    simd::ApplyTanh(data, size);
  }
};

class SigmoidActivation : public Activation
{
public:
  void Apply(float* data, long size) override
  {
    simd::ApplySigmoid(data, size);
  }
};

class ReLUActivation : public Activation
{
public:
  void Apply(float* data, long size) override
  {
    simd::ApplyReLU(data, size);
  }
};

class LeakyReLUActivation : public Activation
{
public:
  LeakyReLUActivation(float negativeSlope = 0.01f)
    : mNegativeSlope(negativeSlope)
  {
  }

  void Apply(float* data, long size) override
  {
    for (long i = 0; i < size; ++i)
      data[i] = data[i] > 0.0f ? data[i] : mNegativeSlope * data[i];
  }

private:
  float mNegativeSlope;
};

class HardTanhActivation : public Activation
{
public:
  void Apply(float* data, long size) override
  {
    for (long i = 0; i < size; ++i)
    {
      float x = data[i];
      data[i] = x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x);
    }
  }
};

class LinearActivation : public Activation
{
public:
  void Apply(float* data, long size) override
  {
    // No-op
    (void)data;
    (void)size;
  }
};

class SwishActivation : public Activation
{
public:
  void Apply(float* data, long size) override
  {
    // swish(x) = x * sigmoid(x)
    for (long i = 0; i < size; ++i)
    {
      float sig = simd::scalar::FastSigmoid(data[i]);
      data[i] = data[i] * sig;
    }
  }
};

class HardSwishActivation : public Activation
{
public:
  void Apply(float* data, long size) override
  {
    for (long i = 0; i < size; ++i)
    {
      float x = data[i];
      if (x <= -3.0f)
        data[i] = 0.0f;
      else if (x >= 3.0f)
        data[i] = x;
      else
        data[i] = x * (x + 3.0f) / 6.0f;
    }
  }
};

// ============================================================================
// Activation Registry
// ============================================================================

class ActivationRegistry
{
public:
  static ActivationRegistry& Instance()
  {
    static ActivationRegistry instance;
    return instance;
  }

  Activation* Get(const std::string& name)
  {
    auto it = mActivations.find(name);
    if (it != mActivations.end())
      return it->second.get();

    // Create on first use
    if (name == "Tanh" || name == "tanh")
    {
      auto activation = std::make_unique<TanhActivation>();
      Activation* ptr = activation.get();
      mActivations["Tanh"] = std::move(activation);
      return ptr;
    }
    else if (name == "Sigmoid" || name == "sigmoid")
    {
      auto activation = std::make_unique<SigmoidActivation>();
      Activation* ptr = activation.get();
      mActivations["Sigmoid"] = std::move(activation);
      return ptr;
    }
    else if (name == "ReLU" || name == "relu")
    {
      auto activation = std::make_unique<ReLUActivation>();
      Activation* ptr = activation.get();
      mActivations["ReLU"] = std::move(activation);
      return ptr;
    }
    else if (name == "LeakyReLU" || name == "leaky_relu")
    {
      auto activation = std::make_unique<LeakyReLUActivation>();
      Activation* ptr = activation.get();
      mActivations["LeakyReLU"] = std::move(activation);
      return ptr;
    }
    else if (name == "HardTanh" || name == "hardtanh")
    {
      auto activation = std::make_unique<HardTanhActivation>();
      Activation* ptr = activation.get();
      mActivations["HardTanh"] = std::move(activation);
      return ptr;
    }
    else if (name == "Linear" || name == "linear" || name == "none")
    {
      auto activation = std::make_unique<LinearActivation>();
      Activation* ptr = activation.get();
      mActivations["Linear"] = std::move(activation);
      return ptr;
    }
    else if (name == "Swish" || name == "swish")
    {
      auto activation = std::make_unique<SwishActivation>();
      Activation* ptr = activation.get();
      mActivations["Swish"] = std::move(activation);
      return ptr;
    }
    else if (name == "HardSwish" || name == "hardswish")
    {
      auto activation = std::make_unique<HardSwishActivation>();
      Activation* ptr = activation.get();
      mActivations["HardSwish"] = std::move(activation);
      return ptr;
    }

    // Fallback to Tanh if unknown
    return Get("Tanh");
  }

private:
  ActivationRegistry() = default;
  std::unordered_map<std::string, std::unique_ptr<Activation>> mActivations;
};

inline Activation* Activation::Get(const std::string& name)
{
  return ActivationRegistry::Instance().Get(name);
}

// ============================================================================
// Fused Operations for WaveNet
// ============================================================================

/**
 * Fused gated activation for WaveNet layers.
 * Computes: output = tanh(x) * sigmoid(gate)
 *
 * This is significantly faster than applying tanh and sigmoid separately
 * because it avoids redundant memory loads/stores.
 */
inline void ApplyGatedActivation(float* activation, float* gate, long size)
{
  simd::ApplyGatedActivation(activation, gate, size);
}

/**
 * Fused gated activation for interleaved data (WaveNet's common layout).
 * Data layout: [channels/2 activation values][channels/2 gate values] per frame
 *
 * @param data Interleaved activation/gate data
 * @param channels Total number of channels (activation channels * 2)
 * @param numFrames Number of time frames
 */
inline void ApplyGatedActivationInterleaved(float* data, long channels, long numFrames)
{
  simd::ApplyGatedActivationInterleaved(data, channels, numFrames);
}

/**
 * Apply activation to a matrix efficiently by processing columns contiguously.
 * This is faster than the NAM column-by-column approach for gated WaveNet.
 *
 * @param z The output matrix after convolution (gated: [2*channels, numFrames], non-gated: [channels, numFrames])
 * @param channels Number of output channels (before gating)
 * @param numFrames Number of time frames
 * @param activation The activation function to apply
 * @param gated Whether this is a gated layer (uses sigmoid for second half)
 */
inline void ApplyWaveNetLayerActivation(
  Eigen::MatrixXf& z,
  long channels,
  int numFrames,
  Activation* activation,
  bool gated)
{
  if (!gated)
  {
    // Simple case: just apply activation to everything
    activation->Apply(z.data(), z.rows() * numFrames);
    return;
  }

  // Gated case: apply tanh to first half, sigmoid to second half, then multiply
  // This is much faster than the NAM column-by-column loop

  // Get pointers to the two halves
  // Note: Eigen is column-major, so we need to process carefully

  // First, apply activations to the full matrix in one go (contiguous memory)
  float* firstHalf = z.data();  // First 'channels' rows
  float* secondHalf = z.data() + channels * numFrames;  // Will handle layout

  // For column-major layout, process frame by frame but with vectorization
  for (int frame = 0; frame < numFrames; ++frame)
  {
    float* frameData = z.data() + frame * z.rows();
    float* gateData = frameData + channels;

    // Apply tanh to activation, sigmoid to gate, then multiply
    simd::ApplyGatedActivation(frameData, gateData, channels);
  }
}

} // namespace activations
} // namespace guitarfx
