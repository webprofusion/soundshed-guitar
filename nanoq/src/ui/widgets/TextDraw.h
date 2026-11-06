#pragma once

// Text drawing every owner-drawn element shares: a line cut to fit with an ellipsis, and a
// line placed in a box.

#include "ui/widgets/Theme.h"

#include <string>
#include <vector>

namespace nanoq::ui
{
enum class HAlign
{
    Left,
    Centre,
    Right
};

/// Cuts `text` to fit `width`, ending it with an ellipsis. Measures, so proportional fonts fit exactly.
[[nodiscard]] std::string Ellipsize(cycfi::artist::canvas& cnv, const std::string& text, const el::font_descr& font,
                                    float width);

[[nodiscard]] float LineHeight(cycfi::artist::canvas& cnv, const el::font_descr& font);

/// `text` broken at spaces into at most `maxLines` lines of `width`; what does not fit ends the
/// last line with an ellipsis.
[[nodiscard]] std::vector<std::string> WrapLines(cycfi::artist::canvas& cnv, const std::string& text,
                                                 const el::font_descr& font, float width, int maxLines);

/// One line of text, centred vertically in `bounds` and placed across it by `align`.
void FillText(cycfi::artist::canvas& cnv, const std::string& text, const el::font_descr& font, el::color colour,
              el::rect bounds, HAlign align);

/// ASCII letters in capitals, the rest unchanged: the web UI's uppercase labels.
[[nodiscard]] std::string Upper(std::string text);
[[nodiscard]] std::string Lower(std::string text);
} // namespace nanoq::ui
