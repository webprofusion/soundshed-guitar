/**
 * LayoutImages.cpp - how the UI addresses a layout image (its URL, its renditions' names and
 * URLs), what kind of image a file is (its MIME type, whether its bytes are what its extension
 * says) and the pixel size read from its header. LayoutSupport.cpp has where the files live.
 */

#include "controller/internal/LayoutSupport.h"
#include "controller/internal/LayoutText.h"

#include "util/FileSystem.h"
#include "util/PathSanitizer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace guitarfx::controller_detail
{
using layout_text::LowerAscii;
using layout_text::LowerExtension;
using layout_text::StartsWith;

namespace
{
/// `value` with everything but A-Z a-z 0-9 - _ . ~ percent-encoded, for one URL path segment.
std::string PercentEncode(const std::string& value)
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string encoded;

    for (const char c : value)
    {
        const auto byte = static_cast<unsigned char>(c);

        if (std::isalnum(byte) != 0 || c == '-' || c == '_' || c == '.' || c == '~')
        {
            encoded += c;
        }
        else
        {
            encoded += '%';
            encoded += kHex[byte >> 4];
            encoded += kHex[byte & 0x0F];
        }
    }

    return encoded;
}

/// One percent-encoded path segment decoded into an image id, or empty when it holds a separator,
/// a bad escape, or decodes to something that could name another folder.
std::string DecodeImageIdSegment(std::string_view segment)
{
    const auto hexValue = [](char c) -> int {
        if (c >= '0' && c <= '9')
        {
            return c - '0';
        }

        if (c >= 'a' && c <= 'f')
        {
            return c - 'a' + 10;
        }

        if (c >= 'A' && c <= 'F')
        {
            return c - 'A' + 10;
        }

        return -1;
    };

    std::string id;

    for (std::size_t i = 0; i < segment.size(); ++i)
    {
        const char c = segment[i];

        if (c == '/' || c == '\\' || c == '?' || c == '#')
        {
            return {};
        }

        if (c != '%')
        {
            id += c;
            continue;
        }

        const int high = i + 2 < segment.size() ? hexValue(segment[i + 1]) : -1;
        const int low = i + 2 < segment.size() ? hexValue(segment[i + 2]) : -1;

        if (high < 0 || low < 0)
        {
            return {};
        }

        id += static_cast<char>((high << 4) | low);
        i += 2;
    }

    // An id is a file stem: nothing that could name another folder once decoded.
    return id.find_first_of("/\\") == std::string::npos && id != "." && id != ".." ? id : std::string{};
}

constexpr std::string_view kImageRoutePrefix = "layout-images/id/";
constexpr std::string_view kRenditionRoutePrefix = "layout-images/rendition/";
} // namespace

std::string LayoutImageUrl(const std::string& imageId, std::uint64_t version)
{
    return std::string(kImageRoutePrefix) + PercentEncode(imageId) + "?v=" + std::to_string(version);
}

std::string LayoutImageIdFromResourcePath(const std::string& path)
{
    if (path.size() <= kImageRoutePrefix.size() || path.compare(0, kImageRoutePrefix.size(), kImageRoutePrefix) != 0)
    {
        return {};
    }

    return DecodeImageIdSegment(std::string_view(path).substr(kImageRoutePrefix.size()));
}

std::uint64_t LayoutImageVersion(const std::filesystem::path& file)
{
    std::error_code ec;
    const auto modified = std::filesystem::last_write_time(file, ec);
    return ec ? 0 : static_cast<std::uint64_t>(modified.time_since_epoch().count());
}

bool IsLayoutRenditionEdge(int edge)
{
    return edge == 1024 || edge == 2048 || edge == 4096;
}

std::filesystem::path ResolveLayoutRenditionsDir(const FileSystem& fileSystem)
{
    return fileSystem.ResolveSettingsDirectory() / "layouts" / "renditions";
}

