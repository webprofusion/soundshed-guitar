#pragma once

/**
 * TonesBrowse.h - What the Tones page's Your Presets and Tone 3000 tabs show, worked out from
 * plain data, as the web UI's tonesPanel/localPresetsModel.ts, tone3000BrowserCache.ts and the
 * parts of tone3000Browser.ts that are not drawing.
 *
 * Your Presets: the order of the cards, the tags on offer, a card's initials and the count.
 * Tone 3000: the categories, the sort names, how much of a tone is in the library, and the
 * results held for an hour so that looking at them again is not another request (Tone3000 is
 * asked only on a miss, results over an hour old, or Search / Enter / Refresh).
 */

#include "uiclient/ClientState.h"
#include "uiclient/Tone3000.h"

#include <chrono>
#include <cstdint>
#include <list>
#include <optional>
#include <string>
#include <vector>

namespace guitarfx::uiclient
{
// ── Your Presets ─────────────────────────────────────────────────────────────

enum class LocalPresetSort
{
    Name,  // Name A-Z
    Rating // Top rated: the highest first, the unrated last, by name within a rating
};

/// The cards in order. `keepOrder` is for Recents, whose order is the point of the folder.
[[nodiscard]] std::vector<PresetSummary> SortLocalPresets(const ClientState& state, std::vector<PresetSummary> presets,
                                                          LocalPresetSort sort, bool keepOrder);

/// Every tag the presets carry, trimmed, once each, in name order (collectPresetTags).
[[nodiscard]] std::vector<std::string> CollectPresetTags(const std::vector<PresetSummary>& presets);

/// Two letters for a card with no artwork (presetInitials): the first two words' initials, else
/// the first two characters, a leading number skipped ("01 Big Muff" is BM); "?" for none.
[[nodiscard]] std::string PresetInitials(const std::string& name);

/// "12 presets", or "3 of 12 presets" when a search or a tag has narrowed them.
[[nodiscard]] std::string DescribePresetCount(std::size_t shown, std::size_t inFolder);

// ── Tone 3000 ────────────────────────────────────────────────────────────────

struct Tone3000Category
{
    const char* id;
    const char* label;
    const char* gear;     // the API's gear
    const char* platform; // "nam" or "ir": results of another platform are left out
};

/// The side list, in order: Pedals (FX), Preamps, Amps, Full Rigs, Cab IRs, Room & Reverb IRs.
[[nodiscard]] const std::vector<Tone3000Category>& Tone3000Categories();
[[nodiscard]] const Tone3000Category& Tone3000CategoryById(const std::string& id);

struct Tone3000SortOption
{
    const char* id;    // the web UI's select value: popular, recent, trending, name
    const char* label; // Most Popular, Most Recent, Trending, Name A-Z
    const char* api;   // downloads-all-time, newest, trending, best-match
};

[[nodiscard]] const std::vector<Tone3000SortOption>& Tone3000SortOptions();
[[nodiscard]] const Tone3000SortOption& Tone3000SortById(const std::string& id);

/// The tones a category shows from a page of results: those of its platform, and those that do
/// not say which they are.
[[nodiscard]] std::vector<tone3000::Tone> FilterTone3000ByPlatform(std::vector<tone3000::Tone> tones,
                                                                   const std::string& platform);

/// How much of a tone the library holds (getToneImportStatus): every model, some, or none, and
/// how many distinct models were found (NAM and IR entries whose toneId or groupId is the tone).
struct Tone3000ImportStatus
{
    enum class Kind
    {
        None,
        Partial,
        Imported
    };

    Kind kind = Kind::None;
    int importedCount = 0;
};

[[nodiscard]] Tone3000ImportStatus Tone3000ToneImportStatus(const ClientState& state, const tone3000::Tone& tone);

/// "1.2K", "34M": a count in a few characters (formatCompactCount).
[[nodiscard]] std::string FormatCompactCount(std::int64_t count);

/// Two letters for a tone with no picture (getToneInitials); "T3" for none.
[[nodiscard]] std::string Tone3000Initials(const std::string& title);

struct Tone3000ResultsRequest
{
    bool favourites = false;
    std::string categoryId;
    std::string query;
    std::string sort;
    int page = 1;
};

struct Tone3000CachedResults
{
    Tone3000ResultsRequest request;
    std::vector<tone3000::Tone> tones;
    int page = 1;
    int totalPages = 1;
    std::chrono::system_clock::time_point fetchedAt;
};

/// The results of the last hour, one entry per request (mode, category, search ignoring case
/// and the ends' spaces, sort, page; favourites ignore all but the page), at most 40.
class Tone3000ResultsCache
{
  public:
    static constexpr std::chrono::minutes kMaxAge{60};
    static constexpr std::size_t kMaxEntries = 40;

    [[nodiscard]] static std::string Key(const Tone3000ResultsRequest& request);
    /// Under an hour old; one from the future (a clock set back) is stale.
    [[nodiscard]] static bool IsFresh(std::chrono::system_clock::time_point fetchedAt,
                                      std::chrono::system_clock::time_point now);

    /// The results for a request if they were fetched within the hour; a stale entry is dropped.
    [[nodiscard]] std::optional<Tone3000CachedResults> Find(const Tone3000ResultsRequest& request,
                                                            std::chrono::system_clock::time_point now);
    void Store(Tone3000CachedResults results);
    void Clear();

  private:
    std::list<std::pair<std::string, Tone3000CachedResults>> mEntries; // oldest stored first
};

/// "just now", "12 min ago", "over an hour ago", "3 hours ago" (describeTone3000ResultsAge).
[[nodiscard]] std::string DescribeTone3000ResultsAge(std::chrono::system_clock::time_point fetchedAt,
                                                     std::chrono::system_clock::time_point now);
} // namespace guitarfx::uiclient
