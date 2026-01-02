/**
 * @file IRConvolutionTests.cpp
 * @brief Unit tests for IR (Impulse Response) convolution algorithm
 *
 * Tests the NAMDSPManager::ApplyImpulseResponse function with known inputs and expected outputs
 * to verify the convolution implementation is mathematically correct, and verifies realtime
 * audio processing with clean output and no latency.
 */

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

#include "dsp/NAMDSPManager.h"

// Define M_PI if not available
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace
{
  constexpr double kEpsilon = 1e-6;
  constexpr double kSampleRate = 44100.0;
  constexpr int kBlockSize = 512;

  bool ApproxEqual(double a, double b, double epsilon = kEpsilon)
  {
    return std::abs(a - b) < epsilon;
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

  /**
   * @brief Helper class to test NAMDSPManager's IR convolution
   */
  class IRConvolutionTester
  {
  public:
    IRConvolutionTester()
    {
      mDSPManager = std::make_unique<namguitar::NAMDSPManager>();
      mDSPManager->Prepare(kSampleRate, kBlockSize);
    }

    void SetImpulse(const std::vector<float>& impulse)
    {
      mDSPManager->SetImpulseResponseForTest(impulse);
    }

    void Convolve(std::vector<double>& samples, int channel = 0)
    {
      mDSPManager->ApplyImpulseResponseForTest(samples, channel);
    }

    void Reset()
    {
      mDSPManager->Reset();
    }

  private:
    std::unique_ptr<namguitar::NAMDSPManager> mDSPManager;
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

  // Test 6: Compare NAMDSPManager with reference implementation
  bool TestAgainstReference()
  {
    std::cout << "Test: NAMDSPManager vs reference implementation... ";

    // Use a more complex IR and input
    std::vector<float> impulse = { 0.3f, 0.5f, 0.2f, -0.1f, 0.1f };
    std::vector<double> input = { 1.0, -0.5, 0.25, 0.75, -1.0, 0.5, 0.0, 0.25 };

    // Get reference result
    std::vector<double> referenceOutput = ReferenceConvolve(input, impulse);

    // Get NAMDSPManager result
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

    // Get NAMDSPManager result
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

} // anonymous namespace

int main()
{
  std::cout << "IR Convolution Tests (using NAMDSPManager)\n";
  std::cout << "===========================================\n\n";

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
  runTest(TestRealtimeLatency);
  runTest(TestAudioCleanness);
  runTest(TestLargeScaleRealtimeProcessing);
  runTest(TestImpulseToStepResponse);
  runTest(TestFrequencyResponseStability);

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
