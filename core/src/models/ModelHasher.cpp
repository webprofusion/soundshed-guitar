#include "ModelHasher.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <sstream>

namespace guitarfx
{
namespace
{
constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

void Fnv1aUpdate(std::uint64_t& hash, const std::uint8_t* data, std::size_t size)
{
    for (std::size_t i = 0; i < size; ++i)
    {
        hash ^= data[i];
        hash *= kFnvPrime;
    }
}

std::string ToHex(std::uint64_t hash)
{
    std::ostringstream stream;
    stream << std::hex << hash;
    return stream.str();
}
} // namespace

/// Computes a 64-bit FNV-1a hash of the file contents.
/// Reads the file in 4KB chunks, XORs each byte into the hash, and multiplies by
/// the FNV prime using the standard FNV-1a offset basis. Returns the hash as a
/// lowercase hexadecimal string, or an empty string if the file cannot be opened.
/// @param filePath Path to the file to hash.
/// @return Hexadecimal string of the 64-bit FNV-1a hash, or empty on failure.
std::string ModelHasher::HashFile(const std::filesystem::path& filePath) const
{
    std::ifstream input(filePath, std::ios::binary);

    if (!input)
    {
        return {};
    }

    std::uint64_t hash = kFnvOffset;
    std::array<char, 4096> buffer{};

    while (input.read(buffer.data(), buffer.size()) || input.gcount() > 0)
    {
        Fnv1aUpdate(hash, reinterpret_cast<const std::uint8_t*>(buffer.data()),
                    static_cast<std::size_t>(input.gcount()));
    }

    return ToHex(hash);
}

std::string ModelHasher::HashBytes(std::span<const std::uint8_t> bytes) const
{
    std::uint64_t hash = kFnvOffset;
    Fnv1aUpdate(hash, bytes.data(), bytes.size());
    return ToHex(hash);
}
} // namespace guitarfx
