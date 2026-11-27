/**
 * UpdateCheck.cpp - version comparison for the update check (see UpdateCheck.h).
 */

#include "uiclient/UpdateCheck.h"

#include <cctype>
#include <sstream>

namespace guitarfx::uiclient
{
std::optional<std::array<int, 3>> ParseVersion(const std::string& version)
{
    // As updateCheck.ts's toNumericSemver: leading non-digits skipped, then up to three numbers
    // joined by dots, then optionally a -prerelease or +build suffix and nothing else.
    std::size_t i = 0;
    const auto n = version.size();

    while (i < n && std::isspace(static_cast<unsigned char>(version[i])))
    {
        ++i;
    }

    while (i < n && !std::isdigit(static_cast<unsigned char>(version[i])))
    {
        ++i;
    }

    if (i == n)
    {
        return std::nullopt;
    }

    std::array<int, 3> parts{0, 0, 0};

    for (int part = 0; part < 3; ++part)
    {
        if (i >= n || !std::isdigit(static_cast<unsigned char>(version[i])))
        {
            if (part == 0)
            {
                return std::nullopt;
            }

            break;
        }

        long value = 0;

        while (i < n && std::isdigit(static_cast<unsigned char>(version[i])))
        {
            value = value * 10 + (version[i] - '0');

            if (value > 1000000000L)
            {
                return std::nullopt;
            }

            ++i;
        }

        parts[static_cast<std::size_t>(part)] = static_cast<int>(value);

        if (part < 2 && i < n && version[i] == '.' && i + 1 < n &&
            std::isdigit(static_cast<unsigned char>(version[i + 1])))
        {
            ++i;
        }
        else
        {
            break;
        }
    }

    // What is left must be a suffix or trailing space.
    if (i < n && version[i] != '-' && version[i] != '+')
    {
        while (i < n && std::isspace(static_cast<unsigned char>(version[i])))
        {
            ++i;
        }

        if (i < n)
        {
            return std::nullopt;
        }
    }

    return parts;
}

bool IsVersionNewer(const std::string& candidate, const std::string& current)
{
    const auto a = ParseVersion(candidate);
    const auto b = ParseVersion(current);
    return a && b && *a > *b;
}

namespace
{
/// The text of a line with its markup taken out: [label](url) and ![label](url) as the label,
/// and ** and * as nothing.
std::string PlainInline(const std::string& line)
{
    std::string out;
    out.reserve(line.size());

    for (std::size_t i = 0; i < line.size();)
    {
        const bool image = line[i] == '!' && i + 1 < line.size() && line[i + 1] == '[';

        if (line[i] == '[' || image)
        {
            const std::size_t open = image ? i + 1 : i;
            const auto close = line.find(']', open + 1);

            if (close != std::string::npos && close + 1 < line.size() && line[close + 1] == '(')
            {
                const auto end = line.find(')', close + 2);

                if (end != std::string::npos)
                {
                    out += line.substr(open + 1, close - open - 1);
                    i = end + 1;
                    continue;
                }
            }
        }

        if (line[i] != '*')
        {
            out += line[i];
        }

        ++i;
    }

    return out;
}

std::string Trimmed(const std::string& text)
{
    const auto first = text.find_first_not_of(" \t\r");

    if (first == std::string::npos)
    {
        return {};
    }

    return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}
} // namespace

std::vector<ReleaseNoteLine> ReleaseNoteLines(const std::string& notes)
{
    std::vector<ReleaseNoteLine> lines;
    std::istringstream in(notes);
    std::string raw;

    while (std::getline(in, raw))
    {
        if (!raw.empty() && raw.back() == '\r')
        {
            raw.pop_back();
        }

        ReleaseNoteLine line;
        const auto trimmed = Trimmed(raw);

        if (trimmed.empty())
        {
            // Runs of blank lines are one gap, and none leads.
            if (!lines.empty() && lines.back().kind != ReleaseNoteLine::Kind::Blank)
            {
                line.kind = ReleaseNoteLine::Kind::Blank;
                lines.push_back(line);
            }

            continue;
        }

        std::size_t hashes = 0;

        while (hashes < raw.size() && raw[hashes] == '#')
        {
            ++hashes;
        }

        if (hashes >= 1 && hashes <= 3 && hashes < raw.size() && raw[hashes] == ' ')
        {
            line.kind = ReleaseNoteLine::Kind::Heading;
            line.level = static_cast<int>(hashes);
            line.text = PlainInline(Trimmed(raw.substr(hashes + 1)));
        }
        else if (raw.rfind("> ", 0) == 0)
        {
            line.kind = ReleaseNoteLine::Kind::Quote;
            line.text = PlainInline(Trimmed(raw.substr(2)));
        }
        else if (trimmed.size() > 2 && (trimmed[0] == '-' || trimmed[0] == '*') && trimmed[1] == ' ')
        {
            line.kind = ReleaseNoteLine::Kind::Bullet;
            line.text = PlainInline(Trimmed(trimmed.substr(2)));
        }
        else
        {
            line.text = PlainInline(trimmed);
        }

        lines.push_back(std::move(line));
    }

    while (!lines.empty() && lines.back().kind == ReleaseNoteLine::Kind::Blank)
    {
        lines.pop_back();
    }

    return lines;
}

std::string ReleaseNotesFirstLine(const std::string& notes)
{
    for (const auto& line : ReleaseNoteLines(notes))
    {
        if (line.kind != ReleaseNoteLine::Kind::Blank && !line.text.empty())
        {
            return line.text;
        }
    }

    return {};
}
} // namespace guitarfx::uiclient
