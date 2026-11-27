// The tuner: a dialog that stays open while the player plays, so what it shows is patched in
// place from the engine's feed rather than rebuilt. The metronome is in Metronome.cpp.

#include "ui/dialogs/DialogChrome.h"
#include "ui/input/AccessBridge.h"
#include "ui/Drawn.h"
#include "ui/performance/IntonationPanel.h"
#include "ui/shell/Shell.h"

#include "uiclient/Instruments.h"
#include "uiclient/Intonation.h"

#include <elements/support/text_utils.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

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

void DrawText(canvas& cnv, const std::string& text, const el::font_descr& font, el::color colour, el::point at,
              int align)
{
    auto state = cnv.new_state();
    cnv.font(font);
    cnv.fill_style(colour);
    cnv.text_align(align);
    cnv.fill_text(text, at);
}
} // namespace

// ── The tuner ───────────────────────────────────────────────────────────────────

namespace
{
// The flat and sharp marks' colour, and the in-tune tick's (tuner.ts).
const el::color kOffPitch = el::rgba(0xe0, 0x78, 0x48, 255);
const el::color kInTune = el::rgba(0x40, 0xc0, 0x80, 255);

// Hz or cents, kept while the app runs, as the web UI keeps its choice for the session.
bool gTunerShowsHz = false;

// The intonation check: whether it shows, and what it has measured, kept while the app runs as
// the web UI keeps them (tunerIntonationView.ts).
bool gTunerIntonation = false;

guitarfx::uiclient::intonation::IntonationTest& Intonation()
{
    static guitarfx::uiclient::intonation::IntonationTest test;
    return test;
}

// Set while the tuner closes only to open again (Intonation shown or hidden, which changes the
// dialog's height): the engine's tuner stays on, so a muted output does not blip through.
bool gTunerReopening = false;
} // namespace

/// The tuner's display (tuner-modal.html, css/modals/tuner.css): -50 and +50 either side of
/// the flat or sharp mark and the reading in cents or Hz; the puck sliding along its track
/// over the in-tune ring, green when in tune; and the note between its neighbours.
class TunerFace final : public el::element, public Drawn, public AccessStatic
{
  public:
    static constexpr float kHeight = 300.0f;
    /// Under the intonation panel, with the gaps closed up, so the panel's buttons stay in view.
    static constexpr float kCompactHeight = 236.0f;

    explicit TunerFace(const Theme& theme, bool compact = false) : mTheme(theme), mCompact(compact)
    {
    }

