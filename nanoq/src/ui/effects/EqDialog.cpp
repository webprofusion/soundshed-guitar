// The EQ dialog the web UI uses for the Global EQ and the Practice Tool's backing-track EQ
// (eq-modal.html, practice-tool-eq-modal.html, css/modals/eq.css): the shared dialog frame
// with an Enabled switch in its header, the EQ Curve card with the handles over the spectrum,
// and the Parametric EQ card with each band's Gain, Freq and Q (and Shelf or Bell for the
// outer two) in a card of its own. One dialog, two sources of parameters.

#include "ui/dialogs/DialogChrome.h"
#include "ui/shell/Shell.h"

#include "dsp/EffectGuids.h"
#include "uiclient/ParamFormat.h"
#include "uiclient/PracticeTool.h"

#include <algorithm>
#include <cmath>

namespace nanoq::ui
{
using guitarfx::uiclient::Topic;

namespace
{
constexpr float kBandKnob = 60.0f;    // a knob's cell in a band card, where the dialog has the room
constexpr float kMinBandKnob = 50.0f; // and the least it can be before the bands go two to a row
constexpr float kBandCardSlack = 2.0f;
constexpr float kBandGap = 18.0f;
constexpr float kBandRowGap = 12.0f;
constexpr float kSectionPad = 16.0f;
constexpr float kCurveHeight = 150.0f;                       // the plot, where the window is tall enough
constexpr float kMinCurveHeight = 96.0f;                     // and the least it gives up to before the dialog scrolls
constexpr float kCurveChrome = 10.0f + 16.0f + 8.0f + 12.0f; // the curve card round its plot
constexpr float kBandHeight = 10.0f + 24.0f + 8.0f + 63.0f + 8.0f; // a band card: padding, title, knobs
constexpr float kDialogMaxWidth = 900.0f;
constexpr float kDialogPadX = 20.0f;
} // namespace

void Shell::OpenEqDialog(EqDialogSpec spec)
{
    // Whatever overlay is up goes first, so its close cannot take this dialog's plot with it.
    CloseOverlay();
    const auto* type = mState.FindEffectType(spec.source.effectType);
    auto syncs = std::make_shared<std::vector<std::function<void()>>>();
    auto read = spec.source.read;
    auto write = spec.source.write;
    auto plot = MakeEqPlot(std::move(spec.source), true);

    const auto resync = [this] {
        if (mOverlayPlot)
        {
            SyncEqPlot(*mOverlayPlot);
        }
    };

    // ── The bands, each in its card (.eq-band) ──
    // Four across where the dialog is wide enough, their knobs drawn in a little where it nearly
    // is (an 800 px window), and two rows of two below that. At their full width the four cards
    // were wider than a small window's dialog, and nothing fixes a dialog's width below what it
    // holds: the dialog took the whole window, and the window grew to hold it (800 x 480 went to
    // 854 and stayed there).
    const float dialogWidth = std::min(kDialogMaxWidth, std::max(280.0f, ViewSize().x - 32.0f));
    const float bandRoom = dialogWidth - 2.0f * kDialogPadX - 2.0f * kSectionPad;
    const auto knobFor = [bandRoom](int columns) {
        const float card = (bandRoom - static_cast<float>(columns - 1) * kBandGap) / static_cast<float>(columns);
        return std::min(kBandKnob, std::floor((card - kBandCardSlack) / 3.0f));
    };
    const int columns = knobFor(4) >= kMinBandKnob ? 4 : 2;
    const float bandKnob = knobFor(columns);
    const float bandCard = 3.0f * bandKnob + kBandCardSlack;

    std::vector<Ptr> bandCards;
    const std::pair<const char*, const char*> bands[] = {
        {"low", "Low"}, {"lowMid", "Low Mid"}, {"highMid", "High Mid"}, {"high", "High"}};

    for (const auto& [prefix, label] : bands)
    {
        std::vector<Ptr> knobs{Stretch()};

        for (const char* suffix : {"Gain", "Freq", "Q"})
        {
            const std::string key = std::string(prefix) + suffix;
            const auto it =
                type != nullptr
                    ? std::find_if(type->parameters.begin(), type->parameters.end(),
                                   [&key](const guitarfx::uiclient::EffectParamInfo& p) { return p.key == key; })
                    : decltype(type->parameters.begin()){};

            if (type == nullptr || it == type->parameters.end())
            {
                continue;
            }

            const auto info = *it;
            auto knob = std::make_shared<ParamKnob>(mTheme, suffix);
            knob->DefaultPosition(guitarfx::uiclient::ParamValueToPosition(info, info.defaultValue));
            knob->onChange = [info, key, write, resync, k = knob.get()](double position) {
                const double v =
                    guitarfx::uiclient::SnapParamValue(info, guitarfx::uiclient::ParamPositionToValue(info, position));
                k->SetValueText(guitarfx::uiclient::FormatParamValue(info, v));
                write(key, v, true);
                resync();
            };
            knob->onReset = [info, key, write, resync] {
                write(key, info.defaultValue, true);
                resync();
            };
            syncs->push_back([this, knob, info, key, read] {
                const auto params = read();
                const auto found = params.find(key);
                const double v = found != params.end() ? found->second : info.defaultValue;
                knob->Update(guitarfx::uiclient::ParamValueToPosition(info, v),
                             guitarfx::uiclient::FormatParamValue(info, v));
                mSession.Repaint(*knob);
            });
            knob->CellWidth(bandKnob);
            knobs.push_back(FixedWidth(bandKnob, Centred(knob)));
        }

        knobs.push_back(Stretch());

        // The outer bands are shelves unless set to bells (lowShape/highShape: 0 shelf, 1 bell).
        auto name = std::make_shared<Text>(mTheme, Upper(label), 10.0f, Weight::SemiBold);
        name->Colour(mTheme.ModalText()).Fixed(true);
        Ptr heading;
        const std::string band = prefix;

        if (band == "low" || band == "high")
        {
            const std::string shapeKey = band + "Shape";
            auto shape = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Chip);
            shape->Size(60, 22).Caret(true);
            shape->onClick = [this, shapeKey, read, write, resync, s = shape.get()] {
                const auto params = read();
                const auto found = params.find(shapeKey);
                const bool bell = found != params.end() && found->second >= 0.5;
                OpenDropdown(s->Bounds(),
                             {{"Shelf", !bell, false,
                               [shapeKey, write, resync] {
                                   write(shapeKey, 0.0, true);
                                   resync();
                               }},
                              {"Bell", bell, false,
                               [shapeKey, write, resync] {
                                   write(shapeKey, 1.0, true);
                                   resync();
                               }}},
                             120.0f);
            };
            syncs->push_back([this, shape, shapeKey, read] {
                const auto params = read();
                const auto found = params.find(shapeKey);
                shape->Label(found != params.end() && found->second >= 0.5 ? "Bell" : "Shelf", 11.0f);
                mSession.Repaint(*shape);
            });
            heading = Row({Stretch(), Middle(name), HGap(8), Middle(shape), Stretch()});
        }
        else
        {
            heading = Row({Stretch(), Middle(name), Stretch()});
        }

        auto card = Over(Pad(0, 10, 0, 8, Col({FixedHeight(24, heading), VGap(8), FixedHeight(63, Row(knobs))})),
                         el::share(Surface(mTheme.ModalBg(), mTheme.ModalRule(), 8.0f)));
        bandCards.push_back(FixedWidth(bandCard, card));
    }

