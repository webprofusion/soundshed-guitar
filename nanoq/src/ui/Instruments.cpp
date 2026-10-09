// The tuner and the metronome: two dialogs that stay open while the player plays, so what they
// show is patched in place from the engine's feeds rather than rebuilt.

#include "ui/Shell.h"

#include "uiclient/Instruments.h"

#include <elements/support/text_utils.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace nanoq::ui
{
using guitarfx::uiclient::Topic;

namespace
{
using cycfi::artist::canvas;

std::string Fixed(double value, int decimals)
{
    char text[64];
    std::snprintf(text, sizeof(text), "%.*f", decimals, value);
    return text;
}

void DrawText(canvas& cnv, const std::string& text, const el::font_descr& font, el::color colour, el::point at, int align)
{
    auto state = cnv.new_state();
    cnv.font(font);
    cnv.fill_style(colour);
    cnv.text_align(align);
    cnv.fill_text(text, at);
}
} // namespace

// ── The tuner ───────────────────────────────────────────────────────────────────

/// The note, its neighbours, a needle over a ±50 cent scale, and the reading in numbers.
class TunerFace final : public el::element
{
public:
    explicit TunerFace(const Theme& theme) : mTheme(theme) {}

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{300.0f, 220.0f}, {el::full_extent, el::full_extent}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        auto area = ctx.bounds;
        const bool inTune = mReading.detected && mReading.inTune;
        const el::color noteColour =
            !mReading.detected ? mTheme.TextMuted() : (inTune ? el::rgba(0x40, 0xc0, 0x80, 255) : mTheme.Accent());

        // The note, with its neighbours either side.
        const float noteHeight = area.height() * 0.5f;
        const float noteSize = std::clamp(noteHeight * 0.7f, 34.0f, 110.0f);
        const el::point middle{area.left + area.width() * 0.5f, area.top + noteHeight * 0.5f};
        DrawText(cnv, mReading.detected ? mReading.note : std::string("-"), Font(Weight::SemiBold).size(noteSize), noteColour,
                 middle, canvas::center | canvas::middle);
        if (mReading.detected)
        {
            const auto side = Font(Weight::Regular).size(noteSize * 0.35f);
            DrawText(cnv, mReading.noteLeft, side, mTheme.TextMuted(), {area.left + area.width() * 0.15f, middle.y},
                     canvas::center | canvas::middle);
            DrawText(cnv, mReading.noteRight, side, mTheme.TextMuted(), {area.left + area.width() * 0.85f, middle.y},
                     canvas::center | canvas::middle);
        }

        // The scale: ±50 cents across, the centre marked.
        const float scaleTop = area.top + noteHeight;
        const float scaleHeight = std::min(60.0f, area.height() * 0.28f);
        const auto scale = el::rect{area.left + area.width() * 0.06f, scaleTop + 8.0f, area.right - area.width() * 0.06f,
                                    scaleTop + 8.0f + scaleHeight};
        const float cy = scale.top + scale.height() * 0.5f;

        cnv.fill_style(mTheme.Border());
        cnv.fill_round_rect(el::rect{scale.left, cy - 3.0f, scale.right, cy + 3.0f}, 3.0f);

        for (int cents = -50; cents <= 50; cents += 10)
        {
            const float x = scale.left + scale.width() * (0.05f + 0.9f * static_cast<float>(cents + 50) / 100.0f);
            const float tick = cents == 0 ? scale.height() : scale.height() * 0.4f;
            cnv.fill_style(cents == 0 ? mTheme.Text() : mTheme.TextMuted());
            cnv.fill_rect(el::rect{x - 1.0f, cy - tick * 0.5f, x + 1.0f, cy + tick * 0.5f});
        }

        if (mReading.detected)
        {
            const float x = scale.left + scale.width() * static_cast<float>(mReading.needle);
            cnv.fill_style(noteColour);
            cnv.fill_round_rect(el::rect{x - 4.0f, scale.top, x + 4.0f, scale.bottom}, 4.0f);
        }

        std::string detail = "Play a string";
        if (mReading.detected)
            detail = std::string(mReading.cents >= 0 ? "+" : "") + Fixed(mReading.cents, 1) + " cents    " +
                     Fixed(mReading.frequency, 1) + " Hz";
        DrawText(cnv, detail, Font(Weight::Medium).size(metrics::kTextSize), mTheme.TextSecondary(),
                 {area.left + area.width() * 0.5f, scale.bottom + 22.0f}, canvas::center | canvas::middle);
    }

    void Set(const guitarfx::uiclient::TunerModel::Reading& reading)
    {
        mReading = reading;
    }

private:
    const Theme& mTheme;
    guitarfx::uiclient::TunerModel::Reading mReading;
};