    [[nodiscard]] float Height() const
    {
        return mCompact ? kCompactHeight : kHeight;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{300.0f, Height()}, {el::full_extent, Height()}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const auto& r = mReading;

        // The cents row: -50, the mark, the reading, +50, spread across.
        const float rowY = b.top + (mCompact ? 24.0f : 30.0f + 12.0f);
        const float inner = b.width() - 60.0f;
        const auto at = [&](float fraction) { return b.left + 30.0f + inner * fraction; };
        const auto marker = Font(Weight::Regular).size(14.0f);
        DrawText(cnv, "-50", marker, mTheme.ModalTextMuted(), {at(0.0f), rowY}, canvas::left | canvas::middle);
        DrawText(cnv, "+50", marker, mTheme.ModalTextMuted(), {at(1.0f), rowY}, canvas::right | canvas::middle);

        if (r.detected)
        {
            const el::point mark{at(1.0f / 3.0f), rowY};

            if (r.inTune)
            {
                cnv.stroke_style(kInTune);
                cnv.line_width(2.5f);
                cnv.line_cap(canvas::round);
                cnv.line_join(canvas::round_join);
                cnv.begin_path();
                cnv.move_to(mark.x - 7.0f, mark.y);
                cnv.line_to(mark.x - 2.0f, mark.y + 5.0f);
                cnv.line_to(mark.x + 7.0f, mark.y - 5.0f);
                cnv.stroke();
            }
            else
            {
                DrawText(cnv, r.cents < 0.0 ? "<" : ">", Font(Weight::SemiBold).size(18.0f), kOffPitch, mark,
                         canvas::center | canvas::middle);
            }
        }

        std::string reading = "-";

        if (r.detected)
        {
            reading = gTunerShowsHz ? Fixed(r.frequency, 1) + " Hz" : (r.cents >= 0.0 ? "+" : "") + Fixed(r.cents, 1);
        }

        DrawText(cnv, reading, Font(Weight::Regular).size(18.0f), mTheme.ModalTextSecondary(), {at(2.0f / 3.0f), rowY},
                 canvas::center | canvas::middle);

        // The track: a line, the in-tune ring at its middle, and the puck at the reading.
        const float trackY = mCompact ? rowY + 62.0f : b.top + 30.0f + 24.0f + 20.0f + 40.0f;
        const float left = b.left + 60.0f, right = b.right - 60.0f;
        cnv.fill_style(mTheme.ModalRule());
        cnv.fill_rect(el::rect{left, trackY - 1.0f, right, trackY + 1.0f});
        constexpr float kPuck = 28.5f;
        cnv.stroke_style(mTheme.ModalBorder());
        cnv.line_width(3.0f);
        cnv.add_circle(cycfi::artist::circle({(left + right) * 0.5f, trackY}, kPuck));
        cnv.stroke();

        // The puck slides to the reading (.tuner-puck's left 0.15 s ease-out) rather than jumping:
        // it closes most of the way in that time and is drawn again until it is there.
        const float target = static_cast<float>(r.detected ? r.needle : 0.5);
        const double now = UiSession::Now();
        const float dt = static_cast<float>(std::clamp(now - mShownAt, 0.0, 0.1));
        mShownAt = now;
        mShown += (target - mShown) * (1.0f - std::exp(-dt / 0.05f));

        if (std::abs(target - mShown) > 0.001f)
        {
            RepaintSoon(b);
        }
        else
        {
            mShown = target;
        }

        const float x = left + (right - left) * mShown;
        const el::color fill = !r.detected ? mTheme.ModalRule().opacity(0.5f)
                               : r.inTune  ? mTheme.Success()
                                           : mTheme.Accent();
        cnv.fill_style(fill);
        cnv.add_circle(cycfi::artist::circle({x, trackY}, kPuck));
        cnv.fill();
        cnv.stroke_style(mTheme.ModalRaised());
        cnv.add_circle(cycfi::artist::circle({x, trackY}, kPuck));
        cnv.stroke();

        // The note, between the semitones below and above; with a string set for the intonation
        // check, the fret between its neighbours, under a FRET caption.
        const float noteY = trackY + (mCompact ? 92.0f : 40.0f + 30.0f + 40.0f);
        const auto side = Font(Weight::Regular).size(24.0f);
        DrawText(cnv, r.detected ? r.noteLeft : std::string("-"), side, mTheme.ModalTextMuted(),
                 {b.left + 30.0f, noteY + 10.0f}, canvas::left | canvas::middle);
        DrawText(cnv, r.detected ? r.noteRight : std::string("-"), side, mTheme.ModalTextMuted(),
                 {b.right - 30.0f, noteY + 10.0f}, canvas::right | canvas::middle);

        if (mFretMode && r.detected)
        {
            DrawText(cnv, "FRET", Font(Weight::SemiBold).size(11.0f), mTheme.ModalTextMuted(),
                     {(b.left + b.right) * 0.5f, noteY - 42.0f}, canvas::center | canvas::middle);
        }

        DrawText(cnv, r.detected ? r.note : std::string("-"), Font(Weight::Regular).size(72.0f),
                 r.detected ? mTheme.ModalText() : mTheme.ModalTextMuted(), {(b.left + b.right) * 0.5f, noteY},
                 canvas::center | canvas::middle);
    }

    /// The reading, and the fret it is nearest while the intonation check has a string set
    /// (tuner.ts's intonationReadout): the fret takes the note's place and its cents the needle.
    void Set(const guitarfx::uiclient::TunerModel::Reading& reading,
             const std::optional<guitarfx::uiclient::intonation::FretReading>& fret = std::nullopt)
    {
        mReading = reading;
        mFretMode = fret.has_value();

        if (fret && reading.detected)
        {
            using guitarfx::uiclient::TunerModel;
            mReading.note = std::to_string(fret->fret);
            mReading.noteLeft = fret->fret > 0 ? std::to_string(fret->fret - 1) : std::string();
            mReading.noteRight =
                fret->fret < guitarfx::uiclient::intonation::kMaxFret ? std::to_string(fret->fret + 1) : std::string();
            mReading.cents = fret->cents;
            mReading.inTune = std::abs(fret->cents) < TunerModel::kInTuneCents;
            mReading.needle = std::clamp(0.5 + (fret->cents / 50.0) * 0.45, 0.05, 0.95);
        }

        SayChange();
    }

