// Coverage for the NAM model cache that sits on the preset-switch critical path.
//
// The cache must be transparent: a model built from a cache hit has to behave identically
// to one parsed from disk, and a model file edited underneath the cache must not keep
// serving stale weights.
//
// See docs/plans/gapless-preset-switching.md and dsp/NamModelCache.h.

#include "dsp/NamModelCache.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "dsp/effects/NAMOversampling.h"
#include "dsp/effects/NAMSampleRate.h"

namespace fs = std::filesystem;
namespace cache = guitarfx::nammodelcache;

namespace nam::factory { void ForceFactoryRegistration(); }

namespace
{
  bool gAllPassed = true;

  void Check(bool condition, const std::string &what)
  {
    if (!condition)
    {
      std::cerr << "FAIL: " << what << std::endl;
      gAllPassed = false;
    }
    else
    {
      std::cout << "  ok: " << what << std::endl;
    }
  }

  /// First .nam file under the test asset tree that this build can actually parse.
  /// The asset set deliberately spans several architectures and slimmable variants.
  fs::path FindLoadableModel()
  {
    const fs::path root = fs::path(GUITARFX_TEST_RESOURCES_DIR) / "assets" / "amps";
    if (!fs::exists(root))
      return {};

    std::vector<fs::path> candidates;
    for (const auto &entry : fs::recursive_directory_iterator(root))
    {
      if (entry.is_regular_file() && entry.path().extension() == ".nam")
        candidates.push_back(entry.path());
    }
    std::sort(candidates.begin(), candidates.end());

    for (const auto &candidate : candidates)
    {
      try
      {
        if (auto model = ::nam::get_dsp(candidate))
          return candidate;
      }
      catch (const std::exception &)
      {
        // Architecture not compiled into this build; try the next file.
      }
    }
    return {};
  }

  /// Run a short impulse-plus-tone burst through a model and capture the output, so two
  /// models can be compared sample-for-sample.
  std::vector<float> Render(::nam::DSP &model, double sampleRate, int blockSize, int blocks)
  {
    model.Reset(sampleRate, blockSize);

    std::vector<NAM_SAMPLE> input(static_cast<size_t>(blockSize));
    std::vector<NAM_SAMPLE> output(static_cast<size_t>(blockSize));
    NAM_SAMPLE *inputChannels[1] = {input.data()};
    NAM_SAMPLE *outputChannels[1] = {output.data()};
    std::vector<float> captured;
    captured.reserve(static_cast<size_t>(blockSize) * blocks);

    int sampleIndex = 0;
    for (int b = 0; b < blocks; ++b)
    {
      for (int i = 0; i < blockSize; ++i, ++sampleIndex)
      {
        const double t = static_cast<double>(sampleIndex) / sampleRate;
        input[static_cast<size_t>(i)] =
          static_cast<NAM_SAMPLE>(0.25 * std::sin(2.0 * 3.14159265358979323846 * 220.0 * t));
      }
      model.process(inputChannels, outputChannels, blockSize);
      for (int i = 0; i < blockSize; ++i)
        captured.push_back(static_cast<float>(output[static_cast<size_t>(i)]));
    }
    return captured;
  }

  void TestCacheHitProducesIdenticalOutput(const fs::path &modelPath)
  {
    std::cout << "\n[cache hit is bit-identical]\n";
    cache::Clear();
    cache::ResetStats();

    auto first = cache::GetModel(modelPath);   // miss: parses the file
    auto second = cache::GetModel(modelPath);  // hit: builds from cached dspData
    Check(first != nullptr, "first load returns a model");
    Check(second != nullptr, "second load returns a model");
    if (!first || !second)
      return;

    const auto statsAfter = cache::GetStats();
    Check(statsAfter.misses == 1, "first load is a miss");
    Check(statsAfter.hits == 1, "second load is a hit");
    Check(statsAfter.entries == 1, "one entry cached");
    Check(statsAfter.bytes > 0, "cached entry reports a nonzero size");

    const auto a = Render(*first, 48000.0, 256, 8);
    const auto b = Render(*second, 48000.0, 256, 8);
    Check(a.size() == b.size(), "both renders are the same length");
    Check(a == b, "a cache hit renders bit-identically to a fresh parse");

    // And identical to the uncached library path, so the cache introduces no drift.
    auto direct = ::nam::get_dsp(modelPath);
    Check(direct != nullptr, "direct get_dsp still works");
    if (direct)
    {
      const auto c = Render(*direct, 48000.0, 256, 8);
      Check(a == c, "cached model matches an uncached get_dsp(path) model");
    }
  }

