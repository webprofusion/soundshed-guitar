#pragma once

/**
 * Optimized WaveNet processor for NAM models.
 * Uses SIMD-accelerated activations and fused operations.
 */

#include "OptimizedActivations.h"
#include "SimdMath.h"
#include <Eigen/Dense>
#include <vector>
#include <string>
#include <memory>
#include <cassert>
#include <cstring>

namespace guitarfx
{
namespace nam
{

// ============================================================================
// Optimized Conv1x1 Layer
// ============================================================================

class OptimizedConv1x1
{
public:
  OptimizedConv1x1() = default;

  OptimizedConv1x1(int inChannels, int outChannels, bool bias)
    : mWeight(outChannels, inChannels)
    , mBias(bias ? outChannels : 0)
    , mHasBias(bias)
  {
    mWeight.setZero();
    if (bias)
      mBias.setZero();
  }

  void SetMaxBufferSize(int maxBufferSize)
  {
    mOutput.resize(mWeight.rows(), maxBufferSize);
  }

  void SetWeights(std::vector<float>::iterator& weights)
  {
    for (int i = 0; i < mWeight.rows(); i++)
      for (int j = 0; j < mWeight.cols(); j++)
        mWeight(i, j) = *(weights++);

    if (mHasBias)
      for (int i = 0; i < mBias.size(); i++)
        mBias(i) = *(weights++);
  }

  void Process(const Eigen::MatrixXf& input, int numFrames)
  {
    assert(numFrames <= mOutput.cols());
    mOutput.leftCols(numFrames).noalias() = mWeight * input.leftCols(numFrames);
    if (mHasBias)
      mOutput.leftCols(numFrames).colwise() += mBias;
  }

  Eigen::Block<Eigen::MatrixXf> GetOutput(int numFrames)
  {
    return mOutput.block(0, 0, mOutput.rows(), numFrames);
  }

  long GetOutChannels() const { return mWeight.rows(); }
  long GetInChannels() const { return mWeight.cols(); }

private:
  Eigen::MatrixXf mWeight;
  Eigen::VectorXf mBias;
  Eigen::MatrixXf mOutput;
  bool mHasBias = false;
};

// ============================================================================
// Optimized Dilated Conv1D Layer
// ============================================================================

class OptimizedConv1D
{
public:
  OptimizedConv1D() = default;

  OptimizedConv1D(int inChannels, int outChannels, int kernelSize, bool bias, int dilation)
    : mDilation(dilation)
    , mBias(bias ? outChannels : 0)
  {
    mWeight.resize(kernelSize);
    for (auto& w : mWeight)
      w.resize(outChannels, inChannels);

    if (bias)
      mBias.setZero();
  }

  void SetWeights(std::vector<float>::iterator& weights)
  {
    if (mWeight.empty())
      return;

    const long outChannels = mWeight[0].rows();
    const long inChannels = mWeight[0].cols();

    // Crazy ordering because that's how PyTorch flattens it
    for (long i = 0; i < outChannels; i++)
      for (long j = 0; j < inChannels; j++)
        for (size_t k = 0; k < mWeight.size(); k++)
          mWeight[k](i, j) = *(weights++);

    for (long i = 0; i < mBias.size(); i++)
      mBias(i) = *(weights++);
  }

  void Process(
    const Eigen::MatrixXf& input,
    Eigen::MatrixXf& output,
    long iStart,
    long nCols,
    long jStart) const
  {
    // Optimized dilated convolution
    for (size_t k = 0; k < mWeight.size(); k++)
    {
      const long offset = mDilation * (static_cast<long>(k) + 1 - static_cast<long>(mWeight.size()));
      if (k == 0)
        output.middleCols(jStart, nCols).noalias() = mWeight[k] * input.middleCols(iStart + offset, nCols);
      else
        output.middleCols(jStart, nCols).noalias() += mWeight[k] * input.middleCols(iStart + offset, nCols);
    }

    if (mBias.size() > 0)
      output.middleCols(jStart, nCols).colwise() += mBias;
  }

