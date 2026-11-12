// The Scale Guide in Jam's Scales section (docs/plans/scale-guide.md), as the web UI draws it
// (ts/scaleGuide/panel.ts): Scales, Chords and Arpeggios, slim rows of controls (the tabs with
// Play and the instrument; what is showing; a progression's steps; position and dots), the neck,
// the chord boxes (a progression's voice-led strip) or an arpeggio's tab (a progression's whole, a
// bar for each chord), a summary and LISTEN. LISTEN has no switch: it is on whenever the section
// shows. At the compact size every control flows as one wrapping row, and the neck's strings close
// up so the whole neck fits a short window under them (css/compact/panels.css).
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
    const auto request =
        sg::ViewRequest(settings, progressions, mScaleGuideStep, mScaleGuidePrevious, mScaleGuideAnchor);
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
    // the next build asks for it plainly. In a progression's arpeggios the choice kept is the anchor
    // chord's run, not the run of the chord showing, so stepping never moves the shapes.
    int position = mScaleGuideView.value("position", 0);

    if (const auto progression = mScaleGuideView.find("progression");
        progression != mScaleGuideView.end() && progression->contains("anchorPosition"))
    {
        position = progression->value("anchorPosition", 0);
        mScaleGuideAnchor = progression->value("anchorStep", 0);
    }

    if (!mScaleGuidePrevious.empty() || position != settings.position)
    {
        mScaleGuidePrevious.clear();
        auto chosen = settings;
        chosen.position = position;
        mSession.Commands().SetSetting(sg::kViewSetting, sg::SettingsJson(chosen));
        mScaleGuideViewKey = sg::ViewRequest(chosen, progressions, mScaleGuideStep, {}, mScaleGuideAnchor).dump();
    }

    return mScaleGuideView;
}

