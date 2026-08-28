#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>

namespace nam
{
class DSP;
}

namespace guitarfx
{
/**
 * Process-wide cache of parsed NAM model data, keyed by file identity.
 *
 * Loading a .nam file is dominated by reading it and parsing the weights blob out of
 * JSON — roughly 5-12 ms for a typical model, and every NAM node pays it twice (left and
 * right channel). That cost sits directly on the preset-switch critical path.
 *
 * nam::dspData is a plain copyable struct, and nam::get_dsp() has an overload that builds
 * a model from one. So the parse result is cached and each construction becomes a struct
 * copy plus model instantiation, which measures ~0.1-1.6 ms — a 80-98% saving per model.
 *
 * Entries are keyed by canonical path + file size + last-write time, so editing or
 * replacing a model file invalidates its entry. Eviction is LRU against a byte budget.
 *
 * Thread-safe: loads are dispatched concurrently by SignalGraphExecutor::CreateProcessors().
 * The file parse itself runs outside the lock, so a cold load never blocks other threads;
 * two threads racing on the same uncached path may both parse it, and the second insert
 * simply wins. That is rare and strictly cheaper than serialising every load.
 */
namespace nammodelcache
{
/**
 * Construct a NAM model for `path`, reusing cached parse output when available.
 * Returns nullptr if the file cannot be read or parsed. Propagates no exceptions.
 */
std::unique_ptr<::nam::DSP> GetModel(const std::filesystem::path& path);

/// Maximum bytes of parsed model data to retain. Setting a lower budget evicts immediately.
void SetBudgetBytes(std::size_t bytes);
[[nodiscard]] std::size_t GetBudgetBytes();

/// Drop every entry. Intended for tests and for reclaiming memory on demand.
void Clear();

struct Stats
{
    std::size_t entries = 0;
    std::size_t bytes = 0;
    std::size_t hits = 0;
    std::size_t misses = 0;
    std::size_t evictions = 0;
};

[[nodiscard]] Stats GetStats();
void ResetStats();
} // namespace nammodelcache
} // namespace guitarfx
