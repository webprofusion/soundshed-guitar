#include "controller/UiBackgroundService.h"

#include "IPluginHost.h"
#include "MessageHandlerRegistry.h"
#include "util/FileIO.h"
#include "util/FileSystem.h"
#include "util/LongPath.h"
#include "util/PathEncoding.h"
#include "util/SessionLog.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <system_error>
#include <vector>

namespace guitarfx
{
namespace
{
bool StartsWith(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

std::string LowerExtension(const std::filesystem::path& extension)
{
    std::string ext = util::PathToUtf8(extension);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

/// The extension a picture is stored under: one spelling per type, so a file's name follows
/// from its bytes and type alone.
std::string StoredExtension(const std::string& lowerExtension)
{
    return lowerExtension == ".jpeg" ? ".jpg" : lowerExtension;
}

/// 16 hex digits of the FNV-1a hash of `bytes`.
std::string KeyFor(const std::vector<std::uint8_t>& bytes)
{
    std::uint64_t hash = 14695981039346656037ULL;

    for (const auto byte : bytes)
    {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }

    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

bool IsKey(std::string_view key)
{
    return key.size() == 16 &&
           std::all_of(key.begin(), key.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

/// Removes every regular file in Directory() but `keep` (a file name; empty keeps none).
void RemoveOthers(const std::string& keep)
{
    std::error_code ec;
    const auto dir = util::LongPathSafe(UiBackgroundService::Directory());

    if (!std::filesystem::is_directory(dir, ec))
    {
        return;
    }

    std::vector<std::filesystem::path> doomed;

    for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
    {
        std::error_code entryEc;

        if (entry.is_regular_file(entryEc) && util::PathToUtf8(entry.path().filename()) != keep)
        {
            doomed.push_back(entry.path());
        }
    }

    for (const auto& path : doomed)
    {
        std::error_code removeEc;
        std::filesystem::remove(path, removeEc);
    }
}
} // namespace

UiBackgroundService::UiBackgroundService(IPluginHost& host, SendMessageFn sendMessage, ReportErrorFn reportError)
    : mHost(host), mSendMessage(std::move(sendMessage)), mReportError(std::move(reportError))
{
}

void UiBackgroundService::RegisterMessageHandlers(MessageHandlerRegistry& registry)
{
    registry.Register("browseUiBackgroundImage", [this](const nlohmann::json& payload) { HandleBrowse(payload); });
    registry.Register("clearUiBackgroundImage", [](const nlohmann::json&) { Clear(); });
}

std::filesystem::path UiBackgroundService::Directory()
{
    return FileSystem{}.ResolveDataV1Directory() / "ui-backgrounds";
}

std::string UiBackgroundService::MimeTypeFor(const std::filesystem::path& extension)
{
    const auto ext = LowerExtension(extension);

    if (ext == ".png")
    {
        return "image/png";
    }

    if (ext == ".jpg" || ext == ".jpeg")
    {
        return "image/jpeg";
    }

    if (ext == ".gif")
    {
        return "image/gif";
    }

    if (ext == ".webp")
    {
        return "image/webp";
    }

    return {};
}

std::optional<std::string> UiBackgroundService::Import(const std::filesystem::path& source, std::string& error)
{
    const auto ext = LowerExtension(source.extension());

    if (MimeTypeFor(ext).empty())
    {
        error = "Backgrounds take PNG, JPEG, GIF or WebP images";
        return std::nullopt;
    }

    std::error_code sizeEc;
    const auto size = std::filesystem::file_size(util::LongPathSafe(source), sizeEc);

    if (sizeEc)
    {
        error = "The image could not be read";
        return std::nullopt;
    }

    if (size > kMaxBytes)
    {
        error = "The image is larger than 32 MB";
        return std::nullopt;
    }

    const auto bytes = util::ReadFileBytes(source);

    if (bytes.empty())
    {
        error = "The image could not be read";
        return std::nullopt;
    }

    const auto fileName = KeyFor(bytes) + StoredExtension(ext);
    const auto dir = Directory();
    std::error_code dirEc;
    std::filesystem::create_directories(util::LongPathSafe(dir), dirEc);

    if (std::string writeError; !util::WriteFileAtomic(dir / util::PathFromUtf8(fileName), bytes, &writeError))
    {
        error = writeError.empty() ? "The image could not be saved" : writeError;
        return std::nullopt;
    }

    RemoveOthers(fileName);
    return std::string(kRoutePrefix) + fileName;
}

void UiBackgroundService::Clear()
{
    RemoveOthers({});
}

std::optional<std::pair<std::filesystem::path, std::string>> UiBackgroundService::ResolveUiResource(
    const std::string& resourcePath)
{
    if (!StartsWith(resourcePath, kRoutePrefix))
    {
        return std::nullopt;
    }

    const std::string_view name = std::string_view(resourcePath).substr(std::string_view(kRoutePrefix).size());
    const auto dot = name.find('.');

    if (dot == std::string_view::npos || !IsKey(name.substr(0, dot)))
    {
        return std::nullopt;
    }

    const auto extension = std::string(name.substr(dot));
    const auto mimeType = MimeTypeFor(extension);

    // Only the spelling Import stores under, so `x.JPEG` or `x.jpeg` never name a file.
    if (mimeType.empty() || StoredExtension(extension) != extension || LowerExtension(extension) != extension)
    {
        return std::nullopt;
    }

    const auto file = Directory() / util::PathFromUtf8(std::string(name));

    if (!util::PathExists(file))
    {
        return std::nullopt;
    }

    return std::make_pair(file, mimeType);
}

void UiBackgroundService::HandleBrowse(const nlohmann::json&)
{
    mHost.BrowseFileAsync(BrowseFileType::ImageFile, "Choose a Background Image",
                          [this](const BrowseFileResult& result) {
                              if (!result.success)
                              {
                                  return;
                              }

                              std::string error;
                              const auto url = Import(result.path, error);

                              if (!url)
                              {
                                  mReportError("Background image not set", error);
                                  return;
                              }

                              util::AppendSessionLog("UI background image set: " + *url);
                              mSendMessage(nlohmann::json{{"type", "uiBackgroundImageSelected"}, {"url", *url}}.dump());
                          });
}
} // namespace guitarfx
