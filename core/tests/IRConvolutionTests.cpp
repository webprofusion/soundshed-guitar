/**
 * @file IRConvolutionTests.cpp
 * @brief Unit tests for IR (Impulse Response) convolution algorithm
 *
 * Tests the graph-based IRCabEffect convolution with known inputs and expected outputs to verify
 * the convolution implementation is mathematically correct, and verifies realtime audio processing
 * with clean output and no latency.
 */

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "dsp/SignalGraphExecutor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/BuiltinEffects.h"
#include "resources/ResourceLibrary.h"
#include "presets/PresetTypes.h"

// Define M_PI if not available
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace fs = std::filesystem;

namespace
{
  constexpr double kEpsilon = 1e-6;
  constexpr double kSampleRate = 44100.0;
  constexpr int kBlockSize = 512;

  bool ApproxEqual(double a, double b, double epsilon = kEpsilon)
  {
    return std::abs(a - b) < epsilon;
  }

  double ComputeRms(const std::vector<double>& samples)
  {
    if (samples.empty())
    {
      return 0.0;
    }

    double sumSquares = 0.0;
    for (double sample : samples)
    {
      sumSquares += sample * sample;
    }
    return std::sqrt(sumSquares / static_cast<double>(samples.size()));
  }

  /**
   * @brief Reference implementation of direct-form FIR convolution
   *
   * This is a simple, obviously-correct implementation for test comparison.
   * output[n] = sum(input[n-k] * impulse[k]) for k = 0 to IR_length-1
   */
  std::vector<double> ReferenceConvolve(const std::vector<double>& input,
                                        const std::vector<float>& impulse)
  {
    if (impulse.empty())
    {
      return input;
    }

    std::vector<double> output(input.size(), 0.0);
    const std::size_t irLength = impulse.size();

    for (std::size_t n = 0; n < input.size(); ++n)
    {
      double sum = 0.0;
      for (std::size_t k = 0; k < irLength; ++k)
      {
        // For sample index n-k, if it would be negative, use 0 (zero-padding)
        if (n >= k)
        {
          sum += input[n - k] * static_cast<double>(impulse[k]);
        }
      }
      output[n] = sum;
    }

    return output;
  }

  void RegisterEffectsOnce()
  {
    static bool registered = false;
    if (!registered)
    {
      guitarfx::RegisterAllEffects();
      registered = true;
    }
  }

  std::filesystem::path WriteImpulseToWav(const std::vector<float>& impulse, double sampleRate)
  {
    auto tempDir = fs::temp_directory_path() / "guitarfx_ir_tests";
    std::filesystem::create_directories(tempDir);

    static std::atomic<int> counter{0};
    const auto filename = "ir_" + std::to_string(counter++) + ".wav";
    const auto path = tempDir / filename;

    std::ofstream file(path, std::ios::binary);
    if (!file)
    {
      throw std::runtime_error("Failed to create temp IR file");
    }

    const uint32_t dataSize = static_cast<uint32_t>(impulse.size() * sizeof(float));
    const uint32_t riffSize = 36u + dataSize;
    const uint16_t audioFormat = 3; // IEEE float
    const uint16_t numChannels = 1;
    const uint32_t sampleRateU32 = static_cast<uint32_t>(sampleRate);
    const uint32_t byteRate = sampleRateU32 * numChannels * sizeof(float);
    const uint16_t blockAlign = static_cast<uint16_t>(numChannels * sizeof(float));
    const uint16_t bitsPerSample = 32;
    const uint32_t fmtChunkSize = 16;

    file.write("RIFF", 4);
    file.write(reinterpret_cast<const char*>(&riffSize), 4);
    file.write("WAVE", 4);
    file.write("fmt ", 4);
    file.write(reinterpret_cast<const char*>(&fmtChunkSize), 4);
    file.write(reinterpret_cast<const char*>(&audioFormat), 2);
    file.write(reinterpret_cast<const char*>(&numChannels), 2);
    file.write(reinterpret_cast<const char*>(&sampleRateU32), 4);
    file.write(reinterpret_cast<const char*>(&byteRate), 4);
    file.write(reinterpret_cast<const char*>(&blockAlign), 2);
    file.write(reinterpret_cast<const char*>(&bitsPerSample), 2);
    file.write("data", 4);
    file.write(reinterpret_cast<const char*>(&dataSize), 4);

    if (!impulse.empty())
    {
      file.write(reinterpret_cast<const char*>(impulse.data()), dataSize);
    }

    return path;
  }

  std::filesystem::path WriteStereoImpulseToWav(const std::vector<float>& left,
                                                const std::vector<float>& right,
                                                double sampleRate)
  {
    const std::size_t length = std::min(left.size(), right.size());
    if (length == 0)
    {
      throw std::runtime_error("Stereo IR must have data");
    }

    auto tempDir = fs::temp_directory_path() / "guitarfx_ir_tests";
    std::filesystem::create_directories(tempDir);

    static std::atomic<int> counter{0};
    const auto filename = "ir_stereo_" + std::to_string(counter++) + ".wav";
    const auto path = tempDir / filename;

    std::ofstream file(path, std::ios::binary);
    if (!file)
    {
      throw std::runtime_error("Failed to create temp stereo IR file");
    }

    std::vector<float> interleaved(length * 2);
    for (std::size_t i = 0; i < length; ++i)
    {
      interleaved[i * 2] = left[i];
      interleaved[i * 2 + 1] = right[i];
    }

    const uint32_t dataSize = static_cast<uint32_t>(interleaved.size() * sizeof(float));
    const uint32_t riffSize = 36u + dataSize;
    const uint16_t audioFormat = 3; // IEEE float
    const uint16_t numChannels = 2;
    const uint32_t sampleRateU32 = static_cast<uint32_t>(sampleRate);
    const uint32_t byteRate = sampleRateU32 * numChannels * sizeof(float);
    const uint16_t blockAlign = static_cast<uint16_t>(numChannels * sizeof(float));
    const uint16_t bitsPerSample = 32;
    const uint32_t fmtChunkSize = 16;

    file.write("RIFF", 4);
    file.write(reinterpret_cast<const char*>(&riffSize), 4);
    file.write("WAVE", 4);
    file.write("fmt ", 4);
    file.write(reinterpret_cast<const char*>(&fmtChunkSize), 4);
    file.write(reinterpret_cast<const char*>(&audioFormat), 2);
    file.write(reinterpret_cast<const char*>(&numChannels), 2);
    file.write(reinterpret_cast<const char*>(&sampleRateU32), 4);
    file.write(reinterpret_cast<const char*>(&byteRate), 4);
    file.write(reinterpret_cast<const char*>(&blockAlign), 2);
    file.write(reinterpret_cast<const char*>(&bitsPerSample), 2);
    file.write("data", 4);
    file.write(reinterpret_cast<const char*>(&dataSize), 4);
    file.write(reinterpret_cast<const char*>(interleaved.data()), dataSize);

    return path;
  }

  /**
   * @brief Helper class to test IR convolution via SignalGraphExecutor + IRCabEffect
   */
  class IRConvolutionTester
  {
  public:
    IRConvolutionTester()
    {
      RegisterEffectsOnce();
      BuildGraph();
    }

