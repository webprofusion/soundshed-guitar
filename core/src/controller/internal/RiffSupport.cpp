#include "controller/internal/RiffSupport.h"

#include "controller/internal/ControllerUtils.h"

#include <system_error>

namespace guitarfx::controller_detail
{

std::filesystem::path ResolveRiffTakePathForRuntime(const std::filesystem::path& storedPath,
                                                    const std::filesystem::path& libraryPath)
{
    if (storedPath.empty() || storedPath.is_absolute())
    {
        return storedPath;
    }

    if (HasUnsafeRelativeSegments(storedPath))
    {
        return storedPath;
    }

    return (libraryPath / storedPath).lexically_normal();
}

std::filesystem::path BuildRiffTakePathForStorage(const std::filesystem::path& runtimePath,
                                                  const std::filesystem::path& libraryPath)
{
    if (runtimePath.empty())
    {
        return runtimePath;
    }

    std::error_code ec;
    auto normalizedRuntimePath = std::filesystem::weakly_canonical(runtimePath, ec);
    if (ec)
    {
        normalizedRuntimePath = runtimePath.lexically_normal();
    }

    ec.clear();
    auto normalizedLibraryPath = std::filesystem::weakly_canonical(libraryPath, ec);
    if (ec)
    {
        normalizedLibraryPath = libraryPath.lexically_normal();
    }

    const auto relativePath = normalizedRuntimePath.lexically_relative(normalizedLibraryPath);
    if (!relativePath.empty() && !relativePath.is_absolute() && !HasUnsafeRelativeSegments(relativePath))
    {
        return relativePath;
    }

    return runtimePath;
}

} // namespace guitarfx::controller_detail
