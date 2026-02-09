#pragma once

#include <filesystem>
#include <string>

namespace guitarfx
{
  class ModelHasher
  {
  public:
    [[nodiscard]] std::string HashFile(const std::filesystem::path &filePath) const;
  };
} // namespace guitarfx
