#pragma once

/**
 * Optimized LSTM processor for NAM models.
 * Uses SIMD-accelerated activations for the gate computations.
 *
 * Note: LSTM is inherently sequential (sample-by-sample) due to recurrent state,
 * so the optimization opportunities are limited to the gate activations.
 */

#include "OptimizedActivations.h"
#include "SimdMath.h"
#include <Eigen/Dense>
#include <vector>
#include <memory>
#include <cassert>

namespace guitarfx
{
namespace nam
{

// ============================================================================
// Optimized LSTM Cell
// ============================================================================

class OptimizedLSTMCell
{
public:
  OptimizedLSTMCell(int inputSize, int hiddenSize, std::vector<float>::iterator& weights)
    : mInputSize(inputSize)
    , mHiddenSize(hiddenSize)
  {
    // Allocate matrices
    mW.resize(4 * hiddenSize, inputSize + hiddenSize);
    mB.resize(4 * hiddenSize);
    mXH.resize(inputSize + hiddenSize);
    mIFGO.resize(4 * hiddenSize);
    mC.resize(hiddenSize);

    // Load weights (row-major order from PyTorch)
    for (int i = 0; i < mW.rows(); ++i)
      for (int j = 0; j < mW.cols(); ++j)
        mW(i, j) = *(weights++);

    for (int i = 0; i < mB.size(); ++i)
      mB(i) = *(weights++);

    // Initial hidden state
    const int hOffset = inputSize;
    for (int i = 0; i < hiddenSize; ++i)
      mXH(i + hOffset) = *(weights++);

    // Initial cell state
    for (int i = 0; i < hiddenSize; ++i)
      mC(i) = *(weights++);
  }

  Eigen::VectorXf GetHiddenState() const
  {
    return mXH(Eigen::placeholders::lastN(mHiddenSize));
  }

  void Process(const Eigen::VectorXf& x)
  {
    // Copy input to xh concatenated vector
    mXH.head(mInputSize) = x;

    // The main matrix multiplication: ifgo = W * [x; h] + b
    mIFGO.noalias() = mW * mXH + mB;

    // Gate indices
    const long iOffset = 0;
    const long fOffset = mHiddenSize;
    const long gOffset = 2 * mHiddenSize;
    const long oOffset = 3 * mHiddenSize;
    const long hOffset = mInputSize;

    // Apply gate activations with SIMD-optimized functions
    // i = sigmoid(ifgo[0:hidden])
    // f = sigmoid(ifgo[hidden:2*hidden])
    // g = tanh(ifgo[2*hidden:3*hidden])
    // o = sigmoid(ifgo[3*hidden:4*hidden])

    // Update cell state: c = f * c + i * g
    for (int i = 0; i < mHiddenSize; ++i)
    {
      float fGate = simd::scalar::FastSigmoid(mIFGO(i + fOffset));
      float iGate = simd::scalar::FastSigmoid(mIFGO(i + iOffset));
      float gGate = simd::scalar::FastTanh(mIFGO(i + gOffset));
      mC(i) = fGate * mC(i) + iGate * gGate;
    }

    // Update hidden state: h = o * tanh(c)
    for (int i = 0; i < mHiddenSize; ++i)
    {
      float oGate = simd::scalar::FastSigmoid(mIFGO(i + oOffset));
      mXH(i + hOffset) = oGate * simd::scalar::FastTanh(mC(i));
    }
  }

  // Vectorized version for when we can batch multiple gates
  void ProcessVectorized(const Eigen::VectorXf& x)
  {
    mXH.head(mInputSize) = x;
    mIFGO.noalias() = mW * mXH + mB;

    const long iOffset = 0;
    const long fOffset = mHiddenSize;
    const long gOffset = 2 * mHiddenSize;
    const long oOffset = 3 * mHiddenSize;
    const long hOffset = mInputSize;

    // Apply vectorized sigmoid to i, f, o gates
    float* ifgoData = mIFGO.data();
    simd::ApplySigmoid(ifgoData + iOffset, mHiddenSize);  // i gate
    simd::ApplySigmoid(ifgoData + fOffset, mHiddenSize);  // f gate
    simd::ApplyTanh(ifgoData + gOffset, mHiddenSize);     // g gate (tanh)
    simd::ApplySigmoid(ifgoData + oOffset, mHiddenSize);  // o gate

    // Update cell state: c = f * c + i * g
    for (int i = 0; i < mHiddenSize; ++i)
    {
      mC(i) = mIFGO(i + fOffset) * mC(i) + mIFGO(i + iOffset) * mIFGO(i + gOffset);
    }

    // Apply tanh to cell state for hidden output
    Eigen::VectorXf tanhC = mC;
    simd::ApplyTanh(tanhC.data(), mHiddenSize);

    // Update hidden state: h = o * tanh(c)
    for (int i = 0; i < mHiddenSize; ++i)
    {
      mXH(i + hOffset) = mIFGO(i + oOffset) * tanhC(i);
    }
  }

private:
  int mInputSize;
  int mHiddenSize;

