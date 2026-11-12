#include "controller/internal/NamResourceMetadata.h"

#include "controller/internal/IrResourceCategory.h"
#include "dsp/EffectGuids.h"
#include "models/ContentHash.h"
#include "resources/ResourceLibrary.h"
#include "util/JsonSkimmer.h"
#include "util/LongPath.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <istream>

namespace guitarfx::controller_detail
{
using util::JsonSkimmer;

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

namespace
{
/// Fills the descriptive fields from the model's top-level "metadata" object.
bool ReadMetadataObject(JsonSkimmer& json, NamFileMetadata& result)
{
    if (json.PeekValue() != '{')
    {
        return json.SkipValue();
    }

    std::string year, month, day;

    const bool ok = json.ForEachMember([&](const std::string& key) {
        if (key == "name")
        {
            return json.ReadString(result.namName);
        }

        if (key == "modeled_by")
        {
            return json.ReadString(result.modeledBy);
        }

        if (key == "gear_make")
        {
            return json.ReadString(result.gearMake);
        }

        if (key == "gear_model")
        {
            return json.ReadString(result.gearModel);
        }

        if (key == "gear_type")
        {
            return json.ReadString(result.gearType);
        }

        if (key == "tone_type")
        {
            return json.ReadString(result.toneType);
        }

        if (key == "input_level_dbu")
        {
            return json.ReadNumber(result.inputLevelDbu);
        }

        if (key == "output_level_dbu")
        {
            return json.ReadNumber(result.outputLevelDbu);
        }

        if (key == "date" && json.PeekValue() == '{')
        {
            return json.ForEachMember([&](const std::string& part) {
                if (part == "year")
                {
                    return json.ReadNumber(year);
                }

                if (part == "month")
                {
                    return json.ReadNumber(month);
                }

                if (part == "day")
                {
                    return json.ReadNumber(day);
                }

                return json.SkipValue();
            });
        }

        if (key == "training" && json.PeekValue() == '{')
        {
            return json.ForEachMember([&](const std::string& part) {
                return part == "final_loss" ? json.ReadNumber(result.trainingFinalLoss) : json.SkipValue();
            });
        }

        return json.SkipValue();
    });

    // date → "YYYY-MM-DD"
    if (!year.empty())
    {
        try
        {
            const int y = std::stoi(year);
            const int m = month.empty() ? 0 : std::stoi(month);
            const int d = day.empty() ? 0 : std::stoi(day);
            char dateBuf[12];
            std::snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d", y, m, d);
            result.modelDate = dateBuf;
        }
        catch (...)
        {
        }
    }

    return ok;
}

/// Whether a WaveNet "config" has the layer layout the A2 trainer writes: per-layer
/// "kernel_sizes", activation arrays and the "head1x1"/"layer1x1" blocks. A1 layers have
/// one "kernel_size" and one activation name.
bool ScanWaveNetConfig(JsonSkimmer& json, bool& hasA2Layers)
{
    if (json.PeekValue() != '{')
    {
        return json.SkipValue();
    }

    return json.ForEachMember([&](const std::string& key) {
        if (key != "layers" || json.PeekValue() != '[')
        {
            return json.SkipValue();
        }

        return json.ForEachElement([&]() {
            if (json.PeekValue() != '{')
            {
                return json.SkipValue();
            }

            return json.ForEachMember([&](const std::string& field) {
                if (field == "kernel_sizes" || field == "head1x1" || field == "layer1x1" ||
                    (field == "activation" && json.PeekValue() == '['))
                {
                    hasA2Layers = true;
                }

                return json.SkipValue();
            });
        });
    });
}
} // namespace

std::string ClassifyNamArchitectureVersion(const std::string& architecture, bool waveNetHasA2Layers)
{
    if (architecture == "SlimmableContainer")
    {
        return "A2";
    }

    if (architecture == "WaveNet")
    {
        return waveNetHasA2Layers ? "A2" : "A1";
    }

    return {};
}

NamFileMetadata ReadNamMetadata(std::istream& in)
{
    NamFileMetadata result;
    bool waveNetHasA2Layers = false;
    JsonSkimmer json(in);

    // Only the top-level members count. An A2 file is a SlimmableContainer whose
    // submodels each carry their own "architecture", "metadata", "version" and
    // "sample_rate", and trainers write the top-level members in any order, metadata
    // after the weights included, so the whole file is walked. A truncated file keeps
    // whatever came before the break.
    json.ForEachMember([&](const std::string& key) {
        if (key == "version")
        {
            return json.ReadString(result.fileVersion);
        }

        if (key == "architecture")
        {
            return json.ReadString(result.architecture);
        }

        if (key == "sample_rate")
        {
            return json.ReadNumber(result.sampleRate);
        }

        if (key == "metadata")
        {
            return ReadMetadataObject(json, result);
        }

        if (key == "config")
        {
            return ScanWaveNetConfig(json, waveNetHasA2Layers);
        }

        return json.SkipValue();
    });

    result.architectureVersion = ClassifyNamArchitectureVersion(result.architecture, waveNetHasA2Layers);
    return result;
}

/// Extracts the recognised metadata fields from a NAM model file. A .nam file is one
/// JSON document, weights included, so it is skimmed (ReadNamMetadata) rather than
/// parsed: folder scans read hundreds of them.
NamFileMetadata TryExtractNamMetadata(const std::filesystem::path& namFilePath)
{
    if (!guitarfx::util::PathExists(namFilePath))
    {
        return {};
    }

    try
    {
        std::ifstream file(guitarfx::util::LongPathSafe(namFilePath), std::ios::binary);

        if (!file)
        {
            return {};
        }

        return ReadNamMetadata(file);
    }
    catch (...)
    {
    }

    return {};
}

namespace
{
/// The architecture_version spellings the Tone3000 API uses; the reader writes "A1"/"A2".
bool IsTone3000ArchitectureVersion(const std::string& value)
{
    return value == "1" || value == "2" || value == "custom";
}
} // namespace

/// Enriches a NAM LibraryResource's metadata map from the .nam file.
///
/// What the file says about its own structure (gear_type, which drives full-rig cab
/// routing, the file version, architecture and sample rate) always replaces what the row
/// held: builds before 1.7 misread A2 files, and the UI once guessed. architectureVersion
/// follows the file too, except for the "1", "2" or "custom" the Tone3000 API gave an
/// import. The descriptive fields (name, gear, calibration levels, date) are filled in
/// where missing, so a user's edit stands, unless `refresh` says to take the file's.
void EnrichNamResourceMetadata(guitarfx::LibraryResource& resource, const std::filesystem::path& namFilePath,
                               NamMetadataRefresh refresh)
{
    const NamFileMetadata meta = TryExtractNamMetadata(namFilePath);
    auto& metadata = resource.metadata;

    const auto setFromFile = [&](const std::string& key, const std::string& value) {
        if (!value.empty())
        {
            metadata[key] = value;
        }
    };

    const auto setDescriptive = [&](const std::string& key, const std::string& value) {
        if (!value.empty() && (refresh == NamMetadataRefresh::FromFile || !metadata.count(key)))
        {
            metadata[key] = value;
        }
    };

    setFromFile("gear_type", meta.gearType);
    setFromFile("namFileVersion", meta.fileVersion);
    setFromFile("architecture", meta.architecture);
    setFromFile("sampleRate", meta.sampleRate);

    // Only a file that was read and named its architecture says anything about the
    // version: a missing or unreadable one leaves the row alone.
    if (!meta.architecture.empty())
    {
        const auto existing = metadata.find("architectureVersion");

        if (existing == metadata.end() || !IsTone3000ArchitectureVersion(existing->second))
        {
            if (!meta.architectureVersion.empty())
            {
                metadata["architectureVersion"] = meta.architectureVersion;
            }
            else if (existing != metadata.end())
            {
                metadata.erase(existing);
            }
        }
    }

    setDescriptive("namName", meta.namName);
    setDescriptive("modeledBy", meta.modeledBy);
    setDescriptive("gearMake", meta.gearMake);
    setDescriptive("gearModel", meta.gearModel);
    setDescriptive("toneType", meta.toneType);
    setDescriptive("inputLevelDbu", meta.inputLevelDbu);
    setDescriptive("outputLevelDbu", meta.outputLevelDbu);
    setDescriptive("modelDate", meta.modelDate);
    setDescriptive("trainingFinalLoss", meta.trainingFinalLoss);
}

bool RefreshContentHashMetadata(guitarfx::LibraryResource& resource)
{
    if (resource.type != "nam" && resource.type != "ir")
    {
        return false;
    }

    const auto hash = guitarfx::ContentHashOfFile(resource.type, resource.filePath);
    const auto existing = resource.metadata.find(guitarfx::kContentHashMetadataKey);

    if (hash)
    {
        if (existing != resource.metadata.end() && existing->second == *hash)
        {
            return false;
        }

        resource.metadata[guitarfx::kContentHashMetadataKey] = *hash;
        return true;
    }

    if (existing == resource.metadata.end())
    {
        return false;
    }

    resource.metadata.erase(existing);
    return true;
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

    // The trainer's gear types (amp, pedal, pedal_amp, amp_cab, amp_pedal_cab, preamp, studio)
    // arrive here with '_' already turned into '-'.
    if (category == "amp" || category == "pedal-amp")
    {
        return std::string{"amp"};
    }

    if (category == "preamp" || category == "outboard" || category == "studio")
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
        category == "amp+cab" || category == "amp-and-cab" || category == "amp-pedal-cab")
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

    if (resource.type == "ir")
    {
        return ResolveIrLibraryCategory(resource, requestedCategory);
    }

    return requestedCategory;
}
} // namespace guitarfx::controller_detail