std::string LayoutRenditionFileName(const std::string& imageId, int edge, std::uint64_t version)
{
    // FNV-1a of the original id: SanitizeFilename can map two ids to one name.
    std::uint32_t hash = 2166136261u;

    for (const char c : imageId)
    {
        hash = (hash ^ static_cast<unsigned char>(c)) * 16777619u;
    }

    static constexpr char kHex[] = "0123456789abcdef";
    std::string hashText;

    for (int shift = 28; shift >= 0; shift -= 4)
    {
        hashText += kHex[(hash >> shift) & 0x0F];
    }

    return util::SanitizeFilename(imageId) + "-" + hashText + "@" + std::to_string(edge) + "-" +
           std::to_string(version) + ".webp";
}

std::string LayoutRenditionUrl(const std::string& imageId, int edge, std::uint64_t version)
{
    return std::string(kRenditionRoutePrefix) + PercentEncode(imageId) + "/" + std::to_string(edge) +
           "?v=" + std::to_string(version);
}

std::optional<std::pair<std::string, int>> LayoutRenditionFromResourcePath(const std::string& path)
{
    if (path.size() <= kRenditionRoutePrefix.size() ||
        path.compare(0, kRenditionRoutePrefix.size(), kRenditionRoutePrefix) != 0)
    {
        return std::nullopt;
    }

    const std::string_view rest = std::string_view(path).substr(kRenditionRoutePrefix.size());
    const auto slash = rest.rfind('/');

    if (slash == std::string_view::npos || slash == 0 || slash + 1 >= rest.size())
    {
        return std::nullopt;
    }

    const std::string_view edgeText = rest.substr(slash + 1);

    if (edgeText.size() > 4 ||
        !std::all_of(edgeText.begin(), edgeText.end(), [](char c) { return c >= '0' && c <= '9'; }))
    {
        return std::nullopt;
    }

    const int edge = std::stoi(std::string(edgeText));
    std::string id = DecodeImageIdSegment(rest.substr(0, slash));

    if (id.empty() || !IsLayoutRenditionEdge(edge))
    {
        return std::nullopt;
    }

    return std::make_pair(std::move(id), edge);
}

namespace
{
std::uint32_t BigEndian(const std::vector<std::uint8_t>& b, std::size_t at, std::size_t count)
{
    std::uint32_t value = 0;

    for (std::size_t i = 0; i < count; ++i)
    {
        value = (value << 8) | b[at + i];
    }

    return value;
}

std::uint32_t LittleEndian(const std::vector<std::uint8_t>& b, std::size_t at, std::size_t count)
{
    std::uint32_t value = 0;

    for (std::size_t i = count; i > 0; --i)
    {
        value = (value << 8) | b[at + i - 1];
    }

    return value;
}

/// A JPEG's frame size, walking its markers from the file so a large EXIF block is skipped
/// rather than read.
std::optional<LayoutImageSize> ReadJpegSize(std::ifstream& input)
{
    input.seekg(2);

    while (input)
    {
        int byte = input.get();

        if (byte != 0xFF)
        {
            return std::nullopt;
        }

        while (byte == 0xFF)
        {
            byte = input.get();
        }

        if (byte < 0)
        {
            return std::nullopt;
        }

        const auto marker = static_cast<std::uint8_t>(byte);

        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8))
        {
            continue; // Markers with no length.
        }

        if (marker == 0xD9 || marker == 0xDA)
        {
            return std::nullopt; // End of image, or scan data with no frame header before it.
        }

        std::array<char, 2> lengthBytes{};

        if (!input.read(lengthBytes.data(), 2))
        {
            return std::nullopt;
        }

        const auto length = static_cast<std::uint16_t>((static_cast<std::uint8_t>(lengthBytes[0]) << 8) |
                                                       static_cast<std::uint8_t>(lengthBytes[1]));
        const bool isFrame = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;

