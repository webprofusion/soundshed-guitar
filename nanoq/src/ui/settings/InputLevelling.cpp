// InputLevelling.cpp - input levelling (the web UI's settings/inputCalibration.ts): the flyout
// under the input meter, with the profiles (choose one, or none; delete one) and Train, and the
// training dialog, which shows the raw input's live and captured peaks and the trim saving
// would apply, and saves a profile under a name.
//
// The engine owns all of it (PluginControllerInputLevelling.cpp): the profiles, the peak hold on
// the raw input and the trim maths. This only draws what it reports and sends commands.

#include "ui/dialogs/DialogChrome.h"
#include "ui/shell/Shell.h"

#include "uiclient/InputLevelling.h"

#include <algorithm>
#include <memory>

namespace nanoq::ui
{
using guitarfx::uiclient::Topic;

namespace
{
/// The flyout's rows: a name over its note.
constexpr float kLevellingRowHeight = 46.0f;

/// `text` cut to at most `bytes`, on a character's edge (the web's maxlength).
std::string Capped(std::string text, std::size_t bytes)
{
    if (text.size() <= bytes)
    {
        return text;
    }

    std::size_t end = bytes;

    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
    {
        --end;
    }

    return text.substr(0, end);
}

/// A training metric (.input-calibration-metric-card): its label in capitals, the figure, and
/// what it is; the captured peak's edged in the accent.
class MetricFace final : public el::element
{
  public:
    MetricFace(const Theme& theme, bool accent) : mTheme(theme), mAccent(accent)
    {
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        cnv.fill_style(mTheme.ModalRaised());
        cnv.fill_round_rect(b, 12.0f);
        cnv.stroke_style(mAccent ? mTheme.Accent().opacity(0.48f) : mTheme.ModalBorder());
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(b.inset(0.5f, 0.5f), 12.0f);

        if (mAccent)
        {
            cnv.stroke_style(mTheme.Accent().opacity(0.18f));
            cnv.stroke_round_rect(b.inset(1.5f, 1.5f), 11.0f);
        }
    }

  private:
    const Theme& mTheme;
    bool mAccent;
};

/// The training row's box (.input-calibration-training-row): raised, tinted a little toward the accent.
class TrainingFace final : public el::element
{
  public:
    explicit TrainingFace(const Theme& theme) : mTheme(theme)
    {
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        cnv.fill_style(mTheme.ModalRaised());
        cnv.fill_round_rect(b, 12.0f);
        cnv.fill_style(mTheme.Accent().opacity(0.12f));
        cnv.fill_round_rect(b, 12.0f);
        cnv.stroke_style(mTheme.ModalBorder());
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(b.inset(0.5f, 0.5f), 12.0f);
    }

