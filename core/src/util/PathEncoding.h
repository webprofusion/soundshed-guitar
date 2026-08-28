#pragma once

#include <filesystem>
#include <string>

namespace guitarfx::util
{
[[nodiscard]] inline std::filesystem::path PathFromUtf8(const std::string& value)
{
    return std::filesystem::u8path(value);
}

[[nodiscard]] inline std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto value = path.generic_u8string();
    return std::string(value.begin(), value.end());
}
} // namespace guitarfx::util