#pragma once

/**
 * Benchmark utilities for comparing NAM processing implementations.
 * Use these to measure performance differences between optimized and fallback paths.
 */

#include "dsp/simd/SimdMath.h"
#include "dsp/simd/OptimizedActivations.h"
#include <chrono>
#include <vector>
#include <string>
#include <iostream>
#include <iomanip>
#include <cmath>
#include <random>

namespace guitarfx
{
namespace benchmark
{

struct BenchmarkResult
{
  std::string name;
  double durationMs;
  double samplesPerSecond;
  long long iterations;
};

template<typename Func>
BenchmarkResult RunBenchmark(const std::string& name, Func&& func, long long iterations)
{
  // Warmup
  for (int i = 0; i < 10; ++i)
    func();

  auto start = std::chrono::high_resolution_clock::now();

  for (long long i = 0; i < iterations; ++i)
    func();

  auto end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

  double durationMs = static_cast<double>(duration.count()) / 1000.0;

  return { name, durationMs, 0.0, iterations };
}

inline void PrintResult(const BenchmarkResult& result)
{
  std::cout << std::left << std::setw(40) << result.name
            << std::right << std::setw(10) << std::fixed << std::setprecision(3)
            << result.durationMs << " ms"
            << " (" << result.iterations << " iterations)"
            << std::endl;
}

inline void PrintSpeedup(const BenchmarkResult& baseline, const BenchmarkResult& optimized)
{
  double speedup = baseline.durationMs / optimized.durationMs;
  std::cout << "  Speedup: " << std::fixed << std::setprecision(2)
            << speedup << "x"
            << std::endl;
}

// ============================================================================
// Activation Benchmarks
// ============================================================================

inline void BenchmarkActivations()
{
  const long bufferSize = 4096;
  const long long iterations = 10000;

  std::vector<float> data(bufferSize);
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_real_distribution<float> dist(-3.0f, 3.0f);

  // Initialize with random data
  for (auto& v : data)
    v = dist(gen);

  std::vector<float> dataBackup = data;

  std::cout << "\n=== Activation Function Benchmarks ===" << std::endl;
  std::cout << "Buffer size: " << bufferSize << ", Iterations: " << iterations << std::endl;
  std::cout << "SIMD Level: ";
  switch (simd::GetSimdLevel())
  {
    case simd::SimdLevel::AVX2: std::cout << "AVX2"; break;
    case simd::SimdLevel::AVX: std::cout << "AVX"; break;
    case simd::SimdLevel::SSE4: std::cout << "SSE4"; break;
    case simd::SimdLevel::SSE2: std::cout << "SSE2"; break;
    default: std::cout << "Scalar"; break;
  }
  std::cout << std::endl << std::endl;

  // Scalar Tanh
  auto scalarTanh = RunBenchmark("Scalar Tanh", [&]() {
    data = dataBackup;  // Reset
    for (long i = 0; i < bufferSize; ++i)
      data[i] = std::tanh(data[i]);
  }, iterations);
  PrintResult(scalarTanh);

  // Fast Scalar Tanh
  auto fastScalarTanh = RunBenchmark("Fast Scalar Tanh", [&]() {
    data = dataBackup;
    simd::scalar::ApplyTanh(data.data(), bufferSize);
  }, iterations);
  PrintResult(fastScalarTanh);
  PrintSpeedup(scalarTanh, fastScalarTanh);

  // SIMD Tanh (auto-dispatched)
  auto simdTanh = RunBenchmark("SIMD Tanh (auto)", [&]() {
    data = dataBackup;
    simd::ApplyTanh(data.data(), bufferSize);
  }, iterations);
  PrintResult(simdTanh);
  PrintSpeedup(scalarTanh, simdTanh);

  std::cout << std::endl;

  // Scalar Sigmoid
  auto scalarSigmoid = RunBenchmark("Scalar Sigmoid", [&]() {
    data = dataBackup;
    for (long i = 0; i < bufferSize; ++i)
      data[i] = 1.0f / (1.0f + std::exp(-data[i]));
  }, iterations);
  PrintResult(scalarSigmoid);

  // SIMD Sigmoid
  auto simdSigmoid = RunBenchmark("SIMD Sigmoid (auto)", [&]() {
    data = dataBackup;
    simd::ApplySigmoid(data.data(), bufferSize);
  }, iterations);
  PrintResult(simdSigmoid);
  PrintSpeedup(scalarSigmoid, simdSigmoid);

  std::cout << std::endl;

  // Gated activation benchmark
  std::vector<float> gate(bufferSize);
  for (auto& v : gate)
    v = dist(gen);
  std::vector<float> gateBackup = gate;

  auto scalarGated = RunBenchmark("Scalar Gated (tanh*sigmoid)", [&]() {
    data = dataBackup;
    gate = gateBackup;
    for (long i = 0; i < bufferSize; ++i)
      data[i] = std::tanh(data[i]) * (1.0f / (1.0f + std::exp(-gate[i])));
  }, iterations);
  PrintResult(scalarGated);

  auto simdGated = RunBenchmark("SIMD Fused Gated (auto)", [&]() {
    data = dataBackup;
    gate = gateBackup;
    simd::ApplyGatedActivation(data.data(), gate.data(), bufferSize);
  }, iterations);
  PrintResult(simdGated);
  PrintSpeedup(scalarGated, simdGated);

  std::cout << std::endl;
}

// ============================================================================
// Matrix Operations Benchmark (simulating WaveNet layer)
// ============================================================================

inline void BenchmarkMatrixOps()
{
  const int channels = 32;
  const int numFrames = 512;
  const long long iterations = 1000;

  std::cout << "\n=== Matrix Operations Benchmark ===" << std::endl;
  std::cout << "Channels: " << channels << ", Frames: " << numFrames << std::endl;
  std::cout << "Iterations: " << iterations << std::endl << std::endl;

  // Simulate a WaveNet layer's activation step
  Eigen::MatrixXf z(2 * channels, numFrames);
  z.setRandom();
  Eigen::MatrixXf zBackup = z;

  // Column-by-column (original NAM approach)
  auto columnWise = RunBenchmark("Column-wise activation (original)", [&]() {
    z = zBackup;
    for (int i = 0; i < numFrames; ++i)
    {
      // Apply tanh to first half
      for (int c = 0; c < channels; ++c)
        z(c, i) = std::tanh(z(c, i));
      // Apply sigmoid to second half
      for (int c = 0; c < channels; ++c)
        z(channels + c, i) = 1.0f / (1.0f + std::exp(-z(channels + c, i)));
      // Multiply
      for (int c = 0; c < channels; ++c)
        z(c, i) *= z(channels + c, i);
    }
  }, iterations);
  PrintResult(columnWise);

  // Optimized: process each frame with SIMD
  auto simdOptimized = RunBenchmark("SIMD per-frame gated (optimized)", [&]() {
    z = zBackup;
    for (int frame = 0; frame < numFrames; ++frame)
    {
      float* activationData = z.data() + frame * z.rows();
      float* gateData = activationData + channels;
      simd::ApplyGatedActivation(activationData, gateData, channels);
    }
  }, iterations);
  PrintResult(simdOptimized);
  PrintSpeedup(columnWise, simdOptimized);

  std::cout << std::endl;
}

inline void RunAllBenchmarks()
{
  std::cout << "======================================" << std::endl;
  std::cout << "  GuitarFX NAM Optimization Benchmarks" << std::endl;
  std::cout << "======================================" << std::endl;

  BenchmarkActivations();
  BenchmarkMatrixOps();

  std::cout << "======================================" << std::endl;
  std::cout << "  Benchmarks Complete" << std::endl;
  std::cout << "======================================" << std::endl;
}

} // namespace benchmark
} // namespace guitarfx
