// Custom effect layouts on disk, end to end through the controller.
//
// An imported layout archive names its own images, and the engine wrote each one to
// imagesDir / fileName as given: "..\..\Startup\x.bat" or "C:/x.dll" went where they pointed.
// Images were also copied into a layout's own folder only on its first save, so a layout that
// picked another layout's image lost it when that layout was deleted. These drive the real
// message handlers against a sandboxed profile.

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "IPluginHost.h"
#include "PluginController.h"
#include "controller/internal/LayoutSupport.h"
#include "util/Base64.h"
#include "util/FileSystem.h"
#include "util/PathEncoding.h"

#ifdef _WIN32
    #include <process.h>
    #define GUITARFX_TEST_PID _getpid()
#else
    #include <unistd.h>
    #define GUITARFX_TEST_PID getpid()
#endif

namespace fs = std::filesystem;

namespace
{
class TestHost final : public guitarfx::IPluginHost
{
  public:
    explicit TestHost(fs::path root) : mRoot(std::move(root))
    {
    }

    void SendMessageToUI(const std::string& jsonMessage) override
    {
        std::lock_guard<std::mutex> lock(mMessageMutex);
        mMessages.push_back(jsonMessage);
        mMessageCv.notify_all();
    }

    void BrowseFileAsync(guitarfx::BrowseFileType, const std::string&,
                         std::function<void(const guitarfx::BrowseFileResult&)> callback) override
    {
        callback(mBrowsePath.empty() ? guitarfx::BrowseFileResult{} : guitarfx::BrowseFileResult{true, mBrowsePath});
    }

    /// The file the next Browse picks; empty cancels it.
    void SetBrowsePath(fs::path path)
    {
        mBrowsePath = std::move(path);
    }

    void SaveFileAsync(guitarfx::BrowseFileType, const std::string&, const std::string&,
                       std::function<void(const guitarfx::BrowseFileResult&)> callback) override
    {
        callback(guitarfx::BrowseFileResult{});
    }

    void RunOnMainThread(std::function<void()> fn) override
    {
        fn();
    }

    [[nodiscard]] fs::path GetUserDataPath() const override
    {
        return mRoot;
    }

    [[nodiscard]] fs::path GetBundledAssetsPath() const override
    {
        return mRoot;
    }

    [[nodiscard]] double GetSampleRate() const override
    {
        return 48000.0;
    }

    [[nodiscard]] int GetBlockSize() const override
    {
        return 512;
    }

    /// The latest message of this type, waiting up to `timeout` for one to arrive.
    std::optional<nlohmann::json> WaitForMessage(const std::string& type,
                                                 std::chrono::milliseconds timeout = std::chrono::seconds(5))
    {
        std::unique_lock<std::mutex> lock(mMessageMutex);
        std::optional<nlohmann::json> found;

        mMessageCv.wait_for(lock, timeout, [&]() {
            for (auto it = mMessages.rbegin(); it != mMessages.rend(); ++it)
            {
                auto parsed = nlohmann::json::parse(*it);

                if (parsed.value("type", "") == type)
                {
                    found = std::move(parsed);
                    return true;
                }
            }

            return false;
        });

        return found;
    }

    [[nodiscard]] std::size_t CountMessages(const std::string& type)
    {
        std::lock_guard<std::mutex> lock(mMessageMutex);
        std::size_t count = 0;

        for (const auto& message : mMessages)
        {
            if (nlohmann::json::parse(message).value("type", "") == type)
            {
                ++count;
            }
        }

        return count;
    }

  private:
    fs::path mRoot;
    fs::path mBrowsePath;
    std::vector<std::string> mMessages;
    std::condition_variable mMessageCv;
    std::mutex mMessageMutex;
};

/// Points every FileSystem at `root` for as long as it lives.
class ScopedPlatformRoot
{
  public:
    explicit ScopedPlatformRoot(const fs::path& root)
    {
        guitarfx::FileSystem::SetPlatformRootOverride(root);
    }

    ~ScopedPlatformRoot()
    {
        guitarfx::FileSystem::SetPlatformRootOverride({});
    }

