// The tuner: a dialog that stays open while the player plays, so what it shows is patched in
// place from the engine's feed rather than rebuilt. The metronome is in Metronome.cpp.

#include "ui/dialogs/DialogChrome.h"
#include "ui/Drawn.h"
#include "ui/shell/Shell.h"

#include "uiclient/Instruments.h"

#include <elements/support/text_utils.hpp>

#include <algorithm>
#include <cmath>
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
} // namespace

/// The tuner's display (tuner-modal.html, css/modals/tuner.css): -50 and +50 either side of
/// the flat or sharp mark and the reading in cents or Hz; the puck sliding along its track
/// over the in-tune ring, green when in tune; and the note between its neighbours.
class TunerFace final : public el::element, public Drawn
{
  public:
    static constexpr float kHeight = 300.0f;

    explicit TunerFace(const Theme& theme) : mTheme(theme)
    {
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{300.0f, kHeight}, {el::full_extent, kHeight}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const auto& r = mReading;

        // The cents row: -50, the mark, the reading, +50, spread across.
        const float rowY = b.top + 30.0f + 12.0f;
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
        const float trackY = b.top + 30.0f + 24.0f + 20.0f + 40.0f;
        const float left = b.left + 60.0f, right = b.right - 60.0f;
        cnv.fill_style(mTheme.ModalRule());
        cnv.fill_rect(el::rect{left, trackY - 1.0f, right, trackY + 1.0f});
        constexpr float kPuck = 28.5f;
        cnv.stroke_style(mTheme.ModalBorder());
        cnv.line_width(3.0f);
        cnv.add_circle(cycfi::artist::circle({(left + right) * 0.5f, trackY}, kPuck));
        cnv.stroke();

        const float x = left + (right - left) * static_cast<float>(r.detected ? r.needle : 0.5);
        const el::color fill = !r.detected ? mTheme.ModalRule().opacity(0.5f)
                               : r.inTune  ? mTheme.Success()
                                           : mTheme.Accent();
        cnv.fill_style(fill);
        cnv.add_circle(cycfi::artist::circle({x, trackY}, kPuck));
        cnv.fill();
        cnv.stroke_style(mTheme.ModalRaised());
        cnv.add_circle(cycfi::artist::circle({x, trackY}, kPuck));
        cnv.stroke();

        // The note, between the semitones below and above.
        const float noteY = trackY + 40.0f + 30.0f + 40.0f;
        const auto side = Font(Weight::Regular).size(24.0f);
        DrawText(cnv, r.detected ? r.noteLeft : std::string("-"), side, mTheme.ModalTextMuted(),
                 {b.left + 30.0f, noteY + 10.0f}, canvas::left | canvas::middle);
        DrawText(cnv, r.detected ? r.noteRight : std::string("-"), side, mTheme.ModalTextMuted(),
                 {b.right - 30.0f, noteY + 10.0f}, canvas::right | canvas::middle);
        DrawText(cnv, r.detected ? r.note : std::string("-"), Font(Weight::Regular).size(72.0f),
                 r.detected ? mTheme.ModalText() : mTheme.ModalTextMuted(), {(b.left + b.right) * 0.5f, noteY},
                 canvas::center | canvas::middle);
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

    auto sync = [this, face, model, mute, live, reference, units] {
        const auto& t = mState.tuner;
        face->Set(model->Push(t.detected, t.noteName, t.octave, t.centOffset, t.frequency));
        // As the web UI's .tuner-mute-btn: muted, it takes the accent.
        mute->Ink(t.liveMode ? mTheme.ModalTextMuted() : mTheme.Accent());
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

    ShowDialog({.title = "Tuner",
                .body = face,
                .footerLeft = Row(
                    {Middle(units), Stretch(), Middle(live), HGap(8), Middle(liveLabel), Stretch(), Middle(reference)}),
                .headerExtras = {mute},
                .width = 560.0f,
                .height = kDialogHeaderHeight + TunerFace::kHeight + kDialogFooterHeight + 14.0f,
                .padX = 0.0f,
                .padY = 0.0f});

    // The tuner listens only while it is open.
    mInstrumentSubs.push_back(mSession.Client().Subscribe(Topic::Tuner, sync));
    mOnOverlayClosed = [&commands] { commands.SetTunerActive(false); };
    commands.SetTunerActive(true);
    sync();
}
} // namespace nanoq::ui
