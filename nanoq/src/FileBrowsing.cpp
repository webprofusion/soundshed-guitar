#include "FileBrowsing.h"

#include "util/PathEncoding.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string_view>
#include <system_error>

namespace nanoq
{
namespace
{
using guitarfx::BrowseFileType;

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool EndsWith(const std::string& text, std::string_view ending)
{
    return text.size() >= ending.size() && text.compare(text.size() - ending.size(), ending.size(), ending) == 0;
}

/// The endings a type offers, in lower case. Empty: every file. The Windows dialog's filters
/// (FileDialogs.cpp), which show every file for what has no filter of its own.
std::vector<std::string_view> Endings(BrowseFileType type)
{
    switch (type)
    {
    case BrowseFileType::NAMModel:
        return {".nam"};
    case BrowseFileType::IRFile:
        return {".wav"};
    case BrowseFileType::PresetFile:
        return {".json", ".soundshed"};
    case BrowseFileType::ImageFile:
        return {".png", ".jpg", ".jpeg", ".webp"};
    case BrowseFileType::AudioFile:
        return {".wav", ".mp3", ".flac", ".ogg", ".aif", ".aiff"};
    case BrowseFileType::ArchiveFile:
        return {".soundshed.preset", ".soundshed.presets", ".zip"};
    case BrowseFileType::PluginFile:
        return {".vst3", ".clap", ".so"};
    default:
        return {};
    }
}

/// One field of a line of /proc/self/mounts: a space, tab, newline or backslash in a path comes
/// as three octal digits after a backslash.
std::string Unescape(const std::string& field)
{
    std::string out;

    for (std::size_t i = 0; i < field.size(); ++i)
    {
        if (field[i] == '\\' && i + 3 < field.size() && field[i + 1] >= '0' && field[i + 1] <= '7')
        {
            out.push_back(static_cast<char>((field[i + 1] - '0') * 64 + (field[i + 2] - '0') * 8 + (field[i + 3] - '0')));
            i += 3;
        }
        else
        {
            out.push_back(field[i]);
        }
    }

    return out;
}

bool IsRemovableMount(const std::string& mountPoint)
{
    return mountPoint == "/mnt" || mountPoint.rfind("/mnt/", 0) == 0 || mountPoint.rfind("/media/", 0) == 0 ||
           mountPoint.rfind("/run/media/", 0) == 0;
}
} // namespace

bool OffersFile(BrowseFileType type, const std::string& name)
{
    if (type == BrowseFileType::Folder || name.empty() || name.front() == '.')
    {
        return false;
    }

    const auto endings = Endings(type);
    const auto lower = Lower(name);
    return endings.empty() || std::any_of(endings.begin(), endings.end(),
                                          [&lower](std::string_view ending) { return EndsWith(lower, ending); });
}

FolderListing ListFolder(const std::filesystem::path& folder, BrowseFileType type)
{
    FolderListing listing;
    listing.path = folder;
    std::error_code ec;
    std::filesystem::directory_iterator it(folder, std::filesystem::directory_options::skip_permission_denied, ec);

    if (ec)
    {
        listing.error = ec.message();
        return listing;
    }

    // By hand, not a range-for: a stick pulled out half way through ends the listing, not the app.
    for (const std::filesystem::directory_iterator end; it != end; it.increment(ec))
    {
        if (ec)
        {
            listing.error = ec.message();
            break;
        }

        FileEntry entry;
        entry.path = it->path();
        entry.name = guitarfx::util::PathToUtf8(entry.path.filename());
        std::error_code entryError;
        entry.folder = it->is_directory(entryError); // through a link, as it opens

        if (entry.folder ? entry.name.front() == '.' : !OffersFile(type, entry.name))
        {
            continue;
        }

        if (!entry.folder)
        {
            const auto bytes = it->file_size(entryError);
            entry.bytes = entryError ? 0 : bytes;
        }

        listing.entries.push_back(std::move(entry));
    }

    std::sort(listing.entries.begin(), listing.entries.end(), [](const FileEntry& a, const FileEntry& b) {
        return a.folder != b.folder ? a.folder : Lower(a.name) < Lower(b.name);
    });
    return listing;
}

std::vector<std::filesystem::path> RemovableMounts(const std::string& procMounts)
{
    std::vector<std::filesystem::path> mounts;
    std::istringstream lines(procMounts);

    for (std::string line; std::getline(lines, line);)
    {
        std::istringstream fields(line);
        std::string device, mountPoint;
        fields >> device >> mountPoint;
        mountPoint = Unescape(mountPoint);

        if (IsRemovableMount(mountPoint))
        {
            std::filesystem::path path(mountPoint);

            // An automount is listed twice once it is in use: its trigger and the disk itself.
            if (std::find(mounts.begin(), mounts.end(), path) == mounts.end())
            {
                mounts.push_back(std::move(path));
            }
        }
    }

    return mounts;
}

bool IsInside(const std::filesystem::path& path, const std::filesystem::path& folder)
{
    const auto [end, ignored] = std::mismatch(folder.begin(), folder.end(), path.begin(), path.end());
    return end == folder.end();
}

bool IsOnMountedDisk(const std::filesystem::path& path, const std::string& procMounts)
{
    const auto mounts = RemovableMounts(procMounts);
    return std::any_of(mounts.begin(), mounts.end(),
                       [&path](const std::filesystem::path& mount) { return IsInside(path, mount); });
}

std::string ReadMounts()
{
    std::ifstream file("/proc/self/mounts", std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::vector<FilePlace> FilePlaces(const std::filesystem::path& home, const std::string& procMounts)
{
    std::vector<FilePlace> places;

    if (!home.empty() && home != home.root_path())
    {
        places.push_back({"Home", home, false});
    }

    for (const auto& mount : RemovableMounts(procMounts))
    {
        places.push_back({guitarfx::util::PathToUtf8(mount.filename()), mount, true});
    }

    places.push_back({"Computer", std::filesystem::path("/"), false});
    return places;
}

std::filesystem::path HomeFolder()
{
    const char* home = std::getenv("HOME");
    return home != nullptr && *home != '\0' ? std::filesystem::path(home) : std::filesystem::path("/");
}
} // namespace nanoq
