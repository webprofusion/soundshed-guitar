#pragma once

// UiBackgroundService — the player's own picture behind the play page's effect panel, drawn in
// place of the theme's backdrop (Stage's stage, Eleven's grille) under every theme.
//
// - "browseUiBackgroundImage" asks the host for an image file, copies it into the profile and
//   answers "uiBackgroundImageSelected" with the URL the web UI draws it from. The UI keeps that
//   URL in the app setting ui.visualizerBackground.image, so the choice is shared by every
//   instance as the theme is.
// - "clearUiBackgroundImage" removes the copy.
//
// The copy lives in <profile>/data/v1/ui-backgrounds, named by a hash of its bytes, and is the
// only file there: picking another picture removes the one before. The web UI loads it through
// the editor's `ui-backgrounds/<file>` route (ResolveUiResource).
//
// Message thread only, but for the static functions, which touch no state.

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace guitarfx
{
class IPluginHost;
class MessageHandlerRegistry;

class UiBackgroundService
{
  public:
    using SendMessageFn = std::function<void(const std::string&)>;
    using ReportErrorFn = std::function<void(const std::string&, const std::string&)>;

    /// The largest picture taken. A backdrop is drawn once, at window size; anything bigger is
    /// a camera original that would only slow the page.
    static constexpr std::size_t kMaxBytes = 32u * 1024u * 1024u;

    /// The route prefix the web UI loads the picture through.
    static constexpr const char* kRoutePrefix = "ui-backgrounds/";

    UiBackgroundService(IPluginHost& host, SendMessageFn sendMessage, ReportErrorFn reportError);

    UiBackgroundService(const UiBackgroundService&) = delete;
    UiBackgroundService& operator=(const UiBackgroundService&) = delete;

    /// Registers "browseUiBackgroundImage" and "clearUiBackgroundImage" with the dispatcher.
    void RegisterMessageHandlers(MessageHandlerRegistry& registry);

    /// The folder: <profile>/data/v1/ui-backgrounds.
    [[nodiscard]] static std::filesystem::path Directory();

    /// The MIME type of a picture with this extension, or empty when it is not one taken
    /// (PNG, JPEG, GIF, WebP).
    [[nodiscard]] static std::string MimeTypeFor(const std::filesystem::path& extension);

    /// Copies the picture at `source` into Directory() and removes any other there. The URL the
    /// UI draws it from, or nullopt with `error` saying why not.
    [[nodiscard]] static std::optional<std::string> Import(const std::filesystem::path& source, std::string& error);

    /// Removes every picture in Directory().
    static void Clear();

    /// The file and MIME type for a UI resource path `ui-backgrounds/<key>.<ext>` (no query), or
    /// nullopt when the path is not one or the file is not there. Any thread.
    [[nodiscard]] static std::optional<std::pair<std::filesystem::path, std::string>> ResolveUiResource(
        const std::string& resourcePath);

  private:
    void HandleBrowse(const nlohmann::json& payload);

    IPluginHost& mHost;
    SendMessageFn mSendMessage;
    ReportErrorFn mReportError;
};
} // namespace guitarfx