    /// The reading as words: the note (or fret) and how far off it is.
    [[nodiscard]] std::string Spoken() const
    {
        const auto& r = mReading;

        if (!r.detected)
        {
            return "No note";
        }

        std::string note = mFretMode ? "Fret " + r.note : r.note;

        // "C#" read as "C sharp", not "C number".
        if (const auto hash = note.find('#'); hash != std::string::npos)
        {
            note.replace(hash, 1, " sharp");
        }

        if (r.inTune)
        {
            return note + ", in tune";
        }

        const int cents = static_cast<int>(std::lround(std::abs(r.cents)));
        return note + ", " + std::to_string(cents) + (cents == 1 ? " cent " : " cents ") +
               (r.cents < 0.0 ? "flat" : "sharp");
    }

    [[nodiscard]] std::optional<AccessInfo> DescribeStatic() const override
    {
        AccessInfo info;
        info.role = AccessRole::Label;
        info.name = "Tuner: " + Spoken();
        return info;
    }

  private:
    /// Tells a screen reader when the note, or which side of in tune it is, changes: at most
    /// once a second, and only once a reading has held for a quarter of one, so a string
    /// settling is not read out at every step.
    void SayChange()
    {
        const auto& r = mReading;
        const std::string state = !r.detected ? std::string() : r.note + (r.inTune ? "=" : r.cents < 0.0 ? "<" : ">");
        const double now = UiSession::Now();

        if (state != mHeldState)
        {
            mHeldState = state;
            mHeldSince = now;
            return;
        }

        if (state.empty() || state == mSaidState || now - mHeldSince < 0.25 || now - mSaidAt < 1.0)
        {
            return;
        }

        mSaidState = state;
        mSaidAt = now;
        AccessBridge::Announce(Spoken());
    }

    std::string mHeldState, mSaidState;
    double mHeldSince = 0.0, mSaidAt = -10.0;
    float mShown = 0.5f; // where the puck is drawn, sliding to the reading
    double mShownAt = 0.0;
    const Theme& mTheme;
    bool mCompact = false;
    guitarfx::uiclient::TunerModel::Reading mReading;
    bool mFretMode = false;
};