    std::vector<Ptr> bandRows;

    for (std::size_t first = 0; first < bandCards.size(); first += static_cast<std::size_t>(columns))
    {
        std::vector<Ptr> row{Stretch()};

        for (std::size_t i = first; i < std::min(bandCards.size(), first + static_cast<std::size_t>(columns)); ++i)
        {
            if (i > first)
            {
                row.push_back(HGap(kBandGap));
            }

            row.push_back(bandCards[i]);
        }

        row.push_back(Stretch());

        if (!bandRows.empty())
        {
            bandRows.push_back(VGap(kBandRowGap));
        }

        bandRows.push_back(FixedHeight(kBandHeight, Row(row)));
    }

    const float rowCount = static_cast<float>((bandCards.size() + static_cast<std::size_t>(columns) - 1) /
                                              static_cast<std::size_t>(columns));
    const float bandsHeight = rowCount * kBandHeight + (rowCount - 1.0f) * kBandRowGap;
    const float sectionCard = kSectionPad + 24.0f + 16.0f + bandsHeight + 18.0f;

    // The plot gives up height to a short window before the dialog has to scroll: at 800 x 480
    // the bands were cut 50 px short of their card's foot.
    const float aroundPlot = kDialogHeaderHeight + 2.0f * 18.0f + kCurveChrome + 16.0f + sectionCard;
    const float curveHeight =
        std::clamp(std::max(200.0f, ViewSize().y - 40.0f) - aroundPlot, kMinCurveHeight, kCurveHeight);
    const float curveCard = kCurveChrome + curveHeight;

    // ── The EQ Curve card (.eq-visualizer) ──
    const auto small = [this](const std::string& text, HAlign align) {
        auto t = std::make_shared<Text>(mTheme, Upper(text), 9.5f, Weight::SemiBold);
        t->Colour(mTheme.ModalTextSecondary()).Alignment(align);
        return t;
    };
    auto curve = Over(Pad(12, 10, 12, 12,
                          Col({FixedHeight(16, Row({small("EQ Curve", HAlign::Left), small("\xC2\xB1"
                                                                                           "12 dB",
                                                                                           HAlign::Right)})),
                               VGap(8), FixedHeight(curveHeight, plot)})),
                      el::share(Surface(mTheme.ModalBg(), mTheme.ModalRule(), 8.0f)));

