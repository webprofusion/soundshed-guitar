#include "util/SessionLog.h"

#include "util/FileSystem.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>

namespace guitarfx::util
{
namespace
{
// Never destroyed, so a line logged from a static destructor at exit still finds it.
std::mutex& LogMutex()
{
    static auto* mutex = new std::mutex();
    return *mutex;
}

std::string LocalTimestamp()
{
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm localTime{};
#ifdef _WIN32
    localtime_s(&localTime, &now);
#else
    localtime_r(&now, &localTime);
#endif
    std::ostringstream oss;
    oss << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}
} // namespace

std::filesystem::path SessionLogPath(const std::filesystem::path& settingsDirectory)
{
    return settingsDirectory / "logs" / "session-log.txt";
}

void AppendSessionLog(const std::filesystem::path& settingsDirectory, std::string_view message)
{
    if (message.empty())
    {
        return;
    }

    try
    {
        const auto logPath =
            SessionLogPath(settingsDirectory.empty() ? FileSystem().ResolveSettingsDirectory() : settingsDirectory);
        const std::lock_guard<std::mutex> lock(LogMutex());
        std::error_code ec;
        std::filesystem::create_directories(logPath.parent_path(), ec);
        std::ofstream output(logPath, std::ios::app);

        if (!output)
        {
            // A profile that refuses writes would otherwise lose the very line saying so.
            const auto fallback = std::filesystem::temp_directory_path(ec) / "Sepia" / "session-log.txt";

            if (!ec)
            {
                std::filesystem::create_directories(fallback.parent_path(), ec);
                output.open(fallback, std::ios::app);
            }
        }

        if (output)
        {
            output << LocalTimestamp() << ' ' << message << '\n';
        }
    }
    catch (...)
    {
        // A diagnostic must never take its caller down with it.
    }
}

void AppendSessionLog(std::string_view message)
{
    AppendSessionLog(FileSystem().ResolveSettingsDirectory(), message);
}
} // namespace guitarfx::util
