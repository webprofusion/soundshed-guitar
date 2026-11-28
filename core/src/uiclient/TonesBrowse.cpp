#include "uiclient/TonesBrowse.h"

#include "uiclient/PresetBrowse.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>

namespace guitarfx::uiclient
{
namespace
{
std::string Trim(const std::string& text)
{
    const auto first = text.find_first_not_of(" \t\r\n");

    if (first == std::string::npos)
    {
        return {};
    }

    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

/// A metadata value as text: a string as it is, a whole number written out (the web's String()).
std::string MetaText(const nlohmann::json& metadata, const char* key)
{
    if (!metadata.is_object())
    {
        return {};
    }

    const auto it = metadata.find(key);

    if (it == metadata.end())
    {
        return {};
    }

    if (it->is_string())
    {
        return it->get<std::string>();
    }

    if (it->is_number_integer())
    {
        return std::to_string(it->get<std::int64_t>());
    }

    return {};
}

/// The length of the UTF-8 sequence that starts with `lead`.
std::size_t SequenceLength(unsigned char lead)
{
    return lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 1;
}

/// A word's letters and digits: ASCII punctuation dropped, anything beyond ASCII kept as a letter.
std::string WordCharacters(const std::string& word)
{
    std::string kept;

    for (const char c : word)
    {
        const auto u = static_cast<unsigned char>(c);

        if (u >= 0x80 || std::isalnum(u))
        {
            kept += c;
        }
    }

    return kept;
}

std::vector<std::string> Words(const std::string& text)
{
    std::vector<std::string> words;
    std::string current;

    for (const char c : text)
    {
        if (std::isspace(static_cast<unsigned char>(c)))
        {
            if (!current.empty())
            {
                words.push_back(current);
                current.clear();
            }
        }
        else
        {
            current += c;
        }
    }

    if (!current.empty())
    {
        words.push_back(current);
    }

    return words;
}

/// The first character of `text` (a whole UTF-8 sequence), upper-cased where it is ASCII.
std::string FirstCharacter(const std::string& text)
{
    if (text.empty())
    {
        return {};
    }

    auto first = text.substr(0, std::min(text.size(), SequenceLength(static_cast<unsigned char>(text[0]))));

    if (first.size() == 1)
    {
        first[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(first[0])));
    }

    return first;
}

bool AllDigits(const std::string& word)
{
    return !word.empty() &&
           std::all_of(word.begin(), word.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
}
} // namespace

std::vector<PresetSummary> SortLocalPresets(const ClientState& state, std::vector<PresetSummary> presets,
                                            LocalPresetSort sort, bool keepOrder)
{
    if (sort == LocalPresetSort::Rating)
    {
        std::stable_sort(presets.begin(), presets.end(), [&state](const PresetSummary& l, const PresetSummary& r) {
            const int lr = PresetRating(state, l.id), rr = PresetRating(state, r.id);
            return lr != rr ? lr > rr : ComparePresetNames(l, r) < 0;
        });
        return presets;
    }

    if (!keepOrder)
    {
        std::stable_sort(presets.begin(), presets.end(),
                         [](const PresetSummary& l, const PresetSummary& r) { return ComparePresetNames(l, r) < 0; });
    }

    return presets;
}

std::vector<std::string> CollectPresetTags(const std::vector<PresetSummary>& presets)
{
    std::set<std::string> seen;
    std::vector<std::string> tags;

    for (const auto& preset : presets)
    {
        for (const auto& tag : preset.tags)
        {
            if (auto trimmed = Trim(tag); !trimmed.empty() && seen.insert(trimmed).second)
            {
                tags.push_back(std::move(trimmed));
            }
        }
    }

    // In name order, ignoring case (the web's collator); exact ties by bytes.
    std::sort(tags.begin(), tags.end(), [](const std::string& l, const std::string& r) {
        const auto ll = Lower(l), lr = Lower(r);
        return ll != lr ? ll < lr : l < r;
    });
    return tags;
}

std::string PresetInitials(const std::string& name)
{
    std::vector<std::string> words;

    for (const auto& word : Words(name))
    {
        if (auto kept = WordCharacters(word); !kept.empty())
        {
            words.push_back(std::move(kept));
        }
    }

    // A number the name starts with is not a word ("01 David Gilmour" is DG).
    while (words.size() > 1 && AllDigits(words.front()))
    {
        words.erase(words.begin());
    }

    if (words.size() >= 2)
    {
        return FirstCharacter(words[0]) + FirstCharacter(words[1]);
    }

    if (words.empty())
    {
        return "?";
    }

    const auto& only = words.front();
    const auto first = FirstCharacter(only);
    const auto rest = only.substr(first.size());
    return first + FirstCharacter(rest);
}

std::string DescribePresetCount(std::size_t shown, std::size_t inFolder)
{
    const char* noun = inFolder == 1 ? "preset" : "presets";
    return shown == inFolder ? std::to_string(inFolder) + " " + noun
                             : std::to_string(shown) + " of " + std::to_string(inFolder) + " " + noun;
}

const std::vector<Tone3000Category>& Tone3000Categories()
{
    static const std::vector<Tone3000Category> categories{
        {"pedal", "Pedals (FX)", "pedal", "nam"},     {"preamp", "Preamps", "outboard", "nam"},
        {"amp", "Amps", "amp", "nam"},                {"full-rig", "Full Rigs", "full-rig", "nam"},
        {"cab", "Cab IRs", "cab", "ir"},              {"space", "Room & Reverb IRs", "space", "ir"},
    };
    return categories;
}

const Tone3000Category& Tone3000CategoryById(const std::string& id)
{
    const auto& all = Tone3000Categories();
    const auto it = std::find_if(all.begin(), all.end(), [&id](const Tone3000Category& c) { return id == c.id; });
    return it != all.end() ? *it : all.front();
}

const std::vector<Tone3000SortOption>& Tone3000SortOptions()
{
    static const std::vector<Tone3000SortOption> options{
        {"popular", "Most Popular", "downloads-all-time"},
        {"recent", "Most Recent", "newest"},
        {"trending", "Trending", "trending"},
        {"name", "Name A-Z", "best-match"},
    };
    return options;
}

const Tone3000SortOption& Tone3000SortById(const std::string& id)
{
    const auto& all = Tone3000SortOptions();
    const auto it = std::find_if(all.begin(), all.end(), [&id](const Tone3000SortOption& o) { return id == o.id; });
    return it != all.end() ? *it : all.front();
}

std::vector<tone3000::Tone> FilterTone3000ByPlatform(std::vector<tone3000::Tone> tones, const std::string& platform)
{
    if (platform.empty())
    {
        return tones;
    }

    std::erase_if(tones, [&platform](const tone3000::Tone& tone) {
        return !tone.platform.empty() && Lower(tone.platform) != platform;
    });
    return tones;
}

Tone3000ImportStatus Tone3000ToneImportStatus(const ClientState& state, const tone3000::Tone& tone)
{
    std::set<std::string> seen;

    for (const char* type : {"nam", "ir"})
    {
        const auto it = state.resources.find(type);

        if (it == state.resources.end())
        {
            continue;
        }

        for (const auto& resource : it->second)
        {
            if (resource.fileMissing)
            {
                continue;
            }

            auto toneId = MetaText(resource.metadata, "toneId");

            if (toneId.empty())
            {
                toneId = MetaText(resource.metadata, "groupId");
            }

            if (toneId.empty() || toneId != tone.id)
            {
                continue;
            }

            std::string key = std::string(type) == "ir" ? MetaText(resource.metadata, "entryName") : std::string();

            if (key.empty())
            {
                key = MetaText(resource.metadata, "modelId");
            }

            seen.insert(key.empty() ? resource.id : key);
        }
    }

    Tone3000ImportStatus status;
    status.importedCount = static_cast<int>(seen.size());
    const int models = tone.modelsCount; // models_count, as the web UI counts

    if (models > 0 && status.importedCount >= models)
    {
        status.kind = Tone3000ImportStatus::Kind::Imported;
    }
    else if (status.importedCount > 0)
    {
        status.kind = Tone3000ImportStatus::Kind::Partial;
    }

    return status;
}

std::string FormatCompactCount(std::int64_t count)
{
    if (count < 0)
    {
        return "0";
    }

    const auto scaled = [](double value, const char* suffix) {
        char text[32];
        std::snprintf(text, sizeof(text), value >= 10.0 ? "%.0f" : "%.1f", value >= 10.0 ? std::floor(value) : value);
        std::string s = text;

        if (s.size() > 2 && s.compare(s.size() - 2, 2, ".0") == 0)
        {
            s.resize(s.size() - 2);
        }

        return s + suffix;
    };

    if (count >= 1'000'000)
    {
        return scaled(static_cast<double>(count) / 1'000'000.0, "M");
    }

    if (count >= 1'000)
    {
        return scaled(static_cast<double>(count) / 1'000.0, "K");
    }

    return std::to_string(count);
}

std::string Tone3000Initials(const std::string& title)
{
    const auto words = Words(title);

    if (words.empty())
    {
        return "T3";
    }

    if (words.size() >= 2)
    {
        return FirstCharacter(words[0]) + FirstCharacter(words[1]);
    }

    std::string compact;

    for (const char c : words.front())
    {
        if (std::isalnum(static_cast<unsigned char>(c)) && compact.size() < 2)
        {
            compact += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
    }

    return compact.empty() ? std::string("T3") : compact;
}

std::string Tone3000ResultsCache::Key(const Tone3000ResultsRequest& request)
{
    if (request.favourites)
    {
        return "favorites|" + std::to_string(request.page);
    }

    return "search|" + request.categoryId + "|" + Lower(Trim(request.query)) + "|" + request.sort + "|" +
           std::to_string(request.page);
}

bool Tone3000ResultsCache::IsFresh(std::chrono::system_clock::time_point fetchedAt,
                                   std::chrono::system_clock::time_point now)
{
    return now >= fetchedAt && now - fetchedAt < kMaxAge;
}

std::optional<Tone3000CachedResults> Tone3000ResultsCache::Find(const Tone3000ResultsRequest& request,
                                                                std::chrono::system_clock::time_point now)
{
    const auto key = Key(request);
    const auto it =
        std::find_if(mEntries.begin(), mEntries.end(), [&key](const auto& entry) { return entry.first == key; });

    if (it == mEntries.end())
    {
        return std::nullopt;
    }

    if (!IsFresh(it->second.fetchedAt, now))
    {
        mEntries.erase(it);
        return std::nullopt;
    }

    return it->second;
}

void Tone3000ResultsCache::Store(Tone3000CachedResults results)
{
    auto key = Key(results.request);
    std::erase_if(mEntries, [&key](const auto& entry) { return entry.first == key; });
    mEntries.emplace_back(std::move(key), std::move(results));

    while (mEntries.size() > kMaxEntries)
    {
        mEntries.pop_front();
    }
}

void Tone3000ResultsCache::Clear()
{
    mEntries.clear();
}

std::string DescribeTone3000ResultsAge(std::chrono::system_clock::time_point fetchedAt,
                                       std::chrono::system_clock::time_point now)
{
    const auto minutes =
        now > fetchedAt ? std::chrono::duration_cast<std::chrono::minutes>(now - fetchedAt).count() : 0;

    if (minutes < 1)
    {
        return "just now";
    }

    if (minutes < 60)
    {
        return std::to_string(minutes) + " min ago";
    }

    const auto hours = minutes / 60;
    return hours == 1 ? std::string("over an hour ago") : std::to_string(hours) + " hours ago";
}
} // namespace guitarfx::uiclient
