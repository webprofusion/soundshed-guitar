#pragma once

/**
 * UpdateCheck - whether a newer Soundshed build is out (the web UI's updateCheck.ts), for the
 * native UIs: the version comparison here, the request in ToneService::CheckForUpdate. The check
 * runs once a session, a few seconds after the first state, unless app.updateCheckEnabled is
 * false; the service answers with its newest release, which counts only when it is newer.
 */

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace guitarfx::uiclient
{
inline constexpr const char* kUpdateCheckEnabledSetting = "app.updateCheckEnabled";
inline constexpr const char* kInstanceIdSetting = "app.instanceId";

struct AvailableUpdate
{
    std::string version;
    std::string downloadUrl; // http(s) only; empty otherwise
    std::string releaseNotes;
};

/// "v1.7.2-beta" as {1, 7, 2}; nothing for text that is not a version.
[[nodiscard]] std::optional<std::array<int, 3>> ParseVersion(const std::string& version);

/// True only when both parse and `candidate` is higher than `current`.
[[nodiscard]] bool IsVersionNewer(const std::string& candidate, const std::string& current);

/// One line of release notes as the update dialog draws it (updateCheck.ts's renderMarkdown):
/// "# " to "### " headings, "> " quotes, "- " and "* " bullets, blank lines, and text. Emphasis
/// markers are taken out and a link or image keeps its label, so nothing from the server is
/// followed.
struct ReleaseNoteLine
{
    enum class Kind
    {
        Text,
        Heading,
        Quote,
        Bullet,
        Blank
    };

    Kind kind = Kind::Text;
    int level = 0; // a heading's 1 to 3
    std::string text;
};

[[nodiscard]] std::vector<ReleaseNoteLine> ReleaseNoteLines(const std::string& notes);

/// The notes' first line with text, plain (the Settings banner's, updateBanner.ts).
[[nodiscard]] std::string ReleaseNotesFirstLine(const std::string& notes);
} // namespace guitarfx::uiclient
