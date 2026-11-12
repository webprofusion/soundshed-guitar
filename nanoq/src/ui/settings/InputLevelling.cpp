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
/// The dialog's status as two lines: the first sentence, then the rest.
std::pair<std::string, std::string> SplitStatus(const std::string& status)
{
    const auto stop = status.find(". ");

    if (stop == std::string::npos)
    {
        return {status, {}};
    }

    return {status.substr(0, stop + 1), status.substr(stop + 2)};
}

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
    auto button = std::make_shared<IconButton>(mTheme, mIcons, "ui-caret");
    button->Size(20, 18).IconSize(11).TouchTarget(36);
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
        title->Colour(mTheme.Pick(el::rgba(220, 227, 243, 173), mTheme.TextSecondary()));
        auto subtitle = std::make_shared<Text>(mTheme,
                                               levelling.trainingActive ? "Training bypass active"
                                               : active != nullptr      ? active->name + " active"
                                                                        : "No active levelling",
                                               10.0f);
        subtitle->Colour(mTheme.TextMuted()).Alignment(Text::Align::Right);

        std::vector<Ptr> rows{FixedHeight(16, Row({title, Stretch(nullptr, 0.001), subtitle})), VGap(6)};
        auto none = std::make_shared<ListRow>(mTheme, "No levelling", active == nullptr ? "OFF" : "", active == nullptr,
                                              false, [this] {
                                                  mSession.Commands().SelectInputLevellingProfile("");
                                                  CloseOverlay();
                                              });
        rows.push_back(FixedHeight(metrics::kRowHeight, none));

        for (const auto& profile : levelling.profiles)
        {
            const bool isActive = active != nullptr && active->id == profile.id;
            // The name, and what it applies (active) or its description or captured peak.
            const std::string label =
                profile.name + "  ·  " + guitarfx::uiclient::InputLevellingProfileNote(profile, isActive);
            auto row = std::make_shared<ListRow>(
                mTheme, label, isActive ? "ACTIVE" : guitarfx::uiclient::FormatInputTrimDb(profile.gainDb), isActive,
                false, [this, id = profile.id] {
                    mSession.Commands().SelectInputLevellingProfile(id);
                    CloseOverlay();
                });
            auto remove = std::make_shared<IconButton>(mTheme, mIcons, "ui-trash");
            remove->Size(28, 28).IconSize(13).Ink(mTheme.TextSecondary());
            remove->onClick = [this, id = profile.id, name = profile.name] {
                OpenConfirm("Delete Input Levelling Profile", "Delete input levelling profile \"" + name + "\"?",
                            "Delete", [this, id, name] {
                                mSession.Commands().DeleteInputLevellingProfile(id);
                                ShowToast("Input levelling profile deleted", name, false);
                            });
            };
            rows.push_back(
                FixedHeight(metrics::kRowHeight,
                            Row({row, HGap(4), FixedWidth(28, Col({Stretch(), FixedHeight(28, remove), Stretch()}))})));
        }

        rows.push_back(VGap(6));
        auto train = std::make_shared<ListRow>(mTheme, "Train Input Level", "TRAIN", false, false,
                                               [this] { OpenInputLevellingTraining(); });
        train->Icon(&mIcons, "ui-io", mTheme.TextSecondary());
        rows.push_back(FixedHeight(metrics::kRowHeight, train));
        rows.push_back(std::make_shared<el::element>());
        Fill(*body, Col(rows));
        mSession.View().layout();
        mSession.View().refresh();
    };
    (*rebuild)();

    const auto& levelling = mState.inputLevelling;
    const float rowCount = static_cast<float>(levelling.profiles.size() + 2);
    const float height = std::min(16.0f + 6.0f + rowCount * metrics::kRowHeight + 6.0f + 20.0f, ViewSize().y - 40.0f);
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

    const auto label = [this](const char* text) {
        auto t = std::make_shared<Text>(mTheme, text, 10.0f, Weight::SemiBold);
        t->Colour(mTheme.TextMuted());
        return t;
    };
    const auto metric = [this, &label](const char* name, std::shared_ptr<Text>& value) {
        value = std::make_shared<Text>(mTheme, "-", 15.0f, Weight::SemiBold);
        return Col({FixedHeight(16, label(name)), FixedHeight(22, value)});
    };
    std::shared_ptr<Text> live, captured, trim;
    auto metrics = Row({metric("LIVE PEAK", live), HGap(12), metric("CAPTURED PEAK", captured), HGap(12),
                        metric("RECOMMENDED TRIM", trim)});
    auto status1 = std::make_shared<Text>(mTheme, "", 11.0f);
    auto status2 = std::make_shared<Text>(mTheme, "", 11.0f);
    status2->Colour(mTheme.TextSecondary());

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

    auto sync = [this, live, captured, trim, status1, status2, save, name, saveSerial] {
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
        const auto [first, rest] = SplitStatus(guitarfx::uiclient::InputLevellingTrainingStatus(levelling));
        status1->Set(first).Colour(levelling.training.clipped ? mTheme.Error() : mTheme.Text());
        status2->Set(rest);
        save->Enabled(guitarfx::uiclient::CanSaveInputLevellingTraining(levelling) &&
                      !Trimmed(name->get_utf8()).empty());
        mSession.View().refresh();
    };
    name->on_text = [sync](std::string_view) { sync(); };
    name->on_enter = [submit](std::string_view) {
        submit();
        return true;
    };
    name->on_escape = [this] { CloseOverlay(); };
    description->on_escape = [this] { CloseOverlay(); };

    auto intro = std::make_shared<Text>(
        mTheme, "Play your loudest picking or strumming. The active profile is bypassed while you train.", 11.0f);
    intro->Colour(mTheme.ModalTextSecondary());

    ShowDialog({.title = "Train Input Level",
                .body = Col({FixedHeight(18, intro), VGap(10), FixedHeight(38, metrics), VGap(8),
                             FixedHeight(18, status1), FixedHeight(18, status2), VGap(6),
                             FixedHeight(32, Row({reset, Stretch()})), VGap(10), FixedHeight(18, label("PROFILE NAME")),
                             FixedHeight(40, PadXY(0, 2, el::share(std::move(nameElement)))), VGap(4),
                             FixedHeight(18, label("DESCRIPTION")),
                             FixedHeight(40, PadXY(0, 2, el::share(std::move(descriptionElement))))}),
                .actions = {cancel, save},
                .width = 520.0f,
                .height = kDialogHeaderHeight + kDialogFooterHeight + 36.0f + 278.0f});

    mInstrumentSubs.push_back(mSession.Client().Subscribe(Topic::InputLevelling, sync));
    // Closed by Cancel, a save or a tap outside: training stops (after a save it already has).
    mOnOverlayClosed = [this] { mSession.Commands().StopInputLevellingTraining(); };
    sync();
}
} // namespace nanoq::ui
