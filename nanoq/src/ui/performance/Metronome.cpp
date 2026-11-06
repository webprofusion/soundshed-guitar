// The metronome, drawn as the web UI's metronome card (metronome-modal.html, css/modals/metronome.css):
// a header band with the tempo's source and a close cross; volume and pan knobs either side of
// the meter and sound pickers; the tempo in large figures with its arrows, TAP, the rhythm picker
// and the round transport button; the beat strip, which is both the accent editor and the beat
// display; and the running state with a hint under it. It stays open while the player plays, so
// it is patched in place from the engine's feeds rather than rebuilt.

#include "ui/performance/MetronomeWidgets.h"
#include "ui/shell/Shell.h"

#include "uiclient/Instruments.h"
#include "uiclient/Metronome.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace nanoq::ui
{
using guitarfx::uiclient::Topic;
using namespace metronome;

namespace
{
constexpr double kMinVolumeDb = -60.0;
constexpr double kMaxVolumeDb = 12.0; // the engine's range (MetronomeSupport.h)
constexpr double kDefaultVolumeDb = -12.0;
constexpr auto kPulse = std::chrono::milliseconds(120); // how long a beat stays lit, as the web UI's

std::string Fixed(double value, int decimals)
{
    char text[64];
    std::snprintf(text, sizeof(text), "%.*f", decimals, value);
    return text;
}

/// "C", "40 L", "25 R": the pan as the web UI's knob writes it.
std::string PanText(double pan)
{
    if (std::abs(pan) < 0.01)
    {
        return "C";
    }

    return std::to_string(std::lround(std::abs(pan) * 100.0)) + (pan < 0.0 ? " L" : " R");
}

char NextLevel(char level)
{
    // accent -> medium -> normal -> silent, as a click on a beat steps it.
    switch (level)
    {
    case 'H':
        return 'M';
    case 'M':
        return 'L';
    case 'L':
        return 'S';
    default:
        return 'H';
    }
}

int TicksPerBeat(const guitarfx::uiclient::MetronomeState& m)
{
    for (const auto& [id, ticks] : m.subdivisions)
    {
        if (id == m.subdivision)
        {
            return std::clamp(ticks, 1, 8);
        }
    }

    return 1;
}

/// A small muted capital label over a control (.metronome-label).
std::shared_ptr<Text> Label(const Theme& theme, const std::string& text)
{
    auto label = std::make_shared<Text>(theme, Upper(text), 10.0f, Weight::SemiBold);
    label->Colour(theme.TextMuted());
    return label;
}
} // namespace

void Shell::OpenMetronome()
{
    using guitarfx::uiclient::ClampBpm;
    auto& commands = mSession.Commands();
    // The web UI's compact layout puts the four controls of the top row in two rows of two, and
    // closes the gaps up.
    const bool compact = !mWide;
    const float gap = compact ? 10.0f : 18.0f;

    auto setBpm = [&commands](double bpm) { commands.SetMetronomeBpm(ClampBpm(bpm)); };

    // ── The header: the tempo's source, beside the shared title and close cross ──
    auto source = std::make_shared<Text>(mTheme, "STANDALONE", 11.0f, Weight::Medium);
    source->Colour(mTheme.ModalTextMuted()).Fixed(true);

    // ── Volume, time signature, sound and pan ──
    auto volume = std::make_shared<ParamKnob>(mTheme, "Volume");
    volume->AccessName("Click volume");
    volume->DefaultPosition((kDefaultVolumeDb - kMinVolumeDb) / (kMaxVolumeDb - kMinVolumeDb));
    volume->onChange = [&commands, volume](double v) {
        const double db = std::round((kMinVolumeDb + v * (kMaxVolumeDb - kMinVolumeDb)) * 10.0) / 10.0;
        volume->SetValueText(Fixed(db, 1) + " dB");
        commands.SetMetronomeVolume(db);
    };
    volume->onReset = [&commands] { commands.SetMetronomeVolume(kDefaultVolumeDb); };

    auto pan = std::make_shared<ParamKnob>(mTheme, "Pan");
    pan->DefaultPosition(0.5);
    pan->onChange = [&commands, pan](double v) {
        const double value = std::round((v * 2.0 - 1.0) * 100.0) / 100.0;
        pan->SetValueText(PanText(value));
        commands.SetMetronomePan(value);
    };
    pan->onReset = [&commands] { commands.SetMetronomePan(0.0); };

    // The meter: simple meters in one column, compound ones and the odd meters' groupings in the
    // other, as the web UI's.
    auto meter = std::make_shared<PickerField>(mTheme, mIcons);
    meter->AccessName("Time signature");
    meter->onClick = [this, meter] {
        std::vector<std::vector<MenuItem>> columns(2);

        for (const auto& option : guitarfx::uiclient::TimeSignatures())
        {
            const auto& s = mState.metronome;
            columns[option.compound ? 1 : 0].push_back(
                {option.label,
                 s.timeSigNum == option.num && s.timeSigDen == option.den && s.grouping == option.grouping, false,
                 [this, option] {
                     mSession.Commands().SetMetronomeTimeSignature(option.num, option.den, option.grouping);
                 }});
        }

        OpenDropdownColumns(meter->Bounds(), std::move(columns), std::max(120.0f, meter->Bounds().width() * 0.6f));
    };

    auto sound = std::make_shared<PickerField>(mTheme, mIcons);
    sound->AccessName("Sound");
    sound->onClick = [this, sound] {
        std::vector<MenuItem> items;

        for (const auto& [id, label] : mState.metronome.clickTypes)
        {
            items.push_back({label, id == mState.metronome.clickType, false,
                             [this, id = id] { mSession.Commands().SetMetronomeClickType(id); }});
        }

        OpenDropdown(sound->Bounds(), std::move(items), std::max(160.0f, sound->Bounds().width()));
    };

    // ── The tempo, TAP, the rhythm and the transport ──
    auto readout = std::make_shared<TempoReadout>(mTheme);
    readout->onWheel = [this, setBpm](int steps) { setBpm(mState.metronome.bpm + steps); };
    // A tempo typed in where it shows, as the web UI's number field takes one.
    readout->editText = [this] { return Fixed(mState.metronome.bpm, 1); };
    readout->onTyped = [setBpm](const std::string& text) {
        try
        {
            setBpm(std::stod(text));
        }
        catch (...)
        {
            // Not a number: the tempo stays as it was.
        }
    };

    auto up = std::make_shared<TempoArrow>(mTheme, true);
    auto down = std::make_shared<TempoArrow>(mTheme, false);
    up->onStep = [this, setBpm](bool fine) { setBpm(mState.metronome.bpm + (fine ? 0.1 : 1.0)); };
    down->onStep = [this, setBpm](bool fine) { setBpm(mState.metronome.bpm - (fine ? 0.1 : 1.0)); };

    auto tapTempo = std::make_shared<guitarfx::uiclient::TapTempo>();
    auto tap = std::make_shared<RoundButton>(mTheme, mIcons, 62.0f, "TAP");
    tap->onClick = [setBpm, tapTempo] {
        if (const auto bpm = tapTempo->Tap(UiSession::Now()))
        {
            setBpm(*bpm);
        }
    };

    auto rhythm = std::make_shared<PickerField>(mTheme, mIcons);
    rhythm->AccessName("Rhythm");
    rhythm->onClick = [this, rhythm] {
        std::vector<MenuItem> items;

        for (const auto& [id, ticks] : mState.metronome.subdivisions)
        {
            items.push_back({id, id == mState.metronome.subdivision, false,
                             [this, id = id] { mSession.Commands().SetMetronomeSubdivision(id); }});
        }

        OpenDropdown(rhythm->Bounds(), std::move(items), std::max(130.0f, rhythm->Bounds().width()));
    };

    auto play = std::make_shared<RoundButton>(mTheme, mIcons, 64.0f, "");
    play->onClick = [this, &commands] { commands.SetMetronomeEnabled(!mState.metronome.enabled); };

    // ── The beats, and the running state ──
    auto beats = std::make_shared<BeatStrip>(mTheme);
    beats->onBeat = [this](std::size_t beat) {
        auto next = mState.metronome.beatPattern;

        if (beat < next.size())
        {
            next[beat] = NextLevel(next[beat]);
            mSession.Commands().SetMetronomeBeatPattern(next);
        }
    };

    auto status = std::make_shared<Text>(mTheme, "Stopped", metrics::kSmallText);
    status->Colour(mTheme.TextSecondary());
    auto hint = std::make_shared<Text>(mTheme,
                                       "Click a beat to cycle accent \xE2\x86\x92 medium \xE2\x86\x92 normal "
                                       "\xE2\x86\x92 off",
                                       11.0f);
    hint->Colour(mTheme.TextMuted()).Fixed(true);

    // ── Layout ──
    const auto view = ViewSize();
    const float width = std::min(760.0f, std::max(360.0f, view.x - 48.0f));
    const float inner = width - 44.0f;

    auto picker = [this](const std::string& name, Ptr field) {
        return Middle(Col({FixedHeight(14, Middle(Label(mTheme, name))), VGap(8), FixedHeight(32, std::move(field))}));
    };

    constexpr float kTopRow = 66.0f;
    Ptr top;

    if (compact)
    {
        const float column = (inner - gap) * 0.5f;
        top = Col({FixedHeight(kTopRow, Row({FixedWidth(column, Centred(volume)), HGap(gap),
                                             FixedWidth(column, picker("Time Signature", meter))})),
                   VGap(gap),
                   FixedHeight(kTopRow, Row({FixedWidth(column, picker("Sound", sound)), HGap(gap),
                                             FixedWidth(column, Centred(pan))}))});
    }
    else
    {
        const float column = (inner - 3.0f * gap) / 4.0f;
        top = FixedHeight(
            kTopRow,
            Row({FixedWidth(column, Centred(volume)), HGap(gap), FixedWidth(column, picker("Time Signature", meter)),
                 HGap(gap), FixedWidth(column, picker("Sound", sound)), HGap(gap), FixedWidth(column, Centred(pan))}));
    }

    auto divider = FixedHeight(1, Fill(mTheme.BorderStrong().opacity(0.6f), std::make_shared<el::element>()));

    auto arrows = Middle(FixedHeight(54, Col({FixedHeight(24, up), VGap(6), FixedHeight(24, down)})));
    auto tempo = FixedWidth(190, Middle(Col({FixedHeight(13, Middle(Label(mTheme, "BPM"))), VGap(12),
                                             FixedHeight(58, Row({FixedWidth(28, arrows), HGap(12), readout}))})));
    constexpr float kBottomRow = 84.0f;
    auto bottom = FixedHeight(kBottomRow, Row({tempo, HGap(20), Middle(tap), HGap(20),
                                               FixedWidth(130, picker("Rhythm", rhythm)), Stretch(), Middle(play)}));

    std::vector<Ptr> footer{Stretch(status)};

    if (!compact)
    {
        footer.push_back(hint);
    }

    const float topHeight = compact ? kTopRow * 2.0f + gap : kTopRow;
    const float bodyHeight = topHeight + gap + 1.0f + gap + kBottomRow + gap + 46.0f + gap + 16.0f;
    auto body = Col({top, VGap(gap), divider, VGap(gap), bottom, VGap(gap), FixedHeight(46, beats), VGap(gap),
                     FixedHeight(16, Middle(Row(footer)))});

    // ── Keeping it current ──
    auto sync = [this, source, volume, pan, meter, sound, readout, up, down, tap, rhythm, play, beats, status] {
        const auto& s = mState.metronome;
        const bool editable = s.editable;
        source->Set(s.source == "host" ? "HOST TEMPO" : "STANDALONE");
        volume->Update((s.volumeDb - kMinVolumeDb) / (kMaxVolumeDb - kMinVolumeDb), Fixed(s.volumeDb, 1) + " dB");
        pan->Update((s.pan + 1.0) / 2.0, PanText(s.pan));
        meter->Set(guitarfx::uiclient::DescribeTimeSignature(s.timeSigNum, s.timeSigDen, s.grouping));
        meter->Enabled(editable);
        std::string soundLabel = s.clickType;

        for (const auto& [id, label] : s.clickTypes)
        {
            if (id == s.clickType)
            {
                soundLabel = label;
            }
        }

        sound->Set(soundLabel);
        sound->Enabled(editable && !s.clickTypes.empty());
        readout->Set(Fixed(ClampBpm(s.bpm), 1), editable);
        rhythm->Set(s.subdivision.empty() ? "1/4" : s.subdivision);
        rhythm->Enabled(editable);
        play->Icon(s.enabled ? "ui-stop" : "ui-play");
        play->Active(s.enabled);
        status->Set(s.enabled ? "Running" : "Stopped");

        for (auto* b : std::initializer_list<Pressable*>{up.get(), down.get(), tap.get(), play.get()})
        {
            b->Enabled(editable);
        }

        beats->Set(s.beatPattern, guitarfx::uiclient::GroupHeads(s.timeSigNum, s.timeSigDen, s.grouping),
                   TicksPerBeat(s), editable);

        if (!s.enabled)
        {
            beats->Light(-1);
        }

        for (const Ptr& e : std::initializer_list<Ptr>{source, volume, pan, meter, sound, readout, up, down, tap,
                                                       rhythm, play, beats, status})
        {
            mSession.Repaint(*e);
        }
    };

    // The beat just played is lit for a moment; a later beat, or the next, takes over.
    auto pulse = [this, beats, counter = std::make_shared<unsigned>(mState.metronome.beatCounter)] {
        const auto& s = mState.metronome;

        if (s.beatCounter == *counter || !s.enabled)
        {
            return;
        }

        *counter = s.beatCounter;
        const unsigned lit = s.beatCounter;
        beats->Light(s.beatIndex);
        mSession.Repaint(*beats);
        mSession.View().post(
            kPulse, [this, alive = std::weak_ptr<bool>(mAlive), strip = std::weak_ptr<BeatStrip>(beats), counter, lit] {
                const auto shown = strip.lock();

                if (alive.lock() && shown && *counter == lit)
                {
                    shown->Light(-1);
                    mSession.Repaint(*shown);
                }
            });
    };

    ShowDialog({.title = "Metronome",
                .body = body,
                .headerExtras = {source},
                .width = width,
                .height = kDialogHeaderHeight + 40.0f + bodyHeight,
                .padX = 22.0f,
                .padY = 20.0f});

    mInstrumentSubs.push_back(mSession.Client().Subscribe(Topic::Metronome, sync));
    mInstrumentSubs.push_back(mSession.Client().Subscribe(Topic::MetronomeBeat, pulse));
    sync();
}
} // namespace nanoq::ui