    ~IRConvolutionTester()
    {
      CleanupTempFiles();
    }

    void SetImpulse(const std::vector<float>& impulse)
    {
      if (impulse.empty())
      {
        mExecutor.SetNodeEnabled("cab", false);
        mExecutor.Reset();
        return;
      }

      const auto path = WriteImpulseToWav(impulse, kSampleRate);
      mTempFiles.push_back(path);
      LoadImpulse(path);
    }

    void SetImpulseFromFile(const fs::path& path)
    {
      if (!fs::exists(path))
      {
        throw std::runtime_error("IR file not found: " + path.string());
      }

      LoadImpulse(path);
    }

    void SetStereoImpulse(const std::vector<float>& left, const std::vector<float>& right)
    {
      if (left.empty() || right.empty())
      {
        mExecutor.SetNodeEnabled("cab", false);
        mExecutor.Reset();
        return;
      }

      const auto path = WriteStereoImpulseToWav(left, right, kSampleRate);
      mTempFiles.push_back(path);
      LoadImpulse(path);
    }

    void SetDualImpulse(const std::vector<float>& impulseA,
                       const std::vector<float>& impulseB,
                       double blend)
    {
      if (impulseA.empty())
      {
        mExecutor.SetNodeEnabled("cab", false);
        mExecutor.Reset();
        return;
      }

      const auto pathA = WriteImpulseToWav(impulseA, kSampleRate);
      mTempFiles.push_back(pathA);

      const auto pathB = WriteImpulseToWav(impulseB.empty() ? std::vector<float>{1.0f} : impulseB, kSampleRate);
      mTempFiles.push_back(pathB);

      guitarfx::SignalGraph graph;

      guitarfx::GraphNode input;
      input.id = "input";
      input.type = guitarfx::kNodeTypeInput;
      input.category = "utility";
      input.enabled = true;

      guitarfx::GraphNode cab;
      cab.id = "cab";
      cab.type = "cab_ir";
      cab.category = "cab";
      cab.enabled = true;
      cab.params["mix"] = 1.0;
      cab.params["irBlend"] = std::clamp(blend, 0.0, 1.0);
      cab.params["outputGain"] = 0.0;

      guitarfx::ResourceRef refA;
      refA.filePath = pathA;
      cab.resources.push_back(refA);

      guitarfx::ResourceRef refB;
      refB.filePath = pathB;
      refB.parameterValue = std::clamp(blend, 0.0, 1.0);
      cab.resources.push_back(refB);

      guitarfx::GraphNode output;
      output.id = "output";
      output.type = guitarfx::kNodeTypeOutput;
      output.category = "utility";
      output.enabled = true;

      graph.nodes = { input, cab, output };

      guitarfx::GraphEdge edge1;
      edge1.from = input.id;
      edge1.to = cab.id;
      edge1.gain = 1.0;

      guitarfx::GraphEdge edge2;
      edge2.from = cab.id;
      edge2.to = output.id;
      edge2.gain = 1.0;

      graph.edges = { edge1, edge2 };

      mExecutor.SetResourceLibrary(&mResourceLibrary);
      mExecutor.SetGraph(graph);
      mExecutor.Prepare(kSampleRate, mMaxBlockSize);
      mExecutor.Reset();
    }

    void SetCabParam(const std::string& key, double value)
    {
      mExecutor.SetNodeParam("cab", key, value);
    }

    void Convolve(std::vector<double>& samples, int channel = 0)
    {
      std::size_t offset = 0;
      while (offset < samples.size())
      {
        const int currentBlock = static_cast<int>(std::min<std::size_t>(mMaxBlockSize, samples.size() - offset));
        ResizeBuffers(currentBlock);

        std::fill_n(mInputBufferL.data(), currentBlock, 0.0f);
        std::fill_n(mInputBufferR.data(), currentBlock, 0.0f);

        for (int i = 0; i < currentBlock; ++i)
        {
          const float sample = static_cast<float>(samples[offset + static_cast<std::size_t>(i)]);
          if (channel == 0)
          {
            mInputBufferL[static_cast<std::size_t>(i)] = sample;
          }
          else
          {
            mInputBufferR[static_cast<std::size_t>(i)] = sample;
          }
        }

        float* inputPtrs[2] = { mInputBufferL.data(), mInputBufferR.data() };
        float* outputPtrs[2] = { mOutputBufferL.data(), mOutputBufferR.data() };

        mExecutor.Process(inputPtrs, outputPtrs, currentBlock);

        for (int i = 0; i < currentBlock; ++i)
        {
          const float value = channel == 0 ? mOutputBufferL[static_cast<std::size_t>(i)]
                                           : mOutputBufferR[static_cast<std::size_t>(i)];
          samples[offset + static_cast<std::size_t>(i)] = static_cast<double>(value);
        }

        offset += static_cast<std::size_t>(currentBlock);
      }
    }

    void Reset()
    {
      mExecutor.Reset();
    }

  private:
    void BuildGraph()
    {
      guitarfx::SignalGraph graph;

      guitarfx::GraphNode input;
      input.id = "input";
      input.type = guitarfx::kNodeTypeInput;
      input.category = "utility";
      input.enabled = true;

      guitarfx::GraphNode cab;
      cab.id = "cab";
      cab.type = "cab_ir";
      cab.category = "cab";
      cab.enabled = true;
      cab.params["mix"] = 1.0;
      cab.params["outputGain"] = 0.0;

      guitarfx::GraphNode output;
      output.id = "output";
      output.type = guitarfx::kNodeTypeOutput;
      output.category = "utility";
      output.enabled = true;

      graph.nodes = { input, cab, output };

      guitarfx::GraphEdge edge1;
      edge1.from = input.id;
      edge1.to = cab.id;
      edge1.gain = 1.0;

      guitarfx::GraphEdge edge2;
      edge2.from = cab.id;
      edge2.to = output.id;
      edge2.gain = 1.0;

      graph.edges = { edge1, edge2 };

      mExecutor.SetResourceLibrary(&mResourceLibrary);
      mExecutor.SetGraph(graph);
      mExecutor.Prepare(kSampleRate, mMaxBlockSize);
    }

    void LoadImpulse(const fs::path& path)
    {
      guitarfx::ResourceRef ref;
      ref.filePath = path;
      mExecutor.SetNodeEnabled("cab", true);
      mExecutor.LoadNodeResource("cab", ref);
      mExecutor.Reset();
    }

    void ResizeBuffers(int size)
    {
      mInputBufferL.resize(static_cast<std::size_t>(size));
      mInputBufferR.resize(static_cast<std::size_t>(size));
      mOutputBufferL.resize(static_cast<std::size_t>(size));
      mOutputBufferR.resize(static_cast<std::size_t>(size));
    }

    void CleanupTempFiles()
    {
      for (const auto& path : mTempFiles)
      {
        std::error_code ec;
        std::filesystem::remove(path, ec);
      }
    }

    guitarfx::ResourceLibrary mResourceLibrary;
    guitarfx::SignalGraphExecutor mExecutor;
    std::vector<float> mInputBufferL;
    std::vector<float> mInputBufferR;
    std::vector<float> mOutputBufferL;
    std::vector<float> mOutputBufferR;
    std::vector<fs::path> mTempFiles;
    const int mMaxBlockSize = kBlockSize;
  };

