#include "controller/internal/IrResourceCategory.h"

#include "controller/internal/NamResourceMetadata.h"
#include "dsp/IRWavLoader.h"
#include "dsp/ImpulseResponseAnalysis.h"
#include "resources/ResourceLibrary.h"
#include "util/PathEncoding.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <string_view>
#include <vector>

namespace guitarfx::controller_detail
{
namespace
{
// Measured on 230 IRs on 2026-10-08 (a user library, two reverb packs, testdata): a cab's
// level falls 30 dB within 110 ms of its loudest 10 ms, a mic'd room blend included, and a
// room's takes 160 ms or more. A reverb mixed with a loud dry hit falls that far at once,
// but still takes 150 ms or more to deliver 99% of its first 400 ms, where no cab passed
// 101 ms. One file read wrong: a reversed "swell" IR, whose loudest moment is its last.
constexpr double kReverbEnvelopeSeconds = 0.14;
constexpr double kReverbEnergySeconds = 0.15;

bool ContainsAny(std::string_view value, std::initializer_list<std::string_view> words)
{
    for (const auto word : words)
    {
        if (value.find(word) != std::string_view::npos)
        {
            return true;
        }
    }

    return false;
}
} // namespace

bool IsIrLibraryCategory(const std::string& category)
{
    return category == kIrCategoryCab || category == kIrCategoryReverb;
}

std::optional<std::string> MapToIrLibraryCategory(const std::string& rawCategory)
{
    static constexpr std::array<std::string_view, 6> kCabTokens = {"cab",      "cabs",   "cabinet",
                                                                   "cabinets", "cab-ir", "cab-irs"};
    static constexpr std::array<std::string_view, 9> kReverbTokens = {
        "reverb", "reverbs", "reverb-ir", "reverb-irs", "space", "room", "rooms", "hall", "plate"};
    const std::string token = NormalizeCategoryToken(rawCategory);

    if (std::find(kCabTokens.begin(), kCabTokens.end(), token) != kCabTokens.end())
    {
        return std::string{kIrCategoryCab};
    }

    if (std::find(kReverbTokens.begin(), kReverbTokens.end(), token) != kReverbTokens.end())
    {
        return std::string{kIrCategoryReverb};
    }

    return std::nullopt;
}

std::optional<std::string> ClassifyIrFileCategory(const std::filesystem::path& filePath)
{
    IRWavData data;

    if (filePath.empty() || !irwav::LoadAudioFile(filePath, data) || data.channels == 0 || data.samples.empty())
    {
        return std::nullopt;
    }

    std::vector<float> mono;
    irwav::DownmixToMono(data, mono);
    const bool rings = EnvelopeDecaySeconds(mono, data.sampleRate, 30.0, 0.01) >= kReverbEnvelopeSeconds ||
                       EnergyDecaySeconds(mono, data.sampleRate, 20.0, 0.4) >= kReverbEnergySeconds;
    return std::string{rings ? kIrCategoryReverb : kIrCategoryCab};
}

std::string ResolveIrLibraryCategory(const LibraryResource& resource, const std::string& requestedCategory)
{
    if (auto mapped = MapToIrLibraryCategory(requestedCategory))
    {
        return *mapped;
    }

    if (auto measured = ClassifyIrFileCategory(resource.filePath))
    {
        return *measured;
    }

    // A file that cannot be read (often a drive since moved) is guessed from its category
    // and the folders it sat in, never its own name: "Twin Reverb" and "Deluxe Reverb" are
    // amps, and their cab IRs are named after them.
    const std::string hints =
        NormalizeCategoryToken(requestedCategory + " " + util::PathToUtf8(resource.filePath.parent_path()));
    return ContainsAny(hints, {"reverb", "room", "hall", "plate", "spring", "space", "ambien", "chamber"})
               ? kIrCategoryReverb
               : kIrCategoryCab;
}

std::optional<std::string> IrCategoryToKeepAsTag(const std::string& oldCategory)
{
    static constexpr std::array<std::string_view, 12> kGenericTokens = {
        "local",         "imported", "ir",   "irs",    "pedal",    "outboard",
        "uncategorized", "folder",   "user", "custom", "built-in", "other"};
    const std::string token = NormalizeCategoryToken(oldCategory);

    if (token.empty() || MapToIrLibraryCategory(oldCategory) ||
        std::find(kGenericTokens.begin(), kGenericTokens.end(), token) != kGenericTokens.end())
    {
        return std::nullopt;
    }

    const auto first = oldCategory.find_first_not_of(" \t\r\n");
    const auto last = oldCategory.find_last_not_of(" \t\r\n");
    return oldCategory.substr(first, last - first + 1);
}
} // namespace guitarfx::controller_detail