void Shell::OpenTuner()
{
    auto& commands = mSession.Commands();
    auto face = std::make_shared<TunerFace>(mTheme);
    auto model = std::make_shared<guitarfx::uiclient::TunerModel>();

    auto mute = std::make_shared<Button>(mTheme, "Mute while tuning", [this, &commands] {
        commands.SetTunerLiveMode(!mState.tuner.liveMode); // live mode on means the sound plays
    }, Button::Style::Plain);
    mute->Dot(true).MinWidth(190);

    auto reference = std::make_shared<Text>(mTheme, "A = 440 Hz", metrics::kTextSize, Weight::Medium);
    reference->Alignment(Text::Align::Centre).Colour(mTheme.TextSecondary());

    auto down = std::make_shared<Button>(mTheme, "\xE2\x88\x92", [this, &commands] {
        commands.SetTunerReference(std::clamp(std::round(mState.tuner.referenceFrequency) - 1.0, 400.0, 480.0));
    }, Button::Style::Plain);
    auto up = std::make_shared<Button>(mTheme, "+", [this, &commands] {
        commands.SetTunerReference(std::clamp(std::round(mState.tuner.referenceFrequency) + 1.0, 400.0, 480.0));
    }, Button::Style::Plain);
    down->MinWidth(44);
    up->MinWidth(44);

    auto sync = [this, face, model, mute, reference] {
        const auto& t = mState.tuner;
        face->Set(model->Push(t.detected, t.noteName, t.octave, t.centOffset, t.frequency));
        mute->On(!t.liveMode);
        reference->Set("A = " + Fixed(t.referenceFrequency, 0) + " Hz");
        mSession.View().refresh(*face);
        mSession.View().refresh(*mute);
        mSession.View().refresh(*reference);
    };

    auto heading = std::make_shared<Text>(mTheme, "Tuner", 15.0f, Weight::SemiBold);
    auto close = std::make_shared<Button>(mTheme, "Done", [this] { CloseOverlay(); }, Button::Style::Primary);
    close->MinWidth(88);

    const auto view = mSession.View().size();
    ShowOverlay(Pad(16, 14, 16, 14,
                    Col({Row({FixedHeight(30, heading), Stretch(nullptr, 0.001), close}), face, VGap(8),
                         FixedHeight(44, Row({mute, Stretch(), down, FixedWidth(120, reference), up}))})),
                std::min(560.0f, std::max(300.0f, view.x - 48.0f)), std::min(420.0f, std::max(300.0f, view.y - 80.0f)));

    // The tuner listens only while it is open.
    mInstrumentSubs.push_back(mSession.Client().Subscribe(Topic::Tuner, sync));
    mOnOverlayClosed = [&commands] { commands.SetTunerActive(false); };
    commands.SetTunerActive(true);
    sync();
}

// ── The metronome ───────────────────────────────────────────────────────────────

