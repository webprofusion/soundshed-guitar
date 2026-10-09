#include "Log.h"

#include "Paths.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace nanoq
{
void Log(const std::string& line)
{
    try
    {
        static std::mutex mutex;
        std::lock_guard<std::mutex> lock(mutex);

        const auto folder = ProfileFolder() / "logs";
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);

        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm local{};
#if defined(_WIN32)
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);

        std::ofstream out(folder / "nanoq.log", std::ios::app);
        out << stamp << "  " << line << '\n';
    }
    catch (...)
    {
    }
}
} // namespace nanoq