  private:
    const Theme& mTheme;
};

std::string Trimmed(std::string text)
{
    const auto first = text.find_first_not_of(" \t\r\n");

    if (first == std::string::npos)
    {
        return {};
    }

    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}
} // namespace

Ptr Shell::BuildInputLevellingButton()
{
    // The web UI's chevron under the input meter; lit while a profile is applied.
    // 18 x 16, edged in the accent while levelling is applied (.input-vu-meter-dropdown-trigger).
    auto button = std::make_shared<IconButton>(mTheme, mIcons, "ui-chevron-down", IconButton::Look::Pill);
    button->Size(18, 16).IconSize(9).TouchTarget(36).AccessName("Input levelling and Pickup Match");
    button->onClick = [this, b = button.get()] { OpenInputLevelling(b->Bounds()); };
    mLive.Bind(
        button,
        [this] {
            const auto& levelling = mState.inputLevelling;
            return !levelling.trainingActive && levelling.Active() != nullptr;
        },
        &IconButton::Lit);

    if (!mState.inputLevelling.known)
    {
        mSession.Commands().GetInputLevelling();
    }

    return button;
}

void Shell::OpenInputLevelling(el::rect anchor)
{
    auto body = MakeSlot();
    auto rebuild = std::make_shared<std::function<void()>>();
    *rebuild = [this, body] {
        const auto& levelling = mState.inputLevelling;
        const auto* active = levelling.trainingActive ? nullptr : levelling.Active();

        auto title = std::make_shared<Text>(mTheme, "INPUT LEVELLING", 9.0f, Weight::SemiBold);
        title->Colour(mTheme.DarkOr(el::rgba(220, 227, 243, 173), mTheme.TextSecondary()));
        auto subtitle = std::make_shared<Text>(mTheme,
                                               levelling.trainingActive ? "Training bypass active"
                                               : active != nullptr      ? active->name + " active"
                                                                        : "No active levelling",
                                               10.0f);
        subtitle->Colour(mTheme.TextMuted()).Alignment(Text::Align::Right);

        std::vector<Ptr> rows{FixedHeight(16, Row({title, Stretch(nullptr, 0.001), subtitle})), VGap(6)};
        // Each row is its name over a note, and what it applies at the right (.input-calibration-
        // toolbar-item).
        auto none = std::make_shared<ListRow>(mTheme, "No levelling", active == nullptr ? "ACTIVE" : "OFF",
                                              active == nullptr, false, [this] {
                                                  mSession.Commands().SelectInputLevellingProfile("");
                                                  CloseOverlay();
                                              });
        none->Note("Use raw input without a global trim");
        rows.push_back(FixedHeight(kLevellingRowHeight, none));

        for (const auto& profile : levelling.profiles)
        {
            const bool isActive = active != nullptr && active->id == profile.id;
            // The name, and what it applies (active) or its description or captured peak.
            auto row = std::make_shared<ListRow>(
                mTheme, profile.name, isActive ? "ACTIVE" : guitarfx::uiclient::FormatInputTrimDb(profile.gainDb),
                isActive, false, [this, id = profile.id] {
                    mSession.Commands().SelectInputLevellingProfile(id);
                    CloseOverlay();
                });
            row->Note(guitarfx::uiclient::InputLevellingProfileNote(profile, isActive));
            auto remove = std::make_shared<IconButton>(mTheme, mIcons, "ui-trash");
            remove->Size(36, 36).IconSize(13).Ink(mTheme.TextMuted()).AccessName("Delete " + profile.name);
            remove->onClick = [this, id = profile.id, name = profile.name] {
                OpenConfirm("Delete Input Levelling Profile", "Delete input levelling profile \"" + name + "\"?",
                            "Delete", [this, id, name] {
                                mSession.Commands().DeleteInputLevellingProfile(id);
                                ShowToast("Input levelling profile deleted", name, false);
                            });
            };
            rows.push_back(
                FixedHeight(kLevellingRowHeight,
                            Row({row, HGap(4), FixedWidth(36, Col({Stretch(), FixedHeight(36, remove), Stretch()}))})));
        }

        rows.push_back(VGap(6));
        auto train = std::make_shared<ListRow>(mTheme, "Train Input Level", "TRAIN", false, false,
                                               [this] { OpenInputLevellingTraining(); });
        train->Icon(&mIcons, "ui-io", mTheme.Accent());
        train->Note("Open the training dialog and save a new profile").TextInk(mTheme.Accent());
        rows.push_back(FixedHeight(kLevellingRowHeight, train));
        rows.push_back(std::make_shared<el::element>());
        Fill(*body, Col(rows));
        mSession.View().layout();
        mSession.View().refresh();
    };
    (*rebuild)();

    const auto& levelling = mState.inputLevelling;
    const float rowCount = static_cast<float>(levelling.profiles.size() + 2);
    const float height = std::min(16.0f + 6.0f + rowCount * kLevellingRowHeight + 6.0f + 20.0f, ViewSize().y - 40.0f);
    ShowPopover(Pad(12, 10, 12, 10, VScroll(body)), anchor, std::min(430.0f, ViewSize().x - 24.0f), height);

    // After ShowPopover, which ends what an earlier panel listened to. A change from anywhere
    // (another editor, a sync) redraws the list; drawn on the next turn, never under a click.
    mInstrumentSubs.push_back(mSession.Client().Subscribe(Topic::InputLevelling, [this, rebuild] {
        mSession.View().post([rebuild, alive = mAlive] {
            if (*alive)
            {
                (*rebuild)();
            }
        });
    }));
}

void Shell::OpenInputLevellingTraining()
{
    // Whatever overlay is up goes first, so its close cannot end this dialog's training.
    CloseOverlay();
    auto& commands = mSession.Commands();
    commands.StartInputLevellingTraining();
    const int saveSerial = mState.inputLevelling.saveSerial;

    const float width = std::min(680.0f, ViewSize().x - 24.0f);
    const float inner = width - 40.0f;
    // One card each, side by side; a narrow window stacks them.
    const bool stackMetrics = width < 560.0f;
    const float cardWidth = stackMetrics ? inner : (inner - 24.0f) / 3.0f;
    const auto metric = [this, cardWidth](const char* name, const char* note, bool accent,
                                          std::shared_ptr<Text>& value) -> Ptr {
        auto l = std::make_shared<Text>(mTheme, Upper(name), 11.0f);
        l->Colour(mTheme.ModalTextMuted());
        value = std::make_shared<Text>(mTheme, "-", 28.0f, Weight::SemiBold);
        value->Colour(mTheme.ModalText());
        auto n = std::make_shared<Paragraph>(mTheme, note, cardWidth - 28.0f, 12.0f);
        n->Colour(mTheme.ModalTextSecondary());
        return Over(Pad(14, 14, 14, 12,
                        Col({FixedHeight(15, l), VGap(6), FixedHeight(34, value), VGap(6),
                             Row({el::share(el::hold_any(n)), Stretch()}), Stretch()})),
                    std::make_shared<MetricFace>(mTheme, accent));
    };
    std::shared_ptr<Text> live, captured, trim;
    auto liveCard = metric("Live Peak", "Raw input before any trim", false, live);
    auto capturedCard = metric("Captured Peak", "Peak hold saved into this profile", true, captured);
    auto trimCard = metric("Recommended Trim", "Applied globally before the chain", false, trim);
    constexpr float kCardHeight = 120.0f;
    const float metricsHeight = stackMetrics ? kCardHeight * 3.0f + 24.0f : kCardHeight;
    Ptr metrics = stackMetrics
                      ? Col({FixedHeight(kCardHeight, liveCard), VGap(12), FixedHeight(kCardHeight, capturedCard),
                             VGap(12), FixedHeight(kCardHeight, trimCard)})
                      : Row({FixedWidth(cardWidth, liveCard), HGap(12), FixedWidth(cardWidth, capturedCard), HGap(12),
                             FixedWidth(cardWidth, trimCard)});
    auto status = std::make_shared<Text>(mTheme, "", 12.0f);
    status->Colour(mTheme.ModalTextSecondary());
    const auto label = [this](const char* text) {
        auto t = std::make_shared<Text>(mTheme, text, 12.0f, Weight::SemiBold);
        t->Colour(mTheme.ModalText());
        return t;
    };

    auto [nameElement, nameBox] = el::input_box("Guitar X, Interface Gain at 0", Font(Weight::Regular), 1.0f);
    auto [descriptionElement, descriptionBox] =
        el::input_box("Single coils, bridge pickup, interface knob at 12 o'clock", Font(Weight::Regular), 1.0f);
    std::shared_ptr<el::basic_input_box> name = nameBox, description = descriptionBox;

    auto submit = [this, name, description] {
        const auto text = Trimmed(name->get_utf8());

        if (text.empty() || !guitarfx::uiclient::CanSaveInputLevellingTraining(mState.inputLevelling))
        {
            return;
        }

        mSession.Commands().SaveInputLevellingProfile(text, Trimmed(description->get_utf8()));
    };
    auto save = std::make_shared<Button>(mTheme, "Save Profile", submit, Button::Style::Primary);
    auto reset = std::make_shared<Button>(
        mTheme, "Reset Peak Hold", [this] { mSession.Commands().ResetInputLevellingTraining(); }, Button::Style::Plain);
    auto cancel = std::make_shared<Button>(mTheme, "Cancel", [this] { CloseOverlay(); }, Button::Style::Plain);
    save->MinWidth(110);
    cancel->MinWidth(88);

    auto sync = [this, live, captured, trim, status, save, name, saveSerial] {
        const auto& levelling = mState.inputLevelling;

        // A save the engine accepted ends the training and the dialog; its toast says so.
        if (levelling.saveSerial != saveSerial && levelling.saveOk)
        {
            mSession.View().post([this, alive = mAlive] {
                if (*alive)
                {
                    CloseOverlay();
                }
            });
            return;
        }

        live->Set(guitarfx::uiclient::FormatInputLevelDbfs(levelling.training.livePeakDbfs));
        captured->Set(guitarfx::uiclient::FormatInputLevelDbfs(levelling.training.capturedPeakDbfs));
        trim->Set(guitarfx::uiclient::FormatInputTrimDb(levelling.training.recommendedTrimDb));
        status->Set(guitarfx::uiclient::InputLevellingTrainingStatus(levelling))
            .Colour(levelling.training.clipped ? mTheme.Error() : mTheme.ModalTextSecondary());
        save->Enabled(guitarfx::uiclient::CanSaveInputLevellingTraining(levelling) &&
                      !Trimmed(name->get_utf8()).empty());
        mSession.View().refresh();
    };
    // The web's maxlength: 80 for the name, 200 for the description.
    name->on_text = [sync, weak = std::weak_ptr<el::basic_input_box>(name)](std::string_view text) {
        if (text.size() > 80)
        {
            if (auto box = weak.lock())
            {
                box->set_text(Capped(std::string(text), 80));
            }
        }

        sync();
    };
    description->on_text = [weak = std::weak_ptr<el::basic_input_box>(description)](std::string_view text) {
        if (text.size() > 200)
        {
            if (auto box = weak.lock())
            {
                box->set_text(Capped(std::string(text), 200));
            }
        }
    };
    name->on_enter = [submit](std::string_view) {
        submit();
        return true;
    };
    name->on_escape = [this] { CloseOverlay(); };
    description->on_escape = [this] { CloseOverlay(); };

    auto intro = std::make_shared<Paragraph>(
        mTheme,
        "Play the loudest picking or strumming you expect to use with this guitar and interface gain setting. "
        "Soundshed will save the captured raw input peak and apply a fixed global trim for this setup.",
        inner, 13.0f);
    intro->Colour(mTheme.ModalTextSecondary());
    reset->MinWidth(130).TextSize(12.0f);
    auto training = Over(
        Pad(14, 12, 14, 12, Row({Middle(status), Stretch(nullptr, 0.001), HGap(12), Middle(FixedHeight(34, reset))})),
        std::make_shared<TrainingFace>(mTheme));
    const float introHeight = 60.0f;

    ShowDialog({.title = "Input Level Training",
                .body = Col({FixedHeight(introHeight, Row({el::share(el::hold_any(intro)), Stretch()})), VGap(18),
                             FixedHeight(metricsHeight, metrics), VGap(18), FixedHeight(60, training), VGap(18),
                             FixedHeight(18, label("Profile Name")), VGap(6),
                             FixedHeight(40, PadXY(0, 2, el::share(std::move(nameElement)))), VGap(10),
                             FixedHeight(18, label("Description")), VGap(6),
                             FixedHeight(40, PadXY(0, 2, el::share(std::move(descriptionElement))))}),
                .actions = {cancel, save},
                .width = width,
                .height = kDialogHeaderHeight + kDialogFooterHeight + 10.0f + introHeight + 18.0f + metricsHeight +
                          18.0f + 60.0f + 18.0f + 24.0f + 40.0f + 10.0f + 24.0f + 40.0f});

    mInstrumentSubs.push_back(mSession.Client().Subscribe(Topic::InputLevelling, sync));
    // Closed by Cancel, a save or a tap outside: training stops (after a save it already has).
    mOnOverlayClosed = [this] { mSession.Commands().StopInputLevellingTraining(); };
    sync();
}
} // namespace nanoq::ui