    ScopedPlatformRoot(const ScopedPlatformRoot&) = delete;
    ScopedPlatformRoot& operator=(const ScopedPlatformRoot&) = delete;
};

/// A folder of this process's own under %TEMP%: other test runs clear theirs whenever they start.
fs::path FreshSandbox(const std::string& leaf)
{
    const fs::path sandbox =
        fs::temp_directory_path() / ("guitarfx-layout-storage-tests-" + std::to_string(GUITARFX_TEST_PID)) / leaf;
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    return sandbox;
}

fs::path ContentDir(const fs::path& sandbox)
{
    return sandbox / "data" / "v1" / "layouts" / "content";
}

std::vector<std::uint8_t> PngBytes()
{
    return {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 'I', 'H', 'D', 'R'};
}

std::vector<std::uint8_t> JpegBytes()
{
    return {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00};
}

std::vector<std::uint8_t> GifBytes()
{
    return {'G', 'I', 'F', '8', '9', 'a', 0x01, 0x00, 0x01, 0x00};
}

void WriteBytes(const fs::path& path, const std::vector<std::uint8_t>& bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void SaveImage(guitarfx::PluginController& controller, const std::string& layoutId, const std::string& imageId,
               const std::string& fileName, const std::vector<std::uint8_t>& bytes)
{
    controller.HandleUIMessage(nlohmann::json{
        {"type", "saveLayoutImage"},
        {"imageId", imageId},
        {"fileName", fileName},
        {"data", guitarfx::util::EncodeBase64(bytes)},
        {"layoutId", layoutId}}.dump());
}

/// Saves a layout whose background layers draw on `imageIds`, one layer each.
void SaveLayout(guitarfx::PluginController& controller, const std::string& layoutId, const std::string& effectType,
                const std::vector<std::string>& imageIds, const std::string& blendId = {})
{
    nlohmann::json backgrounds = nlohmann::json::array();

    for (std::size_t i = 0; i < imageIds.size(); ++i)
    {
        backgrounds.push_back({{"layerIndex", i}, {"type", "image"}, {"value", imageIds[i]}});
    }

    const nlohmann::json layout{{"effectType", effectType},
                                {"layoutId", layoutId},
                                {"version", 1},
                                {"dimensions", {{"width", 400}, {"height", 280}}},
                                {"backgrounds", backgrounds},
                                {"controls", nlohmann::json::array()},
                                {"textLabels", nlohmann::json::array()}};

    controller.HandleUIMessage(nlohmann::json{
        {"type", "saveEffectLayout"},
        {"effectType", effectType},
        {"blendId", blendId},
        {"layoutId", layoutId},
        {"layout", layout}}.dump());
}

/// Every name an archive could use to reach outside the layout's folder is refused and reported,
/// and nothing is written anywhere. A plain name still lands in the layout's own folder.
bool TestImportedImageNamesStayInTheLayoutFolder()
{
    const fs::path sandbox = FreshSandbox("unsafe-names");
    const ScopedPlatformRoot root(sandbox);
    const fs::path outside = sandbox / "outside";
    fs::create_directories(outside);

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    const std::string layoutId = "layout-import";
    const std::size_t errorsBefore = host.CountMessages("error");

    struct Case
    {
        std::string imageId;
        std::string fileName;
        std::vector<std::uint8_t> bytes;
    };

    const std::vector<Case> refused = {
        {"escape", "..\\..\\..\\..\\escape.png", PngBytes()},
        {"escape", "../../../../escape.png", PngBytes()},
        {"absolute", guitarfx::util::PathToUtf8(outside / "absolute.png"), PngBytes()},
        {"drive", "C:drive.png", PngBytes()},
        {"script", "script.bat", PngBytes()},
        {"CON", "CON.png", PngBytes()},
        {"mismatch", "other.png", PngBytes()},
        {"fake", "fake.png", std::vector<std::uint8_t>{'M', 'Z', 0x90, 0x00}},
    };

    for (const auto& c : refused)
    {
        SaveImage(controller, layoutId, c.imageId, c.fileName, c.bytes);
    }

    const std::set<std::string> refusedNames = {"escape.png", "absolute.png", "drive.png", "script.bat",
                                                "CON.png",    "other.png",    "fake.png"};

    for (const auto& entry : fs::recursive_directory_iterator(sandbox))
    {
        if (entry.is_regular_file() && refusedNames.contains(guitarfx::util::PathToUtf8(entry.path().filename())))
        {
            std::cerr << "A refused image was written: " << guitarfx::util::PathToUtf8(entry.path()) << "\n";
            return false;
        }
    }

    if (const auto reported = host.CountMessages("error") - errorsBefore; reported != refused.size())
    {
        std::cerr << "Expected " << refused.size() << " refusals reported, got " << reported << "\n";
        return false;
    }

    SaveImage(controller, layoutId, "face_1", "face_1.png", PngBytes());

    if (!fs::is_regular_file(ContentDir(sandbox) / layoutId / "images" / "face_1.png"))
    {
        std::cerr << "A plain image name was not written to the layout's folder\n";
        return false;
    }

    return true;
}

/// A layout that picks another layout's image keeps it when that layout is deleted: a save takes
/// a copy at once, and a delete gives the copy to a layout that borrowed it before saves did.
bool TestBorrowedImagesSurviveDeletingTheirLayout()
{
    const fs::path sandbox = FreshSandbox("borrowed-images");
    const ScopedPlatformRoot root(sandbox);
    const fs::path content = ContentDir(sandbox);

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);

    SaveImage(controller, "A", "shared_1", "shared_1.png", PngBytes());
    SaveLayout(controller, "A", "fx_a", {"shared_1"});
    SaveLayout(controller, "B", "fx_b", {"shared_1"});

    if (!fs::is_regular_file(content / "B" / "images" / "shared_1.png"))
    {
        std::cerr << "Saving B did not copy the image it picked from A\n";
        return false;
    }

    // C refers to an image that only A holds and was never copied, as a layout saved before
    // every save copied its images does.
    SaveLayout(controller, "C", "fx_c", {"late_2"});
    SaveImage(controller, "A", "late_2", "late_2.png", PngBytes());
    SaveLayout(controller, "A", "fx_a", {"shared_1", "late_2"});

    if (fs::exists(content / "C" / "images" / "late_2.png"))
    {
        std::cerr << "Test setup: C already holds its own copy\n";
        return false;
    }

    controller.HandleUIMessage(
        nlohmann::json{{"type", "deleteLayout"}, {"effectType", "fx_a"}, {"blendId", ""}, {"layoutId", "A"}}.dump());

    if (fs::exists(content / "A"))
    {
        std::cerr << "The deleted layout's folder is still there\n";
        return false;
    }

    if (!fs::is_regular_file(content / "B" / "images" / "shared_1.png") ||
        !fs::is_regular_file(content / "C" / "images" / "late_2.png"))
    {
        std::cerr << "Deleting A took an image another layout draws on\n";
        return false;
    }

    controller.HandleUIMessage(R"({"type":"requestLayoutImages"})");
    const auto loaded = host.WaitForMessage("layoutImagesLoaded");
    std::set<std::string> ids;

    for (const auto& image : loaded ? loaded->value("images", nlohmann::json::array()) : nlohmann::json::array())
    {
        ids.insert(image.value("imageId", ""));
    }

    if (!ids.contains("shared_1") || !ids.contains("late_2"))
    {
        std::cerr << "The surviving images were not listed: " << (loaded ? loaded->dump() : std::string("none"))
                  << "\n";
        return false;
    }

    return true;
}

/// Images an older build left behind still load: one with an upper-case extension (the picker
/// kept the file's own case) and one in the shared folder an id-less layout wrote to.
bool TestLoaderListsLegacyAndUpperCaseImages()
{
    const fs::path sandbox = FreshSandbox("legacy-images");
    const ScopedPlatformRoot root(sandbox);
    const fs::path content = ContentDir(sandbox);
    WriteBytes(content / "L" / "images" / "SHOUT_1.JPG", JpegBytes());
    WriteBytes(content / "images" / "legacy_1.png", PngBytes());

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    SaveLayout(controller, "L", "fx", {"SHOUT_1", "legacy_1"});

    if (!fs::is_regular_file(content / "L" / "images" / "legacy_1.png"))
    {
        std::cerr << "Saving did not copy the image from the legacy folder\n";
        return false;
    }

    controller.HandleUIMessage(R"({"type":"requestLayoutImages"})");
    const auto loaded = host.WaitForMessage("layoutImagesLoaded");
    bool jpeg = false;
    bool png = false;

    for (const auto& image : loaded ? loaded->value("images", nlohmann::json::array()) : nlohmann::json::array())
    {
        const std::string url = image.value("url", "");
        jpeg = jpeg || (image.value("imageId", "") == "SHOUT_1" && image.value("fileName", "") == "SHOUT_1.JPG" &&
                        url.rfind("layout-images/id/SHOUT_1?v=", 0) == 0);
        png = png || (image.value("imageId", "") == "legacy_1" && image.value("fileName", "") == "legacy_1.png" &&
                      url.rfind("layout-images/id/legacy_1?v=", 0) == 0);
    }

    if (!jpeg || !png)
    {
        std::cerr << "Expected SHOUT_1 as JPEG and legacy_1 as PNG: " << (loaded ? loaded->dump() : std::string("none"))
                  << "\n";
        return false;
    }

    return true;
}

/// The file the library lists for each id, keyed by id, as that file's extension.
std::map<std::string, std::string> ListedImageTypes(TestHost& host, guitarfx::PluginController& controller)
{
    controller.HandleUIMessage(R"({"type":"requestLayoutImages"})");
    const auto loaded = host.WaitForMessage("layoutImagesLoaded");
    std::map<std::string, std::string> types;

    for (const auto& image : loaded ? loaded->value("images", nlohmann::json::array()) : nlohmann::json::array())
    {
        types[image.value("imageId", "")] = fs::path(image.value("fileName", "")).extension().string();
    }

    return types;
}

/// One id held by several folders resolves to one file everywhere. The library has always shown
/// the factory's copy over a user layout's, and a user layout's over the legacy folder's, so that
/// stays; a save now copies that same file rather than a different folder's. A layout folder the
/// index does not list is never shown, but can still supply an id found nowhere else.
bool TestSharedImageIdsResolveAsTheLibraryShowsThem()
{
    const fs::path sandbox = FreshSandbox("shared-ids");
    const ScopedPlatformRoot root(sandbox);
    const fs::path content = ContentDir(sandbox);
    const fs::path factory = sandbox / "ui" / "assets" / "layouts" / "factory-one";

    WriteBytes(factory / "images" / "factory-file.png", PngBytes());
    {
        std::ofstream manifest(factory / "layout.json");
        manifest << nlohmann::json{{"formatVersion", 1},
                                   {"layout", {{"effectType", "fx_factory"}, {"version", 1}}},
                                   {"images", {{{"imageId", "dup_1"}, {"fileName", "factory-file.png"}}}}}
                        .dump();
    }

    WriteBytes(content / "images" / "dup_1.gif", GifBytes());
    WriteBytes(content / "images" / "lu_1.gif", GifBytes());
    WriteBytes(content / "U" / "images" / "dup_1.jpg", JpegBytes());
    WriteBytes(content / "U" / "images" / "lu_1.jpg", JpegBytes());
    WriteBytes(content / "Orphan" / "images" / "orphan_1.png", PngBytes());
    WriteBytes(content / "Orphan" / "images" / "lu_1.png", PngBytes());

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    SaveLayout(controller, "U", "fx_u", {});

    const auto listed = ListedImageTypes(host, controller);

    if (listed.count("dup_1") == 0 || listed.at("dup_1") != ".png" || listed.count("lu_1") == 0 ||
        listed.at("lu_1") != ".jpg" || listed.count("orphan_1") != 0)
    {
        std::cerr << "The library resolved shared ids differently from before:";

        for (const auto& [id, type] : listed)
        {
            std::cerr << " " << id << "=" << type;
        }

        std::cerr << "\n";
        return false;
    }

    // The route the UI loads images from resolves an id to the same file the library lists, and
    // also reaches an unindexed folder's image (one picked for a layout not saved yet).
    const auto served = [&controller](const std::string& id) {
        const auto file = controller.ResolveLayoutImageFile(id);
        return file ? file->filename().string() : std::string("(none)");
    };

    if (served("dup_1") != "factory-file.png" || served("lu_1") != "lu_1.jpg" || served("orphan_1") != "orphan_1.png")
    {
        std::cerr << "The image route resolved differently from the library: dup_1=" << served("dup_1")
                  << " lu_1=" << served("lu_1") << " orphan_1=" << served("orphan_1") << "\n";
        return false;
    }

    SaveLayout(controller, "V", "fx_v", {"dup_1", "lu_1", "orphan_1"});
    const fs::path own = content / "V" / "images";

    if (!fs::is_regular_file(own / "dup_1.png") || fs::exists(own / "dup_1.jpg") ||
        !fs::is_regular_file(own / "lu_1.jpg") || fs::exists(own / "lu_1.png") ||
        !fs::is_regular_file(own / "orphan_1.png"))
    {
        std::cerr << "A save copied a different file from the one the library shows:";

        for (const auto& entry : fs::directory_iterator(own))
        {
            std::cerr << " " << entry.path().filename().string();
        }

        std::cerr << "\n";
        return false;
    }

    return true;
}

/// A PNG header declaring `width` x `height`: enough for the size reader and the type check.
std::vector<std::uint8_t> PngOfSize(std::uint32_t width, std::uint32_t height, std::size_t totalBytes = 33)
{
    std::vector<std::uint8_t> bytes = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R'};

    for (const auto value : {width, height})
    {
        for (int shift = 24; shift >= 0; shift -= 8)
        {
            bytes.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    }

    bytes.resize(std::max(bytes.size(), totalBytes), 0);
    return bytes;
}

/// The library lists images by URL with their pixel and byte size, and nothing else of them;
/// the route serves each URL's file with its type, and refuses paths that are not an image id.
bool TestImagesAreListedByUrlAndServedByTheRoute()
{
    const fs::path sandbox = FreshSandbox("image-route");
    const ScopedPlatformRoot root(sandbox);

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    const std::string spaced = "my knob (v2)_17";
    SaveImage(controller, "R", "bg_1", "bg_1.png", PngOfSize(3840, 2160));
    WriteBytes(ContentDir(sandbox) / "R" / "images" / (spaced + ".png"), PngOfSize(512, 512));
    SaveLayout(controller, "R", "fx_r", {"bg_1", spaced});

    controller.HandleUIMessage(R"({"type":"requestLayoutImages"})");
    const auto loaded = host.WaitForMessage("layoutImagesLoaded");
    std::map<std::string, nlohmann::json> byId;

    for (const auto& image : loaded ? loaded->value("images", nlohmann::json::array()) : nlohmann::json::array())
    {
        byId[image.value("imageId", "")] = image;
    }

    const auto& bg = byId["bg_1"];

    if (bg.contains("dataUrl") || bg.value("url", "").rfind("layout-images/id/bg_1?v=", 0) != 0 ||
        bg.value("width", 0) != 3840 || bg.value("height", 0) != 2160 || bg.value("bytes", 0) != 33)
    {
        std::cerr << "The image entry is not URL, size and bytes: " << bg.dump() << "\n";
        return false;
    }

    const std::string spacedUrl = byId[spaced].value("url", "");

    if (spacedUrl.rfind("layout-images/id/my%20knob%20%28v2%29_17?v=", 0) != 0)
    {
        std::cerr << "An id with spaces and brackets was not percent-encoded: " << spacedUrl << "\n";
        return false;
    }

    const auto served = controller.ResolveLayoutImageResource(spacedUrl.substr(0, spacedUrl.find('?')));

    if (!served || served->first.filename().string() != spaced + ".png" || served->second != "image/png")
    {
        std::cerr << "The route did not serve the encoded id's file\n";
        return false;
    }

    for (const std::string refused :
         {"layout-images/id/", "layout-images/id/..", "layout-images/id/%2E%2E", "layout-images/id/a%2Fb",
          "layout-images/id/a/b", "layout-images/id/%zz", "layout-images/bg_1.png", "layout-images/id/not_an_image"})
    {
        if (controller.ResolveLayoutImageResource(refused))
        {
            std::cerr << "The route served " << refused << "\n";
            return false;
        }
    }

    return true;
}

/// Each format's header gives the size without the file being decoded: PNG, GIF, a JPEG with a
/// large EXIF block before its frame, all three WebP kinds, and SVG by attributes or viewBox.
bool TestImageSizesAreReadFromHeaders()
{
    using guitarfx::controller_detail::ReadLayoutImageSize;
    const fs::path dir = FreshSandbox("image-sizes");
    const auto check = [&dir](const std::string& name, const std::vector<std::uint8_t>& bytes, std::uint32_t width,
                              std::uint32_t height) {
        WriteBytes(dir / name, bytes);
        const auto size = ReadLayoutImageSize(dir / name);

        if (!size || size->width != width || size->height != height)
        {
            std::cerr << name << ": expected " << width << "x" << height << ", read "
                      << (size ? std::to_string(size->width) + "x" + std::to_string(size->height) : "nothing") << "\n";
            return false;
        }

        return true;
    };
    const auto text = [](const std::string& s) { return std::vector<std::uint8_t>(s.begin(), s.end()); };
    const auto webp = [](const char (&chunk)[5]) {
        std::vector<std::uint8_t> bytes = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'};
        bytes.insert(bytes.end(), chunk, chunk + 4);
        return bytes;
    };

    std::vector<std::uint8_t> jpeg = {0xFF, 0xD8, 0xFF, 0xE1, 0xC0, 0x02}; // APP1 of 0xC002 bytes
    jpeg.resize(jpeg.size() + 0xC000, 0x00);

    for (const std::uint8_t b : {0xFF, 0xC0, 0x00, 0x11, 0x08, 0x05, 0xA0, 0x0F, 0x00, 0x03})
    {
        jpeg.push_back(b); // SOF0: 8-bit, 1440 high, 3840 wide
    }

    jpeg.resize(jpeg.size() + 16, 0);

    std::vector<std::uint8_t> vp8 = webp("VP8 ");
    vp8.resize(26, 0);

    for (const std::uint8_t b : {0x80, 0x07, 0x38, 0x04})
    {
        vp8.push_back(b); // 1920 x 1080
    }

    vp8.resize(40, 0);

    std::vector<std::uint8_t> vp8l = webp("VP8L");
    vp8l.resize(21, 0);
    // 14-bit width-1 then 14-bit height-1, little-endian: 1024 x 768.
    const std::uint32_t bits = (1024 - 1) | ((768 - 1) << 14);

    for (int i = 0; i < 4; ++i)
    {
        vp8l.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
    }

    vp8l.resize(40, 0);

    std::vector<std::uint8_t> vp8x = webp("VP8X");
    vp8x.resize(24, 0);

    for (const std::uint8_t b : {0xFF, 0x1D, 0x00, 0x6F, 0x17, 0x00})
    {
        vp8x.push_back(b); // 7680 x 6000
    }

    vp8x.resize(40, 0);

    return check("a.png", PngOfSize(4096, 3072), 4096, 3072) &&
           check("a.gif", {'G', 'I', 'F', '8', '9', 'a', 0x40, 0x01, 0xF0, 0x00, 0, 0}, 320, 240) &&
           check("a.jpg", jpeg, 3840, 1440) && check("vp8.webp", vp8, 1920, 1080) &&
           check("vp8l.webp", vp8l, 1024, 768) && check("vp8x.webp", vp8x, 7680, 6000) &&
           check("attrs.svg",
                 text("<?xml version=\"1.0\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\" "
                      "stroke-width=\"3\" width=\"640px\" height='480'><rect/></svg>"),
                 640, 480) &&
           check("box.svg", text("<svg viewBox=\"0 0 1200.4 300\"></svg>"), 1200, 300);
}

/// An imported image well over the old 16 MB limit is stored, and one over 64 MB is refused.
bool TestLargeImportedImagesAreStored()
{
    const fs::path sandbox = FreshSandbox("large-images");
    const ScopedPlatformRoot root(sandbox);
    const fs::path images = ContentDir(sandbox) / "L" / "images";

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    SaveImage(controller, "L", "big_1", "big_1.png", PngOfSize(8000, 8000, 20u * 1024u * 1024u));
    SaveImage(controller, "L", "huge_1", "huge_1.png", PngOfSize(8000, 8000, 65u * 1024u * 1024u));

    std::error_code ec;

    if (fs::file_size(images / "big_1.png", ec) != 20u * 1024u * 1024u || fs::exists(images / "huge_1.png"))
    {
        std::cerr << "Expected the 20 MB image stored and the 65 MB one refused\n";
        return false;
    }

    return true;
}

/// A WebP (VP8X) header declaring `width` x `height`.
std::vector<std::uint8_t> WebpOfSize(std::uint32_t width, std::uint32_t height)
{
    std::vector<std::uint8_t> bytes = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', 'X'};
    bytes.resize(24, 0);

    for (const auto value : {width - 1, height - 1})
    {
        for (int shift = 0; shift < 24; shift += 8)
        {
            bytes.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    }

    bytes.resize(64, 0);
    return bytes;
}

/// The library's entry for one image.
nlohmann::json ListedImage(TestHost& host, guitarfx::PluginController& controller, const std::string& imageId)
{
    controller.HandleUIMessage(R"({"type":"requestLayoutImages"})");
    const auto loaded = host.WaitForMessage("layoutImagesLoaded");

    for (const auto& image : loaded ? loaded->value("images", nlohmann::json::array()) : nlohmann::json::array())
    {
        if (image.value("imageId", "") == imageId)
        {
            return image;
        }
    }

    return nlohmann::json::object();
}

void SaveRendition(guitarfx::PluginController& controller, const std::string& imageId, const std::string& version,
                   int edge, const std::vector<std::uint8_t>& bytes)
{
    controller.HandleUIMessage(nlohmann::json{{"type", "saveLayoutImageRendition"},
                                              {"imageId", imageId},
                                              {"version", version},
                                              {"edge", edge},
                                              {"data", guitarfx::util::EncodeBase64(bytes)}}
                                   .dump());
}

/// A smaller copy the UI makes of a large image is kept for that version of the file, listed with
/// the image and served by its route; copies that are stale, the wrong size or not WebP are not.
bool TestRenditionsAreStoredListedAndServed()
{
    const fs::path sandbox = FreshSandbox("renditions");
    const ScopedPlatformRoot root(sandbox);
    const fs::path copies = sandbox / "data" / "v1" / "layouts" / "renditions";

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    SaveImage(controller, "R", "huge_1", "huge_1.png", PngOfSize(6000, 4000));
    SaveImage(controller, "R", "mid_1", "mid_1.png", PngOfSize(3000, 2000));
    SaveLayout(controller, "R", "fx_r", {"huge_1", "mid_1"});

    const std::string version = ListedImage(host, controller, "huge_1").value("version", "");
    SaveRendition(controller, "huge_1", version, 1024, WebpOfSize(1024, 683));

    const auto saved = host.WaitForMessage("layoutImageRenditionSaved");
    const auto listed = ListedImage(host, controller, "huge_1");
    const auto renditions = listed.value("renditions", nlohmann::json::array());

    if (!saved || saved->value("edge", 0) != 1024 || renditions.size() != 1 ||
        renditions[0].value("width", 0) != 1024 ||
        renditions[0].value("url", "").rfind("layout-images/rendition/huge_1/1024?v=" + version, 0) != 0)
    {
        std::cerr << "The copy was not listed with the image: " << listed.dump() << "\n";
        return false;
    }

    const auto served = controller.ResolveLayoutImageResource("layout-images/rendition/huge_1/1024");

    if (!served || served->second != "image/webp" || served->first.parent_path() != copies)
    {
        std::cerr << "The copy's route did not serve it\n";
        return false;
    }

    const std::size_t keptBefore = static_cast<std::size_t>(std::distance(fs::directory_iterator(copies), {}));
    SaveRendition(controller, "huge_1", "12345", 2048, WebpOfSize(2048, 1365)); // stale version
    SaveRendition(controller, "huge_1", version, 1500, WebpOfSize(1500, 1000)); // a size not kept
    SaveRendition(controller, "huge_1", version, 2048, PngOfSize(2048, 1365));  // not WebP
    SaveRendition(controller, "huge_1", version, 2048, WebpOfSize(1024, 683));  // not the size it says
    const std::string midVersion = ListedImage(host, controller, "mid_1").value("version", "");
    SaveRendition(controller, "mid_1", midVersion, 4096, WebpOfSize(4096, 2731)); // not smaller than its source
    const std::size_t keptAfter = static_cast<std::size_t>(std::distance(fs::directory_iterator(copies), {}));

    if (keptAfter != keptBefore || controller.ResolveLayoutImageResource("layout-images/rendition/huge_1/2048") ||
        controller.ResolveLayoutImageResource("layout-images/rendition/huge_1/1500") ||
        controller.ResolveLayoutImageResource("layout-images/rendition/..%2Fx/1024"))
    {
        std::cerr << "A copy that should have been refused was kept or served\n";
        return false;
    }

    // A new file under the same id: its old copy is no longer listed or served, and is removed
    // once a copy of the new file is saved.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    SaveImage(controller, "R", "huge_1", "huge_1.png", PngOfSize(6000, 4000, 40));
    const auto replaced = ListedImage(host, controller, "huge_1");

    if (replaced.contains("renditions") || controller.ResolveLayoutImageResource("layout-images/rendition/huge_1/1024"))
    {
        std::cerr << "The old file's copy is still offered: " << replaced.dump() << "\n";
        return false;
    }

    SaveRendition(controller, "huge_1", replaced.value("version", ""), 1024, WebpOfSize(1024, 683));
    const auto left = static_cast<std::size_t>(std::distance(fs::directory_iterator(copies), {}));

    if (left != 1 || !controller.ResolveLayoutImageResource("layout-images/rendition/huge_1/1024"))
    {
        std::cerr << "Expected only the new file's copy to remain, found " << left << " files\n";
        return false;
    }

    return true;
}

/// Browsing for a picture the library already holds answers with that image rather than copying the
/// file again under a new id, from any layout's folder; a different picture is still added.
bool TestPickingTheSameImageAgainReusesIt()
{
    const fs::path sandbox = FreshSandbox("same-image-again");
    const ScopedPlatformRoot root(sandbox);
    const fs::path content = ContentDir(sandbox);

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);

    const auto browse = [&](const fs::path& file, const std::string& layoutId) -> std::string {
        const auto before = host.CountMessages("layoutImageSelected");
        host.SetBrowsePath(file);
        controller.HandleUIMessage(
            nlohmann::json{{"type", "browseLayoutImage"}, {"purpose", "knob"}, {"layoutId", layoutId}}.dump());
        const auto selected = host.WaitForMessage("layoutImageSelected");
        return selected && host.CountMessages("layoutImageSelected") == before + 1 ? selected->value("imageId", "")
                                                                                   : std::string();
    };
    const auto imageCount = [&](const std::string& layoutId) {
        std::error_code ec;
        const auto dir = content / layoutId / "images";
        return fs::is_directory(dir, ec) ? std::distance(fs::directory_iterator(dir, ec), fs::directory_iterator())
                                         : std::ptrdiff_t{0};
    };

    const fs::path face = sandbox / "picked" / "face.png";
    const fs::path sameFace = sandbox / "elsewhere" / "face copy.png";
    const fs::path otherFace = sandbox / "picked" / "other.png";
    WriteBytes(face, PngOfSize(64, 64, 80));
    WriteBytes(sameFace, PngOfSize(64, 64, 80));
    WriteBytes(otherFace, PngOfSize(64, 32, 80));

    const std::string first = browse(face, "A");
    const std::string again = browse(sameFace, "A");

    if (first.empty() || again != first || imageCount("A") != 1)
    {
        std::cerr << "Picking the same picture again added a copy: " << first << " then " << again << ", "
                  << imageCount("A") << " files\n";
        return false;
    }

    // Another layout reuses it too; saving that layout is what copies it into its own folder.
    if (const std::string fromB = browse(sameFace, "B"); fromB != first || imageCount("B") != 0)
    {
        std::cerr << "Another layout copied a picture the library holds: " << fromB << "\n";
        return false;
    }

    if (const std::string other = browse(otherFace, "A"); other.empty() || other == first || imageCount("A") != 2)
    {
        std::cerr << "A different picture was not added: " << other << "\n";
        return false;
    }

    return true;
}

/// Each image is listed with its category: a browsed one under what it was picked for, an imported
/// one under its archive's, one refiled under the new one, and a factory image under the folder it
/// ships in, which refiling cannot change.
bool TestImageCategoriesAreFiledListedAndShipped()
{
    const fs::path sandbox = FreshSandbox("image-categories");
    const ScopedPlatformRoot root(sandbox);

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);

    WriteBytes(sandbox / "ui" / "assets" / "layout-images" / "switch" / "factory-bat.png", PngOfSize(32, 48));
    const auto listed = [&](const std::string& imageId) { return ListedImage(host, controller, imageId); };
    const auto category = [&](const std::string& imageId) { return listed(imageId).value("type", ""); };

    const fs::path wood = sandbox / "picked" / "wood.png";
    WriteBytes(wood, PngOfSize(400, 280, 60));
    host.SetBrowsePath(wood);
    controller.HandleUIMessage(nlohmann::json{
        {"type", "browseLayoutImage"}, {"purpose", "background"}, {"layoutId", "A"}, {"pickerId", "picker-7"}}
                                   .dump());
    const auto selected = host.WaitForMessage("layoutImageSelected");
    const std::string woodId = selected ? selected->value("imageId", "") : std::string();

    // An image picker's request id comes back, so the image goes to the picker that asked.
    if (!selected || selected->value("pickerId", "") != "picker-7")
    {
        std::cerr << "The picker's request id did not come back with the image\n";
        return false;
    }

    // The library lists a layout's images once the layout is saved.
    SaveLayout(controller, "A", "fx_a", {woodId});

    if (woodId.empty() || selected->value("category", "") != "background" || category(woodId) != "background")
    {
        std::cerr << "A browsed background was not filed as one: " << (selected ? selected->dump() : "none") << "\n";
        return false;
    }

    controller.HandleUIMessage(
        nlohmann::json{{"type", "setLayoutImageCategory"}, {"imageId", woodId}, {"category", "switch"}}.dump());
    controller.HandleUIMessage(
        nlohmann::json{{"type", "setLayoutImageCategory"}, {"imageId", woodId}, {"category", "drawer"}}.dump());

    if (category(woodId) != "switch")
    {
        std::cerr << "Refiling did not stick, or an unknown category was taken: " << category(woodId) << "\n";
        return false;
    }

    controller.HandleUIMessage(nlohmann::json{{"type", "saveLayoutImage"},
                                              {"imageId", "imported_1"},
                                              {"fileName", "imported_1.png"},
                                              {"data", guitarfx::util::EncodeBase64(PngBytes())},
                                              {"layoutId", "A"},
                                              {"category", "knob"}}
                                   .dump());

    if (category("imported_1") != "knob")
    {
        std::cerr << "An imported image did not take its archive's category\n";
        return false;
    }

    controller.HandleUIMessage(
        nlohmann::json{{"type", "setLayoutImageCategory"}, {"imageId", "factory-bat"}, {"category", "knob"}}.dump());
    const auto factory = listed("factory-bat");

    if (factory.value("type", "") != "switch" || !factory.value("factory", false))
    {
        std::cerr << "The factory image was not listed under its folder, or was refiled: " << factory.dump() << "\n";
        return false;
    }

    return true;
}

/// An image's licence and credit are recorded, listed with it, cleared, refused when the licence is
/// unknown or the credit too long, and taken from an imported archive.
bool TestImageCreditsAreRecordedAndListed()
{
    const fs::path sandbox = FreshSandbox("image-credits");
    const ScopedPlatformRoot root(sandbox);

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);

