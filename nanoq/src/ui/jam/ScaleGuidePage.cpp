// The Scale Guide in Jam's Scales section (docs/plans/scale-guide.md), as the web UI draws it
// (ts/scaleGuide/panel.ts): Scales, Chords and Arpeggios, a card of pickers and view controls, a
// progression's steps, the neck, the chord boxes (a progression's voice-led strip) or an arpeggio's
// tab, a summary and LISTEN.
//
// The choices are the web UI's own app settings (uiclient/ScaleGuide.h), so either product opens
// where the other left off. The view model comes from the engine's music library directly, the
// same code that answers the web UI's scaleGuide message; Nano Q does no music theory itself.

#include "ui/shell/Shell.h"

#include "music/ScaleGuideView.h"
#include "ui/dialogs/DialogChrome.h"
#include "ui/jam/ScaleGuideDraw.h"
#include "ui/widgets/TextDraw.h"
#include "uiclient/ScaleGuide.h"

#include <algorithm>
#include <cmath>

namespace nanoq::ui
{
namespace sg = guitarfx::uiclient::scaleguide;

namespace
{
/// The engine's id for a position being played, which sets it apart from a demo clip.
constexpr const char* kPlayedPosition = "scale-guide";

const nlohmann::json& Catalog()
{
    static const nlohmann::json catalog = guitarfx::music::ScaleGuideCatalogJson();
    return catalog;
}

std::string NameOf(const nlohmann::json& list, const std::string& id)
{
    for (const auto& item : list)
    {
        if (item.value("id", "") == id)
        {
            return item.value("name", id);
        }
    }

    return id;
}

/// "bVII" as a chart writes it.
std::string Numeral(std::string token)
{
    if (!token.empty() && token[0] == 'b')
    {
        token.replace(0, 1, "\xE2\x99\xAD");
    }

    return token;
}

Ptr Label(const Theme& theme, const std::string& text)
{
    auto t = std::make_shared<Text>(theme, Upper(text), 10.0f, Weight::SemiBold);
    t->Colour(theme.TextMuted());
    return FixedHeight(16, t);
}
} // namespace

const nlohmann::json& Shell::ScaleGuideView()
{
    const auto settings = sg::LoadSettings(mState.appSettings);
    const auto progressions = sg::LoadProgressions(mState.appSettings);
    const auto request = sg::ViewRequest(settings, progressions, mScaleGuideStep, mScaleGuidePrevious);
    const auto key = request.dump();

    if (key == mScaleGuideViewKey && !mScaleGuideView.is_null())
    {
        return mScaleGuideView;
    }

    mScaleGuideView = guitarfx::music::BuildScaleGuideView(guitarfx::music::ParseScaleGuideRequest(request));
    mScaleGuideViewKey = key;

    if (const auto progression = mScaleGuideView.find("progression"); progression != mScaleGuideView.end())
    {
        mScaleGuideStep = progression->value("step", 0);
    }

    // A step led from the last shape lands on a shape of the engine's choosing: keep it chosen, so
    // the next build asks for it plainly.
    const int position = mScaleGuideView.value("position", 0);

    if (!mScaleGuidePrevious.empty() || position != settings.position)
    {
        mScaleGuidePrevious.clear();
        auto chosen = settings;
        chosen.position = position;
        mSession.Commands().SetSetting(sg::kViewSetting, sg::SettingsJson(chosen));
        mScaleGuideViewKey = sg::ViewRequest(chosen, progressions, mScaleGuideStep).dump();
    }

    return mScaleGuideView;
}

std::string Shell::ScaleGuideSignature() const
{
    const auto settings = mState.appSettings.find(sg::kViewSetting);
    const auto progressions = mState.appSettings.find(sg::kProgressionsSetting);
    return (settings != mState.appSettings.end() ? settings->dump() : std::string()) + "|" +
           (progressions != mState.appSettings.end() ? progressions->dump() : std::string()) + "|" +
           std::to_string(mScaleGuideStep) + "|" + std::to_string(mScaleGuidePrevious.size()) +
           (ScaleGuidePlaying() ? "|playing" : "");
}

bool Shell::ScaleGuidePlaying() const
{
    return mState.demo.playing && mState.demo.previewId == kPlayedPosition;
}

void Shell::ChangeScaleGuide(const std::function<void(sg::Settings&)>& edit)
{
    // What is sounding is no longer what is showing.
    if (ScaleGuidePlaying())
    {
        mSession.Commands().StopScaleGuide();
    }

    const auto before = sg::LoadSettings(mState.appSettings);
    auto next = before;
    edit(next);

    // A new scale, chord, progression, tuning or mode starts on the whole neck and its first step;
    // arpeggios on their first run, since a run is what that tab shows.
    if (next.mode != before.mode || next.scale != before.scale || next.chord != before.chord ||
        next.progression != before.progression || next.tuning != before.tuning ||
        next.arpeggioStyle != before.arpeggioStyle || next.arpeggioStrings != before.arpeggioStrings)
    {
        next.position = next.position != before.position ? next.position : next.mode == "arpeggio" ? 1 : 0;
        mScaleGuideStep = 0;
        mScaleGuideRound = 1;
        mScaleGuideClean = 0;
    }

    mSession.Commands().SetSetting(sg::kViewSetting, sg::SettingsJson(next));
    Mark(kPage);
}

void Shell::ScaleGuideStep(int index, std::vector<int> fromFrets)
{
    const auto progression = mScaleGuideView.find("progression");

    if (progression == mScaleGuideView.end())
    {
        return;
    }

    const int count = static_cast<int>(progression->value("chords", nlohmann::json::array()).size());

    if (count == 0)
    {
        return;
    }

    // The next chord takes the shape nearest the one showing (or the strip's own, when given).
    if (fromFrets.empty())
    {
        const int position = mScaleGuideView.value("position", 0);
        const auto shapes = mScaleGuideView.value("shapes", nlohmann::json::array());

        if (position > 0 && position <= static_cast<int>(shapes.size()))
        {
            fromFrets = ReadGuideShape(shapes[static_cast<std::size_t>(position - 1)]).frets;
        }
    }

    mScaleGuidePrevious = std::move(fromFrets);
    mScaleGuideStep = ((index % count) + count) % count;
    Mark(kPage);
}

Ptr Shell::BuildScaleGuide(float width)
{
    const auto settings = sg::LoadSettings(mState.appSettings);
    const auto progressions = sg::LoadProgressions(mState.appSettings);
    const auto& view = ScaleGuideView();
    const auto& catalog = Catalog();
    const bool chordMode = settings.mode == "chord";
    const bool arpeggioMode = settings.mode == "arpeggio";
    const bool chordInfo = chordMode || arpeggioMode;
    const bool rootKey = arpeggioMode || (chordMode && settings.progression.empty());
    const auto model = ReadGuideModel(view);
    const auto progression = view.find("progression") != view.end() ? view["progression"] : nlohmann::json();

    // ── Tabs: Scales and Chords, LISTEN's readout, the title ──
    auto mode = std::make_shared<Segmented>(mTheme, std::vector<std::string>{"Scales", "Chords", "Arpeggios"}, 90.0f);
    mode->AccessName("Scales, chords or arpeggios");
    mode->Select(arpeggioMode ? 2 : chordMode ? 1 : 0);
    mode->onSelect = [this](int index) {
        ChangeScaleGuide([index](sg::Settings& s) {
            s.mode = index == 2 ? "arpeggio" : index == 1 ? "chord" : "scale";
        });
    };
    auto live = std::make_shared<Text>(mTheme, mScaleGuideReadout, 13.0f, Weight::SemiBold);
    live->Colour(mTheme.Success()).Alignment(Text::Align::Right);
    mScaleGuideLive = live;
    auto title = std::make_shared<Text>(
        mTheme, view.value("title", "") + " \xC2\xB7 " + view["tuning"].value("name", ""), 12.0f);
    title->Colour(mTheme.TextMuted()).Alignment(Text::Align::Right);

    // ── Pickers ──
    const auto picker = [this](const std::string& text, const std::string& name) {
        auto p = std::make_shared<PickerField>(mTheme, mIcons);
        p->Set(text);
        p->AccessName(name);
        return p;
    };
    const auto field = [this](const std::string& label, Ptr control) {
        return Col({Label(mTheme, label), VGap(4), control});
    };
    const auto keys = catalog["keys"];
    auto key = picker(keys[static_cast<std::size_t>(settings.key)].get<std::string>(), rootKey ? "Root" : "Key");
    key->onClick = [this, weak = std::weak_ptr<PickerField>(key), keys] {
        const auto key = weak.lock();

        if (!key)

        {
            return;
        }

        std::vector<MenuItem> items;

        for (int pc = 0; pc < 12; ++pc)
        {
            items.push_back({keys[static_cast<std::size_t>(pc)].get<std::string>(),
                             pc == sg::LoadSettings(mState.appSettings).key, false,
                             [this, pc] { ChangeScaleGuide([pc](sg::Settings& s) { s.key = pc; }); }});
        }

        OpenDropdown(key->Bounds(), std::move(items), 160.0f);
    };

    const auto chordPicker = [&] {
        auto chord = picker(NameOf(catalog["chords"], settings.chord), "Chord");
        chord->onClick = [this, weak = std::weak_ptr<PickerField>(chord)] {
            const auto chord = weak.lock();

            if (!chord)

            {
                return;
            }

            std::vector<MenuItem> items;
            const auto current = sg::LoadSettings(mState.appSettings).chord;

            for (const auto& c : Catalog()["chords"])
            {
                const auto id = c.value("id", "");
                items.push_back({c.value("name", ""), id == current, false,
                                 [this, id] { ChangeScaleGuide([id](sg::Settings& s) { s.chord = id; }); }});
            }

            OpenDropdown(chord->Bounds(), std::move(items), 200.0f);
        };
        return field("Chord", chord);
    };

    Ptr what;

    if (arpeggioMode)
    {
        what = chordPicker();
    }
    else if (!chordMode)
    {
        auto scale = picker(NameOf(catalog["scales"], settings.scale), "Scale");
        scale->onClick = [this, weak = std::weak_ptr<PickerField>(scale)] {
            const auto scale = weak.lock();

            if (!scale)

            {
                return;
            }

            // One column a group, as the web UI's grouped list.
            std::vector<std::vector<MenuItem>> columns;
            std::string group;
            const auto current = sg::LoadSettings(mState.appSettings).scale;

            for (const auto& s : Catalog()["scales"])
            {
                if (s.value("group", "") != group)
                {
                    group = s.value("group", "");

                    if (columns.empty() || columns.back().size() > 14)
                    {
                        columns.emplace_back();
                    }

                    columns.back().push_back({Upper(group), false, true, nullptr});
                }

                const auto id = s.value("id", "");
                columns.back().push_back({s.value("name", ""), id == current, false,
                                          [this, id] { ChangeScaleGuide([id](sg::Settings& st) { st.scale = id; }); }});
            }

            OpenDropdownColumns(scale->Bounds(), std::move(columns), 210.0f);
        };
        what = field("Scale", scale);
    }
    else
    {
        std::string playing = "One chord";

        if (settings.progression.rfind("b:", 0) == 0)
        {
            playing = NameOf(catalog["progressions"], settings.progression.substr(2));
        }
        else if (settings.progression.rfind("u:", 0) == 0)
        {
            const auto id = settings.progression.substr(2);
            const auto own =
                std::find_if(progressions.begin(), progressions.end(), [&](const auto& p) { return p.id == id; });
            playing = own != progressions.end() ? own->name : playing;
        }

        auto play = picker(playing, "Play");
        play->onClick = [this, weak = std::weak_ptr<PickerField>(play)] {
            const auto play = weak.lock();

            if (!play)

            {
                return;
            }

            const auto current = sg::LoadSettings(mState.appSettings).progression;
            const auto choose = [this](std::string value) {
                return [this, value] { ChangeScaleGuide([value](sg::Settings& s) { s.progression = value; }); };
            };
            std::vector<MenuItem> items;
            items.push_back({"One chord", current.empty(), false, choose("")});
            items.push_back({"PROGRESSIONS", false, true, nullptr});

            for (const auto& p : Catalog()["progressions"])
            {
                const auto id = "b:" + p.value("id", "");
                std::string numerals;

                for (const auto& n : p["numerals"])
                {
                    numerals += (numerals.empty() ? "" : " ") + n.get<std::string>();
                }

                MenuItem item{p.value("name", ""), id == current, false, choose(id)};
                item.subtitle = numerals;
                items.push_back(std::move(item));
            }

            const auto mine = sg::LoadProgressions(mState.appSettings);

            if (!mine.empty())
            {
                items.push_back({"SAVED PROGRESSIONS", false, true, nullptr});

                for (const auto& p : mine)
                {
                    items.push_back({p.name, "u:" + p.id == current, false, choose("u:" + p.id)});
                }
            }

            OpenDropdown(play->Bounds(), std::move(items), 300.0f);
        };
        what = field("Play", play);

        if (settings.progression.empty())
        {
            what = Row({Stretch(what), HGap(14), Stretch(chordPicker())});
        }
    }

    auto tuning = picker(NameOf(catalog["tunings"], settings.tuning), "Tuning");
    tuning->onClick = [this, weak = std::weak_ptr<PickerField>(tuning)] {
        const auto tuning = weak.lock();

        if (!tuning)

        {
            return;
        }

        std::vector<MenuItem> items;
        const auto current = sg::LoadSettings(mState.appSettings).tuning;
        std::string instrument;

        for (const auto& t : Catalog()["tunings"])
        {
            if (t.value("instrument", "") != instrument)
            {
                instrument = t.value("instrument", "");
                items.push_back({Upper(instrument), false, true, nullptr});
            }

            const auto id = t.value("id", "");
            items.push_back({t.value("name", ""), id == current, false,
                             [this, id] { ChangeScaleGuide([id](sg::Settings& s) { s.tuning = id; }); }});
        }

        OpenDropdown(tuning->Bounds(), std::move(items), 260.0f);
    };
    auto capo = picker(settings.capo == 0 ? "None" : std::to_string(settings.capo), "Capo");
    capo->onClick = [this, weak = std::weak_ptr<PickerField>(capo)] {
        const auto capo = weak.lock();

        if (!capo)

        {
            return;
        }

        std::vector<MenuItem> items;
        const int current = sg::LoadSettings(mState.appSettings).capo;

        for (int fret = 0; fret <= sg::kMaxCapo; ++fret)
        {
            items.push_back({fret == 0 ? "None" : std::to_string(fret), fret == current, false,
                             [this, fret] { ChangeScaleGuide([fret](sg::Settings& s) { s.capo = fret; }); }});
        }

        OpenDropdown(capo->Bounds(), std::move(items), 120.0f);
    };

    // ── View, position or shape, handedness, LISTEN ──
    // Order and Fingers number an arpeggio's run; the other tabs have no use for them.
    std::vector<std::string> labelIds{"name", "interval", "degree"};
    std::vector<std::string> labelNames{"Notes", "Intervals", "Degrees"};

    if (arpeggioMode)
    {
        labelIds.insert(labelIds.end(), {"order", "finger"});
        labelNames.insert(labelNames.end(), {"Order", "Fingers"});
    }

    auto labels = std::make_shared<Segmented>(mTheme, labelNames, arpeggioMode ? 68.0f : 76.0f);
    labels->AccessName("Labels");
    const auto chosenLabels = std::find(labelIds.begin(), labelIds.end(), settings.labels);
    labels->Select(chosenLabels == labelIds.end() ? 0 : static_cast<int>(chosenLabels - labelIds.begin()));
    labels->onSelect = [this, labelIds](int index) {
        ChangeScaleGuide([&](sg::Settings& s) { s.labels = labelIds[static_cast<std::size_t>(index)]; });
    };
    std::vector<std::string> positionNames{"All"};
    const auto count = chordMode      ? model.shapes.size()
                       : arpeggioMode ? model.runs.size()
                                      : view.value("positions", nlohmann::json::array()).size();

    for (std::size_t i = 1; i <= count; ++i)
    {
        const auto hint = chordMode      ? model.shapes[i - 1].caged
                          : arpeggioMode ? model.runs[i - 1].start
                                         : std::string();
        positionNames.push_back(std::to_string(i) + (hint.empty() ? "" : " " + hint));
    }

    auto positions = std::make_shared<Segmented>(mTheme, positionNames, chordMode || arpeggioMode ? 44.0f : 36.0f);
    positions->AccessName(chordMode ? "Shape" : "Position");
    positions->Select(model.position);
    positions->onSelect = [this](int index) { ChangeScaleGuide([index](sg::Settings& s) { s.position = index; }); };
    auto lefty = std::make_shared<CheckBox>(mTheme, "Left-handed");
    lefty->Set(settings.leftHanded);
    lefty->onClick = [this] { ChangeScaleGuide([](sg::Settings& s) { s.leftHanded = !s.leftHanded; }); };
    auto listen = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
    listen->Size(96, 30).Label("\xE2\x97\x8F Listen", 11.0f).Lit(settings.listen);
    listen->AccessName("Listen");

    if (settings.listen)
    {
        listen->Ink(mTheme.Success());
    }

    listen->onClick = [this] { ChangeScaleGuide([](sg::Settings& s) { s.listen = !s.listen; }); };

    // Every play control stops whatever is sounding; otherwise each sounds its own thing, from the
    // request this view was drawn from, so what sounds is what shows.
    const bool playing = ScaleGuidePlaying();
    const auto togglePlay = [this](bool scaleExample) {
        if (ScaleGuidePlaying())
        {
            mSession.Commands().StopScaleGuide();
        }
        else if (scaleExample)
        {
            mSession.Commands().PlayScaleExample(nlohmann::json::parse(mScaleGuideViewKey));
        }
        else
        {
            mSession.Commands().PlayScaleGuide(nlohmann::json::parse(mScaleGuideViewKey));
        }
    };
    const auto playButton = [&, togglePlay] {
        auto button = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        button->Size(84, 30).Label(playing ? "\xE2\x96\xA0 Stop" : "\xE2\x96\xB6 Play", 11.0f).Lit(playing);
        button->Disabled(model.position == 0);
        button->AccessName(playing ? "Stop" : "Play");
        button->onClick = [togglePlay] { togglePlay(false); };
        return button;
    };
    // A chord box strums its shape: the request only says which strings there are.
    const auto strum = [this](const std::vector<int>& frets) {
        mSession.Commands().PlayScaleGuideChord(nlohmann::json::parse(mScaleGuideViewKey), frets);
    };
    auto play = playButton();

    std::vector<Ptr> cardRows = {
        Row({FixedWidth(110, field(rootKey ? "Root" : "Key", key)), HGap(14), Stretch(what, 2.0), HGap(14),
             Stretch(field("Tuning", tuning), 1.4), HGap(14), FixedWidth(96, field("Capo", capo))}),
        VGap(12)};
    const auto viewRow =
        Row({FixedWidth(44, Middle(Label(mTheme, "View"))), Middle(labels), HGap(18),
             FixedWidth(chordMode ? 54.0f : 70.0f, Middle(Label(mTheme, chordMode ? "Shape" : "Position"))),
             Middle(positions), Stretch()});
    const auto toggles = Row({Stretch(), Middle(lefty), HGap(12), Middle(listen), HGap(8), Middle(play)});

    // At the compact size the view controls and the toggles take a row each, as the web UI's wrap.
    if (width >= 1000.0f)
    {
        cardRows.push_back(Row({viewRow, FixedWidth(240, toggles)}));
    }
    else
    {
        cardRows.push_back(viewRow);
        cardRows.push_back(VGap(10));
        cardRows.push_back(toggles);
    }

    // ── An arpeggio's style and strings ──
    if (arpeggioMode)
    {
        const std::vector<std::string> styleIds{"sweep", "two", "box"};
        auto style =
            std::make_shared<Segmented>(mTheme, std::vector<std::string>{"Sweep", "2 per string", "Box"}, 84.0f);
        style->AccessName("Style");
        style->Select(settings.arpeggioStyle == "two" ? 1 : settings.arpeggioStyle == "box" ? 2 : 0);
        style->onSelect = [this, styleIds](int index) {
            ChangeScaleGuide([&](sg::Settings& s) { s.arpeggioStyle = styleIds[static_cast<std::size_t>(index)]; });
        };
        std::vector<Ptr> row{FixedWidth(44, Middle(Label(mTheme, "Style"))), Middle(style)};

        // A box takes every string under the hand, so there is nothing to choose.
        if (settings.arpeggioStyle != "box")
        {
            const auto choices = view["arpeggio"].value("stringChoices", std::vector<int>{});
            const int chosen = view["arpeggio"].value("strings", 0);
            std::vector<std::string> names;

            for (const int choice : choices)
            {
                names.push_back(std::to_string(choice));
            }

            auto strings = std::make_shared<Segmented>(mTheme, names, 36.0f);
            strings->AccessName("Strings");
            strings->Select(static_cast<int>(std::find(choices.begin(), choices.end(), chosen) - choices.begin()));
            strings->onSelect = [this, choices](int index) {
                ChangeScaleGuide(
                    [&](sg::Settings& s) { s.arpeggioStrings = choices[static_cast<std::size_t>(index)]; });
            };
            row.insert(row.end(), {HGap(18), FixedWidth(60, Middle(Label(mTheme, "Strings"))), Middle(strings)});
        }

        row.push_back(Stretch());
        cardRows.push_back(VGap(12));
        cardRows.push_back(Row(row));
    }

    // ── A progression's steps ──
    if (progression.is_object())
    {
        std::vector<std::string> steps;

        for (const auto& c : progression["chords"])
        {
            steps.push_back(c.value("name", ""));
        }

        auto stepper = std::make_shared<Segmented>(mTheme, steps, 44.0f);
        stepper->AccessName("Steps");
        stepper->Select(progression.value("step", 0));
        stepper->onSelect = [this](int index) { ScaleGuideStep(index); };
        const auto button = [this](const std::string& glyph, const std::string& name, std::function<void()> click) {
            auto b = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
            b->Size(32, 30).Label(glyph, 12.0f);
            b->AccessName(name);
            b->onClick = std::move(click);
            return b;
        };
        auto round =
            std::make_shared<Text>(mTheme, settings.listen ? "Round " + std::to_string(mScaleGuideRound) : "", 11.0f);
        round->Colour(mTheme.TextMuted());
        cardRows.push_back(VGap(12));
        cardRows.push_back(Row(
            {Middle(Label(mTheme, "Steps")), HGap(8),
             // A long progression scrolls its steps only when they would not fit beside the buttons.
             Middle(static_cast<float>(steps.size()) * 44.0f > width - 300.0f ? HScroll(stepper) : Ptr(stepper)),
             HGap(8), Middle(button("\xE2\x97\x80", "Back one chord", [this] { ScaleGuideStep(mScaleGuideStep - 1); })),
             HGap(4), Middle(button("\xE2\x96\xB6", "Forward one chord", [this] { ScaleGuideStep(mScaleGuideStep + 1); })),
             HGap(4),
             Middle(button("\xE2\x86\xBA", "Restart from the top",
                           [this] {
                               mScaleGuideRound = 1;
                               ScaleGuideStep(0);
                           })),
             HGap(10), Middle(FixedWidth(70, round)), Stretch()}));
    }

    // ── The neck ──
    auto neck = std::make_shared<ScaleNeck>(mTheme, model, settings.labels, settings.leftHanded);
    mScaleNeck = neck;
    neck->onNote = [this](int midi) { mSession.Commands().PlayScaleGuideNote(midi); };
    // LISTEN follows notes in Scales mode only; a chord is matched, not followed.
    neck->SetHeard(chordMode ? std::vector<int>{} : ScaleGuideHeard());

    // ── Chord boxes: the shapes, or a progression's strip ──
    std::vector<Ptr> boxes;

    if (progression.is_object())
    {
        int index = 0;

        for (const auto& c : progression["chords"])
        {
            if (c["shape"].is_object())
            {
                auto box = std::make_shared<ChordBoxView>(mTheme, ReadGuideShape(c["shape"]), model.openMidi,
                                                          model.capo, c.value("rootPitchClass", 0), settings.leftHanded,
                                                          Numeral(c.value("numeral", "")), c.value("name", ""),
                                                          index == progression.value("step", 0));
                const auto frets = ReadGuideShape(c["shape"]).frets;
                box->onClick = [this, index, frets, strum] {
                    strum(frets);
                    ScaleGuideStep(index, frets);
                };
                box->AccessName(c.value("name", "") + ": go to it and hear it strummed");
                boxes.push_back(box);
                boxes.push_back(HGap(8));
            }

            ++index;
        }
    }
    else if (chordMode)
    {
        for (std::size_t i = 0; i < model.shapes.size(); ++i)
        {
            const auto& shape = model.shapes[i];
            auto box = std::make_shared<ChordBoxView>(
                mTheme, shape, model.openMidi, model.capo, model.rootPitchClass, settings.leftHanded,
                shape.caged.empty() ? "" : shape.caged + " shape", "", static_cast<int>(i + 1) == model.position);
            box->onClick = [this, i, strum, frets = shape.frets] {
                ChangeScaleGuide([i](sg::Settings& s) { s.position = static_cast<int>(i + 1); });
                strum(frets);
            };
            box->AccessName((shape.caged.empty() ? "Shape " + std::to_string(i + 1) : shape.caged + " shape") +
                            ": show it and hear it strummed");
            boxes.push_back(box);
            boxes.push_back(HGap(8));
        }
    }

    // ── An arpeggio's run as tab, or why there is none ──
    const auto face = [this](Ptr content) {
        return Rounded(mTheme.Pick(el::rgba(255, 255, 255, 7), el::rgba(20, 24, 34, 6)), 12.0f, std::move(content));
    };
    Ptr tabCard;

    if (arpeggioMode || model.Run() != nullptr)
    {
        const auto note = [&](const std::string& text) {
            auto p = std::make_shared<Paragraph>(mTheme, text, std::max(200.0f, width - 40.0f), 12.0f);
            p->Colour(mTheme.TextMuted());
            return p;
        };
        const auto* run = model.Run();

        if (run != nullptr)
        {
            auto name = std::make_shared<Text>(mTheme, run->name, 15.0f, Weight::SemiBold);
            name->Colour(mTheme.Text());
            auto frets =
                std::make_shared<Text>(mTheme,
                                       run->fretFrom == run->fretTo ? "Fret " + std::to_string(run->fretFrom)
                                                                    : "Frets " + std::to_string(run->fretFrom) +
                                                                          "\xE2\x80\x93" + std::to_string(run->fretTo),
                                       13.0f);
            frets->Colour(mTheme.TextSecondary());
            tabCard =
                face(Pad(16, 10, 16, 10,
                         Col({FixedHeight(30, Row({Middle(FixedWidth(180, name)), HGap(10),
                                                   Middle(FixedWidth(120, frets)), Stretch(), Middle(playButton())})),
                              VGap(4), HScroll(std::make_shared<RunTabView>(mTheme, *run, model.stringNames)), VGap(4),
                              arpeggioMode ? Ptr(note(view["arpeggio"].value("advice", ""))) : Ptr(VGap(0))})));
        }
        else
        {
            const auto text = model.runs.empty() ? view["arpeggio"].value("empty", "")
                                                 : "Pick a position to see one run, with its fingers and pick strokes.";
            tabCard = face(Pad(16, 10, 16, 10, note(text)));
        }
    }

    // ── Summary: the root, the name and notes, then the scale's chords or the chord's formula ──
    const auto tones = view.value("tones", nlohmann::json::array());
    std::string notes;

    for (const auto& t : tones)
    {
        notes += (notes.empty() ? "" : "  ") + t.value("name", "");
    }

    if (!chordInfo && !tones.empty())
    {
        notes += "  " + tones[0].value("name", "");
    }

    auto badge =
        std::make_shared<Text>(mTheme, tones.empty() ? "" : tones[0].value("name", ""), 26.0f, Weight::SemiBold);
    badge->Colour(mTheme.Accent()).Alignment(Text::Align::Centre);
    auto summaryName = std::make_shared<Text>(mTheme,
                                              chordInfo ? view["chord"].value("name", "") + " \xC2\xB7 " +
                                                              view["chord"].value("quality", "")
                                                        : view["scale"].value("name", ""),
                                              19.0f);
    summaryName->Colour(mTheme.Text());
    auto summaryNotes = std::make_shared<Text>(mTheme, notes, 14.0f);
    summaryNotes->Colour(mTheme.TextSecondary());

    std::vector<Ptr> pills;
    std::string middleLabel = chordInfo ? "Formula" : "Chords";

    if (chordInfo)
    {
        auto formula = std::make_shared<Text>(mTheme, view["chord"].value("formula", ""), 15.0f, Weight::Medium);
        formula->Colour(mTheme.Text());
        pills.push_back(FixedHeight(30, formula));
    }
    else if (view["scale"]["diatonic"].empty())
    {
        middleLabel = "Steps";
        auto steps = std::make_shared<Text>(mTheme, view["scale"].value("steps", ""), 15.0f, Weight::Medium);
        steps->Colour(mTheme.Text());
        pills.push_back(FixedHeight(30, steps));
    }
    else
    {
        for (const auto& c : view["scale"]["diatonic"])
        {
            auto pill = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
            pill->Size(58, 34).Label(c.value("name", ""), 13.0f).Lit(&c == &view["scale"]["diatonic"][0]);
            pill->AccessName(c.value("name", "") + ", " + c.value("numeral", "") + ": open in Chords");
            const int root = c.value("rootPitchClass", 0);
            const auto quality = c.value("quality", "");
            pill->onClick = [this, root, quality] {
                ChangeScaleGuide([root, quality](sg::Settings& s) {
                    s.mode = "chord";
                    s.key = root;
                    s.chord = quality.empty() ? "maj" : quality;
                    s.progression.clear();
                });
            };
            pills.push_back(pill);
            pills.push_back(HGap(6));
        }
    }

    // Padded so a scale's face, lit on hover, has room round the text.
    Ptr identity = Pad(8, 4, 8, 4,
                       Row({FixedWidth(56, Middle(FixedHeight(40, badge))), HGap(12),
                            Stretch(Col({Label(mTheme, arpeggioMode ? "Arpeggio"
                                                       : chordMode  ? "Chord"
                                                                    : "Scale"),
                                         VGap(2), FixedHeight(26, summaryName), FixedHeight(20, summaryNotes)}))}));

    // A scale's name and notes play it, one octave up and back.
    if (!chordMode && !arpeggioMode && !tones.empty())
    {
        auto area = std::make_shared<PlayArea>(mTheme, playing);
        area->AccessName("Hear the scale, one octave up and back");
        area->onClick = [togglePlay] { togglePlay(true); };
        identity = Over(area, identity);
    }

    auto summary =
        Row({Stretch(identity), HGap(18), Stretch(Col({Label(mTheme, middleLabel), VGap(6), Row(pills)}), 1.6)});

    // The cards' faces only: a Rounded draws, it never takes a click from what is on it.
    const auto& card = face;
    std::vector<Ptr> page = {
        Row({Middle(mode), HGap(12), Stretch(Middle(FixedHeight(20, live))), HGap(12), Middle(FixedHeight(18, title))}),
        VGap(12), card(Pad(16, 14, 16, 14, Col(cardRows))), VGap(10),
        // The neck fills the page; only a page narrower than its least width scrolls it.
        width >= 600.0f ? Ptr(neck) : HScroll(neck)};

    if (!boxes.empty())
    {
        page.push_back(VGap(10));
        page.push_back(HScroll(Row(boxes)));
    }

    if (tabCard)
    {
        page.push_back(VGap(10));
        page.push_back(tabCard);
    }

    page.push_back(VGap(12));
    page.push_back(card(Pad(16, 12, 16, 12, summary)));
    page.push_back(VGap(16));
    return Col(page);
}

std::vector<int> Shell::ScaleGuideHeard() const
{
    std::vector<int> notes;

    if (const auto current = mScaleGuideFollower.Current())
    {
        notes.push_back(*current);
    }

    for (const int note : mScaleGuideFollower.Trail())
    {
        notes.push_back(note);
    }

    return notes;
}

void Shell::SyncScaleGuideListen()
{
    const auto settings = sg::LoadSettings(mState.appSettings);
    const bool wanted = settings.listen && mPage == Page::Jam && mJamSection == JamSection::Scales;
    const bool chroma = wanted && settings.mode == "chord";

    if (wanted != mScaleGuideListening || chroma != mScaleGuideChroma)
    {
        if (wanted || mScaleGuideListening)
        {
            mSession.Commands().SetTunerListening(wanted, chroma);
        }

        mScaleGuideListening = wanted;
        mScaleGuideChroma = chroma;
        mScaleGuideFollower.Reset();
        mScaleGuideStrums.Reset();
        mScaleGuideReadout = wanted ? "Listening" : "";
        Mark(kPage);
        return;
    }

    const auto& tuner = mState.tuner;

    if (!wanted || tuner.readings == mScaleGuideReadings || mScaleGuideView.is_null())
    {
        return;
    }

    mScaleGuideReadings = tuner.readings;
    const auto setReadout = [this](std::string text) {
        if (text == mScaleGuideReadout)
        {
            return;
        }

        mScaleGuideReadout = std::move(text);

        if (const auto live = mScaleGuideLive.lock())
        {
            live->Set(mScaleGuideReadout);
            mSession.Repaint(*live);
        }
    };
    const auto tones = mScaleGuideView.value("tones", nlohmann::json::array());

    if (!chroma)
    {
        if (!mScaleGuideFollower.Push(tuner.detected ? sg::ReadingMidi(tuner.noteName, tuner.octave) : std::nullopt))
        {
            return;
        }

        if (const auto neck = mScaleNeck.lock(); neck && neck->SetHeard(ScaleGuideHeard()))
        {
            mSession.Repaint(*neck);
        }

        const auto current = mScaleGuideFollower.Current();

        if (!current)
        {
            setReadout("Listening");
            return;
        }

        const int pc = ((*current % 12) + 12) % 12;
        const auto tone =
            std::find_if(tones.begin(), tones.end(), [pc](const auto& t) { return t.value("pitchClass", -1) == pc; });
        setReadout(tone != tones.end()
                       ? tone->value("name", "") + "  " + tone->value("interval", "") + " \xC2\xB7 of the " +
                             settings.mode
                       : tuner.noteName.substr(0, tuner.noteName.find('/')) + "  outside the " + settings.mode);
        return;
    }

    if (!tuner.hasChroma)
    {
        return;
    }

    std::vector<sg::ChordTone> chordTones;

    for (const auto& t : tones)
    {
        chordTones.push_back({t.value("pitchClass", 0), t.value("interval", "")});
    }

    const int root = chordTones.empty() ? 0 : chordTones.front().pitchClass;
    const bool loud = tuner.levelDb > sg::StrumGate::kQuietDb - 2.0;
    const auto match = loud ? sg::MatchChord(tuner.chroma, root, chordTones, settings.match) : sg::ChordMatch{};

    if (mScaleGuideStrums.Push(tuner.levelDb, loud && match.ok))
    {
        if (mScaleGuideView.contains("progression"))
        {
            const auto& progression = mScaleGuideView["progression"];

            if (progression.value("step", 0) == static_cast<int>(progression["chords"].size()) - 1)
            {
                ++mScaleGuideRound;
            }

            mScaleGuideReadout = "\xE2\x9C\x93 Clean";
            ScaleGuideStep(mScaleGuideStep + 1);
        }
        else
        {
            setReadout("\xE2\x9C\x93 Clean \xC3\x97" + std::to_string(++mScaleGuideClean));
        }

        return;
    }

    if (!loud)
    {
        setReadout("Play " + mScaleGuideView.value("title", ""));
        return;
    }

    std::string missing;

    for (const auto& t : tones)
    {
        if (std::find(match.missing.begin(), match.missing.end(), t.value("pitchClass", -1)) != match.missing.end())
        {
            missing += (missing.empty() ? "" : " ") + t.value("name", "");
        }
    }

    setReadout(missing.empty() ? "Clean " + std::to_string(static_cast<int>(std::lround(match.share * 100.0))) + "%"
                               : mScaleGuideView.value("title", "") + "  missing " + missing);
}
} // namespace nanoq::ui