  long GetInChannels() const { return mWeight.empty() ? 0 : mWeight[0].cols(); }
  long GetOutChannels() const { return mWeight.empty() ? 0 : mWeight[0].rows(); }
  long GetKernelSize() const { return static_cast<long>(mWeight.size()); }
  int GetDilation() const { return mDilation; }

private:
  std::vector<Eigen::MatrixXf> mWeight;
  Eigen::VectorXf mBias;
  int mDilation = 1;
};

// ============================================================================
// Optimized WaveNet Layer
// ============================================================================

class OptimizedWaveNetLayer
{
public:
  OptimizedWaveNetLayer(
    int conditionSize,
    int channels,
    int kernelSize,
    int dilation,
    const std::string& activation,
    bool gated)
    : mConv(channels, gated ? 2 * channels : channels, kernelSize, true, dilation)
    , mInputMixin(conditionSize, gated ? 2 * channels : channels, false)
    , m1x1(channels, channels, true)
    , mActivation(activations::Activation::Get(activation))
    , mGated(gated)
    , mChannels(channels)
  {
  }

  void SetMaxBufferSize(int maxBufferSize)
  {
    mInputMixin.SetMaxBufferSize(maxBufferSize);
    mZ.resize(mGated ? 2 * mChannels : mChannels, maxBufferSize);
    m1x1.SetMaxBufferSize(maxBufferSize);
  }

  void SetWeights(std::vector<float>::iterator& weights)
  {
    mConv.SetWeights(weights);
    mInputMixin.SetWeights(weights);
    m1x1.SetWeights(weights);
  }

  void Process(
    const Eigen::MatrixXf& input,
    const Eigen::MatrixXf& condition,
    Eigen::MatrixXf& headInput,
    Eigen::MatrixXf& output,
    long iStart,
    long jStart,
    int numFrames)
  {
    // Input dilated conv
    mConv.Process(input, mZ, iStart, numFrames, 0);

    // Mix-in condition
    mInputMixin.Process(condition, numFrames);
    mZ.leftCols(numFrames).noalias() += mInputMixin.GetOutput(numFrames);

    // Apply activation with SIMD optimization
    if (!mGated)
    {
      // Simple activation
      mActivation->Apply(mZ.data(), mChannels * numFrames);
    }
    else
    {
      // Fused gated activation - MUCH faster than column-by-column
      // Process all frames at once with SIMD
      for (int i = 0; i < numFrames; ++i)
      {
        float* activationData = mZ.data() + i * mZ.rows();
        float* gateData = activationData + mChannels;
        simd::ApplyGatedActivation(activationData, gateData, mChannels);
      }
    }

    // Add to head input
    headInput.leftCols(numFrames).noalias() += mZ.block(0, 0, mChannels, numFrames);

    // Apply 1x1 convolution and residual
    if (!mGated)
    {
      m1x1.Process(mZ, numFrames);
    }
    else
    {
      // Only use first half (activation part) after gating
      Eigen::Map<const Eigen::MatrixXf> activationPart(
        mZ.data(), mChannels, mZ.cols());
      m1x1.Process(activationPart, numFrames);
    }

    output.middleCols(jStart, numFrames).noalias() =
      input.middleCols(iStart, numFrames) + m1x1.GetOutput(numFrames);
  }

  long GetChannels() const { return mChannels; }
  int GetDilation() const { return mConv.GetDilation(); }
  long GetKernelSize() const { return mConv.GetKernelSize(); }

private:
  OptimizedConv1D mConv;
  OptimizedConv1x1 mInputMixin;
  OptimizedConv1x1 m1x1;
  activations::Activation* mActivation;
  Eigen::MatrixXf mZ;
  bool mGated;
  int mChannels;
};

// ============================================================================
// Optimized Layer Array
// ============================================================================

constexpr long kLayerArrayBufferSize = 65536;

class OptimizedLayerArray
{
public:
  OptimizedLayerArray(
    int inputSize,
    int conditionSize,
    int headSize,
    int channels,
    int kernelSize,
    const std::vector<int>& dilations,
    const std::string& activation,
    bool gated,
    bool headBias)
    : mRechannel(inputSize, channels, false)
    , mHeadRechannel(channels, headSize, headBias)
  {
    for (int dilation : dilations)
    {
      mLayers.emplace_back(conditionSize, channels, kernelSize, dilation, activation, gated);
    }

    const long receptiveField = GetReceptiveFieldInternal();
    for (size_t i = 0; i < dilations.size(); ++i)
    {
      mLayerBuffers.emplace_back(channels, kLayerArrayBufferSize + receptiveField - 1);
      mLayerBuffers.back().setZero();
    }

    mBufferStart = receptiveField - 1;
  }