    SaveImage(controller, "A", "knob_1", "knob_1.png", PngBytes());
    SaveLayout(controller, "A", "fx_a", {"knob_1"});
    const auto credit = [&](const std::string& imageId) {
        const auto image = ListedImage(host, controller, imageId);
        return image.value("license", "") + "|" + image.value("credit", "");
    };
    const auto setCredit = [&](const std::string& license, const std::string& text) {
        controller.HandleUIMessage(nlohmann::json{
            {"type", "setLayoutImageCredit"},
            {"imageId", "knob_1"},
            {"license", license},
            {"credit", text}}.dump());
    };

    setCredit("CC-BY-4.0", "Knob by Ada");

    if (credit("knob_1") != "CC-BY-4.0|Knob by Ada")
    {
        std::cerr << "The credit was not listed with the image: " << credit("knob_1") << "\n";
        return false;
    }

    setCredit("WTFPL", "Someone else");
    setCredit("own", std::string(301, 'x'));

    if (credit("knob_1") != "CC-BY-4.0|Knob by Ada")
    {
        std::cerr << "An unknown licence or an overlong credit replaced the credit: " << credit("knob_1") << "\n";
        return false;
    }

    setCredit("", "");

    if (credit("knob_1") != "|")
    {
        std::cerr << "Clearing both fields kept the credit: " << credit("knob_1") << "\n";
        return false;
    }