std::string Shell::ScaleGuideSignature() const
{
    const auto settings = mState.appSettings.find(sg::kViewSetting);
    const auto progressions = mState.appSettings.find(sg::kProgressionsSetting);
    return (settings != mState.appSettings.end() ? settings->dump() : std::string()) + "|" +
           (progressions != mState.appSettings.end() ? progressions->dump() : std::string()) + "|" +
           std::to_string(mScaleGuideStep) + "|" + std::to_string(mScaleGuideAnchor) + "|" +
           std::to_string(mScaleGuidePrevious.size()) + (ScaleGuidePlaying() ? "|playing" : "");
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
        mScaleGuideAnchor = 0;
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
    // Chords and Arpeggios take one chord on a root, or a progression in a key.
    const bool rootKey = chordInfo && settings.progression.empty();
    const auto model = ReadGuideModel(view);
    const auto progression = view.find("progression") != view.end() ? view["progression"] : nlohmann::json();
    const auto wholeRun = arpeggioMode ? ReadGuideProgressionRun(view) : std::nullopt;
    // The compact frame: the pickers say what they are by what they show, as the web UI's do (their
    // names stay for a screen reader), and the controls flow as one wrapping row (`flow`).
    const bool compact = !mWide;
    std::vector<std::pair<Ptr, float>> flow;

    // ── Tabs, LISTEN's readout, then Play, its volume and the instrument ──
    const float modeWidth = compact ? 72.0f : 80.0f;
    auto mode =
        std::make_shared<Segmented>(mTheme, std::vector<std::string>{"Scales", "Chords", "Arpeggios"}, modeWidth);
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

    // ── Pickers, each with its label beside it ──
    const auto picker = [this](const std::string& text, const std::string& name) {
        auto p = std::make_shared<PickerField>(mTheme, mIcons);
        p->Set(text);
        p->AccessName(name);
        return p;
    };
    const auto labelWidth = [](const std::string& label) { return 8.0f * static_cast<float>(label.size()) + 6.0f; };
    const auto field = [this, labelWidth, compact](const std::string& label, Ptr control, float width) -> Ptr {
        if (compact)
        {
            return FixedWidth(width, Middle(control));
        }

        return FixedWidth(labelWidth(label) + 8.0f + width,
                          Row({FixedWidth(labelWidth(label), Middle(Label(mTheme, label))), HGap(8),
                               FixedWidth(width, Middle(control))}));
    };
    const auto fieldWidth = [labelWidth, compact](const std::string& label, float width) {
        return compact ? width : labelWidth(label) + 8.0f + width;
    };
    // A choice that changes nothing about what is sounding: saved, and the page drawn again.
    const auto keep = [this](const std::function<void(sg::Settings&)>& edit) {
        auto next = sg::LoadSettings(mState.appSettings);
        edit(next);
        mSession.Commands().SetSetting(sg::kViewSetting, sg::SettingsJson(next));
        Mark(kPage);
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
        return field("Chord", chord, 150.0f);
    };

    // What is showing: a scale, or for Chords and Arpeggios one chord or a progression in the key.
    const auto keyField = field(rootKey ? "Root" : "Key", key, compact ? 72.0f : 84.0f);
    std::vector<Ptr> what = {keyField, HGap(16)};
    flow.push_back({keyField, fieldWidth(rootKey ? "Root" : "Key", 72.0f)});

    if (!chordMode && !arpeggioMode)
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
        auto scaleField = field("Scale", scale, compact ? 200.0f : 220.0f);
        what.push_back(scaleField);
        flow.push_back({scaleField, fieldWidth("Scale", 200.0f)});
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
        auto playField = field("Play", play, compact ? 200.0f : 220.0f);
        what.push_back(playField);
        flow.push_back({playField, fieldWidth("Play", 200.0f)});

        if (settings.progression.empty())
        {
            auto chordField = chordPicker();
            what.push_back(HGap(16));
            what.push_back(chordField);
            flow.push_back({chordField, fieldWidth("Chord", 150.0f)});
        }
    }

    what.push_back(Stretch());

    // The instrument: tuning, capo and hand are chosen once and then left alone, so they sit behind
    // one button that says what is set, with how LISTEN marks the neck.
    std::string instrumentText = view["tuning"].value("name", "");

    if (settings.capo > 0)
    {
        instrumentText += " \xC2\xB7 Capo " + std::to_string(settings.capo);
    }

    if (settings.leftHanded)
    {
        instrumentText += " \xC2\xB7 Left-handed";
    }

    auto instrument = picker(instrumentText, "Instrument: tuning, capo and hand");
    instrument->onClick = [this, weak = std::weak_ptr<PickerField>(instrument), keep] {
        const auto instrument = weak.lock();

        if (!instrument)
        {
            return;
        }

        const auto current = sg::LoadSettings(mState.appSettings);
        std::vector<std::vector<MenuItem>> columns(3);
        std::string group;

        for (const auto& t : Catalog()["tunings"])
        {
            if (t.value("instrument", "") != group)
            {
                group = t.value("instrument", "");
                columns[0].push_back({Upper(group), false, true, nullptr});
            }

            const auto id = t.value("id", "");
            columns[0].push_back({t.value("name", ""), id == current.tuning, false,
                                  [this, id] { ChangeScaleGuide([id](sg::Settings& s) { s.tuning = id; }); }});
        }

        columns[1].push_back({"CAPO", false, true, nullptr});

        for (int fret = 0; fret <= sg::kMaxCapo; ++fret)
        {
            columns[1].push_back({fret == 0 ? "None" : "Fret " + std::to_string(fret), fret == current.capo, false,
                                  [this, fret] { ChangeScaleGuide([fret](sg::Settings& s) { s.capo = fret; }); }});
        }

        columns[2].push_back({"VIEW", false, true, nullptr});
        columns[2].push_back({"Left-handed", current.leftHanded, false,
                              [keep] { keep([](sg::Settings& s) { s.leftHanded = !s.leftHanded; }); }});
        columns[2].push_back({"Listen marks the whole neck", current.listenEverywhere, false,
                              [keep] { keep([](sg::Settings& s) { s.listenEverywhere = !s.listenEverywhere; }); }});
        OpenDropdownColumns(instrument->Bounds(), std::move(columns), 230.0f);
    };

    // ── On the neck: position or shape, and what the dots say ──
    // Order and Fingers number an arpeggio's run; the other tabs have no use for them.
    std::vector<std::string> labelIds{"name", "interval", "degree"};
    std::vector<std::string> labelNames{"Notes", "Intervals", "Degrees"};

    if (arpeggioMode)
    {
        labelIds.insert(labelIds.end(), {"order", "finger"});
        labelNames.insert(labelNames.end(), {"Order", "Fingers"});
    }

    const float labelSegment = arpeggioMode ? 68.0f : 76.0f;
    auto labels = std::make_shared<Segmented>(mTheme, labelNames, labelSegment);
    labels->AccessName("Dots");
    const auto chosenLabels = std::find(labelIds.begin(), labelIds.end(), settings.labels);
    labels->Select(chosenLabels == labelIds.end() ? 0 : static_cast<int>(chosenLabels - labelIds.begin()));
    labels->onSelect = [keep, labelIds](int index) {
        keep([&](sg::Settings& s) { s.labels = labelIds[static_cast<std::size_t>(index)]; });
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

    const float positionSegment = chordMode || arpeggioMode ? 44.0f : compact ? 32.0f : 36.0f;
    auto positions = std::make_shared<Segmented>(mTheme, positionNames, positionSegment);
    positions->AccessName(chordMode ? "Shape" : "Position");
    positions->Select(model.position);
    positions->onSelect = [this](int index) {
        // In a progression's arpeggios the run chosen is this chord's: the others follow from it.
        mScaleGuideAnchor = mScaleGuideStep;
        ChangeScaleGuide([index](sg::Settings& s) { s.position = index; });
    };
    // Every play control stops whatever is sounding; otherwise each sounds its own thing, from the
    // request this view was drawn from, so what sounds is what shows.
    const bool playing = ScaleGuidePlaying();
    const auto volume = [this] { return sg::LoadSettings(mState.appSettings).playVolume / 100.0; };
    const auto togglePlay = [this, volume](bool scaleExample) {
        if (ScaleGuidePlaying())
        {
            mSession.Commands().StopScaleGuide();
        }
        else if (scaleExample)
        {
            mSession.Commands().PlayScaleExample(nlohmann::json::parse(mScaleGuideViewKey), volume());
        }
        else
        {
            mSession.Commands().PlayScaleGuide(nlohmann::json::parse(mScaleGuideViewKey), volume());
        }
    };
    const auto playButton = [&, togglePlay] {
        auto button = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);

        // Compact, its glyph alone (still named for a screen reader).
        if (compact)
        {
            button->Size(40, 30).Label(playing ? "\xE2\x96\xA0" : "\xE2\x96\xB6", 11.0f).Lit(playing);
        }
        else
        {
            button->Size(84, 30).Label(playing ? "\xE2\x96\xA0 Stop" : "\xE2\x96\xB6 Play", 11.0f).Lit(playing);
        }

        // A progression's arpeggios play whole, whatever the position.
        button->Disabled(model.position == 0 && !wholeRun);
        button->AccessName(playing ? "Stop" : "Play");
        button->onClick = [togglePlay] { togglePlay(false); };
        return button;
    };
    // A chord box strums its shape: the request only says which strings there are.
    const auto strum = [this, volume](const std::vector<int>& frets) {
        mSession.Commands().PlayScaleGuideChord(nlohmann::json::parse(mScaleGuideViewKey), frets, volume());
    };
    auto play = playButton();

    // How loud Play's guitar is going into the preset, like its volume knob.
    auto level = picker("Vol " + std::to_string(settings.playVolume) + "%", "Play volume");
    level->onClick = [this, weak = std::weak_ptr<PickerField>(level), keep] {
        const auto level = weak.lock();

        if (!level)
        {
            return;
        }

        std::vector<MenuItem> items;
        const int current = sg::LoadSettings(mState.appSettings).playVolume;

        for (const int percent : {100, 80, 60, 40, 20, 10})
        {
            items.push_back({std::to_string(percent) + "%", percent == current, false,
                             [keep, percent] { keep([percent](sg::Settings& s) { s.playVolume = percent; }); }});
        }

        OpenDropdown(level->Bounds(), std::move(items), 120.0f);
    };

    const float playWidth = compact ? 40.0f : 84.0f;
    const float levelWidth = compact ? 84.0f : 104.0f;
    const float instrumentWidth = compact ? 130.0f : width >= 1000.0f ? 240.0f : 180.0f;
    const float actionsWidth = playWidth + 6.0f + levelWidth + 6.0f + instrumentWidth;
    const auto actions = FixedWidth(actionsWidth, Row({Middle(play), HGap(6), FixedWidth(levelWidth, Middle(level)),
                                                       HGap(6), FixedWidth(instrumentWidth, Middle(instrument))}));
    std::vector<Ptr> controls;

    if (!compact)
    {
        // Narrow, the actions take a row of their own under the tabs.
        if (width >= 820.0f)
        {
            controls.push_back(
                Row({Middle(mode), HGap(12), Stretch(Middle(FixedHeight(20, live))), HGap(12), actions}));
        }
        else
        {
            controls.push_back(Row({Middle(mode), HGap(12), Stretch(Middle(FixedHeight(20, live)))}));
            controls.push_back(VGap(8));
            controls.push_back(Row({Stretch(), actions}));
        }

        controls.push_back(VGap(10));
        controls.push_back(Row(what));
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
        std::vector<Ptr> row{FixedWidth(labelWidth("Style"), Middle(Label(mTheme, "Style"))), HGap(8), Middle(style)};
        float rowWidth = labelWidth("Style") + 8.0f + 3.0f * 84.0f;

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
            row.insert(row.end(), {HGap(16), FixedWidth(labelWidth("Strings"), Middle(Label(mTheme, "Strings"))),
                                   HGap(8), Middle(strings)});
            rowWidth += 16.0f + labelWidth("Strings") + 8.0f + 36.0f * static_cast<float>(names.size());
        }

        if (compact)
        {
            flow.push_back({FixedWidth(rowWidth, Row(row)), rowWidth});
        }
        else
        {
            row.push_back(Stretch());
            controls.push_back(VGap(10));
            controls.push_back(Row(row));
        }
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
        // LISTEN is always on, so the rounds it counts always show.
        auto round = std::make_shared<Text>(mTheme, "Round " + std::to_string(mScaleGuideRound), 11.0f);
        round->Colour(mTheme.TextMuted());
        std::vector<Ptr> row;

        if (!compact)
        {
            row.insert(row.end(), {FixedWidth(labelWidth("Steps"), Middle(Label(mTheme, "Steps"))), HGap(8)});
        }

        row.insert(
            row.end(),
            {// A long progression scrolls its steps only when they would not fit beside the buttons.
             Middle(static_cast<float>(steps.size()) * 44.0f > width - 300.0f ? HScroll(stepper) : Ptr(stepper)),
             HGap(8), Middle(button("\xE2\x97\x80", "Back one chord", [this] { ScaleGuideStep(mScaleGuideStep - 1); })),
             HGap(4),
             Middle(button("\xE2\x96\xB6", "Forward one chord", [this] { ScaleGuideStep(mScaleGuideStep + 1); })),
             HGap(4),
             Middle(button("\xE2\x86\xBA", "Restart from the top",
                           [this] {
                               mScaleGuideRound = 1;
                               ScaleGuideStep(0);
                           })),
             HGap(10), Middle(FixedWidth(70, round)), Stretch()});

        if (compact)
        {
            // A line of its own: a progression can be long.
            flow.push_back({Row(row), width});
        }
        else
        {
            controls.push_back(VGap(10));
            controls.push_back(Row(row));
        }
    }

    const auto positionLabel = chordMode ? "Shape" : "Position";
    float controlsHeight = 0.0f; // compact: the lines of controls together

    if (!compact)
    {
        controls.push_back(VGap(10));
        controls.push_back(Row(
            {FixedWidth(labelWidth(positionLabel), Middle(Label(mTheme, positionLabel))), HGap(8), Middle(positions),
             Stretch(), FixedWidth(labelWidth("Dots"), Middle(Label(mTheme, "Dots"))), HGap(8), Middle(labels)}));
    }
    else
    {
        // The mode tabs, what is showing, an arpeggio's style, a progression's steps, the position,
        // the dots, and Play with the instrument, line after line as the width allows: two lines
        // on a 740 px window where the full layout's rows took four. LISTEN's readout takes what
        // the first line leaves; Play and the instrument keep to the right of theirs.
        const float gap = 10.0f;
        flow.insert(flow.begin(), {Middle(mode), modeWidth * 3.0f});
        flow.push_back({Middle(positions), positionSegment * static_cast<float>(positionNames.size())});
        flow.push_back({Middle(labels), labelSegment * static_cast<float>(labelNames.size())});
        flow.push_back({actions, actionsWidth});
        std::vector<std::vector<Ptr>> lines(1);
        float used = 0.0f;

        for (std::size_t i = 0; i < flow.size(); ++i)
        {
            const auto& [item, itemWidth] = flow[i];
            const bool last = i + 1 == flow.size();

            if (!lines.back().empty() && used + gap + itemWidth > width)
            {
                lines.emplace_back();
                used = 0.0f;
            }

            auto& line = lines.back();

            if (last)
            {
                // The readout fills the first line; on another line the actions sit at its right.
                line.push_back(lines.size() == 1 ? HGap(gap) : Stretch());
                line.push_back(actions);
                continue;
            }

            if (!line.empty())
            {
                line.push_back(HGap(gap));
            }

            line.push_back(item);
            used += (line.size() > 1 ? gap : 0.0f) + itemWidth;
        }

        for (std::size_t i = 0; i < lines.size(); ++i)
        {
            if (i == 0)
            {
                // What LISTEN heard, in the room the first line leaves (before the actions, when
                // they share it).
                auto& first = lines.front();
                const auto at = lines.size() == 1 ? first.end() - 2 : first.end();
                first.insert(at, {HGap(gap), Stretch(Middle(FixedHeight(20, live)))});
            }
            else
            {
                controls.push_back(VGap(6));
            }

            controls.push_back(Row(lines[i]));
        }

        const float lineHeight = metrics::Touch() ? metrics::kTouchTarget : 34.0f;
        controlsHeight = static_cast<float>(lines.size()) * lineHeight + static_cast<float>(lines.size() - 1) * 6.0f;
    }

    // ── The neck ──
    auto neck = std::make_shared<ScaleNeck>(mTheme, model, settings.labels, settings.leftHanded);
    mScaleNeck = neck;
    neck->onNote = [this, volume](int midi) { mSession.Commands().PlayScaleGuideNote(midi, volume()); };
    // LISTEN follows notes in Scales and Arpeggios; a chord is matched, not followed.
    neck->SetHeard(chordMode ? std::vector<int>{} : ScaleGuideHeard(), settings.listenEverywhere);
    neck->SetPlaying(mScaleGuideMoment.places);

    // A short compact window: the strings close up (to 24 px from 30) so the whole neck shows under
    // the controls, rather than its low strings under the footer. What is left of the stage under
    // the section chips (and their rule), the page's top margin, the controls and the gap under them.
    if (compact && model.openMidi.size() > 1)
    {
        const float room =
            StageHeight() - (metrics::Touch() ? 50.0f : 46.0f) - 1.0f - 10.0f - controlsHeight - 10.0f - 4.0f;
        const float strings = static_cast<float>(model.openMidi.size());
        neck->StringGap(
            std::clamp((room - ScaleNeck::HeightFor(model.openMidi.size(), 0.0f)) / (strings - 1.0f), 24.0f, 30.0f));
    }

    mScaleRunTab.reset();
    mScaleProgressionTab.reset();

    // ── Chord boxes: the shapes, or a progression's strip ──
    std::vector<Ptr> boxes;

    if (progression.is_object() && chordMode)
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

        if (wholeRun)
        {
            // A progression's arpeggios: the whole of it, a bar for each chord, running on to new
            // lines at the page's width. A bar shows its chord on the neck and plays.
            const auto& all = wholeRun->run;
            auto name =
                std::make_shared<Text>(mTheme, all.name.empty() ? "Progression" : all.name, 15.0f, Weight::SemiBold);
            name->Colour(mTheme.Text());
            auto frets =
                std::make_shared<Text>(mTheme,
                                       all.steps.empty()            ? ""
                                       : all.fretFrom == all.fretTo ? "Fret " + std::to_string(all.fretFrom)
                                                                    : "Frets " + std::to_string(all.fretFrom) +
                                                                          "\xE2\x80\x93" + std::to_string(all.fretTo),
                                       13.0f);
            frets->Colour(mTheme.TextSecondary());
            auto slides = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
            slides->Size(84, 30).Label("\xE2\x97\x8F Slides", 11.0f).Lit(settings.arpeggioSlides);
            slides->AccessName("Slides: slide from each shape into the next");

            if (settings.arpeggioSlides)
            {
                slides->Ink(mTheme.Success());
            }

            slides->onClick = [this] {
                ChangeScaleGuide([](sg::Settings& s) { s.arpeggioSlides = !s.arpeggioSlides; });
            };
            const float tabWidth = std::max(240.0f, width - 32.0f);
            auto tab = std::make_shared<ProgressionTabView>(mTheme, *wholeRun, model.stringNames, tabWidth,
                                                            progression.value("step", 0));
            tab->SetPlaying(mScaleGuideMoment.steps);
            mScaleProgressionTab = tab;
            tab->onBar = [this, volume](int chord) {
                // The bar's chord on the neck, and that bar alone played, in place of what was sounding.
                ScaleGuideStep(chord);
                const auto request =
                    sg::ViewRequest(sg::LoadSettings(mState.appSettings), sg::LoadProgressions(mState.appSettings),
                                    mScaleGuideStep, {}, mScaleGuideAnchor);
                mSession.Commands().PlayScaleGuide(request, volume(), mScaleGuideStep);
            };
            tabCard = face(
                Pad(16, 10, 16, 10,
                    Col({FixedHeight(30, Row({Middle(FixedWidth(200, name)), HGap(10), Middle(FixedWidth(120, frets)),
                                              Stretch(), Middle(slides), HGap(8), Middle(playButton())})),
                         VGap(4), HScroll(tab), VGap(4), note(view["arpeggio"].value("advice", ""))})));
        }
        else if (run != nullptr)
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
            auto tab = std::make_shared<RunTabView>(mTheme, *run, model.stringNames);
            tab->SetPlaying(mScaleGuideMoment.steps);
            mScaleRunTab = tab;
            tabCard =
                face(Pad(16, 10, 16, 10,
                         Col({FixedHeight(30, Row({Middle(FixedWidth(180, name)), HGap(10),
                                                   Middle(FixedWidth(120, frets)), Stretch(), Middle(playButton())})),
                              VGap(4), HScroll(tab), VGap(4),
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
    std::vector<Ptr> page = {Col(controls), VGap(10),
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

void Shell::SyncScaleGuidePlayback()
{
    // As the web UI follows it (ts/scaleGuide/playback.ts): each note lit for as long as it sounds,
    // from the moment the engine said when they fall.
    sg::PlaybackMoment moment;

    if (ScaleGuidePlaying() && mPage == Page::Jam && mJamSection == JamSection::Scales)
    {
        moment = sg::PlaybackAt(mState.demo.scaleGuide, (UiSession::Now() - mState.demo.scaleGuideAt) * 1000.0);
    }

    if (moment == mScaleGuideMoment)
    {
        return;
    }

    const bool chordMoved = moment.chord != mScaleGuideMoment.chord;
    mScaleGuideMoment = std::move(moment);

    // A progression's arpeggios step the guide on as each chord comes: the page is rebuilt for it,
    // lit as this moment has it.
    if (chordMoved && mScaleGuideMoment.chord >= 0 && mScaleGuideMoment.chord != mScaleGuideStep)
    {
        ScaleGuideStep(mScaleGuideMoment.chord);
        return;
    }

    if (const auto neck = mScaleNeck.lock(); neck && neck->SetPlaying(mScaleGuideMoment.places))
    {
        mSession.Repaint(*neck);
    }

    if (const auto tab = mScaleRunTab.lock(); tab && tab->SetPlaying(mScaleGuideMoment.steps))
    {
        mSession.Repaint(*tab);
    }

    if (const auto tab = mScaleProgressionTab.lock(); tab && tab->SetPlaying(mScaleGuideMoment.steps))
    {
        mSession.Repaint(*tab);
    }
}

std::vector<int> Shell::ScaleGuideHeard() const
{
    std::vector<int> notes;

    // The guide's own Play is not marked as heard, unless the whole neck is asked for: it would
    // light every place each note is.
    if (ScaleGuidePlaying() && !sg::LoadSettings(mState.appSettings).listenEverywhere)
    {
        return notes;
    }

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
    // LISTEN has no switch: it is on whenever the guide shows, and off as soon as it does not.
    const bool wanted = mPage == Page::Jam && mJamSection == JamSection::Scales;
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
        mScaleGuideReadout.clear();
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

        if (const auto neck = mScaleNeck.lock(); neck && neck->SetHeard(ScaleGuideHeard(), settings.listenEverywhere))
        {
            mSession.Repaint(*neck);
        }

        const auto current = mScaleGuideFollower.Current();

        if (!current)
        {
            setReadout("");
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