        if (isFrame)
        {
            std::array<char, 5> frame{};

            if (!input.read(frame.data(), 5))
            {
                return std::nullopt;
            }

            const auto height = static_cast<std::uint32_t>((static_cast<std::uint8_t>(frame[1]) << 8) |
                                                           static_cast<std::uint8_t>(frame[2]));
            const auto width = static_cast<std::uint32_t>((static_cast<std::uint8_t>(frame[3]) << 8) |
                                                          static_cast<std::uint8_t>(frame[4]));
            return width > 0 && height > 0 ? std::optional<LayoutImageSize>({width, height}) : std::nullopt;
        }

        if (length < 2)
        {
            return std::nullopt;
        }

        input.seekg(length - 2, std::ios::cur);
    }

    return std::nullopt;
}

/// The number at the start of an SVG length ("640", "640px", "48.5"), or 0.
std::uint32_t SvgLength(const std::string& value)
{
    std::size_t used = 0;

    try
    {
        const double number = std::stod(value, &used);
        return used > 0 && number > 0 && number < 1e6 ? static_cast<std::uint32_t>(number + 0.5) : 0;
    }
    catch (...)
    {
        return 0;
    }
}

/// The value of attribute `name` in an SVG start tag, or empty.
std::string SvgAttribute(const std::string& tag, const std::string& name)
{
    std::size_t at = 0;

    while ((at = tag.find(name, at)) != std::string::npos)
    {
        const bool startsWord = at == 0 || std::isspace(static_cast<unsigned char>(tag[at - 1])) != 0;
        std::size_t next = at + name.size();

        while (next < tag.size() && std::isspace(static_cast<unsigned char>(tag[next])) != 0)
        {
            ++next;
        }

        if (startsWord && next < tag.size() && tag[next] == '=')
        {
            ++next;

            while (next < tag.size() && std::isspace(static_cast<unsigned char>(tag[next])) != 0)
            {
                ++next;
            }

            if (next < tag.size() && (tag[next] == '"' || tag[next] == '\''))
            {
                const auto end = tag.find(tag[next], next + 1);
                return end == std::string::npos ? std::string{} : tag.substr(next + 1, end - next - 1);
            }
        }

        at += name.size();
    }

    return {};
}

std::optional<LayoutImageSize> ReadSvgSize(const std::vector<std::uint8_t>& head)
{
    const std::string text(head.begin(), head.end());
    const auto open = LowerAscii(text).find("<svg");

    if (open == std::string::npos)
    {
        return std::nullopt;
    }

    const auto close = text.find('>', open);
    const std::string tag = text.substr(open, close == std::string::npos ? std::string::npos : close - open);
    const std::uint32_t width = SvgLength(SvgAttribute(tag, "width"));
    const std::uint32_t height = SvgLength(SvgAttribute(tag, "height"));

    if (width > 0 && height > 0)
    {
        return LayoutImageSize{width, height};
    }

    std::istringstream viewBox(SvgAttribute(tag, "viewBox"));
    double minX = 0;
    double minY = 0;
    double boxWidth = 0;
    double boxHeight = 0;

    if (viewBox >> minX >> minY >> boxWidth >> boxHeight && boxWidth > 0 && boxHeight > 0)
    {
        return LayoutImageSize{static_cast<std::uint32_t>(boxWidth + 0.5), static_cast<std::uint32_t>(boxHeight + 0.5)};
    }

    return std::nullopt;
}
} // namespace