    controller.HandleUIMessage(nlohmann::json{{"type", "saveLayoutImage"},
                                              {"imageId", "imported_1"},
                                              {"fileName", "imported_1.png"},
                                              {"data", guitarfx::util::EncodeBase64(PngBytes())},
                                              {"layoutId", "A"},
                                              {"license", "CC0-1.0"},
                                              {"credit", "From the archive"}}
                                   .dump());

    if (credit("imported_1") != "CC0-1.0|From the archive")
    {
        std::cerr << "An imported image did not keep its archive's credit: " << credit("imported_1") << "\n";
        return false;
    }

    return true;
}

/// Images an image knob, toggle, slider or overlay draws on, and the layout's own knob image, are
/// the layout's like any other: a save copies each into its own folder, so they survive the layout
/// they came from.
bool TestImageControlReferencesAreCopiedOnSave()
{
    const fs::path sandbox = FreshSandbox("image-controls");
    const ScopedPlatformRoot root(sandbox);
    const fs::path own = ContentDir(sandbox) / "B" / "images";

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    const std::vector<std::string> ids = {"face_1",         "face_base_1",   "switch_off_1",  "switch_on_1",
                                          "switch_strip_1", "strip_1",       "skirt_1",       "glare_1",
                                          "on_1",           "off_1",         "switchstrip_1", "rail_1",
                                          "cap_1",          "sliderstrip_1", "logo_1"};

    for (const auto& id : ids)
    {
        SaveImage(controller, "A", id, id + ".png", PngOfSize(64, 64));
    }

    SaveLayout(controller, "A", "fx_a", {});

    const nlohmann::json layout{
        {"effectType", "fx_b"},
        {"layoutId", "B"},
        {"version", 2},
        {"dimensions", {{"width", 400}, {"height", 280}}},
        {"backgrounds", nlohmann::json::array()},
        {"knobImage", {{"mode", "rotate"}, {"imageId", "face_1"}, {"baseImageId", "face_base_1"}}},
        {"switchImage",
         {{"offImageId", "switch_off_1"},
          {"onImageId", "switch_on_1"},
          {"strip", {{"imageId", "switch_strip_1"}, {"frames", 2}}}}},
        {"controls",
         {{{"paramKey", "a"},
           {"type", "knob"},
           {"position", {{"x", 0}, {"y", 0}}},
           {"style",
            {{"knobImage",
              {{"mode", "filmstrip"},
               {"imageId", "strip_1"},
               {"frames", 64},
               {"baseImageId", "skirt_1"},
               {"glareImageId", "glare_1"}}}}}},
          {{"paramKey", "b"},
           {"type", "toggle"},
           {"position", {{"x", 0}, {"y", 0}}},
           {"style",
            {{"toggleImage",
              {{"onImageId", "on_1"},
               {"offImageId", "off_1"},
               {"strip", {{"imageId", "switchstrip_1"}, {"frames", 2}}}}}}}},
          {{"paramKey", "c"},
           {"type", "slider"},
           {"position", {{"x", 0}, {"y", 0}}},
           {"style",
            {{"sliderImage",
              {{"trackImageId", "rail_1"},
               {"thumbImageId", "cap_1"},
               {"strip", {{"imageId", "sliderstrip_1"}, {"frames", 64}}}}}}}}}},
        {"overlays",
         {{{"id", "o"},
           {"position", {{"x", 0}, {"y", 0}}},
           {"size", {{"width", 10}, {"height", 10}}},
           {"style", {{"imageId", "logo_1"}}}}}},
        {"textLabels", nlohmann::json::array()}};

    if (guitarfx::controller_detail::CollectLayoutImageIds(layout) != ids)
    {
        std::cerr << "CollectLayoutImageIds missed an image control's image\n";
        return false;
    }

    controller.HandleUIMessage(nlohmann::json{
        {"type", "saveEffectLayout"}, {"effectType", "fx_b"}, {"blendId", ""}, {"layoutId", "B"}, {"layout", layout}}
                                   .dump());

    for (const auto& id : ids)
    {
        if (!fs::is_regular_file(own / (id + ".png")))
        {
            std::cerr << "Saving did not copy " << id << " into the layout's own folder\n";
            return false;
        }
    }

    return true;
}

