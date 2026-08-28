#pragma once

#include <cmath>
#include <optional>
#include <string>
#include "json.hpp"

namespace guitarfx
{

inline constexpr double kDefaultNamModelSampleRate = 48000.0;

inline double ResolveNamModelProcessingSampleRate(double expectedSampleRate, double hostSampleRate)
{
    (void)hostSampleRate;
    return expectedSampleRate > 0.0 ? expectedSampleRate : kDefaultNamModelSampleRate;
}

inline bool NeedsNamRuntimeResampling(double modelProcessingSampleRate, double hostSampleRate)
{
    if (modelProcessingSampleRate <= 0.0 || hostSampleRate <= 0.0)
    {
        return true;
    }

    // NAM models and host processing are effectively integer-Hz domains.
    const long long modelHz = static_cast<long long>(std::llround(modelProcessingSampleRate));
    const long long hostHz = static_cast<long long>(std::llround(hostSampleRate));
    return modelHz != hostHz;
}

namespace nam
{

struct ModelMetadata
{
    std::string name;
    std::string author;
    std::optional<double> inputLevel;  // dBu for 0 dBFS
    std::optional<double> outputLevel; // dBu for 0 dBFS
    std::optional<double> loudness;    // dB
    double expectedSampleRate = -1.0;
};

inline std::optional<double> ReadMetadataDouble(const nlohmann::json& meta, const char* primaryKey,
                                                const char* fallbackKey = nullptr)
{
    const auto readValue = [&](const char* key) -> std::optional<double> {
        if (!key || !meta.contains(key))
        {
            return std::nullopt;
        }

        const auto& value = meta[key];
        if (value.is_number())
        {
            return value.get<double>();
        }

        if (value.is_string())
        {
            try
            {
                std::size_t parsedLength = 0;
                const std::string text = value.get<std::string>();
                const double parsed = std::stod(text, &parsedLength);
                if (parsedLength == text.size())
                {
                    return parsed;
                }
            }
            catch (...)
            {
            }
        }

        return std::nullopt;
    };

    if (auto parsed = readValue(primaryKey))
    {
        return parsed;
    }

    return readValue(fallbackKey);
}

inline std::optional<std::string> ReadMetadataString(const nlohmann::json& meta, const char* primaryKey,
                                                     const char* fallbackKey = nullptr)
{
    const auto readValue = [&](const char* key) -> std::optional<std::string> {
        if (!key || !meta.contains(key) || !meta[key].is_string())
        {
            return std::nullopt;
        }

        const std::string value = meta[key].get<std::string>();
        if (value.empty())
        {
            return std::nullopt;
        }
        return value;
    };

    if (auto parsed = readValue(primaryKey))
    {
        return parsed;
    }

    return readValue(fallbackKey);
}

inline double ReadExpectedSampleRateFromNamJson(const nlohmann::json& j)
{
    if (j.contains("metadata") && j["metadata"].is_object())
    {
        if (auto sr = ReadMetadataDouble(j["metadata"], "expected_sample_rate", "sample_rate"))
        {
            return *sr;
        }
    }

    if (auto sr = ReadMetadataDouble(j, "expected_sample_rate", "sample_rate"))
    {
        return *sr;
    }

    if (j.contains("config") && j["config"].is_object())
    {
        if (auto sr = ReadMetadataDouble(j["config"], "sample_rate"))
        {
            return *sr;
        }
    }

    return -1.0;
}

} // namespace nam

} // namespace guitarfx