  void SetMaxBufferSize(int maxBufferSize)
  {
    mRechannel.SetMaxBufferSize(maxBufferSize);
    mHeadRechannel.SetMaxBufferSize(maxBufferSize);
    for (auto& layer : mLayers)
      layer.SetMaxBufferSize(maxBufferSize);
  }

  void SetWeights(std::vector<float>::iterator& weights)
  {
    mRechannel.SetWeights(weights);
    for (auto& layer : mLayers)
      layer.SetWeights(weights);
    mHeadRechannel.SetWeights(weights);
  }

  void AdvanceBuffers(int numFrames)
  {
    mBufferStart += numFrames;
  }

  void PrepareForFrames(long numFrames)
  {
    if (mBufferStart + numFrames > GetBufferSize())
      RewindBuffers();
  }

  void Process(
    const Eigen::MatrixXf& layerInputs,
    const Eigen::MatrixXf& condition,
    Eigen::MatrixXf& headInputs,
    Eigen::MatrixXf& layerOutputs,
    Eigen::MatrixXf& headOutputs,
    int numFrames)
  {
    mRechannel.Process(layerInputs, numFrames);
    mLayerBuffers[0].middleCols(mBufferStart, numFrames) = mRechannel.GetOutput(numFrames);

    const size_t lastLayer = mLayers.size() - 1;
    for (size_t i = 0; i < mLayers.size(); ++i)
    {
      mLayers[i].Process(
        mLayerBuffers[i],
        condition,
        headInputs,
        i == lastLayer ? layerOutputs : mLayerBuffers[i + 1],
        mBufferStart,
        i == lastLayer ? 0 : mBufferStart,
        numFrames);
    }

    mHeadRechannel.Process(headInputs, numFrames);
    headOutputs.leftCols(numFrames) = mHeadRechannel.GetOutput(numFrames);
  }

  long GetReceptiveField() const
  {
    long result = 0;
    for (const auto& layer : mLayers)
      result += layer.GetDilation() * (layer.GetKernelSize() - 1);
    return result;
  }

private:
  long GetReceptiveFieldInternal() const
  {
    long res = 1;
    for (const auto& layer : mLayers)
      res += (layer.GetKernelSize() - 1) * layer.GetDilation();
    return res;
  }

  long GetBufferSize() const
  {
    return mLayerBuffers.empty() ? 0 : mLayerBuffers[0].cols();
  }

  void RewindBuffers()
  {
    const long start = GetReceptiveFieldInternal() - 1;
    for (size_t i = 0; i < mLayerBuffers.size(); ++i)
    {
      const long d = (mLayers[i].GetKernelSize() - 1) * mLayers[i].GetDilation();
      mLayerBuffers[i].middleCols(start - d, d) = mLayerBuffers[i].middleCols(mBufferStart - d, d);
    }
    mBufferStart = start;
  }

  OptimizedConv1x1 mRechannel;
  OptimizedConv1x1 mHeadRechannel;
  std::vector<OptimizedWaveNetLayer> mLayers;
  std::vector<Eigen::MatrixXf> mLayerBuffers;
  long mBufferStart = 0;
};

// ============================================================================
// Layer Array Parameters (matches NAM format)
// ============================================================================

struct LayerArrayParams
{
  int inputSize;
  int conditionSize;
  int headSize;
  int channels;
  int kernelSize;
  std::vector<int> dilations;
  std::string activation;
  bool gated;
  bool headBias;
};

// ============================================================================
// Optimized WaveNet Processor
// ============================================================================

class OptimizedWaveNet
{
public:
  using SampleType = float;

  OptimizedWaveNet(
    const std::vector<LayerArrayParams>& layerArrayParams,
    float headScale,
    std::vector<float>& weights,
    double expectedSampleRate = -1.0)
    : mHeadScale(headScale)
    , mExpectedSampleRate(expectedSampleRate)
  {
    // Build layer arrays
    for (size_t i = 0; i < layerArrayParams.size(); ++i)
    {
      const auto& params = layerArrayParams[i];
      mLayerArrays.emplace_back(
        params.inputSize,
        params.conditionSize,
        params.headSize,
        params.channels,
        params.kernelSize,
        params.dilations,
        params.activation,
        params.gated,
        params.headBias);

      mLayerArrayOutputs.emplace_back(params.channels, 0);

      if (i == 0)
        mHeadArrays.emplace_back(params.channels, 0);

      mHeadArrays.emplace_back(params.headSize, 0);
    }

    mHeadOutput.resize(1, 0);  // Mono output
    SetWeights(weights);

    // Calculate prewarm samples
    mPrewarmSamples = 1;
    for (const auto& la : mLayerArrays)
      mPrewarmSamples += static_cast<int>(la.GetReceptiveField());
  }

