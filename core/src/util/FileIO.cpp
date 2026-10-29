#include "FileIO.h"

#include "LongPath.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <random>
#include <system_error>
#include <thread>

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#endif

namespace guitarfx::util
{
namespace
{
/// How long a replace keeps retrying while readers hold the target open. Readers
/// parse a model and close it within milliseconds; this only runs out when the
/// failure is not transient after all (a read-only target, say).
constexpr auto kReplaceRetryBudget = std::chrono::seconds(2);
constexpr auto kReplaceMaxBackoff = std::chrono::milliseconds(50);

/// `.<12 hex digits>.tmp` beside the target. Same directory, so the rename never
/// crosses a volume; `.tmp` last, so extension-based library scans skip it.
///
/// The target's own name is deliberately left out. Windows caps a path at MAX_PATH
/// unless the host process opts out, and appending to the name pushed a target that
/// only just fitted (a session preset's file name repeats its archive key) over the
/// limit, so it could not be written at all.
std::filesystem::path MakeTemporarySibling(const std::filesystem::path& target)
{
    // random_device alone would do across processes; the counter guarantees two
    // threads of one process never collide even if it were deterministic.
    static std::atomic<std::uint64_t> counter{0};
    std::random_device device;
    const std::uint64_t entropy =
        (static_cast<std::uint64_t>(device()) << 32) ^ device() ^ (counter.fetch_add(1) * 0x9E3779B97F4A7C15ull) ^
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());

    static constexpr char kHex[] = "0123456789abcdef";
    std::string name = ".";

    for (std::size_t i = 0; i < 12; ++i)
    {
        name += kHex[(entropy >> (i * 4)) & 0xF];
    }

    return target.parent_path() / (name + ".tmp");
}

/// Renames `from` to `to`. On Windows, unless `replace` is set, an existing `to`
/// is left untouched and the call fails with ERROR_ALREADY_EXISTS.
std::error_code MoveIntoPlace(const std::filesystem::path& from, const std::filesystem::path& to, bool replace)
{
#ifdef _WIN32

    // Explicitly without MOVEFILE_COPY_ALLOWED: a copy fallback would be exactly
    // the non-atomic write this exists to avoid.
    if (MoveFileExW(from.c_str(), to.c_str(), replace ? MOVEFILE_REPLACE_EXISTING : 0))
    {
        return {};
    }

    return {static_cast<int>(GetLastError()), std::system_category()};
#else
    // POSIX rename always replaces, and a reader keeps the file it already opened. The
    // create-only step is still a check, so a target that already holds these bytes is
    // left alone here too rather than rewritten for nothing; a racing create is harmless,
    // as the rename then just replaces it.
    std::error_code ec;

    if (!replace && std::filesystem::exists(to, ec))
    {
        return std::make_error_code(std::errc::file_exists);
    }

    std::filesystem::rename(from, to, ec);
    return ec;
#endif
}

bool IsAlreadyExists(const std::error_code& ec)
{
#ifdef _WIN32
    const auto code = static_cast<DWORD>(ec.value());
    return code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS;
#else
    return ec == std::errc::file_exists;
#endif
}

bool IsTransientReplaceFailure(const std::error_code& ec)
{
#ifdef _WIN32
    // std::ifstream opens without FILE_SHARE_DELETE, so any reader with the
    // target open makes a replace fail with one of these until it closes.
    const auto code = static_cast<DWORD>(ec.value());
    return code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION;
#else
    // POSIX rename replaces a file that is open; nothing here is worth waiting on.
    return ec == std::errc::device_or_resource_busy;
#endif
}

void SetError(std::string* error, std::string message)
{
    if (error)
    {
        *error = std::move(message);
    }
}
} // namespace

std::vector<std::uint8_t> ReadFileBytes(const std::filesystem::path& path)
{
    std::ifstream input(LongPathSafe(path), std::ios::binary);

    if (!input)
    {
        return {};
    }

    input.seekg(0, std::ios::end);
    const auto size = input.tellg();

    if (size <= 0)
    {
        return {};
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.seekg(0, std::ios::beg);
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    return bytes;
}

bool WriteFileAtomic(const std::filesystem::path& target, std::span<const std::uint8_t> data, std::string* error)
{
    const auto osTarget = LongPathSafe(target);
    const auto temporary = LongPathSafe(MakeTemporarySibling(target));
    std::error_code removeEc;

    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);

        if (!output.is_open())
        {
            SetError(error, "could not create a temporary file beside the target");
            return false;
        }

        if (!data.empty())
        {
            output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }

        output.flush();
        const bool written = output.good();
        output.close();

        // Checked, unlike a plain in-place write: a short write (a full disk) must
        // not be renamed over a good file.
        if (!written || output.fail())
        {
            std::filesystem::remove(temporary, removeEc);
            SetError(error, "could not write the temporary file");
            return false;
        }
    }

    // On Windows a replace disturbs readers two ways: one holding the target open
    // blocks it (hence the retries), and one opening the target at the instant it
    // is replaced fails to open it at all — a failed model load, just like a torn
    // file. So a target that already holds these bytes is never replaced. When
    // several instances extract the same resource, the first creates it and the
    // rest leave it alone, without disturbing an instance already loading it.
    const auto deadline = std::chrono::steady_clock::now() + kReplaceRetryBudget;
    auto backoff = std::chrono::milliseconds(1);

    while (true)
    {
        auto moveEc = MoveIntoPlace(temporary, osTarget, false);

        if (moveEc && IsAlreadyExists(moveEc))
        {
            if (FileContentEquals(target, data))
            {
                std::filesystem::remove(temporary, removeEc);
                return true;
            }

            moveEc = MoveIntoPlace(temporary, osTarget, true);
        }

        if (!moveEc)
        {
            return true;
        }

        // Blocked by a reader, but another instance may meanwhile have put these
        // very bytes in place.
        if (FileContentEquals(target, data))
        {
            std::filesystem::remove(temporary, removeEc);
            return true;
        }

        if (!IsTransientReplaceFailure(moveEc) || std::chrono::steady_clock::now() >= deadline)
        {
            std::filesystem::remove(temporary, removeEc);
            SetError(error, "could not replace the target: " + moveEc.message());
            return false;
        }

        std::this_thread::sleep_for(backoff);
        backoff = std::min(backoff * 2, kReplaceMaxBackoff);
    }
}

bool WriteTextFileAtomic(const std::filesystem::path& target, std::string_view text, std::string* error)
{
    return WriteFileAtomic(target, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()), error);
}

bool FileContentEquals(const std::filesystem::path& path, std::span<const std::uint8_t> data)
{
    const auto osPath = LongPathSafe(path);
    std::error_code ec;
    const auto onDisk = std::filesystem::file_size(osPath, ec);

    if (ec || onDisk != data.size())
    {
        return false;
    }

    std::ifstream input(osPath, std::ios::binary);

    if (!input)
    {
        return false;
    }

    std::vector<char> buffer(std::min<std::size_t>(data.size(), 256 * 1024));
    std::size_t offset = 0;

    while (offset < data.size())
    {
        const auto chunk = std::min(buffer.size(), data.size() - offset);
        input.read(buffer.data(), static_cast<std::streamsize>(chunk));

        if (static_cast<std::size_t>(input.gcount()) != chunk ||
            std::memcmp(buffer.data(), data.data() + offset, chunk) != 0)
        {
            return false;
        }

        offset += chunk;
    }

    return input.peek() == std::char_traits<char>::eof();
}
} // namespace guitarfx::util
