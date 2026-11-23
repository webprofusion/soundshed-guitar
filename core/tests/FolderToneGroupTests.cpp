/**
 * FolderToneGroupTests.cpp - The resource browser Folder tab's tone groups as the native UIs draw
 * them (uiclient/FolderToneLookup.h), against the web UI's folderGroups.ts: files sharing a tone
 * gathered at the first one's place, a tone with one file left a plain row, a file matched later
 * joining the group already shown, and a group's title and creator from whichever member has them.
 */

#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "uiclient/FolderToneLookup.h"

using namespace guitarfx::uiclient;

namespace
{
int gFailures = 0;

void Expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        ++gFailures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

FolderFile File(int index)
{
    FolderFile file;
    file.name = "capture " + std::to_string(index) + ".nam";
    file.path = "D:\\Captures\\capture " + std::to_string(index) + ".nam";
    file.resourceType = "nam";
    return file;
}

std::string Describe(const FolderToneGroups& grouped)
{
    std::string text;

    for (const auto& item : grouped.items)
    {
        text += text.empty() ? "" : ", ";

        if (item.file != nullptr)
        {
            text += item.file->name;
        }
        else
        {
            const auto& group = grouped.groups[item.group];
            text += group.key + " x" + std::to_string(group.files.size());
        }
    }

    return text;
}
} // namespace

int main()
{
    std::vector<FolderFile> files;

    for (int i = 0; i < 6; ++i)
    {
        files.push_back(File(i));
    }

    std::vector<const FolderFile*> listed;

    for (const auto& file : files)
    {
        listed.push_back(&file);
    }

    std::map<std::string, nlohmann::json> details;
    details[files[1].path] = {{"toneId", "10"}, {"provider", "tone3000"}};
    details[files[4].path] = {{"toneId", "10"}, {"toneTitle", "Tone Ten"}, {"creatorName", "maker"}};
    details[files[3].path] = {{"toneId", "20"}};
    const auto detailsFor = [&details](const FolderFile& file) -> const nlohmann::json* {
        const auto it = details.find(file.path);
        return it == details.end() ? nullptr : &it->second;
    };

    auto grouped = GroupFolderFiles(listed, detailsFor);
    Expect(Describe(grouped) == "capture 0.nam, tone:10 x2, capture 2.nam, capture 3.nam, capture 5.nam",
           "groups at the first file's place, a tone of one is a plain row: " + Describe(grouped));
    Expect(grouped.groups.front().title == "Tone Ten", "the title comes from the member that has it");
    Expect(FolderGroupCreator(grouped.groups.front().metadata) == "maker", "so does the creator");
    Expect(FolderGroupCreator(nlohmann::json{{"provider", "tone3000"}}).empty(), "the provider is not a creator");

    // A later lookup matching capture 5 to tone 10: it joins the group, which keeps its place.
    details[files[5].path] = {{"toneId", "10"}};
    grouped = GroupFolderFiles(listed, detailsFor);
    Expect(Describe(grouped) == "capture 0.nam, tone:10 x3, capture 2.nam, capture 3.nam",
           "a later match joins the group shown: " + Describe(grouped));

    Expect(SharedTonePrefix({"Peavey - 5150 II - LEAD - G7.0", "Peavey - 5150 II - CLEAN - G1.5"}) ==
               "Peavey - 5150 II",
           "the names' shared start, to its last separator");
    Expect(SharedTonePrefix({"ProCo Rat - ProCo RAT", "ProCo Rat - ProCo RAT"}) == "ProCo Rat",
           "identical names: the part before the last separator");
    Expect(SharedTonePrefix({"Alpha", "Beta"}).empty(), "no shared part");
    Expect(SharedTonePrefix({"Only - One", ""}).empty(), "one name is not enough");

    Expect(FolderToneLookup::CanLookUp(File(1)), "a capture can be looked up");
    FolderFile wav;
    wav.name = "cab.WAV";
    wav.resourceType = "ir";
    Expect(FolderToneLookup::CanLookUp(wav), "a WAV IR can be looked up");
    wav.name = "cab.aiff";
    Expect(!FolderToneLookup::CanLookUp(wav), "another IR file cannot");
    Expect(FolderPathKey("D:\\Captures\\AC30.nam") == "d:/captures/ac30.nam", "path keys ignore case and slashes");

    if (gFailures == 0)
    {
        std::cout << "FolderToneGroupTests passed\n";
    }

    return gFailures == 0 ? 0 : 1;
}
