// ScaleGuideDraw.cpp - the neck and chord boxes of ScaleGuideDraw.h: the same geometry as the web
// UI's neck.ts (fret spacing half real, half even; inlays; the open string's place left of the nut)
// and chordBox.ts, with colours for the dark and light themes as scale-guide.css has them.

#include "ui/jam/ScaleGuideDraw.h"

#include "ui/widgets/TextDraw.h"

#include <algorithm>
#include <cmath>

namespace nanoq::ui
{
namespace
{
constexpr float kNameX = 14.0f;
constexpr float kOpenX = 44.0f;
constexpr float kNutX = 62.0f;
constexpr float kTop = 24.0f;
constexpr float kStringGap = 30.0f;
constexpr float kDotRadius = 11.5f;
constexpr int kInlays[] = {3, 5, 7, 9, 15, 17, 19, 21};
constexpr int kDoubleInlays[] = {12, 24};
constexpr int kNumbered[] = {3, 5, 7, 9, 12, 15, 17, 19, 21, 24};

constexpr float kBoxStringGap = 15.0f;
constexpr float kBoxFretGap = 18.0f;
constexpr float kBoxPadX = 22.0f;
constexpr float kBoxTop = 30.0f;
constexpr int kBoxRows = 5;

int IntAt(const nlohmann::json& object, const char* key, int fallback = 0)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_number_integer() ? it->get<int>() : fallback;
}

std::string StringAt(const nlohmann::json& object, const char* key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::vector<int> Ints(const nlohmann::json& array)
{
    std::vector<int> out;

    if (array.is_array())
    {
        for (const auto& v : array)
        {
            out.push_back(v.is_number_integer() ? v.get<int>() : -1);
        }
    }

    return out;
}

/// Where fret `fret`'s wire sits between the nut and `end`: half real spacing, half even.
float FretX(int fret, int frets, float end)
{
    if (fret <= 0)
    {
        return kNutX;
    }

    const double real = (1.0 - std::pow(2.0, -fret / 12.0)) / (1.0 - std::pow(2.0, -frets / 12.0));
    return kNutX + (end - kNutX) * static_cast<float>(0.55 * real + 0.45 * fret / static_cast<double>(frets));
}

float NoteX(int fret, int frets, float end)
{
    return fret <= 0 ? kOpenX : (FretX(fret - 1, frets, end) + FretX(fret, frets, end)) * 0.5f;
}

void Circle(cycfi::artist::canvas& cnv, float x, float y, float r)
{
    cnv.begin_path();
    cnv.add_circle(cycfi::artist::circle({x, y}, r));
}

void Line(cycfi::artist::canvas& cnv, float x1, float y1, float x2, float y2)
{
    cnv.begin_path();
    cnv.move_to(x1, y1);
    cnv.line_to(x2, y2);
    cnv.stroke();
}

void Cross(cycfi::artist::canvas& cnv, float x, float y, float half)
{
    Line(cnv, x - half, y - half, x + half, y + half);
    Line(cnv, x - half, y + half, x + half, y - half);
}
} // namespace

GuideShape ReadGuideShape(const nlohmann::json& shape)
{
    GuideShape out;
    out.frets = Ints(shape.value("frets", nlohmann::json::array()));
    out.fingers = Ints(shape.value("fingers", nlohmann::json::array()));
    out.position = IntAt(shape, "position");
    out.caged = StringAt(shape, "caged");

    for (const auto& b : shape.value("barres", nlohmann::json::array()))
    {
        out.barres.push_back({IntAt(b, "fret"), IntAt(b, "fromString"), IntAt(b, "toString")});
    }

    return out;
}

GuideModel ReadGuideModel(const nlohmann::json& view)
{
    GuideModel m;

    if (!view.is_object())
    {
        return m;
    }

    for (const auto& s : view.value("strings", nlohmann::json::array()))
    {
        m.openMidi.push_back(IntAt(s, "openMidi"));
        m.stringNames.push_back(StringAt(s, "name"));
    }

    m.frets = std::max(1, IntAt(view, "frets", 24));
    m.capo = IntAt(view, "capo");
    m.chord = StringAt(view, "mode") == "chord";
    m.position = IntAt(view, "position");
    const auto tones = view.value("tones", nlohmann::json::array());
    m.rootPitchClass = !tones.empty() ? IntAt(tones[0], "pitchClass") : 0;

    for (const auto& d : view.value("dots", nlohmann::json::array()))
    {
        GuideDot dot;
        dot.string = IntAt(d, "string");
        dot.fret = IntAt(d, "fret");
        dot.midi = (dot.string >= 0 && dot.string < static_cast<int>(m.openMidi.size())
                        ? m.openMidi[static_cast<std::size_t>(dot.string)]
                        : 0) +
                   dot.fret;
        dot.name = StringAt(d, "name");
        dot.interval = StringAt(d, "interval");
        dot.degree = IntAt(d, "degree", 1);
        dot.root = d.value("root", false);
        dot.inPosition = d.value("inPosition", true);
        m.dots.push_back(std::move(dot));
    }

    for (const auto& s : view.value("shapes", nlohmann::json::array()))
    {
        m.shapes.push_back(ReadGuideShape(s));
    }

    return m;
}

ScaleNeck::ScaleNeck(const Theme& theme, GuideModel model, std::string labels, bool leftHanded)
    : mTheme(theme), mModel(std::move(model)), mLabels(std::move(labels)), mLeftHanded(leftHanded)
{
}

el::view_limits ScaleNeck::limits(const el::basic_context&) const
{
    const auto strings = static_cast<float>(std::max<std::size_t>(1, mModel.openMidi.size()));
    const float height = kTop + (strings - 1.0f) * kStringGap + 52.0f;
    return {{560.0f, height}, {el::full_extent, height}};
}

bool ScaleNeck::SetHeard(std::vector<int> notes)
{
    if (notes == mHeard)
    {
        return false;
    }

    mHeard = std::move(notes);
    return true;
}

void ScaleNeck::draw(const el::context& ctx)
{
    NoteDrawn(ctx);
    auto& cnv = ctx.canvas;
    const auto b = ctx.bounds;
    const int strings = static_cast<int>(mModel.openMidi.size());

    if (strings == 0)
    {
        return;
    }

    const float width = b.width();
    const float end = width - 8.0f;
    const auto x = [&](float value) { return b.left + (mLeftHanded ? width - value : value); };
    const auto y = [&](int string) { return b.top + kTop + static_cast<float>(strings - 1 - string) * kStringGap; };
    const float top = y(strings - 1) - 18.0f;
    const float bottom = y(0) + 18.0f;
    const float middle = (top + bottom) * 0.5f;
    const int frets = mModel.frets;

    // The board, its inlays and its frets.
    cnv.fill_style(mTheme.Pick(el::rgba(29, 25, 21, 255), el::rgba(232, 220, 203, 255)));
    cnv.begin_path();
    cnv.add_rect({std::min(x(kNutX), x(end)), top, std::max(x(kNutX), x(end)), bottom});
    cnv.fill();
    cnv.fill_style(mTheme.Pick(el::rgba(220, 222, 230, 80), el::rgba(120, 96, 64, 76)));

    for (const int fret : kInlays)
    {
        if (fret <= frets)
        {
            Circle(cnv, x(NoteX(fret, frets, end)), middle, 5.5f);
            cnv.fill();
        }
    }

    for (const int fret : kDoubleInlays)
    {
        if (fret <= frets)
        {
            const float offset = (bottom - top) * 0.22f;
            Circle(cnv, x(NoteX(fret, frets, end)), middle - offset, 5.5f);
            cnv.fill();
            Circle(cnv, x(NoteX(fret, frets, end)), middle + offset, 5.5f);
            cnv.fill();
        }
    }

    cnv.fill_style(mTheme.Pick(el::rgba(141, 144, 151, 255), el::rgba(138, 140, 146, 255)));

    for (int fret = 1; fret <= frets; ++fret)
    {
        cnv.begin_path();
        cnv.add_rect({x(FretX(fret, frets, end)) - 1.25f, top, x(FretX(fret, frets, end)) + 1.25f, bottom});
        cnv.fill();
    }

    cnv.fill_style(mTheme.Pick(el::rgba(230, 224, 207, 255), el::rgba(251, 248, 240, 255)));
    cnv.begin_path();
    cnv.add_round_rect({x(kNutX) - 4.5f, top - 1.0f, x(kNutX) + 4.5f, bottom + 1.0f}, 2.0f);
    cnv.fill();

    if (mModel.capo > 0)
    {
        cnv.fill_style(mTheme.Pick(el::rgba(214, 216, 224, 107), el::rgba(40, 42, 50, 140)));
        const float cx = x(NoteX(mModel.capo, frets, end));
        cnv.begin_path();
        cnv.add_round_rect({cx - 7.0f, top - 4.0f, cx + 7.0f, bottom + 4.0f}, 5.0f);
        cnv.fill();
    }

    // The strings, thicker towards the low ones, their names, and each open string's place.
    const auto nameFont = Font(Weight::Medium).size(13.0f);

    for (int s = 0; s < strings; ++s)
    {
        const bool wound = strings > 5 ? s < strings - 3 : s < strings - 2;
        cnv.stroke_style(wound ? mTheme.Pick(el::rgba(165, 158, 146, 230), el::rgba(138, 127, 112, 255))
                               : mTheme.Pick(el::rgba(201, 203, 209, 230), el::rgba(111, 113, 120, 255)));
        cnv.line_width(0.9f + 1.9f * (1.0f - static_cast<float>(s) / static_cast<float>(std::max(1, strings - 1))));
        Line(cnv, x(kOpenX - 12.0f), y(s), x(end), y(s));
        FillText(cnv, mModel.stringNames[static_cast<std::size_t>(s)], nameFont, mTheme.TextSecondary(),
                 {x(kNameX) - 10.0f, y(s) - 9.0f, x(kNameX) + 10.0f, y(s) + 9.0f}, HAlign::Centre);
        cnv.fill_style(mTheme.Pick(el::rgba(255, 255, 255, 18), el::rgba(20, 24, 34, 20)));
        Circle(cnv, x(kOpenX), y(s), 8.0f);
        cnv.fill();
    }

    const auto numberFont = Font(Weight::Regular).size(12.0f);

    for (const int fret : kNumbered)
    {
        if (fret <= frets)
        {
            const float cx = x(NoteX(fret, frets, end));
            FillText(cnv, std::to_string(fret), numberFont, mTheme.TextSecondary(),
                     {cx - 14.0f, bottom + 10.0f, cx + 14.0f, bottom + 28.0f}, HAlign::Centre);
        }
    }

    // The dots; a chord shape's muted strings have a cross and no open dot.
    const GuideShape* shape =
        mModel.chord && mModel.position > 0 && mModel.position <= static_cast<int>(mModel.shapes.size())
            ? &mModel.shapes[static_cast<std::size_t>(mModel.position - 1)]
            : nullptr;
    const auto muted = [&](int string) {
        return shape != nullptr && string < static_cast<int>(shape->frets.size()) &&
               shape->frets[static_cast<std::size_t>(string)] < 0;
    };
    const auto heardAge = [&](int midi) {
        const auto it = std::find(mHeard.begin(), mHeard.end(), midi);
        return it == mHeard.end() ? -1 : static_cast<int>(it - mHeard.begin());
    };
    const el::color heardColours[] = {mTheme.Success(), mTheme.Success().opacity(0.55f),
                                      mTheme.Success().opacity(0.3f)};

    for (const auto& dot : mModel.dots)
    {
        if (muted(dot.string) && dot.fret <= mModel.capo)
        {
            continue;
        }

        const float cx = x(NoteX(dot.fret, frets, end));
        const float cy = y(dot.string);
        const int age = heardAge(dot.midi);
        const float alpha = dot.inPosition || age >= 0 ? 1.0f : 0.45f;
        const el::color fill = dot.root ? mTheme.Accent()
                               : !dot.inPosition && age < 0
                                   ? mTheme.Pick(el::rgba(58, 63, 73, 255), el::rgba(212, 204, 191, 255))
                                   : mTheme.Pick(el::rgba(18, 23, 32, 255), el::rgba(255, 255, 255, 255));
        Circle(cnv, cx, cy, kDotRadius);
        cnv.fill_style(fill.opacity(alpha * (dot.root && !dot.inPosition && age < 0 ? 0.85f : 1.0f)));
        cnv.fill();

        if (age >= 0 || dot.inPosition)
        {
            Circle(cnv, cx, cy, kDotRadius);
            cnv.stroke_style(age >= 0   ? heardColours[std::min(age, 2)]
                             : dot.root ? mTheme.Accent()
                                        : mTheme.Pick(el::rgba(200, 204, 214, 255), el::rgba(74, 78, 87, 255)));
            cnv.line_width(age == 0 ? 3.5f : age > 0 ? 2.5f : 1.5f);
            cnv.stroke();
        }

        const std::string label = mLabels == "interval" ? dot.interval
                                  : mLabels == "degree" ? std::to_string(dot.degree)
                                  : mLabels == "none"   ? std::string()
                                                        : dot.name;

        if (!label.empty())
        {
            const el::color ink = dot.root ? el::rgba(255, 255, 255, 255)
                                  : !dot.inPosition && age < 0
                                      ? mTheme.Pick(el::rgba(170, 176, 188, 255), el::rgba(109, 101, 88, 255))
                                      : mTheme.Pick(el::rgba(242, 243, 247, 255), el::rgba(22, 24, 29, 255));
            const float size = label.size() > 3 ? 9.5f : 11.5f;
            FillText(cnv, label, Font(Weight::SemiBold).size(size), ink.opacity(alpha),
                     {cx - kDotRadius, cy - kDotRadius, cx + kDotRadius, cy + kDotRadius}, HAlign::Centre);
        }
    }

    if (shape != nullptr)
    {
        cnv.stroke_style(mTheme.Error());
        cnv.line_width(2.2f);

        for (int s = 0; s < strings; ++s)
        {
            if (muted(s))
            {
                Cross(cnv, x(kOpenX), y(s), 5.5f);
            }
        }
    }

    // LISTEN: a played note with no dot is ringed where it sounds.
    for (int age = 0; age < static_cast<int>(mHeard.size()) && age < 3; ++age)
    {
        const int midi = mHeard[static_cast<std::size_t>(age)];
        const bool hasDot =
            std::any_of(mModel.dots.begin(), mModel.dots.end(), [midi](const GuideDot& d) { return d.midi == midi; });

        if (hasDot)
        {
            continue;
        }

        cnv.stroke_style(mTheme.Error().opacity(age == 0 ? 1.0f : age == 1 ? 0.5f : 0.25f));
        cnv.line_width(3.0f);

        for (int s = 0; s < strings; ++s)
        {
            const int fret = midi - mModel.openMidi[static_cast<std::size_t>(s)];

            if (fret >= mModel.capo && fret <= frets)
            {
                Circle(cnv, x(NoteX(fret, frets, end)), y(s), kDotRadius + 3.0f);
                cnv.stroke();
            }
        }
    }
}

ChordBoxView::ChordBoxView(const Theme& theme, GuideShape shape, std::vector<int> openMidi, int capo,
                           int rootPitchClass, bool leftHanded, std::string caption, std::string title, bool selected)
    : mTheme(theme), mShape(std::move(shape)), mOpenMidi(std::move(openMidi)), mCapo(capo),
      mRootPitchClass(rootPitchClass), mLeftHanded(leftHanded), mCaption(std::move(caption)), mTitle(std::move(title)),
      mSelected(selected)
{
    AccessName(mTitle + " at fret " + std::to_string(mShape.position));
}

el::view_limits ChordBoxView::limits(const el::basic_context&) const
{
    const auto strings = static_cast<float>(std::max<std::size_t>(1, mShape.frets.size()));
    const float width = kBoxPadX * 2.0f + (strings - 1.0f) * kBoxStringGap + 12.0f;
    const float height = (mTitle.empty() ? 0.0f : 18.0f) + kBoxTop + kBoxRows * kBoxFretGap + 26.0f;
    return {{width, height}, {width, height}};
}

void ChordBoxView::draw(const el::context& ctx)
{
    NoteDrawn(ctx);
    auto& cnv = ctx.canvas;
    const auto b = ctx.bounds;
    const int strings = static_cast<int>(mShape.frets.size());

    // The card it sits in: the accent when chosen.
    cnv.begin_path();
    cnv.add_round_rect(b.inset(1.0f, 1.0f), 10.0f);
    cnv.fill_style(mSelected ? mTheme.AccentTint() : mTheme.Pick(el::rgba(255, 255, 255, 6), el::rgba(20, 24, 34, 6)));
    cnv.fill();
    cnv.stroke_style(mSelected ? mTheme.Accent() : mTheme.Border());
    cnv.line_width(1.0f);
    cnv.stroke();

    if (strings == 0)
    {
        return;
    }

    float top = b.top;

    if (!mTitle.empty())
    {
        FillText(cnv, mTitle, Font(Weight::SemiBold).size(13.0f), mTheme.Text(),
                 {b.left, b.top + 6.0f, b.right, b.top + 22.0f}, HAlign::Centre);
        top += 18.0f;
    }

    int lowest = 99;
    int highest = 0;

    for (const int fret : mShape.frets)
    {
        if (fret > mCapo)
        {
            lowest = std::min(lowest, fret);
            highest = std::max(highest, fret);
        }
    }

    const int base = highest == 0 || highest - mCapo <= kBoxRows ? mCapo + 1 : lowest;
    const int rows = std::max(kBoxRows, highest - base + 1);
    const float rowGap = kBoxFretGap * kBoxRows / static_cast<float>(rows);
    const float gridTop = top + kBoxTop;
    const auto sx = [&](int s) {
        return b.left + kBoxPadX + 6.0f + static_cast<float>(mLeftHanded ? strings - 1 - s : s) * kBoxStringGap;
    };
    const auto rowY = [&](int fret) { return gridTop + (static_cast<float>(fret - base) + 0.5f) * rowGap; };
    const float left = sx(mLeftHanded ? strings - 1 : 0);
    const float right = sx(mLeftHanded ? 0 : strings - 1);
    const el::color line = mTheme.Pick(el::rgba(220, 224, 236, 140), el::rgba(40, 44, 56, 153));
    const el::color dot = mTheme.Pick(el::rgba(230, 230, 238, 255), el::rgba(34, 37, 44, 255));
    cnv.stroke_style(line);

    for (int row = 0; row <= rows; ++row)
    {
        cnv.line_width(row == 0 && base == mCapo + 1 ? 4.0f : 1.0f);
        Line(cnv, left, gridTop + row * rowGap, right, gridTop + row * rowGap);
    }

    cnv.line_width(1.0f);

    for (int s = 0; s < strings; ++s)
    {
        Line(cnv, sx(s), gridTop, sx(s), gridTop + rows * rowGap);
    }

    const auto small = Font(Weight::Regular).size(10.0f);

    if (base > mCapo + 1)
    {
        const float lx = mLeftHanded ? right + 12.0f : left - 12.0f;
        FillText(cnv, std::to_string(base), small, mTheme.TextMuted(),
                 {lx - 10.0f, rowY(base) - 7.0f, lx + 10.0f, rowY(base) + 7.0f}, HAlign::Centre);
    }

    cnv.fill_style(dot);

    for (const auto& barre : mShape.barres)
    {
        const float from = std::min(sx(barre.fromString), sx(barre.toString));
        const float to = std::max(sx(barre.fromString), sx(barre.toString));
        cnv.begin_path();
        cnv.add_round_rect({from - 6.0f, rowY(barre.fret) - 6.0f, to + 6.0f, rowY(barre.fret) + 6.0f}, 6.0f);
        cnv.fill();
    }

    for (int s = 0; s < strings; ++s)
    {
        const int fret = mShape.frets[static_cast<std::size_t>(s)];
        const float cx = sx(s);

        if (fret < 0)
        {
            cnv.stroke_style(mTheme.Error());
            cnv.line_width(2.0f);
            Cross(cnv, cx, gridTop - 10.0f, 4.0f);
            continue;
        }

        const int midi = (s < static_cast<int>(mOpenMidi.size()) ? mOpenMidi[static_cast<std::size_t>(s)] : 0) + fret;
        const bool root = ((midi % 12) + 12) % 12 == mRootPitchClass;

        if (fret <= mCapo)
        {
            Circle(cnv, cx, gridTop - 10.0f, 4.5f);
            cnv.stroke_style(root ? mTheme.Accent() : dot);
            cnv.line_width(1.4f);
            cnv.stroke();
            continue;
        }

        Circle(cnv, cx, rowY(fret), 6.5f);
        cnv.fill_style(root ? mTheme.Accent() : dot);
        cnv.fill();
        const int finger =
            s < static_cast<int>(mShape.fingers.size()) ? mShape.fingers[static_cast<std::size_t>(s)] : 0;

        if (finger > 0)
        {
            FillText(cnv, std::to_string(finger), Font(Weight::SemiBold).size(9.0f),
                     root ? el::rgba(255, 255, 255, 255)
                          : mTheme.Pick(el::rgba(21, 21, 26, 255), el::rgba(255, 255, 255, 255)),
                     {cx - 6.0f, rowY(fret) - 6.0f, cx + 6.0f, rowY(fret) + 6.0f}, HAlign::Centre);
        }
    }

    if (!mCaption.empty())
    {
        FillText(cnv, mCaption, small, mTheme.TextMuted(), {b.left, b.bottom - 20.0f, b.right, b.bottom - 6.0f},
                 HAlign::Centre);
    }
}
} // namespace nanoq::ui
