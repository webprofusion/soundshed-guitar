// Tone3000Key.cpp - the player's own Tone3000 API key, for when the relay is off (the web UI's
// tone3000Settings.ts Save, and tone3000-required-modal.html over a shared preset's install).
// The key typed in is tried first (ToneService::VerifyTone3000Key starts a session with it), and
// kept only when Tone3000 takes it; it is never shown again, only that one is stored.

#include "ui/dialogs/DialogChrome.h"
#include "ui/shell/Shell.h"

#include "uiclient/Tone3000.h"

#include <algorithm>

namespace nanoq::ui
{
void Shell::OpenTone3000KeyDialog(bool required, std::function<void()> proceed, std::function<void()> cancel)
{
    const float width = std::min(480.0f, std::max(240.0f, ViewSize().x - 48.0f)); // as Dialogs.cpp's DialogWidth
    const float textWidth = width - 40.0f;
    auto [boxElement, box] = el::input_box("Enter your Tone3000 API key", Font(Weight::Regular), 1.0f);
    auto status = std::make_shared<Text>(mTheme, "", 12.0f);
    status->Colour(mTheme.ModalTextSecondary());
    // Set once a key is being tried: a second Save waits for the first.
    auto busy = std::make_shared<bool>(false);
    // Closing the dialog any other way than Save is Cancel Import.
    auto settled = std::make_shared<bool>(false);
    auto cancelOnce = [settled, cancel] {
        if (!*settled && cancel)
        {
            *settled = true;
            cancel();
        }
    };

    auto save = [this, box, status, busy, settled, proceed, required] {
        auto key = box->get_utf8();
        const auto first = key.find_first_not_of(" \t\r\n");
        key =
            first == std::string::npos ? std::string() : key.substr(first, key.find_last_not_of(" \t\r\n") - first + 1);

        if (key.empty() || *busy)
        {
            if (key.empty())
            {
                status->Set("Tone3000 API key required");
                status->Colour(mTheme.Error());
                mSession.Repaint(*status);
            }

            return;
        }

        *busy = true;
        status->Set("Verifying Tone3000 API key...");
        status->Colour(mTheme.ModalTextSecondary());
        mSession.Repaint(*status);
        mSession.Tones().VerifyTone3000Key(key, [this, alive = std::weak_ptr<bool>(mAlive), key, status, busy, settled,
                                                 proceed, required](bool ok, const std::string& error) {
            if (!alive.lock())
            {
                return;
            }

            *busy = false;

            if (!ok)
            {
                status->Set(error.empty() ? std::string("Tone3000 API key is invalid") : error);
                status->Colour(mTheme.Error());
                mSession.Repaint(*status);
                return;
            }

            mSession.Commands().SetSetting(guitarfx::uiclient::tone3000::kApiKeySetting, key);
            *settled = true;
            CloseOverlay();
            ShowToast("Tone3000 API key saved", {}, false);

            if (required && proceed)
            {
                proceed();
            }
        });
    };

    box->on_enter = [save](std::string_view) {
        save();
        return true;
    };
    box->on_escape = [this, cancelOnce] {
        cancelOnce();
        DismissOverlay();
    };

    std::vector<Ptr> body;
    const auto paragraph = [&](const std::string& text) {
        auto p = std::make_shared<Paragraph>(mTheme, text, textWidth, 13.0f);
        p->Colour(mTheme.ModalTextSecondary());
        body.push_back(Row({el::share(el::hold_any(p)), Stretch()}));
        body.push_back(VGap(10));
    };

    if (required)
    {
        paragraph("This shared preset includes Tone3000 resources.");
    }

    paragraph("You need your own Tone3000 API key while the Soundshed API is off. Get one from tone3000.com, under "
              "your account Settings (Legacy API Key), then paste it below.");

    {
        auto label = std::make_shared<Text>(mTheme, "TONE3000 API KEY", 11.0f, Weight::SemiBold);
        label->Colour(mTheme.ModalTextSecondary());
        body.push_back(FixedHeight(20, Middle(label)));
        body.push_back(FixedHeight(40, el::share(std::move(boxElement))));
        body.push_back(VGap(6));
        body.push_back(FixedHeight(20, Middle(status)));
    }

    std::vector<Ptr> actions;

    if (required)
    {
        actions.push_back(DialogButton(
            "Cancel Import",
            [this, cancelOnce] {
                cancelOnce();
                DismissOverlay();
            },
            false));
        actions.push_back(DialogButton(
            "Open Settings",
            [this, cancelOnce] {
                cancelOnce();
                CloseOverlay();
                mSettingsSection = SettingsSection::General;
                ShowPage(Page::Settings);
            },
            false));
        actions.push_back(DialogButton("Save Key and Continue", save, true));
    }
    else
    {
        actions.push_back(DialogButton("Cancel", [this] { DismissOverlay(); }, false));
        actions.push_back(DialogButton("Save", save, true));
    }

    mNextOverlayInterrupts = true;
    ShowDialog({.title = required ? "Tone3000 API Key Required" : "Tone3000 API Key",
                .body = Col(body),
                .actions = std::move(actions),
                .width = width,
                .height = kDialogHeaderHeight + kDialogFooterHeight + (required ? 210.0f : 170.0f),
                .onClose = [this, cancelOnce] {
                    cancelOnce();
                    DismissOverlay();
                }});
}
} // namespace nanoq::ui
