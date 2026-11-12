#pragma once

/**
 * LayoutSupport.h — On-disk locations for custom effect layouts.
 *
 * A layout is an index entry in one shared settings file plus a content
 * directory of its own. Both the layout handlers and the startup loader need
 * to agree on where those live, so the path rules are stated once here.
 */

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace guitarfx
{
class FileSystem;
}

namespace guitarfx::controller_detail
{
/// Index file listing every known effect layout.
[[nodiscard]] std::filesystem::path ResolveEffectLayoutsSettingsPath(const FileSystem& fileSystem);

/// Reads the layout index, returning an empty object when it is missing or unreadable.
[[nodiscard]] nlohmann::json LoadEffectLayoutsSettings(const FileSystem& fileSystem);

/// Writes the layout index atomically; false when it could not be written.
bool SaveEffectLayoutsSettings(const FileSystem& fileSystem, const nlohmann::json& root);

/// Content directory for one layout (images and layout.json).
[[nodiscard]] std::filesystem::path ResolveLayoutDir(const FileSystem& fileSystem, const std::string& layoutId);

[[nodiscard]] std::filesystem::path ResolveLayoutFilePath(const FileSystem& fileSystem, const std::string& layoutId);

/// Where images went when a layout had no id yet. Nothing writes here any more, but layouts
/// saved before that still refer to images in it.
[[nodiscard]] std::filesystem::path ResolveLegacyLayoutImagesDir(const FileSystem& fileSystem);

/// The factory layouts shipped with the app: one folder each, holding layout.json and images/.
[[nodiscard]] std::filesystem::path ResolveFactoryLayoutsDir(const std::filesystem::path& bundledAssetsRoot);

/// Largest image a layout takes, from the file picker or an imported archive. The UI loads
/// images by URL (LayoutImageUrl), so this bounds only the one base64 message an archive's
/// image arrives in when a layout is imported.
inline constexpr std::size_t kMaxLayoutImageBytes = 64u * 1024u * 1024u;

/// The URL the UI draws image `imageId` from: `layout-images/id/<id>` with the id
/// percent-encoded (everything but A-Z a-z 0-9 - _ . ~), and `?v=<version>` so a file that
/// changes under the same id is fetched again. The editor's resource provider serves it.
[[nodiscard]] std::string LayoutImageUrl(const std::string& imageId, std::uint64_t version);

/// The image id in a `layout-images/id/<id>` resource path (no query), percent-decoded, or
/// empty when the path is not one.
[[nodiscard]] std::string LayoutImageIdFromResourcePath(const std::string& path);

/// The version a layout image's URLs carry: its file's last-write time in clock ticks, so a file
/// replaced under the same id is fetched again and its smaller copies are made again. 0 when the
/// time cannot be read.
[[nodiscard]] std::uint64_t LayoutImageVersion(const std::filesystem::path& file);

/// True for a longest edge a smaller copy of a large layout image is kept at: 1024, 2048 or 4096.
[[nodiscard]] bool IsLayoutRenditionEdge(int edge);

/// Where smaller copies of large layout images are kept: one folder for every layout's images and
/// the factory's, outside every layout's own folder, so older builds never list them and losing
/// one costs only the time to make it again.
[[nodiscard]] std::filesystem::path ResolveLayoutRenditionsDir(const FileSystem& fileSystem);

/// File name of image `imageId`'s copy at `edge` px for source `version`: the id made safe for a
/// file name plus a hash of the original id (two ids that sanitise alike stay apart), the edge
/// and the version, as WebP.
[[nodiscard]] std::string LayoutRenditionFileName(const std::string& imageId, int edge, std::uint64_t version);

/// The URL the UI draws that copy from: `layout-images/rendition/<id>/<edge>?v=<version>`.
[[nodiscard]] std::string LayoutRenditionUrl(const std::string& imageId, int edge, std::uint64_t version);

/// The image id and edge in a `layout-images/rendition/<id>/<edge>` resource path (no query), or
/// nullopt when the path is not one.
[[nodiscard]] std::optional<std::pair<std::string, int>> LayoutRenditionFromResourcePath(const std::string& path);

/// The pixel size an image file declares, read from its header without decoding it.
struct LayoutImageSize
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

/// The pixel size in a PNG, JPEG, GIF or WebP header, or an SVG's width and height attributes
/// (else its viewBox); nullopt when the file cannot be read or declares none.
[[nodiscard]] std::optional<LayoutImageSize> ReadLayoutImageSize(const std::filesystem::path& path);

/// MIME type of an image a layout can use, from its extension in any case (".JPG" as ".jpg"),
/// or empty for a type layouts do not take.
[[nodiscard]] std::string LayoutImageMimeType(const std::filesystem::path& extension);

/// True when `fileName` can be written inside a layout's images folder as `imageId`: a bare
/// name with no folder, drive or root in it, an image extension layouts take, and `imageId`
/// as its stem, which is how the loader reads the id back. An imported archive names its own
/// images, and "..\..\x.bat" or "C:/x.dll" would otherwise be written where they point.
[[nodiscard]] bool IsSafeLayoutImageFileName(const std::string& fileName, const std::string& imageId);

/// True when `bytes` begin the way an image with that extension does, so an archive cannot
/// write something else under an image's name.
[[nodiscard]] bool LooksLikeLayoutImage(const std::vector<std::uint8_t>& bytes, const std::filesystem::path& extension);

/// One image a layout can draw on: its id and the file that holds it. A factory image comes with
/// the app and cannot be refiled; one from a factory category folder carries that category.
struct LayoutImageSource
{
    std::string imageId;
    std::filesystem::path path;
    std::string category;
    bool factory = false;
};

/// True for a category the designer files images under: "knob", "background", "switch" and
/// "general" (shown as Misc).
[[nodiscard]] bool IsLayoutImageCategory(std::string_view category);

/// Factory images that belong to no layout, one folder per category beside the factory layouts:
/// ui/assets/layout-images/<category>/<imageId>.<ext>.
[[nodiscard]] std::filesystem::path ResolveFactoryLayoutImagesDir(const std::filesystem::path& factoryLayoutsDir);

/// The category the designer filed each user image under, by id. Ids are shared by every layout,
/// so the categories live in one file beside the layout index.
[[nodiscard]] std::map<std::string, std::string> LoadLayoutImageCategories(const FileSystem& fileSystem);

/// Files `imageId` under `category`, or forgets its category when `category` is empty. False when
/// the category is not one IsLayoutImageCategory takes, or the file could not be written.
bool SaveLayoutImageCategory(const FileSystem& fileSystem, const std::string& imageId, const std::string& category);

/// Where a layout image came from: the licence it was shared under and the credit it asks for.
struct LayoutImageCredit
{
    std::string license;
    std::string credit;
};

/// True for a licence the designer offers: the Creative Commons 4.0 licences, CC0, "own" (the
/// user's own work) and "other".
[[nodiscard]] bool IsLayoutImageLicense(std::string_view license);

/// The licence and credit recorded for each user image, by id (layouts/indexes/image-credits.json).
[[nodiscard]] std::map<std::string, LayoutImageCredit> LoadLayoutImageCredits(const FileSystem& fileSystem);

/// Records `credit` for `imageId`, or forgets it when both fields are empty. False for a licence
/// IsLayoutImageLicense does not take, a credit over 300 bytes, or a file that could not be written.
bool SaveLayoutImageCredit(const FileSystem& fileSystem, const std::string& imageId, const LayoutImageCredit& credit);

/// Every image a layout can draw on, each id once, in the order the library first lists it.
/// Ids are shared by every layout, and when two folders hold the same id the later one wins, in
/// this order: the legacy shared folder, then each indexed layout's folder in index order, then
/// the factory layouts, then the factory category folders. That is the file the UI has always shown for an id, so
/// everything that reads an image by id resolves it here. With `includeUnindexed`, layout folders the index does not
/// list (a layout never saved, or one a delete left behind) come first, so they only supply an id found nowhere else.
/// `excludeImagesDir` is left out entirely.
[[nodiscard]] std::vector<LayoutImageSource> CollectLayoutImageSources(
    const FileSystem& fileSystem, const std::filesystem::path& factoryLayoutsDir, bool includeUnindexed = false,
    const std::filesystem::path& excludeImagesDir = {});

/// The library image (CollectLayoutImageSources, unindexed folders included) whose file holds exactly
/// the bytes of `file`, so picking a picture the library already has reuses it instead of adding a
/// copy under a new id. One in `preferredImagesDir` (the layout's own folder) wins over the rest.
[[nodiscard]] std::optional<LayoutImageSource> FindIdenticalLayoutImage(
    const FileSystem& fileSystem, const std::filesystem::path& factoryLayoutsDir, const std::filesystem::path& file,
    const std::filesystem::path& preferredImagesDir);

/// The image ids a layout document draws on, each once, in the order they appear: image background
/// layers, custom knob faces, image knobs (face, base, glare), image toggles and sliders, and image
/// overlays. The UI's layoutImageIds (layoutTypes.ts) lists the same places.
[[nodiscard]] std::vector<std::string> CollectLayoutImageIds(const nlohmann::json& layout);

/// Copies each of `imageIds` that `layoutId`'s own images folder lacks into it, from the file
/// CollectLayoutImageSources resolves the id to, which is the one the user saw. A layout can pick
/// any image in the library, and without its own copy it loses that image when the layout it came
/// from is deleted. Returns the ids found nowhere.
std::vector<std::string> CopyMissingLayoutImages(const FileSystem& fileSystem,
                                                 const std::filesystem::path& factoryLayoutsDir,
                                                 const std::string& layoutId, const std::vector<std::string>& imageIds,
                                                 const std::function<void(const std::string&)>& log);
} // namespace guitarfx::controller_detail