/// A save the engine cannot complete says so, naming the layout, rather than claiming success.
bool TestFailedSaveIsReported()
{
    const fs::path sandbox = FreshSandbox("failed-save");
    const ScopedPlatformRoot root(sandbox);

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    controller.HandleUIMessage(R"({"type":"saveEffectLayout","layoutId":"X","layout":{"version":1}})");

    const auto failed = host.WaitForMessage("layoutSaveFailed", std::chrono::milliseconds(500));

    if (!failed || failed->value("layoutId", "") != "X" || host.CountMessages("layoutSaved") != 0)
    {
        std::cerr << "A save with no effect type was not reported as failed\n";
        return false;
    }

    return true;
}

/// A layout listed under an effect type and a blend of it is gone from both once deleted.
bool TestDeleteRemovesTheLayoutFromEveryKey()
{
    const fs::path sandbox = FreshSandbox("delete-every-key");
    const ScopedPlatformRoot root(sandbox);

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    SaveLayout(controller, "L", "fx", {});
    SaveLayout(controller, "L", "fx", {}, "blend1");
    controller.HandleUIMessage(
        nlohmann::json{{"type", "deleteLayout"}, {"effectType", "fx"}, {"blendId", ""}, {"layoutId", "L"}}.dump());

    std::ifstream input(sandbox / "data" / "v1" / "layouts" / "indexes" / "effect-layouts.json");
    const auto index = nlohmann::json::parse(input, nullptr, false);
    const auto associations =
        index.is_object() ? index.value("associations", nlohmann::json::object()) : nlohmann::json::object();

    if (associations.contains("fx") || associations.contains("fx::blend1"))
    {
        std::cerr << "The deleted layout is still indexed: " << index.dump() << "\n";
        return false;
    }

    const auto library = host.WaitForMessage("layoutLibraryLoaded");
    const auto byEffectType =
        library
            ? library->value("layoutLibrary", nlohmann::json::object()).value("byEffectType", nlohmann::json::object())
            : nlohmann::json::object();

    if (!library || byEffectType.contains("fx") || byEffectType.contains("fx::blend1"))
    {
        std::cerr << "The library still lists the deleted layout\n";
        return false;
    }

    return true;
}
} // namespace