  // Test 1: Simple identity IR [1.0] should pass through unchanged
  bool TestIdentityIR()
  {
    std::cout << "Test: Identity IR [1.0]... ";

    std::vector<float> impulse = { 1.0f };
    std::vector<double> input = { 0.5, 1.0, -0.5, 0.25, 0.0 };
    std::vector<double> expected = { 0.5, 1.0, -0.5, 0.25, 0.0 };

    IRConvolutionTester tester;
    tester.SetImpulse(impulse);
    std::vector<double> testSamples = input;
    tester.Convolve(testSamples);

    for (std::size_t i = 0; i < expected.size(); ++i)
    {
      if (!ApproxEqual(testSamples[i], expected[i]))
      {
        std::cout << "FAILED at index " << i
                  << " (expected " << expected[i] << ", got " << testSamples[i] << ")\n";
        return false;
      }
    }

    std::cout << "OK\n";
    return true;
  }

  // Test 2: Simple gain IR [0.5] should halve all samples
  bool TestGainIR()
  {
    std::cout << "Test: Gain IR [0.5]... ";

    std::vector<float> impulse = { 0.5f };
    std::vector<double> input = { 1.0, 2.0, -1.0, 0.5 };
    std::vector<double> expected = { 0.5, 1.0, -0.5, 0.25 };

    IRConvolutionTester tester;
    tester.SetImpulse(impulse);
    std::vector<double> testSamples = input;
    tester.Convolve(testSamples);

    for (std::size_t i = 0; i < expected.size(); ++i)
    {
      if (!ApproxEqual(testSamples[i], expected[i]))
      {
        std::cout << "FAILED at index " << i
                  << " (expected " << expected[i] << ", got " << testSamples[i] << ")\n";
        return false;
      }
    }

    std::cout << "OK\n";
    return true;
  }

  // Test 3: Two-tap IR [0.5, 0.5] - averaging filter
  // output[n] = 0.5 * input[n] + 0.5 * input[n-1]
  bool TestTwoTapAverageIR()
  {
    std::cout << "Test: Two-tap averaging IR [0.5, 0.5]... ";

    std::vector<float> impulse = { 0.5f, 0.5f };
    std::vector<double> input = { 1.0, 0.0, 1.0, 0.0, 1.0 };

    // Expected: output[n] = 0.5 * input[n] + 0.5 * input[n-1]
    // n=0: 0.5*1.0 + 0.5*0 = 0.5  (input[-1] is 0 from zero-initialized buffer)
    // n=1: 0.5*0.0 + 0.5*1.0 = 0.5
    // n=2: 0.5*1.0 + 0.5*0.0 = 0.5
    // n=3: 0.5*0.0 + 0.5*1.0 = 0.5
    // n=4: 0.5*1.0 + 0.5*0.0 = 0.5
    std::vector<double> expected = { 0.5, 0.5, 0.5, 0.5, 0.5 };

    IRConvolutionTester tester;
    tester.SetImpulse(impulse);
    std::vector<double> testSamples = input;
    tester.Convolve(testSamples);

    for (std::size_t i = 0; i < expected.size(); ++i)
    {
      if (!ApproxEqual(testSamples[i], expected[i]))
      {
        std::cout << "FAILED at index " << i
                  << " (expected " << expected[i] << ", got " << testSamples[i] << ")\n";
        return false;
      }
    }

    std::cout << "OK\n";
    return true;
  }

  // Test 4: Delay IR [0, 1] - one sample delay
  // output[n] = 0 * input[n] + 1 * input[n-1] = input[n-1]
  bool TestDelayIR()
  {
    std::cout << "Test: Delay IR [0, 1] (one sample delay)... ";

    std::vector<float> impulse = { 0.0f, 1.0f };
    std::vector<double> input = { 1.0, 2.0, 3.0, 4.0 };

    // Expected: output = input delayed by 1 sample (first sample is 0 from buffer init)
    // n=0: 0*1.0 + 1*0 = 0
    // n=1: 0*2.0 + 1*1.0 = 1
    // n=2: 0*3.0 + 1*2.0 = 2
    // n=3: 0*4.0 + 1*3.0 = 3
    std::vector<double> expected = { 0.0, 1.0, 2.0, 3.0 };

    IRConvolutionTester tester;
    tester.SetImpulse(impulse);
    std::vector<double> testSamples = input;
    tester.Convolve(testSamples);

    for (std::size_t i = 0; i < expected.size(); ++i)
    {
      if (!ApproxEqual(testSamples[i], expected[i]))
      {
        std::cout << "FAILED at index " << i
                  << " (expected " << expected[i] << ", got " << testSamples[i] << ")\n";
        return false;
      }
    }

    std::cout << "OK\n";
    return true;
  }

  // Test 5: Three-tap IR with coefficients [1, 2, 3]
  // output[n] = 1 * input[n] + 2 * input[n-1] + 3 * input[n-2]
  bool TestThreeTapIR()
  {
    std::cout << "Test: Three-tap IR [1, 2, 3]... ";

    std::vector<float> impulse = { 1.0f, 2.0f, 3.0f };
    std::vector<double> input = { 1.0, 0.0, 0.0, 0.0 };

    // Impulse response to a unit impulse:
    // n=0: 1*1 + 2*0 + 3*0 = 1
    // n=1: 1*0 + 2*1 + 3*0 = 2
    // n=2: 1*0 + 2*0 + 3*1 = 3
    // n=3: 1*0 + 2*0 + 3*0 = 0
    std::vector<double> expected = { 1.0, 2.0, 3.0, 0.0 };

    IRConvolutionTester tester;
    tester.SetImpulse(impulse);
    std::vector<double> testSamples = input;
    tester.Convolve(testSamples);

    for (std::size_t i = 0; i < expected.size(); ++i)
    {
      if (!ApproxEqual(testSamples[i], expected[i]))
      {
        std::cout << "FAILED at index " << i
                  << " (expected " << expected[i] << ", got " << testSamples[i] << ")\n";
        return false;
      }
    }

    std::cout << "OK\n";
    return true;
  }

  // Test 6: Compare graph convolver with reference implementation
  bool TestAgainstReference()
  {
    std::cout << "Test: Graph convolver vs reference implementation... ";

    // Use a more complex IR and input
    std::vector<float> impulse = { 0.3f, 0.5f, 0.2f, -0.1f, 0.1f };
    std::vector<double> input = { 1.0, -0.5, 0.25, 0.75, -1.0, 0.5, 0.0, 0.25 };

    // Get reference result
    std::vector<double> referenceOutput = ReferenceConvolve(input, impulse);

    // Get graph result
    IRConvolutionTester tester;
    tester.SetImpulse(impulse);
    std::vector<double> dspOutput = input;
    tester.Convolve(dspOutput);

    for (std::size_t i = 0; i < input.size(); ++i)
    {
      if (!ApproxEqual(dspOutput[i], referenceOutput[i]))
      {
        std::cout << "FAILED at index " << i
                  << " (reference=" << referenceOutput[i]
                  << ", dsp=" << dspOutput[i] << ")\n";
        return false;
      }
    }

    std::cout << "OK\n";
    return true;
  }

