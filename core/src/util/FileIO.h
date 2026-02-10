#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace guitarfx::util
{

[[nodiscard]] std::vector<std::uint8_t> ReadFileBytes(const std::filesystem::path& path);

} // namespace guitarfx::util
