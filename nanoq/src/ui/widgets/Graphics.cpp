#include "ui/widgets/Graphics.h"

#include <elements/support/text_utils.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace nanoq::ui
{
namespace
{
using Attributes = std::map<std::string, std::string>;

/// What a shape inherits from the groups around it.
struct Paint
{
    std::optional<float> fill = 1.0f; // the default SVG fill is black: painted
    std::optional<float> stroke;      // and no stroke
    float strokeWidth = 1.0f;
    float opacity = 1.0f;
    artist::canvas::line_cap_enum cap = artist::canvas::butt;
    artist::canvas::join_enum join = artist::canvas::miter_join;
    artist::affine_transform transform = artist::affine_identity;
};

std::string Trim(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t\r\n");

    if (first == std::string_view::npos)
    {
        return {};
    }

    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

std::vector<float> Numbers(std::string_view text)
{
    std::vector<float> out;
    std::string buffer(text);
    const char* s = buffer.c_str();

    while (*s != '\0')
    {
        if (std::isdigit(static_cast<unsigned char>(*s)) || *s == '-' || *s == '+' || *s == '.')
        {
            char* end = nullptr;
            const float value = std::strtof(s, &end);

            if (end == s)
            {
                ++s;
                continue;
            }

            out.push_back(value);
            s = end;
        }
        else
        {
            ++s;
        }
    }

    return out;
}

float Number(const Attributes& a, const char* key, float fallback = 0.0f)
{
    const auto it = a.find(key);

    if (it == a.end())
    {
        return fallback;
    }

    const auto n = Numbers(it->second);
    return n.empty() ? fallback : n.front();
}

/// A paint value as a strength: none is nullopt; a colour is 1, or less for a grey darker than
/// the icons' own near-white (#e9edf8), so a many-toned icon keeps its shading in one colour.
std::optional<float> PaintStrength(const std::string& value)
{
    const auto v = Trim(value);

    if (v.empty() || v == "none" || v == "transparent")
    {
        return std::nullopt;
    }

    if (v[0] != '#')
    {
        return 1.0f; // currentColor, a named colour, a url(): painted
    }

    unsigned r = 0, g = 0, b = 0;

    if (v.size() == 7)
    {
        const auto rgb = std::strtoul(v.c_str() + 1, nullptr, 16);
        r = (rgb >> 16) & 0xff;
        g = (rgb >> 8) & 0xff;
        b = rgb & 0xff;
    }
    else if (v.size() == 4)
    {
        const auto rgb = std::strtoul(v.c_str() + 1, nullptr, 16);
        r = ((rgb >> 8) & 0xf) * 17;
        g = ((rgb >> 4) & 0xf) * 17;
        b = (rgb & 0xf) * 17;
    }
    else
    {
        return 1.0f;
    }

    constexpr float kIconWhite = 0.925f;
    const float luminance =
        (0.2126f * static_cast<float>(r) + 0.7152f * static_cast<float>(g) + 0.0722f * static_cast<float>(b)) / 255.0f;
    return std::clamp(luminance / kIconWhite, 0.25f, 1.0f);
}

artist::affine_transform ParseTransform(std::string_view text)
{
    auto result = artist::affine_identity;
    std::size_t at = 0;

    while (at < text.size())
    {
        const auto open = text.find('(', at);
        const auto close = text.find(')', open);

        if (open == std::string_view::npos || close == std::string_view::npos)
        {
            break;
        }

        const auto name = Trim(text.substr(at, open - at));
        const auto n = Numbers(text.substr(open + 1, close - open - 1));
        artist::affine_transform t = artist::affine_identity;

        if (name == "matrix" && n.size() == 6)
        {
            t = {n[0], n[1], n[2], n[3], n[4], n[5]};
        }
        else if (name == "translate" && !n.empty())
        {
            t = artist::make_translation(n[0], n.size() > 1 ? n[1] : 0.0f);
        }
        else if (name == "scale" && !n.empty())
        {
            t = artist::make_scale(n[0], n.size() > 1 ? n[1] : n[0]);
        }
        else if (name == "rotate" && !n.empty())
        {
            const double rad = n[0] * 3.14159265358979 / 180.0;

            if (n.size() == 3)
            {
                t = artist::make_translation(n[1], n[2]) * artist::make_rotation(rad) *
                    artist::make_translation(-n[1], -n[2]);
            }
            else
            {
                t = artist::make_rotation(rad);
            }
        }

        result = result * t;
        at = close + 1;
    }

    return result;
}

/// The element's attributes with its `style` declarations folded in (style wins, as in CSS).
Attributes WithStyle(Attributes a)
{
    const auto it = a.find("style");

    if (it == a.end())
    {
        return a;
    }

    std::stringstream declarations(it->second);
    std::string declaration;

    while (std::getline(declarations, declaration, ';'))
    {
        const auto colon = declaration.find(':');

        if (colon != std::string::npos)
        {
            a[Trim(declaration.substr(0, colon))] = Trim(declaration.substr(colon + 1));
        }
    }

    return a;
}

Paint Inherit(const Paint& parent, const Attributes& a)
{
    Paint p = parent;
    p.transform = artist::affine_identity;

    auto has = [&a](const char* key) { return a.find(key) != a.end(); };

    if (has("fill"))
    {
        p.fill = PaintStrength(a.at("fill"));
    }

    if (has("stroke"))
    {
        p.stroke = PaintStrength(a.at("stroke"));
    }

    if (has("stroke-width"))
    {
        p.strokeWidth = Number(a, "stroke-width", p.strokeWidth);
    }

    if (has("fill-opacity") && p.fill)
    {
        p.fill = *p.fill * Number(a, "fill-opacity", 1.0f);
    }

    if (has("stroke-opacity") && p.stroke)
    {
        p.stroke = *p.stroke * Number(a, "stroke-opacity", 1.0f);
    }

    if (has("opacity"))
    {
        p.opacity *= Number(a, "opacity", 1.0f);
    }

    if (has("stroke-linecap"))
    {
        const auto& v = a.at("stroke-linecap");
        p.cap = v == "round" ? artist::canvas::round : v == "square" ? artist::canvas::square : artist::canvas::butt;
    }

    if (has("stroke-linejoin"))
    {
        const auto& v = a.at("stroke-linejoin");
        p.join = v == "round"   ? artist::canvas::round_join
                 : v == "bevel" ? artist::canvas::bevel_join
                                : artist::canvas::miter_join;
    }

    if (has("transform"))
    {
        p.transform = ParseTransform(a.at("transform"));
    }

    return p;
}

std::optional<artist::path> ShapePath(const std::string& name, const Attributes& a)
{
    artist::path path;

    if (name == "path")
    {
        const auto it = a.find("d");

        if (it == a.end())
        {
            return std::nullopt;
        }

        return artist::path(std::string_view(it->second));
    }

    if (name == "line")
    {
        path.move_to(Number(a, "x1"), Number(a, "y1"));
        path.line_to(Number(a, "x2"), Number(a, "y2"));
        return path;
    }

    if (name == "circle")
    {
        path.add_circle(Number(a, "cx"), Number(a, "cy"), Number(a, "r"));
        return path;
    }

    if (name == "ellipse")
    {
        // Four quarter curves (0.5523 is the cubic Bezier circle constant).
        const float cx = Number(a, "cx"), cy = Number(a, "cy"), rx = Number(a, "rx"), ry = Number(a, "ry", rx);
        constexpr float k = 0.5523f;
        path.move_to(cx + rx, cy);
        path.bezier_curve_to(cx + rx, cy + ry * k, cx + rx * k, cy + ry, cx, cy + ry);
        path.bezier_curve_to(cx - rx * k, cy + ry, cx - rx, cy + ry * k, cx - rx, cy);
        path.bezier_curve_to(cx - rx, cy - ry * k, cx - rx * k, cy - ry, cx, cy - ry);
        path.bezier_curve_to(cx + rx * k, cy - ry, cx + rx, cy - ry * k, cx + rx, cy);
        path.close();
        return path;
    }

    if (name == "rect")
    {
        const float x = Number(a, "x"), y = Number(a, "y"), w = Number(a, "width"), h = Number(a, "height");
        const float rx = Number(a, "rx", Number(a, "ry"));

        if (rx > 0.0f)
        {
            path.add_round_rect(x, y, w, h, rx);
        }
        else
        {
            path.add_rect(x, y, w, h);
        }

        return path;
    }

    if (name == "polyline" || name == "polygon")
    {
        const auto it = a.find("points");

        if (it == a.end())
        {
            return std::nullopt;
        }

        const auto n = Numbers(it->second);

        if (n.size() < 4)
        {
            return std::nullopt;
        }

        path.move_to(n[0], n[1]);

        for (std::size_t i = 2; i + 1 < n.size(); i += 2)
        {
            path.line_to(n[i], n[i + 1]);
        }

        if (name == "polygon")
        {
            path.close();
        }

        return path;
    }

    return std::nullopt;
}

/// Reads `name attr="v" ...` from the inside of a tag.
std::pair<std::string, Attributes> ReadTag(std::string_view tag)
{
    std::size_t i = 0;

    while (i < tag.size() && !std::isspace(static_cast<unsigned char>(tag[i])) && tag[i] != '/')
    {
        ++i;
    }

    std::string name(tag.substr(0, i));

    Attributes attributes;

    while (i < tag.size())
    {
        while (i < tag.size() && (std::isspace(static_cast<unsigned char>(tag[i])) || tag[i] == '/'))
        {
            ++i;
        }

        const auto eq = tag.find('=', i);

        if (eq == std::string_view::npos)
        {
            break;
        }

        const auto key = Trim(tag.substr(i, eq - i));
        auto q = eq + 1;

        while (q < tag.size() && std::isspace(static_cast<unsigned char>(tag[q])))
        {
            ++q;
        }

        if (q >= tag.size() || (tag[q] != '"' && tag[q] != '\''))
        {
            break;
        }

        const char quote = tag[q];
        const auto end = tag.find(quote, q + 1);

        if (end == std::string_view::npos)
        {
            break;
        }

        attributes[key] = std::string(tag.substr(q + 1, end - q - 1));
        i = end + 1;
    }

    return {std::move(name), std::move(attributes)};
}
} // namespace

// ── Icons ───────────────────────────────────────────────────────────────────────

std::optional<SvgIcon> ParseSvgIcon(const std::string& svg)
{
    SvgIcon icon;
    std::vector<Paint> stack{Paint{}};
    std::vector<artist::affine_transform> transforms{artist::affine_identity};
    std::string skipUntil; // inside <defs> and the like: nothing there is drawn

    std::size_t at = 0;

    while ((at = svg.find('<', at)) != std::string::npos)
    {
        if (svg.compare(at, 4, "<!--") == 0)
        {
            const auto end = svg.find("-->", at);
            at = end == std::string::npos ? svg.size() : end + 3;
            continue;
        }

        const auto close = svg.find('>', at);

        if (close == std::string::npos)
        {
            break;
        }

        std::string_view tag(svg.data() + at + 1, close - at - 1);
        at = close + 1;

        if (tag.empty() || tag[0] == '?' || tag[0] == '!')
        {
            continue;
        }

        if (tag[0] == '/')
        {
            const auto name = Trim(tag.substr(1));

            if (!skipUntil.empty())
            {
                if (name == skipUntil)
                {
                    skipUntil.clear();
                }

                continue;
            }

            if ((name == "g" || name == "svg") && stack.size() > 1)
            {
                stack.pop_back();
                transforms.pop_back();
            }

            continue;
        }

        if (!skipUntil.empty())
        {
            continue;
        }

        const bool selfClosing = tag.back() == '/';
        auto [name, raw] = ReadTag(tag);

        // Editor metadata (sodipodi:, inkscape:) draws nothing.
        if (name.find(':') != std::string::npos)
        {
            continue;
        }

        if (name == "defs" || name == "clipPath" || name == "mask" || name == "linearGradient" ||
            name == "radialGradient" || name == "title" || name == "desc" || name == "style")
        {
            if (!selfClosing)
            {
                skipUntil = name;
            }

            continue;
        }

        const auto attributes = WithStyle(std::move(raw));
        const auto paint = Inherit(stack.back(), attributes);
        const auto transform = transforms.back() * paint.transform;

        if (name == "svg" || name == "g")
        {
            if (name == "svg")
            {
                if (const auto it = attributes.find("viewBox"); it != attributes.end())
                {
                    const auto n = Numbers(it->second);

                    if (n.size() == 4 && n[2] > 0.0f && n[3] > 0.0f)
                    {
                        icon.viewBox = el::rect{n[0], n[1], n[0] + n[2], n[1] + n[3]};
                    }
                }
            }

            if (!selfClosing)
            {
                stack.push_back(paint);
                transforms.push_back(transform);
            }

            continue;
        }

        auto path = ShapePath(name, attributes);

        if (!path)
        {
            continue;
        }

        SvgIcon::Shape shape;
        shape.path = std::move(*path);
        shape.transform = transform;
        // A line has no inside to fill.
        shape.fillOpacity = (paint.fill && name != "line") ? *paint.fill * paint.opacity : 0.0f;
        shape.strokeOpacity = paint.stroke ? *paint.stroke * paint.opacity : 0.0f;
        shape.strokeWidth = paint.strokeWidth;
        shape.cap = paint.cap;
        shape.join = paint.join;

        if (shape.fillOpacity > 0.0f || shape.strokeOpacity > 0.0f)
        {
            icon.shapes.push_back(std::move(shape));
        }
    }

    if (icon.shapes.empty())
    {
        return std::nullopt;
    }

    return icon;
}

void DrawIcon(artist::canvas& cnv, const SvgIcon& icon, el::rect bounds, el::color colour)
{
    const auto& vb = icon.viewBox;

    if (vb.width() <= 0.0f || vb.height() <= 0.0f || bounds.width() <= 0.0f || bounds.height() <= 0.0f)
    {
        return;
    }

    const float scale = std::min(bounds.width() / vb.width(), bounds.height() / vb.height());
    const float ox = bounds.left + (bounds.width() - vb.width() * scale) * 0.5f - vb.left * scale;
    const float oy = bounds.top + (bounds.height() - vb.height() * scale) * 0.5f - vb.top * scale;

    auto state = cnv.new_state();
    // Artist's transform() replaces the whole matrix, so each shape's is built on what the
    // canvas had (the view's scale and position) rather than added to it.
    const auto base = cnv.transform() * artist::make_translation(ox, oy) * artist::make_scale(scale);

    for (const auto& shape : icon.shapes)
    {
        cnv.transform(base * shape.transform);
        cnv.begin_path();
        cnv.add_path(shape.path);

        const bool stroke = shape.strokeOpacity > 0.0f;

        if (shape.fillOpacity > 0.0f)
        {
            cnv.fill_style(colour.opacity(colour.alpha * shape.fillOpacity));

            if (stroke)
            {
                cnv.fill_preserve();
            }
            else
            {
                cnv.fill();
            }
        }

        if (stroke)
        {
            cnv.stroke_style(colour.opacity(colour.alpha * shape.strokeOpacity));
            cnv.line_width(shape.strokeWidth);
            cnv.line_cap(shape.cap);
            cnv.line_join(shape.join);
            cnv.stroke();
        }
    }
}

const SvgIcon* Icons::Find(const std::string& name)
{
    if (name.empty())
    {
        return nullptr;
    }

    auto it = mIcons.find(name);

    if (it == mIcons.end())
    {
        std::optional<SvgIcon> icon;
        std::ifstream file(mFolder / (name + ".svg"), std::ios::binary);

        if (file)
        {
            std::stringstream text;
            text << file.rdbuf();
            try
            {
                icon = ParseSvgIcon(text.str());
            }
            catch (...)
            {
                icon.reset();
            }
        }

        it = mIcons.emplace(name, std::move(icon)).first;
    }

    return it->second ? &*it->second : nullptr;
}

std::shared_ptr<artist::image> RemoteImages::Find(const std::string& url, const std::string* bytes)
{
    if (url.empty())
    {
        return nullptr;
    }

    if (const auto it = mImages.find(url); it != mImages.end())
    {
        return it->second;
    }

    if (bytes == nullptr)
    {
        return nullptr; // still on its way: asked again when it arrives
    }

    std::shared_ptr<artist::image> image;

    if (!bytes->empty())
    {
        // Named by the URL's hash; the decoder reads the format from the bytes.
        std::error_code ec;
        std::filesystem::create_directories(mFolder, ec);
        const auto file = mFolder / (std::to_string(std::hash<std::string>{}(url)) + ".img");
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
        }
        try
        {
            image = std::make_shared<artist::image>(file);

            if (image->size().x <= 0.0f)
            {
                image.reset();
            }
        }
        catch (...)
        {
            image.reset();
        }
    }

    mImages[url] = image;
    return image;
}

float MeasureTextWidth(const std::string& text, const el::font_descr& font)
{
    // Kept for the life of the process and never destroyed: the drawing backend may already be
    // gone when static destructors run.
    static auto* image = new artist::image(1.0f, 1.0f);
    static auto* offscreen = new artist::offscreen_image(*image);
    static auto* cnv = new artist::canvas(offscreen->context());
    return el::measure_text(*cnv, text, font).x;
}

void Icons::Add(const std::string& name, const std::string& svg)
{
    std::optional<SvgIcon> icon;
    try
    {
        icon = ParseSvgIcon(svg);
    }
    catch (...)
    {
        icon.reset();
    }
    mIcons[name] = std::move(icon);
}

void Icons::Draw(artist::canvas& cnv, const std::string& name, el::rect bounds, el::color colour)
{
    const auto* icon = Find(name);

    if (icon == nullptr)
    {
        icon = Find("gear");
    }

    if (icon != nullptr)
    {
        DrawIcon(cnv, *icon, bounds, colour);
    }
}

std::shared_ptr<artist::image> Images::Find(const std::string& relativePath)
{
    if (relativePath.empty())
    {
        return nullptr;
    }

    auto it = mImages.find(relativePath);

    if (it == mImages.end())
    {
        std::shared_ptr<artist::image> image;
        const auto file = mFolder / std::filesystem::path(relativePath);
        std::error_code ec;

        if (std::filesystem::is_regular_file(file, ec))
        {
            try
            {
                image = std::make_shared<artist::image>(file);
            }
            catch (...)
            {
                image.reset(); // a backend that cannot decode it throws
            }
        }

        it = mImages.emplace(relativePath, std::move(image)).first;
    }

    return it->second;
}

void DrawImageFitted(artist::canvas& cnv, const artist::image& image, el::rect bounds)
{
    const auto size = image.size();

    if (size.x <= 0.0f || size.y <= 0.0f)
    {
        return;
    }

    const float scale = std::min(bounds.width() / size.x, bounds.height() / size.y);
    const float w = size.x * scale, h = size.y * scale;
    const float x = bounds.left + (bounds.width() - w) * 0.5f;
    const float y = bounds.top + (bounds.height() - h) * 0.5f;
    cnv.draw(image, el::rect{x, y, x + w, y + h});
}

void DrawImageCovering(artist::canvas& cnv, const artist::image& image, el::rect bounds)
{
    const auto size = image.size();

    if (size.x <= 0.0f || size.y <= 0.0f || bounds.width() <= 0.0f || bounds.height() <= 0.0f)
    {
        return;
    }

    // The largest part of the picture with the bounds' aspect, from its centre.
    const float aspect = bounds.width() / bounds.height();
    float w = size.x, h = size.x / aspect;

    if (h > size.y)
    {
        h = size.y;
        w = size.y * aspect;
    }

    const float x = (size.x - w) * 0.5f, y = (size.y - h) * 0.5f;
    cnv.draw(image, el::rect{x, y, x + w, y + h}, bounds);
}

void StrokeDashedRoundRect(artist::canvas& cnv, el::rect r, float radius, float dash, float gap)
{
    // The outline as points about 2 px apart, then the dashes laid along it by distance.
    radius = std::max(0.0f, std::min({radius, r.width() * 0.5f, r.height() * 0.5f}));
    std::vector<el::point> points;
    const auto line = [&points](el::point a, el::point b) {
        const int steps = std::max(1, static_cast<int>(std::hypot(b.x - a.x, b.y - a.y) / 2.0f));

        for (int i = 0; i < steps; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(steps);
            points.push_back({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t});
        }
    };
    const auto arc = [&points, radius](el::point centre, float from) {
        constexpr int kSteps = 12;
        constexpr float kQuarter = 1.5707964f;

        for (int i = 0; i < kSteps; ++i)
        {
            const float a = from + kQuarter * static_cast<float>(i) / static_cast<float>(kSteps);
            points.push_back({centre.x + radius * std::cos(a), centre.y + radius * std::sin(a)});
        }
    };
    line({r.left + radius, r.top}, {r.right - radius, r.top});
    arc({r.right - radius, r.top + radius}, -1.5707964f);
    line({r.right, r.top + radius}, {r.right, r.bottom - radius});
    arc({r.right - radius, r.bottom - radius}, 0.0f);
    line({r.right - radius, r.bottom}, {r.left + radius, r.bottom});
    arc({r.left + radius, r.bottom - radius}, 1.5707964f);
    line({r.left, r.bottom - radius}, {r.left, r.top + radius});
    arc({r.left + radius, r.top + radius}, 3.1415927f);
    points.push_back(points.front());

    float travelled = 0.0f;
    bool drawing = false;

    for (std::size_t i = 0; i < points.size(); ++i)
    {
        if (i > 0)
        {
            travelled += std::hypot(points[i].x - points[i - 1].x, points[i].y - points[i - 1].y);
        }

        const bool on = std::fmod(travelled, dash + gap) < dash;

        if (on && !drawing)
        {
            cnv.begin_path();
            cnv.move_to(points[i].x, points[i].y);
            drawing = true;
        }
        else if (drawing)
        {
            cnv.line_to(points[i].x, points[i].y);

            if (!on)
            {
                cnv.stroke();
                drawing = false;
            }
        }
    }

    if (drawing)
    {
        cnv.stroke();
    }
}
} // namespace nanoq::ui