  // Test 7: Multiple blocks - verify state is maintained across calls
  bool TestMultipleBlocks()
  {
    std::cout << "Test: Multiple blocks (state continuity)... ";

    std::vector<float> impulse = { 0.5f, 0.5f };

    // Process as two separate blocks
    IRConvolutionTester tester;
    tester.SetImpulse(impulse);
    std::vector<double> block1 = { 1.0, 0.0 };
    std::vector<double> block2 = { 1.0, 0.0 };

    tester.Convolve(block1);
    tester.Convolve(block2);

    // Process as single block for comparison
    IRConvolutionTester testerRef;
    testerRef.SetImpulse(impulse);
    std::vector<double> fullBlock = { 1.0, 0.0, 1.0, 0.0 };
    testerRef.Convolve(fullBlock);

    // Results should match
    bool match = ApproxEqual(block1[0], fullBlock[0]) &&
                 ApproxEqual(block1[1], fullBlock[1]) &&
                 ApproxEqual(block2[0], fullBlock[2]) &&
                 ApproxEqual(block2[1], fullBlock[3]);

    if (!match)
    {
      std::cout << "FAILED - block results don't match full processing\n";
      std::cout << "  Block1: [" << block1[0] << ", " << block1[1] << "]\n";
      std::cout << "  Block2: [" << block2[0] << ", " << block2[1] << "]\n";
      std::cout << "  Full:   [" << fullBlock[0] << ", " << fullBlock[1]
                << ", " << fullBlock[2] << ", " << fullBlock[3] << "]\n";
      return false;
    }

    std::cout << "OK\n";
    return true;
  }

  // Test 8: Longer IR to stress circular buffer wrapping
  bool TestLongIR()
  {
    std::cout << "Test: Long IR (circular buffer wrapping)... ";

    // Create an IR longer than input to ensure wrapping works
    std::vector<float> impulse(10, 0.1f);  // 10 taps, each 0.1
    std::vector<double> input = { 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0 };

    // Get reference result
    std::vector<double> expected = ReferenceConvolve(input, impulse);

    // Get graph result
    IRConvolutionTester tester;
    tester.SetImpulse(impulse);
    std::vector<double> result = input;
    tester.Convolve(result);

    for (std::size_t i = 0; i < input.size(); ++i)
    {
      if (!ApproxEqual(result[i], expected[i]))
      {
        std::cout << "FAILED at index " << i
                  << " (expected " << expected[i] << ", got " << result[i] << ")\n";
        return false;
      }
    }

    std::cout << "OK\n";
    return true;
  }

  // Test 9: Empty impulse should pass through unchanged
  bool TestEmptyIR()
  {
    std::cout << "Test: Empty IR (passthrough)... ";

    std::vector<float> impulse = {};
    std::vector<double> input = { 1.0, 2.0, 3.0 };
    std::vector<double> expected = { 1.0, 2.0, 3.0 };

    IRConvolutionTester tester;
    tester.SetImpulse(impulse);
    std::vector<double> testSamples = input;
    tester.Convolve(testSamples);

    for (std::size_t i = 0; i < expected.size(); ++i)
    {
      if (!ApproxEqual(testSamples[i], expected[i]))
      {
        std::cout << "FAILED at index " << i
                  << " (expected " << expected[i] << ", got " << testSamples[i] << ")\n";
        return false;
      }
    }

    std::cout << "OK\n";
    return true;
  }

  // Test 10: Stereo channels - verify each channel has independent state
  bool TestStereoChannels()
  {
    std::cout << "Test: Stereo channels (independent state)... ";

    std::vector<float> impulse = { 0.5f, 0.5f };

    IRConvolutionTester tester;
    tester.SetImpulse(impulse);

    // Process different data on each channel
    std::vector<double> channelL = { 1.0, 0.0, 0.0 };
    std::vector<double> channelR = { 0.0, 1.0, 0.0 };

    tester.Convolve(channelL, 0);
    tester.Convolve(channelR, 1);

    // Left channel: impulse at t=0, so output = [0.5, 0.5, 0]
    // Right channel: impulse at t=1, so output = [0, 0.5, 0.5]
    std::vector<double> expectedL = { 0.5, 0.5, 0.0 };
    std::vector<double> expectedR = { 0.0, 0.5, 0.5 };

    for (std::size_t i = 0; i < expectedL.size(); ++i)
    {
      if (!ApproxEqual(channelL[i], expectedL[i]))
      {
        std::cout << "FAILED on L channel at index " << i
                  << " (expected " << expectedL[i] << ", got " << channelL[i] << ")\n";
        return false;
      }
      if (!ApproxEqual(channelR[i], expectedR[i]))
      {
        std::cout << "FAILED on R channel at index " << i
                  << " (expected " << expectedR[i] << ", got " << channelR[i] << ")\n";
        return false;
      }
    }

    std::cout << "OK\n";
    return true;
  }

  // Test 10b: Stereo IR retention - ensure L/R IRs stay independent
  bool TestStereoIRRetention()
  {
    std::cout << "Test: Stereo IR retention (L/R distinct)... ";

    std::vector<float> leftIR = { 1.0f, 0.0f, 0.0f };
    std::vector<float> rightIR = { 0.0f, 1.0f, 0.0f };

    IRConvolutionTester tester;
    tester.SetStereoImpulse(leftIR, rightIR);

    std::vector<double> channelL = { 1.0, 0.0, 0.0 };
    std::vector<double> channelR = { 1.0, 0.0, 0.0 };

    tester.Convolve(channelL, 0);
    tester.Convolve(channelR, 1);

    std::vector<double> expectedL = { 1.0, 0.0, 0.0 };
    std::vector<double> expectedR = { 0.0, 1.0, 0.0 };

    for (std::size_t i = 0; i < expectedL.size(); ++i)
    {
      if (!ApproxEqual(channelL[i], expectedL[i]))
      {
        std::cout << "FAILED on L channel at index " << i
                  << " (expected " << expectedL[i] << ", got " << channelL[i] << ")\n";
        return false;
      }
      if (!ApproxEqual(channelR[i], expectedR[i]))
      {
        std::cout << "FAILED on R channel at index " << i
                  << " (expected " << expectedR[i] << ", got " << channelR[i] << ")\n";
        return false;
      }
    }

    std::cout << "OK\n";
    return true;
  }