namespace
{
char NextLevel(char level)
{
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

constexpr double kMinBpm = 30.0;
constexpr double kMaxBpm = 300.0;
constexpr double kMinVolumeDb = -60.0;
constexpr double kMaxVolumeDb = 12.0; // the engine's range (MetronomeSupport.h)
} // namespace

void Shell::OpenMetronome()
{
    using guitarfx::uiclient::ClampBpm;
    auto& commands = mSession.Commands();
    const auto& m = mState.metronome;

    auto bpmText = std::make_shared<Text>(mTheme, "120", 44.0f, Weight::SemiBold);
    bpmText->Alignment(Text::Align::Centre);
    auto bpmLabel = std::make_shared<Text>(mTheme, "BPM", 11.0f, Weight::SemiBold);
    bpmLabel->Colour(mTheme.TextMuted()).Alignment(Text::Align::Centre);

    auto setBpm = [&commands](double bpm) { commands.SetMetronomeBpm(ClampBpm(bpm)); };

    auto slower = std::make_shared<Button>(mTheme, "\xE2\x80\xB9", [this, setBpm] { setBpm(mState.metronome.bpm - 1.0); },
                                           Button::Style::Plain);
    auto faster = std::make_shared<Button>(mTheme, "\xE2\x80\xBA", [this, setBpm] { setBpm(mState.metronome.bpm + 1.0); },
                                           Button::Style::Plain);
    slower->MinWidth(52).TextSize(22);
    faster->MinWidth(52).TextSize(22);

    auto bpmSlider = std::make_shared<ParamSlider>(mTheme, "Tempo");
    bpmSlider->onChange = [setBpm](double v) { setBpm(kMinBpm + v * (kMaxBpm - kMinBpm)); };

    auto tapTempo = std::make_shared<guitarfx::uiclient::TapTempo>();
    auto tap = std::make_shared<Button>(mTheme, "TAP", [setBpm, tapTempo] {
        if (const auto bpm = tapTempo->Tap(UiSession::Now()))
            setBpm(*bpm);
    }, Button::Style::Plain);
    auto startStop = std::make_shared<Button>(mTheme, "Start", [this, &commands] {
        commands.SetMetronomeEnabled(!mState.metronome.enabled);
    }, Button::Style::Primary);

    auto meter = std::make_shared<Button>(mTheme, "4/4", [this] {
        const std::pair<int, int> meters[] = {{2, 4}, {3, 4}, {4, 4}, {5, 4}, {6, 4}, {7, 4},
                                              {3, 8}, {5, 8}, {6, 8}, {7, 8}, {9, 8}, {12, 8}};
        std::vector<MenuItem> items;
        for (const auto& [num, den] : meters)
            items.push_back({std::to_string(num) + "/" + std::to_string(den),
                             mState.metronome.timeSigNum == num && mState.metronome.timeSigDen == den, false,
                             [this, num = num, den = den] {
                                 mSession.Commands().SetMetronomeTimeSignature(num, den);
                                 OpenMetronome();
                             }});
        OpenMenu("Time signature", std::move(items));
    }, Button::Style::Plain);
    auto subdivision = std::make_shared<Button>(mTheme, "Subdivision", [this] {
        std::vector<MenuItem> items;
        for (const auto& [id, ticks] : mState.metronome.subdivisions)
            items.push_back({id, id == mState.metronome.subdivision, false, [this, id = id] {
                                 mSession.Commands().SetMetronomeSubdivision(id);
                                 OpenMetronome();
                             }});
        OpenMenu("Subdivision", std::move(items));
    }, Button::Style::Plain);
    auto click = std::make_shared<Button>(mTheme, "Click", [this] {
        std::vector<MenuItem> items;
        for (const auto& [id, label] : mState.metronome.clickTypes)
            items.push_back({label, id == mState.metronome.clickType, false, [this, id = id] {
                                 mSession.Commands().SetMetronomeClickType(id);
                                 OpenMetronome();
                             }});
        OpenMenu("Click sound", std::move(items));
    }, Button::Style::Plain);

    auto volume = std::make_shared<ParamSlider>(mTheme, "Volume");
    volume->onChange = [&commands](double v) {
        commands.SetMetronomeVolume(std::round((kMinVolumeDb + v * (kMaxVolumeDb - kMinVolumeDb)) * 2.0) / 2.0);
    };

    // One button per beat: a tap steps its accent through accent, medium, normal and silent.
    auto beats = std::make_shared<std::vector<std::shared_ptr<Button>>>();
    std::vector<Ptr> beatRow;
    for (std::size_t i = 0; i < m.beatPattern.size(); ++i)
    {
        auto beat = std::make_shared<Button>(mTheme, std::to_string(i + 1), [this, i] {
            auto next = mState.metronome.beatPattern;
            if (i < next.size())
            {
                next[i] = NextLevel(next[i]);
                mSession.Commands().SetMetronomeBeatPattern(next);
            }
        }, Button::Style::Plain);
        beat->MinWidth(48);
        beats->push_back(beat);
        beatRow.push_back(beat);
        beatRow.push_back(HGap(4));
    }
    beatRow.push_back(Stretch());

    auto sync = [this, bpmText, bpmSlider, volume, startStop, meter, subdivision, click, slower, faster, tap, beats] {
        const auto& s = mState.metronome;
        char bpm[32];
        std::snprintf(bpm, sizeof(bpm), s.bpm == std::round(s.bpm) ? "%.0f" : "%.1f", s.bpm);
        bpmText->Set(bpm);
        bpmSlider->Update((s.bpm - kMinBpm) / (kMaxBpm - kMinBpm), std::string(bpm) + " BPM");
        volume->Update((s.volumeDb - kMinVolumeDb) / (kMaxVolumeDb - kMinVolumeDb), Fixed(s.volumeDb, 1) + " dB");
        startStop->Set(s.enabled ? "Stop" : "Start");
        meter->Set(std::to_string(s.timeSigNum) + "/" + std::to_string(s.timeSigDen));
        subdivision->Set(s.subdivision.empty() ? "Subdivision" : s.subdivision);
        std::string clickLabel = "Click";
        for (const auto& [id, label] : s.clickTypes)
            if (id == s.clickType)
                clickLabel = label;
        click->Set(clickLabel);
        for (auto* b : {slower.get(), faster.get(), tap.get()})
            b->Enabled(s.editable);

        for (std::size_t i = 0; i < beats->size() && i < s.beatPattern.size(); ++i)
        {
            const char level = s.beatPattern[i];
            (*beats)[i]->Set(std::to_string(i + 1) + (level == 'H' ? ">" : level == 'S' ? "-" : ""));
            (*beats)[i]->Selected(static_cast<int>(i) == s.beatIndex);
        }

        auto& view = mSession.View();
        for (const Ptr& e : std::initializer_list<Ptr>{bpmText, bpmSlider, volume, startStop, meter, subdivision, click})
            view.refresh(*e);
        for (auto& b : *beats)
            view.refresh(*b);
    };

    auto heading = std::make_shared<Text>(mTheme, "Metronome", 15.0f, Weight::SemiBold);
    auto close = std::make_shared<Button>(mTheme, "Done", [this] { CloseOverlay(); }, Button::Style::Plain);
    close->MinWidth(88);

    auto page = Col({Row({FixedHeight(30, heading), Stretch(nullptr, 0.001), close}),
                     FixedHeight(76, Row({slower, Stretch(Col({Stretch(bpmText), FixedHeight(16, bpmLabel)})), faster})),
                     bpmSlider, VGap(6), FixedHeight(44, Row({tap, HGap(8), startStop})), VGap(8),
                     FixedHeight(44, Row(beatRow)), VGap(6), FixedHeight(44, Row({meter, HGap(8), subdivision, HGap(8), click, Stretch()})),
                     VGap(6), volume});

    const auto view = mSession.View().size();
    ShowOverlay(Pad(16, 14, 16, 14, page), std::min(560.0f, std::max(320.0f, view.x - 48.0f)),
                std::min(500.0f, std::max(320.0f, view.y - 60.0f)));

    mInstrumentSubs.push_back(mSession.Client().Subscribe(Topic::Metronome, sync));
    mInstrumentSubs.push_back(mSession.Client().Subscribe(Topic::MetronomeBeat, sync));
    sync();
}
} // namespace nanoq::ui
