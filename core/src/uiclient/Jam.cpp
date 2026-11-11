#include "uiclient/Jam.h"

#include "uiclient/ToneSharing.h"

#include <algorithm>
#include <cctype>

namespace guitarfx::uiclient::jam
{
namespace
{
std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

void AppendUtf8(std::string& out, unsigned long code)
{
    if (code < 0x80)
    {
        out += static_cast<char>(code);
    }
    else if (code < 0x800)
    {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
    else if (code < 0x10000)
    {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
    else
    {
        out += static_cast<char>(0xF0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

std::string StringField(const nlohmann::json& object, const char* key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

/// A stored video: all four fields strings, as the web UI's isJamVideoSummary demands.
std::optional<Video> StoredVideo(const nlohmann::json& value)
{
    if (!value.is_object())
    {
        return std::nullopt;
    }

    for (const char* key : {"videoId", "title", "channelTitle", "thumbnailUrl"})
    {
        if (!value.contains(key) || !value[key].is_string())
        {
            return std::nullopt;
        }
    }

    Video video;
    video.videoId = value["videoId"].get<std::string>();
    // Saved before titles were decoded: decode them now.
    video.title = DecodeYouTubeText(value["title"].get<std::string>());
    video.channelTitle = DecodeYouTubeText(value["channelTitle"].get<std::string>());
    video.thumbnailUrl = value["thumbnailUrl"].get<std::string>();
    return video;
}

std::vector<Video> StoredVideos(const nlohmann::json& value)
{
    std::vector<Video> videos;

    if (!value.is_array())
    {
        return videos;
    }

    for (const auto& entry : value)
    {
        if (auto video = StoredVideo(entry))
        {
            videos.push_back(std::move(*video));
        }
    }

    return videos;
}
} // namespace

std::string NormalizeQuery(const std::string& rawQuery)
{
    std::string collapsed;
    bool space = false;

    for (const char c : rawQuery)
    {
        if (std::isspace(static_cast<unsigned char>(c)))
        {
            space = !collapsed.empty();
            continue;
        }

        if (space)
        {
            collapsed += ' ';
        }

        space = false;
        collapsed += c;
    }

    if (collapsed.empty())
    {
        return {};
    }

    // /\bbacking\s+track\b/i, on the collapsed text.
    const auto lower = Lower(collapsed);

    for (auto at = lower.find("backing track"); at != std::string::npos; at = lower.find("backing track", at + 1))
    {
        const auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
        const bool before = at == 0 || !word(lower[at - 1]);
        const auto end = at + 13;
        const bool after = end >= lower.size() || !word(lower[end]);

        if (before && after)
        {
            return collapsed;
        }
    }

    return collapsed + " backing track";
}

std::string DecodeYouTubeText(const std::string& value)
{
    std::string out;
    out.reserve(value.size());
    std::size_t i = 0;

    while (i < value.size())
    {
        const auto semi = value[i] == '&' ? value.find(';', i + 1) : std::string::npos;

        if (semi == std::string::npos || semi - i > 12)
        {
            out += value[i++];
            continue;
        }

        const auto body = value.substr(i + 1, semi - i - 1);
        bool decoded = false;

        if (body.size() > 1 && body[0] == '#')
        {
            const bool hex = body[1] == 'x' || body[1] == 'X';
            const auto digits = body.substr(hex ? 2 : 1);
            const bool valid = !digits.empty() && std::all_of(digits.begin(), digits.end(), [hex](char c) {
                return hex ? std::isxdigit(static_cast<unsigned char>(c)) != 0
                           : std::isdigit(static_cast<unsigned char>(c)) != 0;
            });

            if (valid && digits.size() <= 8)
            {
                const unsigned long code = std::stoul(digits, nullptr, hex ? 16 : 10);

                if (code > 0 && code <= 0x10FFFF)
                {
                    AppendUtf8(out, code);
                    decoded = true;
                }
            }
        }
        else
        {
            static const std::pair<const char*, char> kEntities[] = {
                {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}};
            const auto name = Lower(body);

            for (const auto& [entity, c] : kEntities)
            {
                if (name == entity)
                {
                    out += c;
                    decoded = true;
                    break;
                }
            }
        }

        if (decoded)
        {
            i = semi + 1;
        }
        else
        {
            out += value[i++];
        }
    }

    return out;
}

std::string SearchUrl(const std::string& normalizedQuery, const std::string& apiKey)
{
    return "https://www.googleapis.com/youtube/v3/search?part=snippet&type=video&videoEmbeddable=true&videoSyndicated=true&maxResults=" +
           std::to_string(kSearchMaxResults) + "&q=" + tones::EncodeQueryValue(normalizedQuery) +
           "&key=" + tones::EncodeQueryValue(apiKey);
}

std::vector<Video> ParseSearchResults(const nlohmann::json& payload)
{
    std::vector<Video> videos;

    if (!payload.is_object() || !payload.contains("items") || !payload["items"].is_array())
    {
        return videos;
    }

    for (const auto& item : payload["items"])
    {
        if (!item.is_object())
        {
            continue;
        }

        const auto id = item.find("id");
        const auto snippet = item.find("snippet");

        if (id == item.end() || !id->is_object() || snippet == item.end() || !snippet->is_object())
        {
            continue;
        }

        Video video;
        video.videoId = StringField(*id, "videoId");

        if (video.videoId.empty())
        {
            continue;
        }

        const auto title = snippet->find("title");
        video.title = title != snippet->end() && title->is_string() ? DecodeYouTubeText(title->get<std::string>())
                                                                    : video.videoId;
        const auto channel = snippet->find("channelTitle");
        video.channelTitle = channel != snippet->end() && channel->is_string()
                                 ? DecodeYouTubeText(channel->get<std::string>())
                                 : std::string("Unknown channel");

        if (const auto thumbs = snippet->find("thumbnails"); thumbs != snippet->end() && thumbs->is_object())
        {
            for (const char* size : {"high", "medium", "default"})
            {
                if (thumbs->contains(size) && (*thumbs)[size].is_object())
                {
                    video.thumbnailUrl = StringField((*thumbs)[size], "url");

                    if (!video.thumbnailUrl.empty())
                    {
                        break;
                    }
                }
            }
        }

        videos.push_back(std::move(video));
    }

    return videos;
}

std::string DescribeError(const nlohmann::json& payload)
{
    if (payload.is_object() && payload.contains("error") && payload["error"].is_object())
    {
        auto message = StringField(payload["error"], "message");
        const auto first = message.find_first_not_of(" \t\r\n");

        if (first != std::string::npos)
        {
            return message.substr(first, message.find_last_not_of(" \t\r\n") - first + 1);
        }
    }

    return "Request failed.";
}

std::string ApiKey(const nlohmann::json& appSettings)
{
    if (!appSettings.is_object())
    {
        return {};
    }

    auto key = StringField(appSettings, kApiKeySetting);
    const auto first = key.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? std::string() : key.substr(first, key.find_last_not_of(" \t\r\n") - first + 1);
}

std::vector<Video> Favorites(const nlohmann::json& appSettings)
{
    if (!appSettings.is_object() || !appSettings.contains(kFavoritesSetting))
    {
        return {};
    }

    return StoredVideos(appSettings[kFavoritesSetting]);
}

nlohmann::json VideosJson(const std::vector<Video>& videos)
{
    auto list = nlohmann::json::array();

    for (const auto& video : videos)
    {
        list.push_back({{"videoId", video.videoId},
                        {"title", video.title},
                        {"channelTitle", video.channelTitle},
                        {"thumbnailUrl", video.thumbnailUrl}});
    }

    return list;
}

bool IsFavorite(const std::vector<Video>& favorites, const std::string& videoId)
{
    return std::any_of(favorites.begin(), favorites.end(), [&videoId](const Video& v) { return v.videoId == videoId; });
}

std::vector<Video> ToggleFavorite(std::vector<Video> favorites, const Video& video)
{
    const auto it = std::find_if(favorites.begin(), favorites.end(),
                                 [&video](const Video& v) { return v.videoId == video.videoId; });

    if (it != favorites.end())
    {
        favorites.erase(it);
        return favorites;
    }

    favorites.insert(favorites.begin(), video);
    return favorites;
}

std::optional<SearchCache> ReadSearchCache(const nlohmann::json& appSettings)
{
    if (!appSettings.is_object() || !appSettings.contains(kSearchCacheSetting))
    {
        return std::nullopt;
    }

    const auto& record = appSettings[kSearchCacheSetting];

    if (!record.is_object())
    {
        return std::nullopt;
    }

    SearchCache cache;
    cache.normalizedQuery = NormalizeQuery(StringField(record, "normalizedQuery"));

    if (cache.normalizedQuery.empty())
    {
        return std::nullopt;
    }

    const auto query = StringField(record, "query");
    const auto first = query.find_first_not_of(" \t\r\n");
    cache.query = first == std::string::npos ? cache.normalizedQuery
                                             : query.substr(first, query.find_last_not_of(" \t\r\n") - first + 1);

    if (record.contains("results"))
    {
        cache.results = StoredVideos(record["results"]);
    }

    return cache;
}

std::optional<SearchCache> CachedSearch(const nlohmann::json& appSettings, const std::string& query)
{
    const auto normalized = NormalizeQuery(query);

    if (normalized.empty())
    {
        return std::nullopt;
    }

    auto cache = ReadSearchCache(appSettings);

    if (!cache || cache->normalizedQuery != normalized)
    {
        return std::nullopt;
    }

    return cache;
}

nlohmann::json SearchCacheJson(const std::string& query, const std::vector<Video>& results)
{
    const auto normalized = NormalizeQuery(query);

    if (normalized.empty())
    {
        return nullptr;
    }

    const auto first = query.find_first_not_of(" \t\r\n");
    const auto trimmed =
        first == std::string::npos ? std::string() : query.substr(first, query.find_last_not_of(" \t\r\n") - first + 1);
    return {{"query", trimmed.empty() ? normalized : trimmed},
            {"normalizedQuery", normalized},
            {"results", VideosJson(results)}};
}

std::string EmbedUrl(const std::string& videoId)
{
    return "https://api.sepia.invalid/v1/embed/youtube?v=" + tones::EncodeQueryValue(videoId);
}

std::string WatchUrl(const std::string& videoId)
{
    return "https://www.youtube.com/watch?v=" + tones::EncodeQueryValue(videoId);
}

bool StaysInWebView(const std::string& url)
{
    const auto lower = Lower(url);
    const auto starts = [&lower](const char* prefix) { return lower.rfind(prefix, 0) == 0; };

    // The player's wrapper page (juce/source/PluginEditor.cpp keeps it in the web view for the
    // same reason).
    if (starts("https://api.sepia.invalid/") && lower.find("/embed/") != std::string::npos)
    {
        return true;
    }

    return starts("about:blank");
}
} // namespace guitarfx::uiclient::jam