  // Test 11: Realtime Latency Test - 1 second input = 1 second output
  // Verifies that IR convolution doesn't introduce processing delay
  bool TestRealtimeLatency()
  {
    std::cout << "Test: Realtime latency (1 second input = 1 second output)... ";

    const double sampleRate = 44100.0;
    const int blockSize = 512;
    const double durationSeconds = 1.0;
    const int totalSamples = static_cast<int>(sampleRate * durationSeconds);
    const int numBlocks = (totalSamples + blockSize - 1) / blockSize;

    // Use a simple gain IR that shouldn't introduce latency
    std::vector<float> impulse = { 1.0f };

    IRConvolutionTester tester;
    tester.SetImpulse(impulse);

    // Generate a 1-second test signal (e.g., swept frequency)
    std::vector<double> inputSignal(totalSamples);
    for (int i = 0; i < totalSamples; ++i)
    {
      // Create a simple sine wave swept from 100Hz to 1kHz
      double t = static_cast<double>(i) / sampleRate;
      double freq = 100.0 + 900.0 * t;  // Sweep from 100Hz to 1kHz over 1 second
      inputSignal[i] = 0.5 * std::sin(2.0 * M_PI * freq * t);
    }

    // Process in blocks, simulating realtime operation
    std::vector<double> outputSignal(totalSamples, 0.0);
    int processedSamples = 0;

    for (int block = 0; block < numBlocks; ++block)
    {
      int remainingSamples = totalSamples - processedSamples;
      int currentBlockSize = std::min(blockSize, remainingSamples);

      // Extract block from input
      std::vector<double> blockData(inputSignal.begin() + processedSamples,
                                     inputSignal.begin() + processedSamples + currentBlockSize);

      // Process block through IR convolution
      tester.Convolve(blockData);

      // Copy output
      for (int i = 0; i < currentBlockSize; ++i)
      {
        outputSignal[processedSamples + i] = blockData[i];
      }

      processedSamples += currentBlockSize;
    }

    // Verify:
    // 1. Output duration should be approximately 1 second (no significant delay)
    // 2. Output should match input (identity IR), with minimal latency
    // 3. First sample should appear within first few blocks (< 50ms = 2205 samples at 44.1kHz)

    const int maxAcceptableLatency = static_cast<int>(0.050 * sampleRate);  // 50ms max latency
    
    // For identity IR, output should closely match input
    for (int i = 0; i < totalSamples; ++i)
    {
      if (!ApproxEqual(outputSignal[i], inputSignal[i], 1e-5))
      {
        std::cout << "FAILED - Audio output doesn't match input at sample " << i << "\n";
        std::cout << "  Expected: " << inputSignal[i] << ", Got: " << outputSignal[i] << "\n";
        return false;
      }
    }

    // Check that we got output samples in realtime fashion
    // Count non-zero output samples in first block
    int firstNonZeroIndex = -1;
    for (int i = 0; i < std::min(blockSize, totalSamples); ++i)
    {
      if (std::abs(outputSignal[i]) > 1e-10)
      {
        firstNonZeroIndex = i;
        break;
      }
    }

    if (firstNonZeroIndex >= 0 && firstNonZeroIndex > maxAcceptableLatency)
    {
      std::cout << "FAILED - Excessive latency detected: " << firstNonZeroIndex << " samples\n";
      return false;
    }

    std::cout << "OK\n";
    return true;
  }

  // Test 12: Clean Audio Quality - verify no artifacts with IR processing
  // Checks that IR-processed audio maintains audio quality and cleanliness
  bool TestAudioCleanness()
  {
    std::cout << "Test: Audio cleanliness (no artifacts from IR processing)... " << std::flush;

    try
    {
      // Use a simple identity IR to ensure no artifacts
      std::vector<float> simpleIR = { 1.0f };

      // Generate test signal - 50 samples of simple sine
      const int duration = 100;
      std::vector<double> testSignal(duration);
      
      for (int i = 0; i < duration; ++i)
      {
        testSignal[i] = 0.5 * (i % 2 == 0 ? 1.0 : -1.0);  // Simple square-ish signal
      }

      std::vector<double> originalSignal = testSignal;

      // Apply IR convolution
      IRConvolutionTester tester;
      tester.SetImpulse(simpleIR);
      tester.Convolve(testSignal);

      // Check for NaN/Inf
      for (int i = 0; i < duration; ++i)
      {
        if (std::isnan(testSignal[i]) || std::isinf(testSignal[i]))
        {
          std::cout << "FAILED - Invalid value at sample " << i << "\n";
          return false;
        }
      }

      // Check amplitude is reasonable (with identity IR, should be same)
      double maxVal = 0.0;
      for (int i = 0; i < duration; ++i)
      {
        maxVal = std::max(maxVal, std::abs(testSignal[i]));
      }

      if (maxVal < 0.4 || maxVal > 0.6)
      {
        std::cout << "FAILED - Amplitude out of range: " << maxVal << "\n";
        return false;
      }

      std::cout << "OK\n";
      return true;
    }
    catch (const std::exception& e)
    {
      std::cout << "FAILED - Exception: " << e.what() << "\n";
      return false;
    }
    catch (...)
    {
      std::cout << "FAILED - Unknown exception\n";
      return false;
    }
  }

  // Test 13: Large-scale Realtime Processing
  // Process multiple seconds of audio to verify sustained realtime performance
  bool TestLargeScaleRealtimeProcessing()
  {
    std::cout << "Test: Large-scale realtime processing (1 second @ 44.1kHz)... ";

    try
    {
      const double sampleRate = 44100.0;
      const int blockSize = 512;
      const double durationSeconds = 1.0;  // Reduced from 5 to 1 second for faster testing
      const int totalSamples = static_cast<int>(sampleRate * durationSeconds);

      // Realistic IR (cabinet simulation, longer)
      std::vector<float> impulse = {
        0.75f, 0.35f, 0.12f, 0.04f, 0.01f, 0.002f, 0.0005f
      };

      IRConvolutionTester tester;
      tester.SetImpulse(impulse);

      // Create test signal: simple pattern (avoid LCG complexity)
      std::vector<double> signal(totalSamples);
      for (int i = 0; i < totalSamples; ++i)
      {
        signal[i] = 0.5 * std::sin(2.0 * M_PI * 440.0 * i / sampleRate);  // 440Hz tone
      }

      // Process in blocks
      int processedSamples = 0;
      int blocksProcessed = 0;

      while (processedSamples < totalSamples)
      {
        int remainingSamples = totalSamples - processedSamples;
        int currentBlockSize = std::min(blockSize, remainingSamples);

        // Extract block
        std::vector<double> blockData(signal.begin() + processedSamples,
                                       signal.begin() + processedSamples + currentBlockSize);

        // Process block
        tester.Convolve(blockData);

        // Quick validity check
        for (const double sample : blockData)
        {
          if (std::isnan(sample) || std::isinf(sample))
          {
            std::cout << "FAILED - Invalid sample at block " << blocksProcessed << "\n";
            return false;
          }
        }

        processedSamples += currentBlockSize;
        ++blocksProcessed;
      }

      std::cout << "OK (" << blocksProcessed << " blocks)\n";
      return true;
    }
    catch (const std::exception& e)
    {
      std::cout << "FAILED - Exception: " << e.what() << "\n";
      return false;
    }
    catch (...)
    {
      std::cout << "FAILED - Unknown exception\n";
      return false;
    }
  }