std::optional<LayoutImageSize> ReadLayoutImageSize(const std::filesystem::path& path)
{
    try
    {
        std::ifstream input(path, std::ios::binary);

        if (!input)
        {
            return std::nullopt;
        }

        std::vector<std::uint8_t> head(4096);
        input.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
        head.resize(static_cast<std::size_t>(input.gcount()));
        input.clear();

        const auto sized = [](std::uint32_t width, std::uint32_t height) -> std::optional<LayoutImageSize> {
            return width > 0 && height > 0 ? std::optional<LayoutImageSize>({width, height}) : std::nullopt;
        };

        if (StartsWith(head, 0, "\x89PNG\r\n\x1a\n") && head.size() >= 24 && StartsWith(head, 12, "IHDR"))
        {
            return sized(BigEndian(head, 16, 4), BigEndian(head, 20, 4));
        }

        if ((StartsWith(head, 0, "GIF87a") || StartsWith(head, 0, "GIF89a")) && head.size() >= 10)
        {
            return sized(LittleEndian(head, 6, 2), LittleEndian(head, 8, 2));
        }

        if (StartsWith(head, 0, "RIFF") && StartsWith(head, 8, "WEBP") && head.size() >= 30)
        {
            if (StartsWith(head, 12, "VP8 "))
            {
                return sized(LittleEndian(head, 26, 2) & 0x3FFF, LittleEndian(head, 28, 2) & 0x3FFF);
            }

            if (StartsWith(head, 12, "VP8L"))
            {
                const std::uint32_t bits = LittleEndian(head, 21, 4);
                return sized((bits & 0x3FFF) + 1, ((bits >> 14) & 0x3FFF) + 1);
            }

            if (StartsWith(head, 12, "VP8X"))
            {
                return sized(LittleEndian(head, 24, 3) + 1, LittleEndian(head, 27, 3) + 1);
            }

            return std::nullopt;
        }

        if (StartsWith(head, 0, "\xFF\xD8\xFF"))
        {
            return ReadJpegSize(input);
        }

        if (LowerExtension(path.extension()) == ".svg")
        {
            return ReadSvgSize(head);
        }
    }
    catch (...)
    {
    }

    return std::nullopt;
}

std::string LayoutImageMimeType(const std::filesystem::path& extension)
{
    const std::string ext = LowerExtension(extension);

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

    if (ext == ".svg")
    {
        return "image/svg+xml";
    }

    return {};
}

bool LooksLikeLayoutImage(const std::vector<std::uint8_t>& bytes, const std::filesystem::path& extension)
{
    const std::string ext = LowerExtension(extension);

    if (ext == ".png")
    {
        return StartsWith(bytes, 0, "\x89PNG\r\n\x1a\n");
    }

    if (ext == ".jpg" || ext == ".jpeg")
    {
        return StartsWith(bytes, 0, "\xFF\xD8\xFF");
    }

    if (ext == ".gif")
    {
        return StartsWith(bytes, 0, "GIF87a") || StartsWith(bytes, 0, "GIF89a");
    }

    if (ext == ".webp")
    {
        return StartsWith(bytes, 0, "RIFF") && StartsWith(bytes, 8, "WEBP");
    }

    if (ext == ".svg")
    {
        // Markup, optionally after a byte-order mark and whitespace, with an <svg> element early on.
        std::size_t start = StartsWith(bytes, 0, "\xEF\xBB\xBF") ? 3 : 0;

        while (start < bytes.size() && std::isspace(bytes[start]) != 0)
        {
            ++start;
        }

        if (start >= bytes.size() || bytes[start] != '<')
        {
            return false;
        }

        const std::size_t end = std::min(bytes.size(), start + 4096);
        const std::string head(bytes.begin() + static_cast<std::ptrdiff_t>(start),
                               bytes.begin() + static_cast<std::ptrdiff_t>(end));
        return LowerAscii(head).find("<svg") != std::string::npos;
    }

    return false;
}

bool IsLayoutImageLicense(std::string_view license)
{
    static constexpr std::array<std::string_view, 9> kLicenses = {"CC0-1.0",         "CC-BY-4.0",    "CC-BY-SA-4.0",
                                                                  "CC-BY-ND-4.0",    "CC-BY-NC-4.0", "CC-BY-NC-SA-4.0",
                                                                  "CC-BY-NC-ND-4.0", "own",          "other"};
    return std::find(kLicenses.begin(), kLicenses.end(), license) != kLicenses.end();
}
} // namespace guitarfx::controller_detail
