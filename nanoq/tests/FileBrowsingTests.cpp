// What the in-app file picker knows about files (FileBrowsing.h), which is all of it but the
// drawing: which files a dialog offers, how a folder lists, and what counts as a USB stick.

#include "FileBrowsing.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace
{
namespace fs = std::filesystem;
using guitarfx::BrowseFileType;

int gFailures = 0;

void Check(bool ok, const std::string& what)
{
    if (!ok)
    {
        std::cerr << "[FileBrowsingTests] " << what << std::endl;
        ++gFailures;
    }
}

void Touch(const fs::path& path)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << "12345";
}

void TestOffers()
{
    Check(nanoq::OffersFile(BrowseFileType::NAMModel, "Plexi.nam"), "a model");
    Check(nanoq::OffersFile(BrowseFileType::NAMModel, "PLEXI.NAM"), "the case of a FAT stick's name");
    Check(!nanoq::OffersFile(BrowseFileType::NAMModel, "Plexi.wav"), "an IR is no model");
    Check(!nanoq::OffersFile(BrowseFileType::NAMModel, "._Plexi.nam"), "macOS's shadow file on a stick");
    Check(nanoq::OffersFile(BrowseFileType::IRFile, "4x12.WAV"), "an IR");
    Check(nanoq::OffersFile(BrowseFileType::ArchiveFile, "Rig.soundshed.preset"), "a two-part extension");
    Check(!nanoq::OffersFile(BrowseFileType::ArchiveFile, "Rig.preset"), "half an extension");
    Check(nanoq::OffersFile(BrowseFileType::Any, "notes.txt"), "Any takes anything");
    Check(nanoq::OffersFile(BrowseFileType::TablatureFile, "song.gp5"), "no filter for a score, as on Windows");
    Check(!nanoq::OffersFile(BrowseFileType::Folder, "Plexi.nam"), "a folder dialog offers no files");
}

void TestListing(const fs::path& root)
{
    Touch(root / "b.nam");
    Touch(root / "A.nam");
    Touch(root / "ignored.wav");
    Touch(root / ".hidden.nam");
    Touch(root / "Zed" / "inside.nam");
    Touch(root / "alpha" / "inside.nam");
    Touch(root / ".Trashes" / "x.nam");

    const auto models = nanoq::ListFolder(root, BrowseFileType::NAMModel);
    Check(models.error.empty(), "a readable folder reports no error: " + models.error);
    Check(models.entries.size() == 4, "two folders and two models, got " + std::to_string(models.entries.size()));

    if (models.entries.size() == 4)
    {
        Check(models.entries[0].folder && models.entries[0].name == "alpha", "folders first, by name");
        Check(models.entries[1].folder && models.entries[1].name == "Zed", "whatever the case");
        Check(!models.entries[2].folder && models.entries[2].name == "A.nam", "then files");
        Check(models.entries[3].name == "b.nam", "whatever the case");
        Check(models.entries[3].bytes == 5, "a file's size");
    }

    const auto folders = nanoq::ListFolder(root, BrowseFileType::Folder);
    Check(folders.entries.size() == 2, "a folder dialog lists only folders");

    const auto gone = nanoq::ListFolder(root / "nowhere", BrowseFileType::NAMModel);
    Check(!gone.error.empty() && gone.entries.empty(), "a folder that has gone says so");
}

void TestMounts()
{
    const std::string mounts = "proc /proc proc rw 0 0\n"
                               "/dev/mmcblk0p2 / ext4 rw 0 0\n"
                               "/dev/sda1 /media/usb-stick vfat rw 0 0\n"
                               "/dev/sdb1 /run/media/system/MY\\040STICK exfat rw 0 0\n"
                               "systemd-1 /run/media/system/AUTO autofs rw 0 0\n"
                               "/dev/sdc1 /run/media/system/AUTO vfat rw 0 0\n"
                               "/dev/sdd1 /mnt/backup ext4 rw 0 0\n"
                               "tmpfs /mnt2 tmpfs rw 0 0\n"
                               "tmpfs /run/user/1000 tmpfs rw 0 0\n";

    const auto found = nanoq::RemovableMounts(mounts);
    Check(found.size() == 4, "four disks, got " + std::to_string(found.size()));
    Check(std::find(found.begin(), found.end(), fs::path("/run/media/system/MY STICK")) != found.end(),
          "a label's space comes back");
    Check(std::find(found.begin(), found.end(), fs::path("/mnt2")) == found.end(), "only /mnt itself and under it");

    const auto places = nanoq::FilePlaces("/home/soundshed", mounts);
    Check(places.size() == 6, "Home, four disks and the file system, got " + std::to_string(places.size()));
    Check(places.front().name == "Home" && !places.front().removable, "Home comes first");
    Check(places.back().name == "Computer", "the file system comes last");
    Check(places[1].removable && places[1].name == "usb-stick", "a disk goes by its folder's name");
    Check(nanoq::FilePlaces("/", "").size() == 1, "no Home when home is the top of the file system");

    Check(nanoq::IsOnMountedDisk("/run/media/system/MY STICK/Amps/Plexi.nam", mounts), "a file on a stick");
    Check(nanoq::IsOnMountedDisk("/media/usb-stick/Plexi.nam", mounts), "and another");
    Check(!nanoq::IsOnMountedDisk("/media/usb-stick-2/Plexi.nam", mounts), "a neighbour is not inside it");
    Check(!nanoq::IsOnMountedDisk("/home/soundshed/Plexi.nam", mounts), "the card is not removable");
}
} // namespace

int main()
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = fs::temp_directory_path() / ("nanoq-file-browsing-" + std::to_string(stamp));

    TestOffers();
    TestListing(root);
    TestMounts();

    std::error_code ec;
    fs::remove_all(root, ec);

    if (gFailures == 0)
    {
        std::cout << "[FileBrowsingTests] passed" << std::endl;
    }

    return gFailures == 0 ? 0 : 1;
}