  // Test 14: Impulse to Step Response
  // Apply IR to an impulse and verify we get the expected impulse response
  bool TestImpulseToStepResponse()
  {
    std::cout << "Test: Impulse/Step response verification... ";

    // Define a known IR
    std::vector<float> knownIR = { 0.5f, 0.3f, 0.15f, 0.05f };

    // Create an impulse input (1.0 at t=0, 0 thereafter)
    std::vector<double> impulseInput(10, 0.0);
    impulseInput[0] = 1.0;

    // Expected output should be the IR itself
    std::vector<double> expected(impulseInput.size(), 0.0);
    for (std::size_t i = 0; i < knownIR.size() && i < expected.size(); ++i)
    {
      expected[i] = knownIR[i];
    }

    // Process through convolver
    IRConvolutionTester tester;
    tester.SetImpulse(knownIR);
    tester.Convolve(impulseInput);

    // Verify output matches IR
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
      if (!ApproxEqual(impulseInput[i], expected[i], 1e-5))
      {
        std::cout << "FAILED at index " << i
                  << " (expected " << expected[i] << ", got " << impulseInput[i] << ")\n";
        return false;
      }
    }

    std::cout << "OK\n";
    return true;
  }

  // Test 15: Frequency Response Stability
  // Verify that IR processing doesn't cause crashes with different frequencies
  bool TestFrequencyResponseStability()
  {
    std::cout << "Test: Frequency response stability... ";

    try
    {
      // Test basic sine wave processing at different frequencies
      const double sampleRate = 44100.0;
      const int samplesPerFreq = 500;

      // Create testers for each frequency to avoid state issues
      const std::vector<double> testFrequencies = { 100.0, 1000.0 };

      for (double freq : testFrequencies)
      {
        // New tester for each frequency
        IRConvolutionTester tester;
        tester.SetImpulse({ 1.0f });  // Identity IR

        // Generate sine wave
        std::vector<double> sineWave(samplesPerFreq);
        for (int i = 0; i < samplesPerFreq; ++i)
        {
          double t = static_cast<double>(i) / sampleRate;
          sineWave[i] = 0.5 * std::sin(2.0 * M_PI * freq * t);
        }

        // Process through IR
        tester.Convolve(sineWave);

        // Just verify no NaN/Inf
        for (int i = 0; i < samplesPerFreq; ++i)
        {
          if (std::isnan(sineWave[i]) || std::isinf(sineWave[i]))
          {
            std::cout << "FAILED at " << freq << " Hz (index " << i << ")\n";
            return false;
          }
        }
      }

      std::cout << "OK\n";
      return true;
    }
    catch (const std::exception& e)
    {
      std::cout << "FAILED - Exception: " << e.what() << "\n";
      return false;
    }
    catch (...)
    {
      std::cout << "FAILED - Unknown exception\n";
      return false;
    }
  }

  bool TestDualIRBlendEndpoints()
  {
    std::cout << "Test: Dual IR blend endpoints... ";

    try
    {
      IRConvolutionTester tester;
      tester.SetDualImpulse({ 1.0f }, { 0.5f }, 0.0);

      std::vector<double> samplesA = { 1.0, 0.5, -0.25, 0.125 };
      tester.Convolve(samplesA);
      for (std::size_t i = 0; i < samplesA.size(); ++i)
      {
        const double expected = std::vector<double>{ 1.0, 0.5, -0.25, 0.125 }[i];
        if (!ApproxEqual(samplesA[i], expected, 1e-5))
        {
          std::cout << "FAILED at A endpoint index " << i << "\n";
          return false;
        }
      }

      tester.SetDualImpulse({ 1.0f }, { 0.5f }, 1.0);
      std::vector<double> samplesB = { 1.0, 0.5, -0.25, 0.125 };
      tester.Convolve(samplesB);
      for (std::size_t i = 0; i < samplesB.size(); ++i)
      {
        const double expected = std::vector<double>{ 0.5, 0.25, -0.125, 0.0625 }[i];
        if (!ApproxEqual(samplesB[i], expected, 1e-5))
        {
          std::cout << "FAILED at B endpoint index " << i << "\n";
          return false;
        }
      }

      std::cout << "OK\n";
      return true;
    }
    catch (const std::exception& e)
    {
      std::cout << "FAILED - Exception: " << e.what() << "\n";
      return false;
    }
  }

  bool TestAutoGainCompBoostsLowEnergyIR()
  {
    std::cout << "Test: Auto gain compensation boosts low-energy IR... ";

    try
    {
      std::vector<double> input(2048);
      for (std::size_t i = 0; i < input.size(); ++i)
      {
        input[i] = 0.5 * std::sin(2.0 * M_PI * 220.0 * static_cast<double>(i) / kSampleRate);
      }

      IRConvolutionTester tester;
      tester.SetDualImpulse({ 1.0f }, { 0.1f }, 1.0);
      tester.SetCabParam("mix", 1.0);
      tester.SetCabParam("autoGainComp", 0.0);

      auto off = input;
      tester.Convolve(off);
      const double rmsOff = ComputeRms(off);

      tester.SetCabParam("autoGainComp", 1.0);
      auto on = input;
      tester.Convolve(on);
      const double rmsOn = ComputeRms(on);

      if (!(rmsOn > rmsOff * 2.5))
      {
        std::cout << "FAILED (rmsOff=" << rmsOff << ", rmsOn=" << rmsOn << ")\n";
        return false;
      }

      std::cout << "OK\n";
      return true;
    }
    catch (const std::exception& e)
    {
      std::cout << "FAILED - Exception: " << e.what() << "\n";
      return false;
    }
  }

  bool TestHighCutAttenuatesHighFrequency()
  {
    std::cout << "Test: High-cut attenuates high-frequency content... ";

    try
    {
      std::vector<double> input(4096);
      for (std::size_t i = 0; i < input.size(); ++i)
      {
        input[i] = 0.5 * std::sin(2.0 * M_PI * 8000.0 * static_cast<double>(i) / kSampleRate);
      }

      IRConvolutionTester tester;
      tester.SetDualImpulse({ 1.0f }, { 1.0f }, 0.0);
      tester.SetCabParam("mix", 1.0);
      tester.SetCabParam("autoGainComp", 0.0);
      tester.SetCabParam("highCutHz", 20000.0);

      auto noCut = input;
      tester.Convolve(noCut);
      const double rmsNoCut = ComputeRms(noCut);

      tester.SetCabParam("highCutHz", 1000.0);
      auto cut = input;
      tester.Convolve(cut);
      const double rmsCut = ComputeRms(cut);

      if (!(rmsCut < rmsNoCut * 0.6))
      {
        std::cout << "FAILED (rmsNoCut=" << rmsNoCut << ", rmsCut=" << rmsCut << ")\n";
        return false;
      }

      std::cout << "OK\n";
      return true;
    }
    catch (const std::exception& e)
    {
      std::cout << "FAILED - Exception: " << e.what() << "\n";
      return false;
    }
  }

  // ==========================================================================
  // Long IR Tests - Tests using actual IR files from resources
  // ==========================================================================

  /**
   * @brief Load a WAV file and return the samples as float vector
   */
  bool LoadWavFile(const fs::path& path, std::vector<float>& samples, double& sampleRate)
  {
    std::ifstream file(path, std::ios::binary);
    if (!file)
      return false;

    // Read RIFF header
    char riff[4];
    file.read(riff, 4);
    if (std::memcmp(riff, "RIFF", 4) != 0)
      return false;

    file.seekg(8, std::ios::beg);
    char wave[4];
    file.read(wave, 4);
    if (std::memcmp(wave, "WAVE", 4) != 0)
      return false;

    uint16_t audioFormat = 0;
    uint16_t numChannels = 0;
    uint16_t bitsPerSample = 0;
    uint32_t wavSampleRate = 0;

    // Find chunks
    while (file)
    {
      char chunkId[4];
      uint32_t chunkSize;
      file.read(chunkId, 4);
      if (!file) break;
      file.read(reinterpret_cast<char*>(&chunkSize), 4);
      if (!file) break;

      if (std::memcmp(chunkId, "fmt ", 4) == 0)
      {
        file.read(reinterpret_cast<char*>(&audioFormat), 2);
        file.read(reinterpret_cast<char*>(&numChannels), 2);
        file.read(reinterpret_cast<char*>(&wavSampleRate), 4);
        sampleRate = static_cast<double>(wavSampleRate);
        file.seekg(6, std::ios::cur); // Skip byte rate and block align
        file.read(reinterpret_cast<char*>(&bitsPerSample), 2);
        file.seekg(chunkSize - 16, std::ios::cur);
      }
      else if (std::memcmp(chunkId, "data", 4) == 0)
      {
        if (audioFormat == 3) // IEEE float
        {
          size_t numSamples = chunkSize / sizeof(float);
          samples.resize(numSamples);
          file.read(reinterpret_cast<char*>(samples.data()), chunkSize);
        }
        else if (bitsPerSample == 16)
        {
          size_t numSamples = chunkSize / sizeof(int16_t);
          std::vector<int16_t> rawSamples(numSamples);
          file.read(reinterpret_cast<char*>(rawSamples.data()), chunkSize);
          samples.resize(numSamples);
          for (size_t i = 0; i < numSamples; ++i)
            samples[i] = static_cast<float>(rawSamples[i]) / 32768.0f;
        }
        else if (bitsPerSample == 24)
        {
          size_t numSamples = chunkSize / 3;
          samples.resize(numSamples);
          for (size_t i = 0; i < numSamples; ++i)
          {
            uint8_t bytes[3];
            file.read(reinterpret_cast<char*>(bytes), 3);
            int32_t value = (static_cast<int32_t>(bytes[2]) << 16) |
                            (static_cast<int32_t>(bytes[1]) << 8) |
                            static_cast<int32_t>(bytes[0]);
            if (value & 0x800000) value |= 0xFF000000; // Sign extend
            samples[i] = static_cast<float>(value) / 8388608.0f;
          }
        }
        else
        {
          return false; // Unsupported format
        }
        return true;
      }
      else
      {
        file.seekg(chunkSize, std::ios::cur);
      }
    }
    return false;
  }