  void TestEditedFileInvalidates(const fs::path &modelPath)
  {
    std::cout << "\n[edited file invalidates its entry]\n";
    cache::Clear();
    cache::ResetStats();

    // Work on a copy so the shared asset tree is never modified.
    const fs::path temp = fs::temp_directory_path() / "soundshed-nam-cache-test.nam";
    std::error_code ec;
    fs::remove(temp, ec);
    fs::copy_file(modelPath, temp, fs::copy_options::overwrite_existing, ec);
    Check(!ec, "copied model to a temp path");
    if (ec)
      return;

    auto first = cache::GetModel(temp);
    Check(first != nullptr, "temp copy loads");
    Check(cache::GetStats().misses == 1, "temp copy is a miss");

    auto second = cache::GetModel(temp);
    Check(second != nullptr, "temp copy loads again");
    Check(cache::GetStats().hits == 1, "unchanged temp copy is a hit");

    // Rewrite it with different content and a newer timestamp.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    fs::copy_file(modelPath, temp, fs::copy_options::overwrite_existing, ec);
    fs::last_write_time(temp, fs::file_time_type::clock::now() + std::chrono::seconds(5), ec);

    const auto before = cache::GetStats();
    auto third = cache::GetModel(temp);
    const auto after = cache::GetStats();
    Check(third != nullptr, "rewritten file still loads");
    Check(after.misses == before.misses + 1, "a changed write time forces a re-parse");
    Check(after.hits == before.hits, "a changed write time is not served from cache");

    fs::remove(temp, ec);
  }

  void TestMissingFileIsHandled()
  {
    std::cout << "\n[missing file]\n";
    const fs::path missing = fs::temp_directory_path() / "soundshed-nam-cache-does-not-exist.nam";
    std::error_code ec;
    fs::remove(missing, ec);
    Check(cache::GetModel(missing) == nullptr, "a missing model returns nullptr rather than throwing");
  }

  void TestBudgetEvicts(const fs::path &modelPath)
  {
    std::cout << "\n[budget eviction]\n";
    cache::Clear();
    cache::ResetStats();

    const std::size_t originalBudget = cache::GetBudgetBytes();

    auto model = cache::GetModel(modelPath);
    Check(model != nullptr, "model loads before the budget is tightened");
    Check(cache::GetStats().entries == 1, "entry is cached under the default budget");

    // A model still under construction holds the cached data alive through its shared_ptr,
    // so dropping the entry is safe even while a live model references that parse.
    cache::SetBudgetBytes(1);
    const auto evicted = cache::GetStats();
    Check(evicted.entries == 0, "tightening the budget evicts");
    Check(evicted.bytes == 0, "evicted bytes are accounted for");
    Check(evicted.evictions > 0, "eviction is counted");

    auto stillUsable = Render(*model, 48000.0, 256, 4);
    Check(!stillUsable.empty(), "a model built before eviction still renders");

    cache::SetBudgetBytes(originalBudget);
    Check(cache::GetBudgetBytes() == originalBudget, "budget restores");
  }

  void TestOversampledRendering(const fs::path &modelPath)
  {
    std::cout << "\n[time-scaled oversampled rendering]\n";
    auto model = cache::GetModel(modelPath);
    Check(model != nullptr, "model loads for oversampling");
    if (!model)
      return;

    constexpr double fallbackSampleRate = 48000.0;
    constexpr int blockSize = 64;
    const double modelSampleRate = guitarfx::ResolveNamModelProcessingSampleRate(
      model->GetExpectedSampleRate(), fallbackSampleRate);
    const double hostSampleRate = modelSampleRate;

    guitarfx::NamOversamplingProcessor oversampling;
    oversampling.Prepare(
      *model,
      hostSampleRate,
      modelSampleRate,
      blockSize,
      2,
      dsp::EAntiAliasFilterPhase::MinimumPhaseCascadedFIR);

    Check(oversampling.GetTimeScale() >= 2, "2x request applies a time-scaled NAM rendering rate");
    Check(oversampling.GetRenderingSampleRate() > modelSampleRate, "rendering rate exceeds the model rate");

    std::vector<NAM_SAMPLE> input(blockSize);
    std::vector<NAM_SAMPLE> output(blockSize);
    bool finite = true;
    int sampleIndex = 0;
    for (int block = 0; block < 4; ++block)
    {
      for (int i = 0; i < blockSize; ++i, ++sampleIndex)
      {
        input[static_cast<std::size_t>(i)] = static_cast<NAM_SAMPLE>(
          0.1 * std::sin(2.0 * 3.14159265358979323846 * 220.0 * sampleIndex / hostSampleRate));
      }
      oversampling.Process(*model, input.data(), output.data(), blockSize);
      finite = finite && std::all_of(output.begin(), output.end(), [](NAM_SAMPLE sample)
      {
        return std::isfinite(static_cast<double>(sample));
      });
    }
    Check(finite, "oversampled real-model output remains finite");
  }
} // namespace

int main()
{
  nam::factory::ForceFactoryRegistration();

  const fs::path modelPath = FindLoadableModel();
  if (modelPath.empty())
  {
    std::cout << "NamModelCacheTests skipped: no loadable .nam asset found" << std::endl;
    return 0;
  }

  TestCacheHitProducesIdenticalOutput(modelPath);
  TestEditedFileInvalidates(modelPath);
  TestMissingFileIsHandled();
  TestBudgetEvicts(modelPath);
  TestOversampledRendering(modelPath);

  if (gAllPassed)
    std::cout << "\nNamModelCacheTests passed" << std::endl;
  return gAllPassed ? 0 : 1;
}
