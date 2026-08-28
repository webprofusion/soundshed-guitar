#include "controller/internal/NamResourceMetadata.h"

#include "dsp/EffectGuids.h"
#include "resources/ResourceLibrary.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace guitarfx::controller_detail
{
bool IsNamEffectType(const std::string& type)
{
    return type == guitarfx::EffectGuids::kAmpNam || type == "amp_nam" ||
           type == guitarfx::EffectGuids::kAmpNamOptimized || type == "amp_nam_optimized" ||
           type == guitarfx::EffectGuids::kAmpNamBlend || type == "amp_nam_blend" ||
           type == guitarfx::EffectGuids::kFxNam || type == "fx_nam";
}

// NAM types eligible for interface calibration injection (amp + FX).
// Each calibratable NAM node with a loaded model can receive the shared
// interface calibration value; per-node useCalibration still governs whether
// metadata-based auto-gain is actually applied in the DSP effect.
bool IsNamCalibratableEffectType(const std::string& type)
{
    return type == guitarfx::EffectGuids::kAmpNam || type == "amp_nam" ||
           type == guitarfx::EffectGuids::kAmpNamOptimized || type == "amp_nam_optimized" ||
           type == guitarfx::EffectGuids::kAmpNamBlend || type == "amp_nam_blend" ||
           type == guitarfx::EffectGuids::kFxNam || type == "fx_nam";
}

/// Extracts all recognised metadata fields from a NAM model file header.
/// NAM .nam files are a JSON header followed by binary weights; we read the
/// first 64 KB and use targeted string searches to avoid a full JSON parse.
NamFileMetadata TryExtractNamMetadata(const std::filesystem::path& namFilePath)
{
    NamFileMetadata result;

    if (!std::filesystem::exists(namFilePath))
    {
        return result;
    }

    try
    {
        std::ifstream file(namFilePath, std::ios::binary);

        if (!file)
        {
            return result;
        }

        constexpr std::size_t kBufSize = 65536;
        std::vector<char> buf(kBufSize);
        file.read(buf.data(), static_cast<std::streamsize>(kBufSize));
        const auto len = static_cast<std::size_t>(file.gcount());

        if (len == 0)
        {
            return result;
        }

        const std::string_view content(buf.data(), len);

        // Extract a JSON string value: find "key" : "value" and return value.
        const auto extractStr = [](const std::string_view sv, const std::string_view key) -> std::string {
            const auto needle = std::string("\"").append(key).append("\"");
            const auto kp = sv.find(needle);

            if (kp == std::string_view::npos)
            {
                return {};
            }

            const auto cp = sv.find(':', kp + needle.size());

            if (cp == std::string_view::npos)
            {
                return {};
            }

            auto p = cp + 1;

            while (p < sv.size() && std::isspace(static_cast<unsigned char>(sv[p])))
            {
                ++p;
            }

            if (p >= sv.size() || sv[p] != '"')
            {
                return {};
            }

            ++p;
            const auto eq = sv.find('"', p);

            if (eq == std::string_view::npos)
            {
                return {};
            }

            return std::string(sv.substr(p, eq - p));
        };

        // Extract a JSON number value: find "key" : number and return as string.
        const auto extractNum = [](const std::string_view sv, const std::string_view key) -> std::string {
            const auto needle = std::string("\"").append(key).append("\"");
            const auto kp = sv.find(needle);

            if (kp == std::string_view::npos)
            {
                return {};
            }

            const auto cp = sv.find(':', kp + needle.size());

            if (cp == std::string_view::npos)
            {
                return {};
            }

            auto p = cp + 1;

            while (p < sv.size() && std::isspace(static_cast<unsigned char>(sv[p])))
            {
                ++p;
            }

            if (p >= sv.size())
            {
                return {};
            }

            if (!std::isdigit(static_cast<unsigned char>(sv[p])) && sv[p] != '-')
            {
                return {};
            }

            const auto ns = p;

            while (p < sv.size() && (std::isdigit(static_cast<unsigned char>(sv[p])) || sv[p] == '.' || sv[p] == '-' ||
                                     sv[p] == '+' || sv[p] == 'e' || sv[p] == 'E'))
            {
                ++p;
            }

            return std::string(sv.substr(ns, p - ns));
        };

        // Return the full text of a named JSON sub-object { ... }.
        const auto extractObjContent = [](const std::string_view sv, const std::string_view key) -> std::string_view {
            const auto needle = std::string("\"").append(key).append("\"");
            const auto kp = sv.find(needle);

            if (kp == std::string_view::npos)
            {
                return {};
            }

            auto p = sv.find('{', kp + needle.size());

            if (p == std::string_view::npos)
            {
                return {};
            }

            int depth = 0;
            bool ins = false, esc = false;

            for (std::size_t i = p; i < sv.size(); ++i)
            {
                const char c = sv[i];

                if (esc)
                {
                    esc = false;
                    continue;
                }

                if (ins)
                {
                    if (c == '\\')
                    {
                        esc = true;
                    }
                    else if (c == '"')
                    {
                        ins = false;
                    }

                    continue;
                }

                if (c == '"')
                {
                    ins = true;
                }
                else if (c == '{')
                {
                    ++depth;
                }
                else if (c == '}')
                {
                    if (--depth == 0)
                    {
                        return sv.substr(p, i - p + 1);
                    }
                }
            }

            return sv.substr(p); // buffer truncated — still usable
        };

        // Top-level fields
        result.fileVersion = extractStr(content, "version");
        result.architecture = extractStr(content, "architecture");
        result.sampleRate = extractNum(content, "sample_rate");

        // metadata sub-object
        const auto metaContent = extractObjContent(content, "metadata");

        if (!metaContent.empty())
        {
            result.namName = extractStr(metaContent, "name");
            result.modeledBy = extractStr(metaContent, "modeled_by");
            result.gearMake = extractStr(metaContent, "gear_make");
            result.gearModel = extractStr(metaContent, "gear_model");
            result.gearType = extractStr(metaContent, "gear_type");
            result.toneType = extractStr(metaContent, "tone_type");
            result.inputLevelDbu = extractNum(metaContent, "input_level_dbu");
            result.outputLevelDbu = extractNum(metaContent, "output_level_dbu");

            // date sub-object → "YYYY-MM-DD"
            const auto dateContent = extractObjContent(metaContent, "date");

            if (!dateContent.empty())
            {
                const auto yearStr = extractNum(dateContent, "year");

                if (!yearStr.empty())
                {
                    try
                    {
                        const int y = std::stoi(yearStr);
                        const auto ms = extractNum(dateContent, "month");
                        const auto ds = extractNum(dateContent, "day");
                        const int m = ms.empty() ? 0 : std::stoi(ms);
                        const int d = ds.empty() ? 0 : std::stoi(ds);
                        char dateBuf[12];
                        std::snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d", y, m, d);
                        result.modelDate = dateBuf;
                    }
                    catch (...)
                    {
                    }
                }
            }

            // training.final_loss
            const auto trainContent = extractObjContent(metaContent, "training");

            if (!trainContent.empty())
            {
                result.trainingFinalLoss = extractNum(trainContent, "final_loss");
            }
        }
    }
    catch (...)
    {
    }

    return result;
}

/// Enriches a NAM LibraryResource's metadata map from the .nam file header.
/// Uses setIfMissing semantics for all fields except gear_type, which always
/// takes the file's authoritative value (required for full-rig signal routing).
void EnrichNamResourceMetadata(guitarfx::LibraryResource& resource, const std::filesystem::path& namFilePath)
{
    const NamFileMetadata meta = TryExtractNamMetadata(namFilePath);

    const auto setIfMissing = [&](const std::string& key, const std::string& value) {
        if (!value.empty() && !resource.metadata.count(key))
        {
            resource.metadata[key] = value;
        }
    };

    // gear_type: always prefer the file's value — it drives full-rig cab routing.
    if (!meta.gearType.empty())
    {
        resource.metadata["gear_type"] = meta.gearType;
    }

    setIfMissing("namFileVersion", meta.fileVersion);
    setIfMissing("architecture", meta.architecture);
    setIfMissing("sampleRate", meta.sampleRate);
    setIfMissing("namName", meta.namName);
    setIfMissing("modeledBy", meta.modeledBy);
    setIfMissing("gearMake", meta.gearMake);
    setIfMissing("gearModel", meta.gearModel);
    setIfMissing("toneType", meta.toneType);
    setIfMissing("inputLevelDbu", meta.inputLevelDbu);
    setIfMissing("outputLevelDbu", meta.outputLevelDbu);
    setIfMissing("modelDate", meta.modelDate);
    setIfMissing("trainingFinalLoss", meta.trainingFinalLoss);
}

std::string NormalizeCategoryToken(std::string value)
{
    const auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(),
                                            [&](char ch) { return !isSpace(static_cast<unsigned char>(ch)); }));
    value.erase(
        std::find_if(value.rbegin(), value.rend(), [&](char ch) { return !isSpace(static_cast<unsigned char>(ch)); })
            .base(),
        value.end());
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        if (ch == '_' || ch == ' ')
        {
            return '-';
        }

        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::optional<std::string> MapToLibraryCategory(const std::string& rawCategory)
{
    const std::string category = NormalizeCategoryToken(rawCategory);

    if (category.empty())
    {
        return std::nullopt;
    }

    if (category == "amp")
    {
        return std::string{"amp"};
    }

    if (category == "preamp" || category == "outboard")
    {
        return std::string{"preamp"};
    }

    if (category == "pedal" || category == "stomp" || category == "stompbox" || category == "effect" ||
        category == "fx")
    {
        return std::string{"pedal"};
    }

    if (category == "cab" || category == "cabinet" || category == "ir")
    {
        return std::string{"cab"};
    }

    if (category == "full-rig" || category == "fullrig" || category == "amp-cab" || category == "ampcab" ||
        category == "amp+cab" || category == "amp-and-cab")
    {
        return std::string{"full-rig"};
    }

    return std::nullopt;
}

std::string ResolveResourceLibraryCategory(const guitarfx::LibraryResource& resource,
                                           const std::string& requestedCategory)
{
    auto metadataValue = [&](const char* key) -> std::string {
        const auto it = resource.metadata.find(key);
        return it != resource.metadata.end() ? it->second : std::string{};
    };

    if (resource.type == "nam")
    {
        // Prefer the Tone3000 category context for NAM imports.
        if (auto mapped = MapToLibraryCategory(metadataValue("tone3000Category")); mapped.has_value())
        {
            return *mapped;
        }

        if (auto mapped = MapToLibraryCategory(metadataValue("gear")); mapped.has_value())
        {
            return *mapped;
        }

        // Otherwise use NAM-native metadata from the file header.
        if (auto mapped = MapToLibraryCategory(metadataValue("gear_type")); mapped.has_value())
        {
            return *mapped;
        }

        // Fall back to requested category only when it maps to a supported bucket.
        if (auto mapped = MapToLibraryCategory(requestedCategory); mapped.has_value())
        {
            return *mapped;
        }

        // If no metadata maps cleanly, use the most likely default for NAM.
        return "amp";
    }

    return requestedCategory;
}
} // namespace guitarfx::controller_detail