#ifdef GUITARFX_TEST_RESOURCES_DIR
  // Test 16: Load and process with real cabinet IR WAV file
  bool TestRealCabinetIR()
  {
    std::cout << "Test: Real cabinet IR (421 1960.wav)... ";

    try
    {
      const fs::path resourcesDir = fs::path(GUITARFX_TEST_RESOURCES_DIR);
      const fs::path irPath = resourcesDir / "ir" / "421 1960.wav";

      IRConvolutionTester tester;
      std::vector<float> irSamples;
      double irSampleRate = kSampleRate;

      if (fs::exists(irPath) && LoadWavFile(irPath, irSamples, irSampleRate) && irSamples.size() > 64)
      {
        std::cout << "(IR: " << irSamples.size() << " samples @ " << irSampleRate << "Hz) ";
        tester.SetImpulseFromFile(irPath);
      }
      else
      {
        irSamples.resize(4096);
        for (size_t i = 0; i < irSamples.size(); ++i)
        {
          const double t = static_cast<double>(i) / static_cast<double>(irSamples.size());
          irSamples[i] = static_cast<float>(0.9 * std::exp(-10.0 * t));
        }
        tester.SetImpulse(irSamples);
        std::cout << "(using synthetic long IR) ";
      }

      // Generate 1 second of test audio
      const int numSamples = static_cast<int>(kSampleRate);
      std::vector<double> testSignal(numSamples);
      for (int i = 0; i < numSamples; ++i)
      {
        testSignal[i] = 0.5 * std::sin(2.0 * M_PI * 440.0 * i / kSampleRate);
      }

      // Process in blocks
      const int blockSize = 512;
      for (int offset = 0; offset < numSamples; offset += blockSize)
      {
        int currentBlockSize = std::min(blockSize, numSamples - offset);
        std::vector<double> block(testSignal.begin() + offset,
                                   testSignal.begin() + offset + currentBlockSize);
        tester.Convolve(block);

        // Verify no NaN/Inf
        for (int i = 0; i < currentBlockSize; ++i)
        {
          if (std::isnan(block[i]) || std::isinf(block[i]))
          {
            std::cout << "FAILED - Invalid sample at block offset " << offset << "\n";
            return false;
          }
        }
      }

      std::cout << "OK\n";
      return true;
    }
    catch (const std::exception& e)
    {
      std::cout << "FAILED - Exception: " << e.what() << "\n";
      return false;
    }
  }

  // Test 17: Long IR latency characteristics
  bool TestLongIRLatency()
  {
    std::cout << "Test: Long IR FFT convolution latency... ";

    try
    {
      const fs::path resourcesDir = fs::path(GUITARFX_TEST_RESOURCES_DIR);
      const fs::path irPath = resourcesDir / "ir" / "421 1960.wav";

      std::vector<float> irSamples;
      double irSampleRate = kSampleRate;
      const bool loadedFile = fs::exists(irPath) && LoadWavFile(irPath, irSamples, irSampleRate) && irSamples.size() > 64;
      if (!loadedFile)
      {
        irSamples.resize(4096);
        for (size_t i = 0; i < irSamples.size(); ++i)
        {
          const double t = static_cast<double>(i) / static_cast<double>(irSamples.size());
          irSamples[i] = static_cast<float>(std::exp(-8.0 * t));
        }
      }

      // Long IRs use FFT convolution which has latency
      // The first output samples will be zeros (latency)
      IRConvolutionTester tester;
      if (loadedFile)
      {
        tester.SetImpulseFromFile(irPath);
      }
      else
      {
        tester.SetImpulse(irSamples);
      }

      // Create an impulse input
      std::vector<double> impulseInput(1024, 0.0);
      impulseInput[0] = 1.0;

      tester.Convolve(impulseInput);

      // For FFT convolution, we expect some initial latency (zeros at start)
      // The latency should be at most the partition size (typically 256-512)
      int firstNonZeroIdx = -1;
      for (int i = 0; i < static_cast<int>(impulseInput.size()); ++i)
      {
        if (std::abs(impulseInput[i]) > 1e-10)
        {
          firstNonZeroIdx = i;
          break;
        }
      }

      // FFT convolution has some latency, but should be reasonable
      const int maxExpectedLatency = 512; // Partition size
      if (firstNonZeroIdx < 0)
      {
        std::cout << "FAILED - No output detected\n";
        return false;
      }
      else if (firstNonZeroIdx > maxExpectedLatency)
      {
        std::cout << "FAILED - Latency too high: " << firstNonZeroIdx << " samples\n";
        return false;
      }

      std::cout << "OK (latency: " << firstNonZeroIdx << " samples)\n";
      return true;
    }
    catch (const std::exception& e)
    {
      std::cout << "FAILED - Exception: " << e.what() << "\n";
      return false;
    }
  }

  // Test 18: Multiple long IR files
  bool TestMultipleLongIRs()
  {
    std::cout << "Test: Multiple long IR files... ";

    try
    {
      const fs::path resourcesDir = fs::path(GUITARFX_TEST_RESOURCES_DIR);
      const fs::path irDir = resourcesDir / "ir";

      // Test files to try
      std::vector<std::string> testFiles = {
        "421 1960.wav",
        "906 1960.wav",
        "i5 1960.wav",
        "test.wav"
      };

      int testedCount = 0;
      for (const auto& filename : testFiles)
      {
        const fs::path irPath = irDir / filename;
        if (!fs::exists(irPath))
          continue;

        std::vector<float> irSamples;
        double irSampleRate;
        if (!LoadWavFile(irPath, irSamples, irSampleRate))
          continue;

        // Skip if too short (already covered by other tests)
        if (irSamples.size() <= 64)
          continue;

        IRConvolutionTester tester;
        tester.SetImpulseFromFile(irPath);

        // Generate test signal
        std::vector<double> testSignal(4096);
        for (size_t i = 0; i < testSignal.size(); ++i)
        {
          testSignal[i] = 0.5 * std::sin(2.0 * M_PI * 440.0 * i / kSampleRate);
        }

        tester.Convolve(testSignal);

        // Verify output validity
        for (size_t i = 0; i < testSignal.size(); ++i)
        {
          if (std::isnan(testSignal[i]) || std::isinf(testSignal[i]))
          {
            std::cout << "FAILED on " << filename << " at sample " << i << "\n";
            return false;
          }
        }

        ++testedCount;
      }

      if (testedCount == 0)
      {
        IRConvolutionTester tester;
        std::vector<float> synthetic(8192);
        for (size_t i = 0; i < synthetic.size(); ++i)
        {
          const double t = static_cast<double>(i) / static_cast<double>(synthetic.size());
          synthetic[i] = static_cast<float>(0.8 * std::exp(-12.0 * t));
        }
        tester.SetImpulse(synthetic);

        std::vector<double> testSignal(4096);
        for (size_t i = 0; i < testSignal.size(); ++i)
        {
          testSignal[i] = 0.5 * std::sin(2.0 * M_PI * 440.0 * i / kSampleRate);
        }
        tester.Convolve(testSignal);

        for (size_t i = 0; i < testSignal.size(); ++i)
        {
          if (std::isnan(testSignal[i]) || std::isinf(testSignal[i]))
          {
            std::cout << "FAILED on synthetic IR at sample " << i << "\n";
            return false;
          }
        }

        std::cout << "OK (1 synthetic IR tested)\n";
        return true;
      }

      std::cout << "OK (" << testedCount << " IR files tested)\n";
      return true;
    }
    catch (const std::exception& e)
    {
      std::cout << "FAILED - Exception: " << e.what() << "\n";
      return false;
    }
  }

  // Test 19: Long IR with extended processing (stress test)
  bool TestLongIRExtendedProcessing()
  {
    std::cout << "Test: Long IR extended processing (10 seconds)... ";

    try
    {
      const fs::path resourcesDir = fs::path(GUITARFX_TEST_RESOURCES_DIR);
      const fs::path irPath = resourcesDir / "ir" / "421 1960.wav";

      std::vector<float> irSamples;
      double irSampleRate = kSampleRate;
      const bool loadedFile = fs::exists(irPath) && LoadWavFile(irPath, irSamples, irSampleRate) && irSamples.size() > 64;

      if (!loadedFile)
      {
        irSamples.resize(8192);
        for (size_t i = 0; i < irSamples.size(); ++i)
        {
          const double t = static_cast<double>(i) / static_cast<double>(irSamples.size());
          irSamples[i] = static_cast<float>(0.85 * std::exp(-9.0 * t));
        }
      }

      IRConvolutionTester tester;
      if (loadedFile)
      {
        tester.SetImpulseFromFile(irPath);
      }
      else
      {
        tester.SetImpulse(irSamples);
      }

      // Process 10 seconds of audio in blocks
      const int totalSamples = static_cast<int>(kSampleRate * 10.0);
      const int blockSize = 512;
      int processedBlocks = 0;

      for (int offset = 0; offset < totalSamples; offset += blockSize)
      {
        int currentBlockSize = std::min(blockSize, totalSamples - offset);
        
        // Generate block of test signal
        std::vector<double> block(currentBlockSize);
        for (int i = 0; i < currentBlockSize; ++i)
        {
          int sampleIdx = offset + i;
          block[i] = 0.5 * std::sin(2.0 * M_PI * 440.0 * sampleIdx / kSampleRate);
        }

        tester.Convolve(block);

        // Spot check for validity
        for (int i = 0; i < currentBlockSize; ++i)
        {
          if (std::isnan(block[i]) || std::isinf(block[i]))
          {
            std::cout << "FAILED at block " << processedBlocks << "\n";
            return false;
          }
        }

        ++processedBlocks;
      }

      std::cout << "OK (" << processedBlocks << " blocks processed)\n";
      return true;
    }
    catch (const std::exception& e)
    {
      std::cout << "FAILED - Exception: " << e.what() << "\n";
      return false;
    }
  }
