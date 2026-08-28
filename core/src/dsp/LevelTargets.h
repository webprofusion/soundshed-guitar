#pragma once

#include <atomic>
#include <cstdint>
#include <cmath>

namespace guitarfx
{
// Nominal operating point for level-sensitive DSP stages.
inline constexpr double kDefaultNominalOperatingLevelDbfs = -18.0;
inline constexpr double kMinNominalOperatingLevelDbfs = -30.0;
inline constexpr double kMaxNominalOperatingLevelDbfs = -6.0;

// Final output protection target used by the mixer ceiling/limiter.
inline constexpr double kDefaultOutputProtectionCeilingDbfs = -1.0;
inline constexpr double kMinOutputProtectionCeilingDbfs = -6.0;
inline constexpr double kMaxOutputProtectionCeilingDbfs = 0.0;

inline double DbToLinearGain(double db)
{
    return std::pow(10.0, db / 20.0);
}

inline double SanitizeNominalOperatingLevelDbfs(double value)
{
    if (!std::isfinite(value))
    {
        return kDefaultNominalOperatingLevelDbfs;
    }

    return std::clamp(value, kMinNominalOperatingLevelDbfs, kMaxNominalOperatingLevelDbfs);
}

inline double SanitizeOutputProtectionCeilingDbfs(double value)
{
    if (!std::isfinite(value))
    {
        return kDefaultOutputProtectionCeilingDbfs;
    }

    return std::clamp(value, kMinOutputProtectionCeilingDbfs, kMaxOutputProtectionCeilingDbfs);
}

inline std::atomic<double>& NominalOperatingLevelStorage()
{
    static std::atomic<double> value{kDefaultNominalOperatingLevelDbfs};
    return value;
}

inline std::atomic<double>& OutputProtectionCeilingStorage()
{
    static std::atomic<double> value{kDefaultOutputProtectionCeilingDbfs};
    return value;
}

inline std::atomic<std::uint64_t>& LevelTargetsRevisionStorage()
{
    static std::atomic<std::uint64_t> revision{1};
    return revision;
}

inline double GetNominalOperatingLevelDbfs()
{
    return NominalOperatingLevelStorage().load(std::memory_order_relaxed);
}

inline void SetNominalOperatingLevelDbfs(double value)
{
    const double sanitized = SanitizeNominalOperatingLevelDbfs(value);
    const double previous = NominalOperatingLevelStorage().exchange(sanitized, std::memory_order_relaxed);

    if (previous != sanitized)
    {
        LevelTargetsRevisionStorage().fetch_add(1, std::memory_order_relaxed);
    }
}

inline double GetOutputProtectionCeilingDbfs()
{
    return OutputProtectionCeilingStorage().load(std::memory_order_relaxed);
}

inline void SetOutputProtectionCeilingDbfs(double value)
{
    const double sanitized = SanitizeOutputProtectionCeilingDbfs(value);
    const double previous = OutputProtectionCeilingStorage().exchange(sanitized, std::memory_order_relaxed);

    if (previous != sanitized)
    {
        LevelTargetsRevisionStorage().fetch_add(1, std::memory_order_relaxed);
    }
}

inline double GetOutputProtectionCeilingLinear()
{
    return DbToLinearGain(GetOutputProtectionCeilingDbfs());
}

inline std::uint64_t GetLevelTargetsRevision()
{
    return LevelTargetsRevisionStorage().load(std::memory_order_relaxed);
}
} // namespace guitarfx