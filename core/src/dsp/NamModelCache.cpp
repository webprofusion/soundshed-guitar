#include "dsp/NamModelCache.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"

namespace guitarfx::nammodelcache
{
namespace
{
// Parsed model data plus enough file identity to detect an edited or replaced file.
struct Entry
{
    std::shared_ptr<const ::nam::dspData> data;
    std::uintmax_t fileSize = 0;
    std::int64_t writeTime = 0;
    std::size_t bytes = 0;
};

struct CacheState
{
    std::mutex mutex;
    std::unordered_map<std::string, Entry> entries;
    std::list<std::string> lru; // Front = most recently used.
    std::unordered_map<std::string, std::list<std::string>::iterator> lruPositions;
    std::size_t bytes = 0;
    std::size_t budgetBytes = 512u * 1024u * 1024u;
    std::size_t hits = 0;
    std::size_t misses = 0;
    std::size_t evictions = 0;
};

CacheState& State()
{
    static CacheState state;
    return state;
}

std::size_t EstimateBytes(const ::nam::dspData& data)
{
    // Weights dominate; the JSON blobs are small by comparison but are counted so a
    // pathological metadata payload cannot slip past the budget entirely.
    return data.weights.size() * sizeof(float) + data.version.size() + data.architecture.size() +
           data.config.dump().size() + data.metadata.dump().size();
}

/// Canonical path where possible, so two spellings of one file share an entry.
std::string MakeKey(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(path, ec);
    const auto& resolved = ec ? path : canonical;
    // u8string avoids the code-page conversion that string() performs, which throws
    // on model names outside the active ANSI code page.
    const auto utf8 = resolved.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

bool StatFile(const std::filesystem::path& path, std::uintmax_t& outSize, std::int64_t& outWriteTime)
{
    std::error_code ec;
    outSize = std::filesystem::file_size(path, ec);

    if (ec)
    {
        return false;
    }

    const auto writeTime = std::filesystem::last_write_time(path, ec);

    if (ec)
    {
        return false;
    }

    outWriteTime = writeTime.time_since_epoch().count();
    return true;
}

// Caller must hold the lock.
void TouchLocked(CacheState& state, const std::string& key)
{
    auto positionIt = state.lruPositions.find(key);

    if (positionIt != state.lruPositions.end())
    {
        state.lru.erase(positionIt->second);
    }

    state.lru.push_front(key);
    state.lruPositions[key] = state.lru.begin();
}

// Caller must hold the lock.
void EraseLocked(CacheState& state, const std::string& key)
{
    auto entryIt = state.entries.find(key);

    if (entryIt == state.entries.end())
    {
        return;
    }

    state.bytes -= std::min(state.bytes, entryIt->second.bytes);
    state.entries.erase(entryIt);

    auto positionIt = state.lruPositions.find(key);

    if (positionIt != state.lruPositions.end())
    {
        state.lru.erase(positionIt->second);
        state.lruPositions.erase(positionIt);
    }
}

// Caller must hold the lock. Entries still referenced by a live model are only
// dropped from the cache; the shared_ptr keeps the data alive until that model
// finishes constructing, so eviction can never free data in use.
void EvictToBudgetLocked(CacheState& state)
{
    while (state.bytes > state.budgetBytes && !state.lru.empty())
    {
        const std::string victim = state.lru.back();
        EraseLocked(state, victim);
        ++state.evictions;
    }
}
} // namespace

std::unique_ptr<::nam::DSP> GetModel(const std::filesystem::path& path)
{
    std::uintmax_t fileSize = 0;
    std::int64_t writeTime = 0;

    if (!StatFile(path, fileSize, writeTime))
    {
        return nullptr;
    }

    const std::string key = MakeKey(path);
    auto& state = State();

    std::shared_ptr<const ::nam::dspData> cached;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        auto it = state.entries.find(key);

        if (it != state.entries.end())
        {
            if (it->second.fileSize == fileSize && it->second.writeTime == writeTime)
            {
                cached = it->second.data;
                TouchLocked(state, key);
                ++state.hits;
            }
            else
            {
                // File changed on disk since it was cached.
                EraseLocked(state, key);
            }
        }

        if (!cached)
        {
            ++state.misses;
        }
    }

    try
    {
        if (cached)
        {
            // get_dsp takes a non-const reference and may consume the data, so the cached
            // copy is never handed out directly.
            ::nam::dspData copy = *cached;
            return ::nam::get_dsp(copy);
        }

        ::nam::dspData parsed;
        auto model = ::nam::get_dsp(path, parsed);

        if (!model)
        {
            return nullptr;
        }

        const std::size_t bytes = EstimateBytes(parsed);
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            // A concurrent load of the same path may have inserted already; replacing is
            // harmless and keeps the newest stat data.
            EraseLocked(state, key);

            Entry entry;
            entry.data = std::make_shared<const ::nam::dspData>(std::move(parsed));
            entry.fileSize = fileSize;
            entry.writeTime = writeTime;
            entry.bytes = bytes;

            state.bytes += bytes;
            state.entries.emplace(key, std::move(entry));
            TouchLocked(state, key);
            EvictToBudgetLocked(state);
        }

        return model;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[NamModelCache] ERROR: failed to load model: " << e.what() << "\n";
        return nullptr;
    }
    catch (...)
    {
        std::cerr << "[NamModelCache] ERROR: unknown failure loading model\n";
        return nullptr;
    }
}

void SetBudgetBytes(std::size_t bytes)
{
    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.budgetBytes = bytes;
    EvictToBudgetLocked(state);
}

std::size_t GetBudgetBytes()
{
    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.budgetBytes;
}

void Clear()
{
    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.entries.clear();
    state.lru.clear();
    state.lruPositions.clear();
    state.bytes = 0;
}

Stats GetStats()
{
    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    Stats stats;
    stats.entries = state.entries.size();
    stats.bytes = state.bytes;
    stats.hits = state.hits;
    stats.misses = state.misses;
    stats.evictions = state.evictions;
    return stats;
}

void ResetStats()
{
    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.hits = 0;
    state.misses = 0;
    state.evictions = 0;
}
} // namespace guitarfx::nammodelcache