void Shell::OpenTuner()
{
    auto& commands = mSession.Commands();
    auto face = std::make_shared<TunerFace>(mTheme, gTunerIntonation);
    auto model = std::make_shared<guitarfx::uiclient::TunerModel>();
    const auto setReference = [&commands](double hz) {
        commands.SetTunerReference(std::clamp(std::round(hz * 10.0) / 10.0, 400.0, 480.0));
    };

    // The header's mute: the output is silent while tuning unless the tuner is live.
    auto mute = std::make_shared<IconButton>(mTheme, mIcons, "ui-speaker", IconButton::Look::Ghost);
    mute->Size(28, 28).IconSize(18);
    mute->onClick = [this, &commands] { commands.SetTunerLiveMode(!mState.tuner.liveMode); };

    // ── The controls: the reading's unit, the live switch and the reference pitch ──
    auto units = std::make_shared<Segmented>(mTheme, std::vector<std::string>{"Cents", "Hz"});
    units->Select(gTunerShowsHz ? 1 : 0);
    units->AccessName("Reading");
    auto live = std::make_shared<ToggleSwitch>(mTheme);
    live->AccessName("Live Tuner");
    live->onClick = [this, &commands] { commands.SetTunerLiveMode(!mState.tuner.liveMode); };
    auto liveLabel = std::make_shared<Text>(mTheme, "Live Tuner", 13.0f);
    liveLabel->Colour(mTheme.ModalTextSecondary()).Fixed(true);
    auto reference = std::make_shared<Stepper>(mTheme, 92.0f);
    reference->onStep = [this, setReference](int direction, bool) {
        setReference(mState.tuner.referenceFrequency + 0.1 * direction);
    };
    reference->AccessName("Reference pitch, Hz");
    // A pitch typed in where it shows.
    reference->editText = [this] { return Fixed(mState.tuner.referenceFrequency, 1); };
    reference->onTyped = [setReference](const std::string& text) {
        char* end = nullptr;
        const double hz = std::strtod(text.c_str(), &end);

        if (end != text.c_str() && hz >= 400.0 && hz <= 480.0)
        {
            setReference(hz);
        }
    };

    // ── The intonation check: its switch in the header, and its panel and buttons under the face ──
    auto& intonation = Intonation();
    intonation.ResetReadings();
    // On, it takes the accent, as the web UI's .tuner-intonation-btn.active.
    auto intonationBtn = std::make_shared<Button>(mTheme, "Intonation", nullptr,
                                                  gTunerIntonation ? Button::Style::Primary : Button::Style::Plain);
    intonationBtn->OnClick([this] {
        gTunerIntonation = !gTunerIntonation;
        // The dialog changes height: it opens again, after this click is done with its button.
        mSession.PostToUi([this] {
            gTunerReopening = true;
            OpenTuner();
        });
    });
    auto panel = std::make_shared<IntonationPanel>(mTheme, IntonationPanel::Part::Status);
    auto strings = std::make_shared<IntonationPanel>(mTheme, IntonationPanel::Part::Strings);
    auto setOpen = std::make_shared<Button>(mTheme, "Set open string", nullptr, Button::Style::Primary);
    auto newString = std::make_shared<Button>(mTheme, "New string");
    auto clear = std::make_shared<Button>(mTheme, "Clear results");
    // The last reading handed to the check: the feed may notify more than once per reading.
    auto lastReading = std::make_shared<std::uint64_t>(mState.tuner.readings);

    auto sync = [this, face, model, mute, live, reference, units, panel, strings, setOpen, newString, clear,
                 lastReading] {
        const auto& t = mState.tuner;
        auto& check = Intonation();
        check.SetReferenceFrequency(t.referenceFrequency);

        if (t.readings != *lastReading)
        {
            *lastReading = t.readings;
            check.Push({t.detected, t.frequency, t.levelDb});
        }

        const auto reading = model->Push(t.detected, t.noteName, t.octave, t.centOffset, t.frequency);
        face->Set(reading, gTunerIntonation && reading.detected ? check.Readout(reading.frequency) : std::nullopt);

        if (gTunerIntonation)
        {
            const auto view = check.GetView();
            setOpen->Enabled(!view.hasString && view.candidate.has_value());
            newString->Enabled(view.hasString);
            clear->Enabled(!view.strings.empty());
            panel->Set(view);
            strings->Set(view);

            for (el::element* e : std::initializer_list<el::element*>{panel.get(), strings.get(), setOpen.get(),
                                                                      newString.get(), clear.get()})
            {
                mSession.Repaint(*e);
            }
        }

        // As the web UI's .tuner-mute-btn: muted, it takes the accent.
        mute->Ink(t.liveMode ? mTheme.ModalTextMuted() : mTheme.Accent());
        mute->Tooltip(t.liveMode ? "Mute output while tuning" : "Unmute output");
        mute->AccessName(t.liveMode ? "Mute output while tuning" : "Unmute output");
        live->Set(t.liveMode);
        reference->Set(Fixed(t.referenceFrequency, 1));
        units->Select(gTunerShowsHz ? 1 : 0);

        for (el::element* e :
             std::initializer_list<el::element*>{face.get(), mute.get(), live.get(), reference.get(), units.get()})
        {
            mSession.Repaint(*e);
        }
    };
    units->onSelect = [sync](int index) {
        gTunerShowsHz = index == 1;
        sync();
    };
    setOpen->OnClick([sync] {
        Intonation().SetOpenString();
        sync();
    });
    newString->OnClick([sync] {
        Intonation().NewString();
        sync();
    });
    clear->OnClick([sync] {
        Intonation().Clear();
        sync();
    });

    constexpr float kButtonsHeight = 48.0f;
    Ptr body = face;

    if (gTunerIntonation)
    {
        // The web UI's order: the step, the measurement and advice, the buttons, then the strings.
        body = Col({face, panel,
                    FixedHeight(kButtonsHeight, Pad(20, 0, 20, 8,
                                                    Middle(Row({Middle(setOpen), HGap(8), Middle(newString), HGap(8),
                                                                Middle(clear), Stretch()})))),
                    strings});
    }

    ShowDialog({.title = "Tuner",
                .body = body,
                .footerLeft = Row(
                    {Middle(units), Stretch(), Middle(live), HGap(8), Middle(liveLabel), Stretch(), Middle(reference)}),
                .headerExtras = {intonationBtn, mute},
                .width = 560.0f,
                .height = kDialogHeaderHeight + face->Height() + kDialogFooterHeight + 14.0f +
                          (gTunerIntonation ? panel->Height() + kButtonsHeight + strings->Height() : 0.0f),
                .padX = 0.0f,
                .padY = 0.0f});

    // The tuner listens only while it is open.
    mInstrumentSubs.push_back(mSession.Client().Subscribe(Topic::Tuner, sync));
    // ShowDialog closed the dialog it replaces first, so a reopening is over by here.
    gTunerReopening = false;
    mOnOverlayClosed = [&commands] {
        if (!gTunerReopening)
        {
            commands.SetTunerActive(false);
        }
    };
    commands.SetTunerActive(true);
    sync();
}
} // namespace nanoq::ui
