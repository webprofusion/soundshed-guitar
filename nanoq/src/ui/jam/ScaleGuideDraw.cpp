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

constexpr float kTabSlot = 30.0f;
constexpr float kTabLeft = 30.0f;
constexpr float kTabRight = 12.0f;
constexpr float kTabStrokesY = 12.0f;
constexpr float kTabTop = 34.0f;
constexpr float kTabStringGap = 15.0f;
constexpr float kTabIntervalGap = 22.0f;
constexpr float kTabNumberGap = 18.0f;
/// A flowed tab: the room a bar line takes, the row the chord names sit in, the gap between lines,
/// and the fewest notes a line holds however narrow the page.
constexpr float kTabBarGap = 14.0f;
constexpr float kTabChordRow = 20.0f;
constexpr float kTabLineGap = 12.0f;
constexpr int kTabFewestSlots = 6;
/// A slide's slanted line beside its number: how far it runs and how much it rises.
constexpr float kTabSlideLength = 12.0f;
constexpr float kTabSlideRise = 4.0f;

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

GuideRun ReadRun(const nlohmann::json& r)
{
    GuideRun run;
    run.name = StringAt(r, "name");
    run.start = StringAt(r, "start");
    run.fretFrom = IntAt(r, "fretFrom");
    run.fretTo = IntAt(r, "fretTo");

    for (const auto& n : r.value("notes", nlohmann::json::array()))
    {
        run.notes.push_back({IntAt(n, "string"), IntAt(n, "fret"), StringAt(n, "interval"), n.value("root", false)});
    }

    for (const auto& s : r.value("steps", nlohmann::json::array()))
    {
        run.steps.push_back({IntAt(s, "note"), StringAt(s, "stroke")});
    }

    return run;
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

std::optional<GuideProgressionRun> ReadGuideProgressionRun(const nlohmann::json& view)
{
    if (!view.is_object() || !view.contains("progressionRun") || !view["progressionRun"].is_object())
    {
        return std::nullopt;
    }

    const auto& whole = view["progressionRun"];
    GuideProgressionRun out;
    out.run = ReadRun(whole);
    out.slides = whole.value("slides", false);

    for (const auto& b : whole.value("bars", nlohmann::json::array()))
    {
        out.bars.push_back({IntAt(b, "chord"), StringAt(b, "name"), StringAt(b, "numeral"), IntAt(b, "from"),
                            std::max(0, IntAt(b, "count"))});
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
    m.arpeggio = StringAt(view, "mode") == "arpeggio";

    if (const auto arpeggio = view.find("arpeggio"); arpeggio != view.end())
    {
        for (const auto& r : arpeggio->value("runs", nlohmann::json::array()))
        {
            m.runs.push_back(ReadRun(r));
        }
    }

    if (const auto run = view.find("run"); run != view.end() && run->is_object())
    {
        m.chosen = ReadRun(*run);
    }

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
        dot.order = IntAt(d, "order");
        dot.finger = IntAt(d, "finger", -1);
        m.dots.push_back(std::move(dot));
    }

    for (const auto& s : view.value("shapes", nlohmann::json::array()))
    {
        m.shapes.push_back(ReadGuideShape(s));
    }

    return m;
}

const GuideRun* GuideModel::Run() const
{
    return chosen ? &*chosen : nullptr;
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

int ScaleNeck::NoteAt(const el::context& ctx, el::point point) const
{
    const auto b = ctx.bounds;
    const int strings = static_cast<int>(mModel.openMidi.size());
    const float width = b.width();
    const float end = width - 8.0f;
    constexpr float kReach = kDotRadius + 4.0f;

    for (const auto& dot : mModel.dots)
    {
        const float value = NoteX(dot.fret, mModel.frets, end);
        const float cx = b.left + (mLeftHanded ? width - value : value);
        const float cy = b.top + kTop + static_cast<float>(strings - 1 - dot.string) * kStringGap;

        if (std::hypot(point.x - cx, point.y - cy) <= kReach)
        {
            return dot.midi;
        }
    }

    return -1;
}

bool ScaleNeck::click(const el::context& ctx, el::mouse_button btn)
{
    if (btn.state != el::mouse_button::left)
    {
        return false;
    }

    if (btn.down)
    {
        mPressedNote = NoteAt(ctx, btn.pos);
        return mPressedNote >= 0;
    }

    const int note = mPressedNote;
    mPressedNote = -1;

    if (note >= 0 && note == NoteAt(ctx, btn.pos) && onNote)
    {
        onNote(note);
    }

    return true;
}

bool ScaleNeck::SetHeard(std::vector<int> notes, bool everywhere)
{
    if (notes == mHeard && everywhere == mHeardEverywhere)
    {
        return false;
    }

    mHeard = std::move(notes);
    mHeardEverywhere = everywhere;
    return true;
}

bool ScaleNeck::SetPlaying(std::vector<std::pair<int, int>> places)
{
    if (places == mPlaying)
    {
        return false;
    }

    mPlaying = std::move(places);
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
    // A heard note marks every place it is, or only its places in the chosen position when it has any.
    const auto heardAge = [&](const GuideDot& dot) {
        const auto it = std::find(mHeard.begin(), mHeard.end(), dot.midi);

        if (it == mHeard.end())
        {
            return -1;
        }

        const bool elsewhere = !mHeardEverywhere && !dot.inPosition &&
                               std::any_of(mModel.dots.begin(), mModel.dots.end(), [&dot](const GuideDot& other) {
                                   return other.midi == dot.midi && other.inPosition;
                               });
        return elsewhere ? -1 : static_cast<int>(it - mHeard.begin());
    };
    const el::color heardColours[] = {mTheme.Success(), mTheme.Success().opacity(0.55f),
                                      mTheme.Success().opacity(0.3f)};
    // A scale position has a run too, for the tab; the neck only traces an arpeggio's.
    const GuideRun* run = mModel.arpeggio ? mModel.Run() : nullptr;

    // The way an arpeggio run goes, under its dots.
    if (run != nullptr && !run->notes.empty())
    {
        cnv.begin_path();

        for (std::size_t i = 0; i < run->notes.size(); ++i)
        {
            const float px = x(NoteX(run->notes[i].fret, frets, end));
            const float py = y(run->notes[i].string);

            if (i == 0)
            {
                cnv.move_to(px, py);
            }
            else
            {
                cnv.line_to(px, py);
            }
        }

        cnv.stroke_style(mTheme.Accent().opacity(0.55f));
        cnv.line_width(2.5f);
        cnv.stroke();
    }

    // Order and fingers number one run: with none chosen the neck has only its notes to name.
    const bool runLabels = mLabels == "order" || mLabels == "finger";
    const std::string labels = runLabels && run == nullptr ? std::string("name") : mLabels;

    for (const auto& dot : mModel.dots)
    {
        if (muted(dot.string) && dot.fret <= mModel.capo)
        {
            continue;
        }

        const float cx = x(NoteX(dot.fret, frets, end));
        const float cy = y(dot.string);

        // The note Play is sounding: filled as a root is and ringed, where it is fretted and nowhere else.
        if (std::find(mPlaying.begin(), mPlaying.end(), std::make_pair(dot.string, dot.fret)) != mPlaying.end())
        {
            // A glow round it sets a sounding root apart from one at rest.
            Circle(cnv, cx, cy, kDotRadius + 6.0f);
            cnv.fill_style(mTheme.Accent().opacity(0.35f));
            cnv.fill();
            Circle(cnv, cx, cy, kDotRadius + 1.5f);
            cnv.fill_style(mTheme.Accent());
            cnv.fill();
            cnv.stroke_style(mTheme.Pick(el::rgba(242, 243, 247, 255), el::rgba(22, 24, 29, 255)));
            cnv.line_width(3.0f);
            cnv.stroke();
            const std::string name = mLabels == "none" ? std::string() : dot.name;
            FillText(cnv, name, Font(Weight::SemiBold).size(name.size() > 3 ? 9.5f : 11.5f),
                     el::rgba(255, 255, 255, 255), {cx - kDotRadius, cy - kDotRadius, cx + kDotRadius, cy + kDotRadius},
                     HAlign::Centre);
            continue;
        }

        const int age = heardAge(dot);
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

        const std::string label = labels == "interval" ? dot.interval
                                  : labels == "degree" ? std::to_string(dot.degree)
                                  : labels == "none"   ? std::string()
                                  : labels == "order"  ? (dot.order > 0 ? std::to_string(dot.order) : std::string())
                                  : labels == "finger" ? (dot.finger >= 0 ? std::to_string(dot.finger) : std::string())
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

RunTabView::RunTabView(const Theme& theme, GuideRun run, std::vector<std::string> stringNames)
    : mTheme(theme), mRun(std::move(run)), mStringNames(std::move(stringNames))
{
}

bool RunTabView::SetPlaying(std::vector<int> steps)
{
    if (steps == mPlaying)
    {
        return false;
    }

    mPlaying = std::move(steps);
    return true;
}

el::view_limits RunTabView::limits(const el::basic_context&) const
{
    const float width = kTabLeft + static_cast<float>(mRun.steps.size()) * kTabSlot + kTabRight;
    const auto strings = static_cast<float>(std::max<std::size_t>(1, mStringNames.size()));
    const float height = kTabTop + (strings - 1.0f) * kTabStringGap + kTabIntervalGap + 8.0f;
    return {{width, height}, {width, height}};
}

void RunTabView::draw(const el::context& ctx)
{
    NoteDrawn(ctx);
    auto& cnv = ctx.canvas;
    const auto b = ctx.bounds;
    const int strings = static_cast<int>(mStringNames.size());
    const auto stringY = [&](int s) { return b.top + kTabTop + static_cast<float>(strings - 1 - s) * kTabStringGap; };
    const auto slotX = [&](std::size_t slot) {
        return b.left + kTabLeft + (static_cast<float>(slot) + 0.5f) * kTabSlot;
    };
    const float end = b.left + kTabLeft + static_cast<float>(mRun.steps.size()) * kTabSlot + 4.0f;
    const auto small = Font(Weight::Regular).size(11.0f);
    const el::color line = mTheme.Pick(el::rgba(220, 224, 236, 140), el::rgba(40, 44, 56, 153));
    const el::color ink = mTheme.Text();

    // The strings, each broken around its numbers the way printed tab sits on its lines.
    for (int s = 0; s < strings; ++s)
    {
        const float y = stringY(s);
        float from = b.left + kTabLeft - 8.0f;
        cnv.stroke_style(line);
        cnv.line_width(1.0f);

        for (std::size_t slot = 0; slot < mRun.steps.size(); ++slot)
        {
            const auto note = static_cast<std::size_t>(mRun.steps[slot].note);

            if (note < mRun.notes.size() && mRun.notes[note].string == s)
            {
                Line(cnv, from, y, slotX(slot) - kTabNumberGap * 0.5f, y);
                from = slotX(slot) + kTabNumberGap * 0.5f;
            }
        }

        Line(cnv, from, y, end, y);
        FillText(cnv, mStringNames[static_cast<std::size_t>(s)], small, mTheme.TextMuted(),
                 {b.left + 2.0f, y - 8.0f, b.left + 22.0f, y + 8.0f}, HAlign::Centre);
    }

    const float strokeY = b.top + kTabStrokesY;
    const float intervalY = stringY(0) + kTabIntervalGap;

    for (std::size_t slot = 0; slot < mRun.steps.size(); ++slot)
    {
        const auto& step = mRun.steps[slot];
        const auto index = static_cast<std::size_t>(step.note);

        if (index >= mRun.notes.size())
        {
            continue;
        }

        const auto& note = mRun.notes[index];
        const float cx = slotX(slot);
        cnv.stroke_style(ink);
        cnv.line_width(1.5f);

        if (step.stroke == "down")
        {
            cnv.begin_path();
            cnv.move_to(cx - 5.0f, strokeY + 5.0f);
            cnv.line_to(cx - 5.0f, strokeY - 5.0f);
            cnv.line_to(cx + 5.0f, strokeY - 5.0f);
            cnv.line_to(cx + 5.0f, strokeY + 5.0f);
            cnv.stroke();
        }
        else if (step.stroke == "up")
        {
            cnv.begin_path();
            cnv.move_to(cx - 5.0f, strokeY - 5.0f);
            cnv.line_to(cx, strokeY + 5.0f);
            cnv.line_to(cx + 5.0f, strokeY - 5.0f);
            cnv.stroke();
        }
        else
        {
            FillText(cnv, step.stroke == "hammer" ? "h" : "p", Font(Weight::Regular).size(13.0f), ink,
                     {cx - 8.0f, strokeY - 8.0f, cx + 8.0f, strokeY + 8.0f}, HAlign::Centre);
        }

        const float y = stringY(note.string);
        // The note Play is sounding reads larger, in the accent.
        const bool sounding = std::find(mPlaying.begin(), mPlaying.end(), static_cast<int>(slot)) != mPlaying.end();
        FillText(cnv, std::to_string(note.fret), Font(Weight::SemiBold).size(sounding ? 16.0f : 13.0f),
                 note.root || sounding ? mTheme.Accent() : ink, {cx - 12.0f, y - 9.0f, cx + 12.0f, y + 9.0f},
                 HAlign::Centre);
        FillText(cnv, note.interval, small, note.root ? mTheme.Accent() : mTheme.TextMuted(),
                 {cx - 14.0f, intervalY - 8.0f, cx + 14.0f, intervalY + 8.0f}, HAlign::Centre);
    }
}

ProgressionTabView::ProgressionTabView(const Theme& theme, GuideProgressionRun whole,
                                       std::vector<std::string> stringNames, float width, int activeChord)
    : mTheme(theme), mWhole(std::move(whole)), mStringNames(std::move(stringNames)), mActive(activeChord)
{
    // As ts/scaleGuide/runTab.ts lays it out: a bar stays on one line where a line can hold it.
    const float edge = std::max(width - kTabRight, kTabLeft + kTabFewestSlots * kTabSlot + kTabBarGap);
    int line = 0;
    float x = kTabLeft;

    for (std::size_t b = 0; b < mWhole.bars.size(); ++b)
    {
        const auto& bar = mWhole.bars[b];
        // A chord with no run keeps a short bar, so the progression still reads whole.
        const int slots = std::max(bar.count, 2);
        const float whole = static_cast<float>(slots) * kTabSlot + kTabBarGap;

        if (x > kTabLeft && x + whole > edge && kTabLeft + whole <= edge)
        {
            ++line;
            x = kTabLeft;
        }

        Segment segment{b, line, x, x, true, true, {}};

        for (int slot = 0; slot < slots; ++slot)
        {
            if (x > kTabLeft && x + kTabSlot + kTabBarGap > edge)
            {
                segment.last = false;
                mSegments.push_back(segment);
                ++line;
                x = kTabLeft;
                segment = Segment{b, line, x, x, false, true, {}};
            }

            if (slot < bar.count)
            {
                segment.cells.emplace_back(bar.from + slot, x + kTabSlot * 0.5f);
            }

            x += kTabSlot;
            segment.to = x;
        }

        mSegments.push_back(segment);
        x += kTabBarGap;
    }

    mLines = line + 1;

    for (const auto& segment : mSegments)
    {
        mWidth = std::max(mWidth, segment.to + kTabBarGap * 0.5f + kTabRight);
    }
}

float ProgressionTabView::LineHeight() const
{
    const auto strings = static_cast<float>(std::max<std::size_t>(1, mStringNames.size()));
    return kTabChordRow + kTabTop + (strings - 1.0f) * kTabStringGap + kTabIntervalGap + 8.0f;
}

bool ProgressionTabView::SetPlaying(std::vector<int> steps)
{
    if (steps == mPlaying)
    {
        return false;
    }

    mPlaying = std::move(steps);
    return true;
}

el::view_limits ProgressionTabView::limits(const el::basic_context&) const
{
    const float height = static_cast<float>(mLines) * LineHeight() + static_cast<float>(mLines - 1) * kTabLineGap;
    return {{mWidth, height}, {mWidth, height}};
}

int ProgressionTabView::BarAt(const el::context& ctx, el::point point) const
{
    const float pitch = LineHeight() + kTabLineGap;
    const float x = point.x - ctx.bounds.left;
    const float y = point.y - ctx.bounds.top;
    const int line = static_cast<int>(std::floor(y / pitch));

    if (y < 0.0f || y - static_cast<float>(line) * pitch > LineHeight())
    {
        return -1;
    }

    for (const auto& segment : mSegments)
    {
        if (segment.line == line && x >= segment.from - 4.0f && x <= segment.to + 4.0f)
        {
            return mWhole.bars[segment.bar].chord;
        }
    }

    return -1;
}

bool ProgressionTabView::click(const el::context& ctx, el::mouse_button btn)
{
    if (btn.state != el::mouse_button::left)
    {
        return false;
    }

    if (btn.down)
    {
        mPressedBar = BarAt(ctx, btn.pos);
        return mPressedBar >= 0;
    }

    const int bar = mPressedBar;
    mPressedBar = -1;

    if (bar >= 0 && bar == BarAt(ctx, btn.pos) && onBar)
    {
        onBar(bar);
    }

    return true;
}

void ProgressionTabView::draw(const el::context& ctx)
{
    NoteDrawn(ctx);
    auto& cnv = ctx.canvas;
    const auto b = ctx.bounds;
    const int strings = static_cast<int>(mStringNames.size());
    const auto& run = mWhole.run;
    const float pitch = LineHeight() + kTabLineGap;
    const auto lineTop = [&](int line) { return b.top + static_cast<float>(line) * pitch; };
    const auto stringY = [&](int line, int s) {
        return lineTop(line) + kTabChordRow + kTabTop + static_cast<float>(strings - 1 - s) * kTabStringGap;
    };
    const auto noteAt = [&](int step) -> const GuideRunNote* {
        if (step < 0 || step >= static_cast<int>(run.steps.size()))
        {
            return nullptr;
        }

        const auto index = static_cast<std::size_t>(run.steps[static_cast<std::size_t>(step)].note);
        return index < run.notes.size() ? &run.notes[index] : nullptr;
    };
    const auto small = Font(Weight::Regular).size(11.0f);
    const el::color line = mTheme.Pick(el::rgba(220, 224, 236, 140), el::rgba(40, 44, 56, 153));
    const el::color ink = mTheme.Text();

    // The chord showing on the neck, behind its bar.
    for (const auto& segment : mSegments)
    {
        if (mWhole.bars[segment.bar].chord == mActive)
        {
            const float top = lineTop(segment.line);
            cnv.begin_path();
            cnv.add_round_rect(
                {b.left + segment.from - 4.0f, top + 1.0f, b.left + segment.to + 4.0f, top + LineHeight() - 1.0f},
                6.0f);
            cnv.fill_style(mTheme.Accent().opacity(0.14f));
            cnv.fill();
        }
    }

    // The strings of each line, broken around their numbers, with their names at its start.
    for (int l = 0; l < mLines; ++l)
    {
        float end = kTabLeft;

        for (const auto& segment : mSegments)
        {
            end = segment.line == l ? std::max(end, segment.to + kTabBarGap * 0.5f) : end;
        }

        for (int s = 0; s < strings; ++s)
        {
            const float y = stringY(l, s);
            float from = b.left + kTabLeft - 8.0f;
            cnv.stroke_style(line);
            cnv.line_width(1.0f);

            for (const auto& segment : mSegments)
            {
                for (const auto& [step, x] : segment.cells)
                {
                    const auto* note = noteAt(step);

                    if (segment.line == l && note != nullptr && note->string == s)
                    {
                        Line(cnv, from, y, b.left + x - kTabNumberGap * 0.5f, y);
                        from = b.left + x + kTabNumberGap * 0.5f;
                    }
                }
            }

            Line(cnv, from, y, b.left + end, y);
            FillText(cnv, mStringNames[static_cast<std::size_t>(s)], small, mTheme.TextMuted(),
                     {b.left + 2.0f, y - 8.0f, b.left + 22.0f, y + 8.0f}, HAlign::Centre);
        }
    }

    const auto slide = [&](float cx, float cy, float side, bool rising) {
        const float inner = cx + side * (kTabNumberGap * 0.5f - 1.0f);
        const float outer = inner + side * kTabSlideLength;
        const float lift = (rising ? -1.0f : 1.0f) * side * kTabSlideRise;
        cnv.stroke_style(ink);
        cnv.line_width(1.5f);
        Line(cnv, inner, cy - lift, outer, cy + lift);
    };

    for (std::size_t i = 0; i < mSegments.size(); ++i)
    {
        const auto& segment = mSegments[i];
        const auto& bar = mWhole.bars[segment.bar];
        const float top = lineTop(segment.line);
        const float strokeY = top + kTabChordRow + kTabStrokesY;
        const float intervalY = stringY(segment.line, 0) + kTabIntervalGap;

        if (segment.first)
        {
            const auto nameFont = Font(Weight::SemiBold).size(14.0f);
            const float nameWidth = el::measure_text(cnv, bar.name, nameFont).x;
            const float left = b.left + segment.from + 4.0f;
            FillText(cnv, bar.name, nameFont, ink, {left, top + 2.0f, left + nameWidth + 2.0f, top + 20.0f},
                     HAlign::Left);
            FillText(cnv, bar.numeral, small, mTheme.TextMuted(),
                     {left + nameWidth + 6.0f, top + 3.0f, left + nameWidth + 70.0f, top + 20.0f}, HAlign::Left);
        }

        if (bar.count == 0)
        {
            FillText(cnv, "no run", small, mTheme.TextMuted(),
                     {b.left + segment.from, intervalY - 8.0f, b.left + segment.to, intervalY + 8.0f}, HAlign::Centre);
        }

        for (const auto& [step, x] : segment.cells)
        {
            const auto* note = noteAt(step);

            if (note == nullptr)
            {
                continue;
            }

            const auto& stroke = run.steps[static_cast<std::size_t>(step)].stroke;
            const float cx = b.left + x;
            const float y = stringY(segment.line, note->string);
            cnv.stroke_style(ink);
            cnv.line_width(1.5f);

            if (stroke == "down")
            {
                cnv.begin_path();
                cnv.move_to(cx - 5.0f, strokeY + 5.0f);
                cnv.line_to(cx - 5.0f, strokeY - 5.0f);
                cnv.line_to(cx + 5.0f, strokeY - 5.0f);
                cnv.line_to(cx + 5.0f, strokeY + 5.0f);
                cnv.stroke();
            }
            else if (stroke == "up")
            {
                cnv.begin_path();
                cnv.move_to(cx - 5.0f, strokeY - 5.0f);
                cnv.line_to(cx, strokeY + 5.0f);
                cnv.line_to(cx + 5.0f, strokeY - 5.0f);
                cnv.stroke();
            }
            else
            {
                FillText(cnv,
                         stroke == "hammer"  ? "h"
                         : stroke == "slide" ? "sl."
                                             : "p",
                         Font(Weight::Regular).size(13.0f), ink,
                         {cx - 10.0f, strokeY - 8.0f, cx + 10.0f, strokeY + 8.0f}, HAlign::Centre);
            }

            // A slide is drawn as its two halves, out of one number and into the next.
            const auto* before = noteAt(step - 1);

            if (stroke == "slide" && before != nullptr)
            {
                slide(cx, y, -1.0f, note->fret > before->fret);
            }

            const auto* after = noteAt(step + 1);

            if (after != nullptr && run.steps[static_cast<std::size_t>(step + 1)].stroke == "slide")
            {
                slide(cx, y, 1.0f, after->fret > note->fret);
            }

            // The note Play is sounding reads larger, in the accent.
            const bool sounding = std::find(mPlaying.begin(), mPlaying.end(), step) != mPlaying.end();
            FillText(cnv, std::to_string(note->fret), Font(Weight::SemiBold).size(sounding ? 16.0f : 13.0f),
                     note->root || sounding ? mTheme.Accent() : ink, {cx - 12.0f, y - 9.0f, cx + 12.0f, y + 9.0f},
                     HAlign::Centre);
            FillText(cnv, note->interval, small, note->root ? mTheme.Accent() : mTheme.TextMuted(),
                     {cx - 14.0f, intervalY - 8.0f, cx + 14.0f, intervalY + 8.0f}, HAlign::Centre);
        }

        // The bar line, where the chord ends: heavier after the last.
        if (segment.last)
        {
            const float x = b.left + segment.to + kTabBarGap * 0.5f;
            cnv.stroke_style(line);
            cnv.line_width(i + 1 == mSegments.size() ? 3.5f : 1.5f);
            Line(cnv, x, stringY(segment.line, strings - 1), x, stringY(segment.line, 0));
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

void PlayArea::draw(const el::context& ctx)
{
    NoteDrawn(ctx);

    if (!mHover && !mDown && !mPlaying)
    {
        return;
    }

    auto& cnv = ctx.canvas;
    cnv.begin_path();
    cnv.add_round_rect(ctx.bounds, 10.0f);
    cnv.fill_style(mTheme.AccentTint());
    cnv.fill();
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