  void SetMaxBufferSize(int maxBufferSize)
  {
    mMaxBufferSize = maxBufferSize;
    mCondition.resize(GetConditionDim(), maxBufferSize);

    for (auto& ha : mHeadArrays)
      ha.resize(ha.rows(), maxBufferSize);

    for (auto& lao : mLayerArrayOutputs)
      lao.resize(lao.rows(), maxBufferSize);

    mHeadOutput.resize(mHeadOutput.rows(), maxBufferSize);
    mHeadOutput.setZero();

    for (auto& la : mLayerArrays)
      la.SetMaxBufferSize(maxBufferSize);
  }

  void Reset(double sampleRate, int maxBufferSize)
  {
    mExternalSampleRate = sampleRate;
    SetMaxBufferSize(maxBufferSize);
    Prewarm();
  }

  void Prewarm()
  {
    if (mMaxBufferSize == 0)
      SetMaxBufferSize(4096);

    std::vector<SampleType> inputBuffer(mMaxBufferSize, 0.0f);
    std::vector<SampleType> outputBuffer(mMaxBufferSize, 0.0f);

    int samplesProcessed = 0;
    while (samplesProcessed < mPrewarmSamples)
    {
      Process(inputBuffer.data(), outputBuffer.data(), mMaxBufferSize);
      samplesProcessed += mMaxBufferSize;
    }
  }

  void Process(SampleType* input, SampleType* output, int numFrames)
  {
    assert(numFrames <= mMaxBufferSize);

    PrepareForFrames(numFrames);
    SetConditionArray(input, numFrames);

    // Main processing
    mHeadArrays[0].setZero();

    for (size_t i = 0; i < mLayerArrays.size(); ++i)
    {
      mLayerArrays[i].Process(
        i == 0 ? mCondition : mLayerArrayOutputs[i - 1],
        mCondition,
        mHeadArrays[i],
        mLayerArrayOutputs[i],
        mHeadArrays[i + 1],
        numFrames);
    }

    // Copy to output with head scale
    const size_t finalHeadArray = mHeadArrays.size() - 1;
    assert(mHeadArrays[finalHeadArray].rows() == 1);

    for (int s = 0; s < numFrames; ++s)
    {
      output[s] = mHeadScale * mHeadArrays[finalHeadArray](0, s);
    }

    AdvanceBuffers(numFrames);
  }

  double GetExpectedSampleRate() const { return mExpectedSampleRate; }

private:
  void SetWeights(std::vector<float>& weights)
  {
    auto it = weights.begin();
    for (auto& la : mLayerArrays)
      la.SetWeights(it);
    mHeadScale = *(it++);

    if (it != weights.end())
    {
      throw std::runtime_error("Weight mismatch: not all weights consumed");
    }
  }

  void PrepareForFrames(long numFrames)
  {
    for (auto& la : mLayerArrays)
      la.PrepareForFrames(numFrames);
  }

  void AdvanceBuffers(int numFrames)
  {
    for (auto& la : mLayerArrays)
      la.AdvanceBuffers(numFrames);
  }

  void SetConditionArray(SampleType* input, int numFrames)
  {
    for (int j = 0; j < numFrames; ++j)
      mCondition(0, j) = input[j];
  }

  int GetConditionDim() const { return 1; }  // Mono audio input

  std::vector<OptimizedLayerArray> mLayerArrays;
  std::vector<Eigen::MatrixXf> mLayerArrayOutputs;
  std::vector<Eigen::MatrixXf> mHeadArrays;
  Eigen::MatrixXf mHeadOutput;
  Eigen::MatrixXf mCondition;

  float mHeadScale = 1.0f;
  double mExpectedSampleRate = -1.0;
  double mExternalSampleRate = -1.0;
  int mMaxBufferSize = 0;
  int mPrewarmSamples = 0;
};

} // namespace nam
} // namespace guitarfx