    // ── The Parametric EQ card (.eq-section) ──
    auto sectionTitle = std::make_shared<Text>(mTheme, "PARAMETRIC EQ", 14.0f, Weight::SemiBold);
    sectionTitle->Colour(mTheme.ModalText()).Alignment(HAlign::Centre);
    auto section = Over(Pad(kSectionPad, kSectionPad, kSectionPad, 18,
                            Col({FixedHeight(24, sectionTitle), VGap(16), FixedHeight(bandsHeight, Col(bandRows))})),
                        el::share(Surface(mTheme.ModalRaised(), mTheme.ModalRule(), 10.0f)));

    // ── The header: Enabled, and Reset where the dialog has one ──
    auto enabled = std::make_shared<ToggleSwitch>(mTheme, spec.enabled());
    enabled->AccessName("Enabled");
    enabled->onClick = [enabled, getter = spec.enabled, setter = spec.setEnabled, this] {
        setter(!getter());
        enabled->Set(getter());
        mSession.Repaint(*enabled);
    };
    auto enabledLabel = std::make_shared<Text>(mTheme, "ENABLED", 11.0f, Weight::SemiBold);
    enabledLabel->Colour(mTheme.ModalTextSecondary()).Fixed(true);
    std::vector<Ptr> header{Tight({Middle(enabled), HGap(8), Middle(enabledLabel)})};

    if (spec.reset)
    {
        auto reset = std::make_shared<Button>(
            mTheme, "Reset",
            [reset = spec.reset, syncs, resync] {
                reset();

                for (auto& fn : *syncs)
                {
                    fn();
                }

                resync();
            },
            Button::Style::Plain);
        reset->TextSize(11.0f).MinWidth(64);
        header.push_back(reset);
    }

    ShowDialog({.title = spec.title,
                .body = Col({FixedHeight(curveCard, curve), VGap(16), FixedHeight(sectionCard, section)}),
                .headerExtras = header,
                .width = dialogWidth,
                .height = kDialogHeaderHeight + 2.0f * 18.0f + curveCard + 16.0f + sectionCard,
                .padX = kDialogPadX});

    // The dialog's plot goes with it, and the spectrum watch back to the effect editor's.
    mOnOverlayClosed = [this, closed = std::move(spec.onClosed)] {
        mOverlayPlot.reset();

        if (closed)
        {
            closed();
        }
    };

    auto sync = [this, syncs, enabled, getter = spec.enabled] {
        for (auto& fn : *syncs)
        {
            fn();
        }

        enabled->Set(getter());
        mSession.Repaint(*enabled);

        if (mOverlayPlot)
        {
            SyncEqPlot(*mOverlayPlot);
        }
    };

    if (spec.topic)
    {
        mInstrumentSubs.push_back(mSession.Client().Subscribe(*spec.topic, sync));
    }

    sync();
}

void Shell::OpenGlobalEq()
{
    // The Global EQ: the global chain's post EQ, over the output's spectrum. The engine does not
    // echo a global chain edit, so the knobs follow the mirrored state.
    auto& commands = mSession.Commands();
    const auto params = [this]() -> std::map<std::string, double> {
        if (const auto* node = mState.globalChain.postChainGraph.FindNode("global_eq"))
        {
            return node->params;
        }

        return {};
    };
    const auto enabled = [this] {
        const auto* node = mState.globalChain.postChainGraph.FindNode("global_eq");
        return node != nullptr && node->enabled;
    };

    EqDialogSpec spec;
    spec.title = "Global EQ";
    spec.source.effectType = guitarfx::EffectGuids::kEqParametric;
    spec.source.address = {"post", "global_eq", {}};
    spec.source.feedKeys = {"post::global_eq"};
    spec.source.read = params;
    spec.source.write = [&commands](const std::string& key, double value, bool) {
        commands.SetGlobalChainParam("eq." + key, value);
    };
    spec.enabled = enabled;
    spec.setEnabled = [&commands](bool on) { commands.SetGlobalChainToggle("eq.enabled", on); };
    spec.topic = Topic::GlobalChain;
    OpenEqDialog(std::move(spec));
}

void Shell::OpenPracticeEq()
{
    // The backing-track EQ: the same ParametricEQEffect as the signal path's, on the track only.
    EqDialogSpec spec;
    spec.title = "Backing Track EQ";
    spec.source.effectType = guitarfx::EffectGuids::kEqParametric;
    spec.source.read = [this]() { return mPractice->GetEq().params; };
    spec.source.write = [this](const std::string& key, double value, bool) { mPractice->SetEqParam(key, value); };
    spec.enabled = [this] { return mPractice->GetEq().enabled; };
    spec.setEnabled = [this](bool on) { mPractice->SetEqEnabled(on); };
    spec.reset = [this] { mPractice->ResetEq(); };
    spec.onClosed = [this] { Mark(kPage); }; // the EQ button's light
    spec.topic = Topic::PracticeTool;
    OpenEqDialog(std::move(spec));
}
} // namespace nanoq::ui
