#pragma once

// Pictures: the effect icons and the equipment artwork the web UI shows.
//
// Artist draws no SVG files, but it does parse SVG path data, which is most of what the icons in
// core/ui/images/icons are. Icons reads the rest of the small subset they use (path, line,
// circle, ellipse, rect, polyline and polygon, under groups with transforms, styled by attribute
// or by `style`) into Artist paths once, and draws them in one colour, as the web UI's icons
// take `currentColor`. Images loads the stock artwork (PNG, JPEG) once per file.

#include "ui/widgets/TextDraw.h"
#include "ui/widgets/Theme.h"

#include <artist/image.hpp>
#include <artist/path.hpp>

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nanoq::ui
{
namespace artist = cycfi::artist;

/// One icon, read from its SVG file.
struct SvgIcon
{
    struct Shape
    {
        artist::path path;
        artist::affine_transform transform = artist::affine_identity;
        // How strongly each is painted, 0 for not at all: a grey in a many-toned icon draws
        // the colour fainter, as `fill-opacity` does.
        float fillOpacity = 0.0f;
        float strokeOpacity = 1.0f;
        float strokeWidth = 2.0f;
        artist::canvas::line_cap_enum cap = artist::canvas::round;
        artist::canvas::join_enum join = artist::canvas::round_join;
    };

    el::rect viewBox{0, 0, 24, 24};
    std::vector<Shape> shapes;
};

/// Reads an icon from SVG text; nullopt if it holds nothing drawable.
[[nodiscard]] std::optional<SvgIcon> ParseSvgIcon(const std::string& svg);

/// Draws an icon fitted to `bounds`, centred, in `colour`.
void DrawIcon(artist::canvas& cnv, const SvgIcon& icon, el::rect bounds, el::color colour);

/// The icons by name ("amp", "speaker", ...: the files in images/icons, without the extension),
/// each read the first time it is asked for.
class Icons
{
  public:
    explicit Icons(std::filesystem::path folder) : mFolder(std::move(folder))
    {
    }

    /// The icon, or null when the file is missing or unreadable.
    [[nodiscard]] const SvgIcon* Find(const std::string& name);

    /// Draws `name` (or, if it is missing, the gear) into `bounds`.
    void Draw(artist::canvas& cnv, const std::string& name, el::rect bounds, el::color colour);

    /// Adds an icon from SVG text under `name`, for the ones the web UI writes inline in its
    /// markup rather than keeping as files.
    void Add(const std::string& name, const std::string& svg);

  private:
    std::filesystem::path mFolder;
    std::map<std::string, std::optional<SvgIcon>> mIcons;
};

/// The artwork by path relative to the ui folder ("images/equipment/amps/full-rig-1.jpg"), each
/// loaded the first time it is asked for.
class Images
{
  public:
    explicit Images(std::filesystem::path uiFolder) : mFolder(std::move(uiFolder))
    {
    }

    /// The image, or null when the file is missing or cannot be decoded.
    [[nodiscard]] std::shared_ptr<artist::image> Find(const std::string& relativePath);

  private:
    std::filesystem::path mFolder;
    std::map<std::string, std::shared_ptr<artist::image>> mImages;
};

/// The width `text` takes in `font`, measured on a small offscreen canvas: for laying out a row
/// before it is drawn. UI thread.
[[nodiscard]] float MeasureTextWidth(const std::string& text, const el::font_descr& font);

/// Pictures downloaded at run time (tone sharing packs, Tone3000 tones, avatars), from bytes the
/// caller fetched. Artist reads pictures from files, so each is written once to a cache folder
/// and loaded from there. UI thread.
class RemoteImages
{
  public:
    explicit RemoteImages(std::filesystem::path cacheFolder) : mFolder(std::move(cacheFolder))
    {
    }

    /// The picture for `url`, given its bytes: null when `bytes` is null (still fetching) or the
    /// picture cannot be decoded.
    [[nodiscard]] std::shared_ptr<artist::image> Find(const std::string& url, const std::string* bytes);

  private:
    std::filesystem::path mFolder;
    std::map<std::string, std::shared_ptr<artist::image>> mImages; // a null entry: tried, failed
};

/// Draws `image` as large as fits `bounds` (contain), centred, with its aspect kept.
void DrawImageFitted(artist::canvas& cnv, const artist::image& image, el::rect bounds);

/// Draws `image` to fill `bounds` (cover), centred, cropping what overflows.
void DrawImageCovering(artist::canvas& cnv, const artist::image& image, el::rect bounds);

/// Strokes a rounded rectangle dashed, as CSS's `border-style: dashed`, in the stroke style and
/// width already set: `dash` drawn and `gap` left out, all the way round.
void StrokeDashedRoundRect(artist::canvas& cnv, el::rect r, float radius, float dash, float gap);
} // namespace nanoq::ui
