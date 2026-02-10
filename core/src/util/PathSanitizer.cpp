#include "PathSanitizer.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace
{
    std::string ToUpperAscii(std::string value)
    {
        for (char& c : value) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return value;
    }

    bool IsWindowsReservedName(const std::string& name)
    {
        if (name.empty()) return false;
        const auto dotPos = name.find('.');
        const std::string upper = ToUpperAscii(dotPos == std::string::npos ? name : name.substr(0, dotPos));
        static const std::array<const char*, 22> kReserved = {
            "CON", "PRN", "AUX", "NUL",
            "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
            "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
        };
        return std::any_of(kReserved.begin(), kReserved.end(), [&](const char* r) { return upper == r; });
    }
}

namespace guitarfx::util
{

std::string SanitizePathSegment(const std::string& raw, bool allowDots)
{
    std::string result;
    result.reserve(raw.size());
    for (unsigned char c : raw)
    {
        if (std::isalnum(c) || c == '-' || c == '_') result.push_back(static_cast<char>(c));
        else if (allowDots && c == '.') result.push_back('.');
        else if (std::isspace(c)) result.push_back('_');
    }
    while (!result.empty() && result.front() == '.') result.erase(result.begin());
    while (!result.empty() && result.back() == '.') result.pop_back();
    if (result.empty() || result == "." || result == "..") result = "resource";
    if (IsWindowsReservedName(result)) result = "_" + result;
    return result;
}

std::filesystem::path SanitizeSubfolderPath(const std::string& raw)
{
    std::filesystem::path result;
    std::string segment;
    auto push = [&]() {
        if (segment.empty()) return;
        std::string s = SanitizePathSegment(segment, true);
        if (!s.empty() && s != "." && s != "..") result /= s;
        segment.clear();
    };
    for (char c : raw) { if (c == '/' || c == '\\') push(); else segment.push_back(c); }
    push();
    return result;
}

std::string SanitizeFilename(const std::string& raw)
{
    return SanitizePathSegment(raw, true);
}

} // namespace guitarfx::util