  Eigen::MatrixXf mW;        // Weight matrix [4*hidden, input+hidden]
  Eigen::VectorXf mB;        // Bias [4*hidden]
  Eigen::VectorXf mXH;       // Concatenated [input; hidden]
  Eigen::VectorXf mIFGO;     // Gate outputs [i; f; g; o]
  Eigen::VectorXf mC;        // Cell state
};

// ============================================================================
// Optimized LSTM Network
// ============================================================================

class OptimizedLSTM
{
public:
  using SampleType = float;

  OptimizedLSTM(
    int numLayers,
    int inputSize,
    int hiddenSize,
    std::vector<float>& weights,
    double expectedSampleRate = -1.0)
    : mHiddenSize(hiddenSize)
    , mExpectedSampleRate(expectedSampleRate)
  {
    mInput.resize(1);  // Mono input

    auto it = weights.begin();

    // Create LSTM layers
    for (int i = 0; i < numLayers; ++i)
    {
      int layerInputSize = (i == 0) ? inputSize : hiddenSize;
      mLayers.emplace_back(layerInputSize, hiddenSize, it);
    }

    // Output head weights
    mHeadWeight.resize(hiddenSize);
    for (int i = 0; i < hiddenSize; ++i)
      mHeadWeight(i) = *(it++);

    mHeadBias = *(it++);

    assert(it == weights.end());
  }

  void Reset(double sampleRate, int maxBufferSize)
  {
    mExternalSampleRate = sampleRate;
    mMaxBufferSize = maxBufferSize;
    Prewarm();
  }

  void Prewarm()
  {
    // LSTM needs about 0.5 seconds to settle
    int prewarmSamples = static_cast<int>(0.5 * mExpectedSampleRate);
    if (prewarmSamples <= 0)
      prewarmSamples = 1;

    const int bufferSize = std::max(mMaxBufferSize, 4096);
    std::vector<SampleType> inputBuffer(bufferSize, 0.0f);
    std::vector<SampleType> outputBuffer(bufferSize, 0.0f);

    int samplesProcessed = 0;
    while (samplesProcessed < prewarmSamples)
    {
      Process(inputBuffer.data(), outputBuffer.data(), bufferSize);
      samplesProcessed += bufferSize;
    }
  }

  void Process(SampleType* input, SampleType* output, int numFrames)
  {
    // LSTM processes sample-by-sample due to recurrent nature
    for (int i = 0; i < numFrames; ++i)
    {
      output[i] = ProcessSample(input[i]);
    }
  }

  double GetExpectedSampleRate() const { return mExpectedSampleRate; }

private:
  SampleType ProcessSample(SampleType x)
  {
    if (mLayers.empty())
      return x;

    mInput(0) = x;

    // Process through LSTM layers
    mLayers[0].ProcessVectorized(mInput);

    for (size_t i = 1; i < mLayers.size(); ++i)
    {
      mLayers[i].ProcessVectorized(mLayers[i - 1].GetHiddenState());
    }

    // Output head: dot product + bias
    return mHeadWeight.dot(mLayers.back().GetHiddenState()) + mHeadBias;
  }

  std::vector<OptimizedLSTMCell> mLayers;
  Eigen::VectorXf mInput;
  Eigen::VectorXf mHeadWeight;
  float mHeadBias = 0.0f;

  int mHiddenSize;
  double mExpectedSampleRate = -1.0;
  double mExternalSampleRate = -1.0;
  int mMaxBufferSize = 0;
};

} // namespace nam
} // namespace guitarfx
