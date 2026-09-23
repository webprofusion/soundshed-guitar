#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace guitarfx
{
class ModelHasher
{
  public:
    [[nodiscard]] std::string HashFile(const std::filesystem::path& filePath) const;
    /// The same hash HashFile() gives a file holding exactly these bytes.
    [[nodiscard]] std::string HashBytes(std::span<const std::uint8_t> bytes) const;
};
} // namespace guitarfx