int main()
{
    struct Test
    {
        const char* name;
        bool (*run)();
    };

    const Test tests[] = {
        {"Imported image names stay in the layout folder", TestImportedImageNamesStayInTheLayoutFolder},
        {"Borrowed images survive deleting their layout", TestBorrowedImagesSurviveDeletingTheirLayout},
        {"Loader lists legacy and upper-case images", TestLoaderListsLegacyAndUpperCaseImages},
        {"Shared image ids resolve as the library shows them", TestSharedImageIdsResolveAsTheLibraryShowsThem},
        {"Images are listed by URL and served by the route", TestImagesAreListedByUrlAndServedByTheRoute},
        {"Image sizes are read from headers", TestImageSizesAreReadFromHeaders},
        {"Large imported images are stored", TestLargeImportedImagesAreStored},
        {"Renditions are stored, listed and served", TestRenditionsAreStoredListedAndServed},
        {"Picking the same image again reuses it", TestPickingTheSameImageAgainReusesIt},
        {"Image categories are filed, listed and shipped", TestImageCategoriesAreFiledListedAndShipped},
        {"Image credits are recorded and listed", TestImageCreditsAreRecordedAndListed},
        {"Image control references are copied on save", TestImageControlReferencesAreCopiedOnSave},
        {"A failed save is reported", TestFailedSaveIsReported},
        {"Delete removes the layout from every key", TestDeleteRemovesTheLayoutFromEveryKey},
    };

    int failed = 0;

    for (const auto& test : tests)
    {
        bool ok = false;

        try
        {
            ok = test.run();
        }
        catch (const std::exception& e)
        {
            std::cerr << "Exception: " << e.what() << "\n";
        }

        std::cout << (ok ? "[PASS] " : "[FAIL] ") << test.name << "\n";
        failed += ok ? 0 : 1;
    }

    std::error_code ec;
    fs::remove_all(fs::temp_directory_path() / ("guitarfx-layout-storage-tests-" + std::to_string(GUITARFX_TEST_PID)),
                   ec);
    return failed == 0 ? 0 : 1;
}
