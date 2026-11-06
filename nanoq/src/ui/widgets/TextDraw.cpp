// See TextDraw.h.

#include "ui/widgets/TextDraw.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <vector>

namespace nanoq::ui
{
namespace artist = cycfi::artist;

std::string Ellipsize(artist::canvas& cnv, const std::string& text, const el::font_descr& font, float width)
{
    if (text.empty() || el::measure_text(cnv, text, font).x <= width)
    {
        return text;
    }

    static const std::string kEllipsis = "\xE2\x80\xA6";
    std::string cut = text;

    while (!cut.empty())
    {
        // Drop one whole UTF-8 character from the end: its continuation bytes, then the byte
        // that leads them. A lead byte left behind makes the string invalid, and Quartz's text
        // measuring crashes on one.
        while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80)
        {
            cut.pop_back();
        }

        if (!cut.empty())
        {
            cut.pop_back();
        }

        if (el::measure_text(cnv, cut + kEllipsis, font).x <= width)
        {
            return cut + kEllipsis;
        }
    }

    return kEllipsis;
}

std::vector<std::string> WrapLines(artist::canvas& cnv, const std::string& text, const el::font_descr& font,
                                   float width, int maxLines)
{
    std::vector<std::string> lines;
    std::string line;
    std::size_t i = 0;

    while (i < text.size() && static_cast<int>(lines.size()) < maxLines)
    {
        auto end = text.find(' ', i);

        if (end == std::string::npos)
        {
            end = text.size();
        }

        const auto word = text.substr(i, end - i);
        const auto candidate = line.empty() ? word : line + " " + word;

        if (!line.empty() && el::measure_text(cnv, candidate, font).x > width)
        {
            lines.push_back(line);
            line = word;
        }
        else
        {
            line = candidate;
        }

        i = end + 1;
    }

    if (static_cast<int>(lines.size()) < maxLines && !line.empty())
    {
        lines.push_back(line);
    }

    if (!lines.empty() && (i < text.size() || el::measure_text(cnv, lines.back(), font).x > width))
    {
        lines.back() = Ellipsize(cnv, lines.back() + (i < text.size() ? "..." : ""), font, width);
    }

    return lines;
}

float LineHeight(artist::canvas& cnv, const el::font_descr& font)
{
    return el::measure_text(cnv, "Ag", font).y;
}

void FillText(artist::canvas& cnv, const std::string& text, const el::font_descr& font, el::color colour,
              el::rect bounds, HAlign align)
{
    auto state = cnv.new_state();
    cnv.font(font);
    cnv.fill_style(colour);

    float x = bounds.left;
    int flags = artist::canvas::left;

    if (align == HAlign::Centre)
    {
        x = bounds.left + bounds.width() * 0.5f;
        flags = artist::canvas::center;
    }
    else if (align == HAlign::Right)
    {
        x = bounds.right;
        flags = artist::canvas::right;
    }

    cnv.text_align(flags | artist::canvas::middle);
    cnv.fill_text(text, el::point{x, bounds.top + bounds.height() * 0.5f});
}

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string Upper(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return c < 0x80 ? static_cast<char>(std::toupper(c)) : static_cast<char>(c);
    });
    return text;
}
} // namespace nanoq::ui
