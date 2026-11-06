#pragma once

// What the in-app file picker (ui/dialogs/FilePicker.cpp) knows about files, with nothing drawn: which
// files a dialog offers, what is in a folder, and where a removable disk has been mounted. Linux
// has no file dialog to call, and a kiosk no desktop to show one on.

#include "IPluginHost.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nanoq
{
struct FileEntry
{
    std::string name; // UTF-8
    std::filesystem::path path;
    bool folder = false;
    std::uintmax_t bytes = 0; // a file's size
};

struct FolderListing
{
    std::filesystem::path path;
    std::vector<FileEntry> entries; // folders first, then files, each by name whatever the case
    std::string error;              // why it could not be read
};

/// A place the picker offers to start from.
struct FilePlace
{
    std::string name;
    std::filesystem::path path;
    bool removable = false; // something mounted: a USB stick, on a kiosk
};

/// Whether a file of this name is offered for `type`, by the same extensions as the Windows
/// dialog (any case). Hidden files are not, and for Folder no file is.
[[nodiscard]] bool OffersFile(guitarfx::BrowseFileType type, const std::string& name);

/// Reads a folder: its subfolders, hidden ones left out, and the files `type` offers. May take a
/// while (a USB stick waking up): not on the UI thread.
[[nodiscard]] FolderListing ListFolder(const std::filesystem::path& folder, guitarfx::BrowseFileType type);

/// The mount points in /proc/self/mounts' text that a removable disk gets: /media, /run/media
/// (systemd-mount, udisks) and /mnt. A label's spaces come back from the file's escapes.
[[nodiscard]] std::vector<std::filesystem::path> RemovableMounts(const std::string& procMounts);

/// Whether `path` is `folder` or somewhere inside it.
[[nodiscard]] bool IsInside(const std::filesystem::path& path, const std::filesystem::path& folder);

/// Whether `path` is on one of RemovableMounts.
[[nodiscard]] bool IsOnMountedDisk(const std::filesystem::path& path, const std::string& procMounts);

/// /proc/self/mounts, or nothing where there is no such file. Quick: the kernel writes it, and
/// nothing on a disk is touched.
[[nodiscard]] std::string ReadMounts();

/// Home, then what is mounted, then the whole file system.
[[nodiscard]] std::vector<FilePlace> FilePlaces(const std::filesystem::path& home, const std::string& procMounts);

/// $HOME, or the top of the file system without one.
[[nodiscard]] std::filesystem::path HomeFolder();
} // namespace nanoq