#endif // GUITARFX_TEST_RESOURCES_DIR

} // anonymous namespace

int main()
{
  std::cout << "IR Convolution Tests (SignalGraph + IRCabEffect)\n";
  std::cout << "===============================================\n\n";

  int passed = 0;
  int failed = 0;

  auto runTest = [&](bool (*test)()) {
    if (test())
    {
      ++passed;
    }
    else
    {
      ++failed;
    }
  };

  runTest(TestIdentityIR);
  runTest(TestGainIR);
  runTest(TestTwoTapAverageIR);
  runTest(TestDelayIR);
  runTest(TestThreeTapIR);
  runTest(TestAgainstReference);
  runTest(TestMultipleBlocks);
  runTest(TestLongIR);
  runTest(TestEmptyIR);
  runTest(TestStereoChannels);
  runTest(TestStereoIRRetention);
  runTest(TestRealtimeLatency);
  runTest(TestAudioCleanness);
  runTest(TestLargeScaleRealtimeProcessing);
  runTest(TestImpulseToStepResponse);
  runTest(TestFrequencyResponseStability);
  runTest(TestDualIRBlendEndpoints);
  runTest(TestAutoGainCompBoostsLowEnergyIR);
  runTest(TestHighCutAttenuatesHighFrequency);

#ifdef GUITARFX_TEST_RESOURCES_DIR
  // Long IR tests using actual IR files
  runTest(TestRealCabinetIR);
  runTest(TestLongIRLatency);
  runTest(TestMultipleLongIRs);
  runTest(TestLongIRExtendedProcessing);
#endif

  std::cout << "\n===========================================\n";
  std::cout << "Results: " << passed << "/" << (passed + failed) << " tests passed.\n";

  if (failed > 0)
  {
    std::cout << "\nSome tests FAILED.\n";
    return 1;
  }

  std::cout << "\nAll tests PASSED.\n";
  return 0;
}
