#pragma once

#include <filesystem>
#include <string>

namespace guitarfx::util
{

[[nodiscard]] std::string SanitizePathSegment(const std::string& raw, bool allowDots);
[[nodiscard]] std::filesystem::path SanitizeSubfolderPath(const std::string& raw);
[[nodiscard]] std::string SanitizeFilename(const std::string& raw);

} // namespace guitarfx::util
