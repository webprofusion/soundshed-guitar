#include "controller/MetronomeClickLibrary.h"

#include "IPluginHost.h"
#include "util/FileIO.h"
#include "util/Wav.h"

#include <algorithm>
#include <iostream>
#include <string_view>
#include <system_error>

namespace guitarfx
{
namespace
{
constexpr const char* kManifestPath = "metronome/kits.json";

/// The kit that ships in every build. Used only when the manifest is missing,
/// so a broken install still clicks.
constexpr const char* kFallbackKitId = "kit1";
constexpr const char* kFallbackKitLabel = "Drum Kit";
constexpr const char* kFallbackKitLow = "metronome/kit1/Low.wav";
constexpr const char* kFallbackKitHigh = "metronome/kit1/High.wav";
} // namespace

const std::vector<std::vector<float>>* MetronomeClickLibrary::ClickSamples::Voice(
    controller_detail::ClickVoice voice) const
{
    const auto usable = [](const std::vector<std::vector<float>>& channels) {
        return !channels.empty() && !channels.front().empty();
    };

    switch (voice)
    {
    case controller_detail::ClickVoice::High:

        if (usable(high))
        {
            return &high;
        }

        return usable(low) ? &low : nullptr;

    case controller_detail::ClickVoice::Sub:

        if (usable(sub))
        {
            return &sub;
        }

        [[fallthrough]];

    case controller_detail::ClickVoice::Low:
    default:

        if (usable(low))
        {
            return &low;
        }

        return usable(high) ? &high : nullptr;
    }
}

bool MetronomeClickLibrary::ClickSamples::Empty() const
{
    return Voice(controller_detail::ClickVoice::Low) == nullptr &&
           Voice(controller_detail::ClickVoice::High) == nullptr;
}

MetronomeClickLibrary::MetronomeClickLibrary(IPluginHost& host, const std::filesystem::path& resourceRoot)
    : mHost(host), mResourceRoot(resourceRoot)
{
}

std::filesystem::path MetronomeClickLibrary::ResolvePath(const std::string& rawPath) const
{
    if (rawPath.empty())
    {
        return {};
    }

    std::filesystem::path path{rawPath};

    if (path.is_absolute())
    {
        return path;
    }

    std::error_code ec;

    for (const auto& root : {mHost.GetBundledAssetsPath(), mResourceRoot})
    {
        if (root.empty())
        {
            continue;
        }

        const auto candidateUi = root / "ui" / path;

        if (std::filesystem::exists(candidateUi, ec))
        {
            return candidateUi;
        }

        const auto candidateRoot = root / path;

        if (std::filesystem::exists(candidateRoot, ec))
        {
            return candidateRoot;
        }
    }

    // Deliberately not returning `path` itself: a bare relative path would be
    // read against the process working directory, which hosted is the DAW's.
    // Nothing resolved it, so there is no file.
    return {};
}

nlohmann::json MetronomeClickLibrary::LoadManifest() const
{
    const auto path = ResolvePath(kManifestPath);
    std::error_code ec;

    if (path.empty() || !std::filesystem::exists(path, ec))
    {
        return {};
    }

    const auto bytes = util::ReadFileBytes(path);

    if (bytes.empty())
    {
        return {};
    }

    auto parsed = nlohmann::json::parse(std::string{bytes.begin(), bytes.end()}, nullptr, false);

    if (parsed.is_discarded())
    {
        std::cerr << "[Plugin] Metronome kit manifest is not valid JSON: " << path.generic_string() << std::endl;
        return {};
    }

    // Accept either a bare array or `{ "kits": [...] }` so the file can grow
    // sibling fields later without breaking older builds.
    if (parsed.is_object())
    {
        const auto kits = parsed.find("kits");
        return (kits != parsed.end() && kits->is_array()) ? *kits : nlohmann::json{};
    }

    return parsed.is_array() ? parsed : nlohmann::json{};
}

void MetronomeClickLibrary::AppendEntries(const nlohmann::json& entries)
{
    if (!entries.is_array())
    {
        return;
    }

    for (const auto& entry : entries)
    {
        if (!entry.is_object())
        {
            continue;
        }

        const std::string id = entry.value("id", "");

        if (id.empty())
        {
            continue;
        }

        ClickTypeConfig config;
        config.id = id;
        config.label = entry.value("label", id);
        config.lowPath = ResolvePath(entry.value("lowPath", ""));
        config.highPath = ResolvePath(entry.value("highPath", ""));
        config.subPath = ResolvePath(entry.value("subPath", ""));

        std::error_code ec;
        const auto exists = [&ec](const std::filesystem::path& path) {
            return !path.empty() && std::filesystem::exists(path, ec);
        };

        if (!exists(config.lowPath) && !exists(config.highPath))
        {
            continue;
        }

        const auto existing =
            std::find_if(mTypes.begin(), mTypes.end(), [&id](const ClickTypeConfig& c) { return c.id == id; });

        if (existing != mTypes.end())
        {
            *existing = std::move(config);
        }
        else
        {
            mTypes.push_back(std::move(config));
        }
    }
}

void MetronomeClickLibrary::Rebuild(const nlohmann::json* overrides)
{
    mTypes.clear();

    AppendEntries(LoadManifest());

    if (mTypes.empty())
    {
        nlohmann::json fallback = nlohmann::json::array();
        fallback.push_back({{"id", kFallbackKitId},
                            {"label", kFallbackKitLabel},
                            {"lowPath", kFallbackKitLow},
                            {"highPath", kFallbackKitHigh}});
        AppendEntries(fallback);
    }

    if (overrides != nullptr)
    {
        AppendEntries(*overrides);
    }
}

const MetronomeClickLibrary::ClickTypeConfig* MetronomeClickLibrary::Find(const std::string& id) const
{
    for (const auto& config : mTypes)
    {
        if (config.id == id)
        {
            return &config;
        }
    }

    return mTypes.empty() ? nullptr : &mTypes.front();
}

std::shared_ptr<MetronomeClickLibrary::ClickSamples> MetronomeClickLibrary::Load(const ClickTypeConfig& config,
                                                                                 double targetSampleRate) const
{
    if (targetSampleRate <= 0.0)
    {
        return nullptr;
    }

    auto samples = std::make_shared<ClickSamples>();

    auto loadWav = [&](const std::filesystem::path& path, std::vector<std::vector<float>>& target,
                       std::string_view label) {
        if (path.empty())
        {
            return;
        }

        if (!std::filesystem::exists(path))
        {
            std::cerr << "[Plugin] Metronome " << label << " sample not found: " << path.generic_string() << std::endl;
            return;
        }

        const auto bytes = util::ReadFileBytes(path);

        if (bytes.empty())
        {
            std::cerr << "[Plugin] Metronome " << label << " sample empty: " << path.generic_string() << std::endl;
            return;
        }

        const auto wavData = util::DecodePcmWav(bytes);

        if (!wavData)
        {
            std::cerr << "[Plugin] Metronome " << label << " sample unsupported WAV: " << path.generic_string()
                      << std::endl;
            return;
        }

        auto resampled = util::ConvertToSampleRate(*wavData, targetSampleRate);

        if (resampled.empty() || resampled.front().empty())
        {
            std::cerr << "[Plugin] Metronome " << label << " sample empty after resample: " << path.generic_string()
                      << std::endl;
            return;
        }

        // Channels must be the same length: Render() indexes them together.
        std::size_t minFrames = resampled.front().size();

        for (const auto& channel : resampled)
        {
            if (channel.empty())
            {
                return;
            }

            minFrames = std::min(minFrames, channel.size());
        }

        for (auto& channel : resampled)
        {
            if (channel.size() > minFrames)
            {
                channel.resize(minFrames);
            }
        }

        target = std::move(resampled);
    };

    loadWav(config.lowPath, samples->low, "low");
    loadWav(config.highPath, samples->high, "high");
    loadWav(config.subPath, samples->sub, "subdivision");

    if (samples->Empty())
    {
        return nullptr;
    }

    return samples;
}
} // namespace guitarfx
