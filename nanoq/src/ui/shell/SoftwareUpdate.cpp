// SoftwareUpdate.cpp - the web UI's update check (updateCheck.ts): once a session, five seconds
// after the first state and unless app.updateCheckEnabled is off, ask the Soundshed service for
// its newest release (uiclient's ToneService::CheckForUpdate). A newer one is announced with a
// toast and an Update button beside the gear (the compact top bar's end), which shows its notes
// and opens the download.

#include "ui/shell/Shell.h"

#include "ui/dialogs/DialogChrome.h"
#include "uiclient/UpdateCheck.h"

#include <cstdio>
#include <random>

namespace nanoq::ui
{
namespace
{
/// A random UUID (version 4), as crypto.randomUUID() makes for the instance id.
std::string RandomUuid()
{
    std::random_device device;
    std::mt19937_64 rng((static_cast<std::uint64_t>(device()) << 32) ^ device());
    std::uint8_t bytes[16];

    for (auto& b : bytes)
    {
        b = static_cast<std::uint8_t>(rng());
    }

    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3F) | 0x80);
    char text[37];
    std::snprintf(text, sizeof(text), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", bytes[0],
                  bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9], bytes[10],
                  bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return text;
}

/// The banner's download arrow, 18 px in the info blue.
class BannerIcon final : public el::element
{
  public:
    BannerIcon(Icons& icons, el::color ink) : mIcons(icons), mInk(ink)
    {
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{18.0f, 18.0f}, {18.0f, 18.0f}};
    }

    void draw(const el::context& ctx) override
    {
        mIcons.Draw(ctx.canvas, "ui-download", ctx.bounds, mInk);
    }

  private:
    Icons& mIcons;
    el::color mInk;
};

/// The banner's face: the info blue at 10% with a 35% edge, radius 10.
class BannerFace final : public el::element
{
  public:
    explicit BannerFace(el::color info) : mInfo(info)
    {
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        cnv.fill_style(mInfo.opacity(0.10f));
        cnv.fill_round_rect(ctx.bounds, 10.0f);
        cnv.stroke_style(mInfo.opacity(0.35f));
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(ctx.bounds.inset(0.5f, 0.5f), 10.0f);
    }

  private:
    el::color mInfo;
};
} // namespace

void Shell::SyncUpdateCheck()
{
    if (mUpdateChecked || !mState.haveState)
    {
        return;
    }

    const auto enabled = mState.appSettings.find(guitarfx::uiclient::kUpdateCheckEnabledSetting);

    if (enabled != mState.appSettings.end() && ((enabled->is_boolean() && !enabled->get<bool>()) ||
                                                (enabled->is_string() && enabled->get<std::string>() != "true")))
    {
        return; // turned off; turned back on in Settings, it checks then
    }

    const auto& version = mState.environment.version;

    if (version.empty())
    {
        return;
    }

    const double now = UiSession::Now();

    if (mUpdateCheckAt == 0.0)
    {
        mUpdateCheckAt = now + 5.0; // not in the way of the start-up
        return;
    }

    if (now < mUpdateCheckAt)
    {
        return;
    }

    mUpdateChecked = true;

    std::string instanceId = mState.appSettings.value(guitarfx::uiclient::kInstanceIdSetting, std::string());

    if (instanceId.empty())
    {
        instanceId = RandomUuid();
        mSession.Commands().SetSetting(guitarfx::uiclient::kInstanceIdSetting, instanceId);
    }

    const auto& env = mState.environment;
    mSession.Tones().CheckForUpdate(
        version, env.os.empty() ? std::string("Unknown") : env.os, env.cpu.empty() ? std::string("x64") : env.cpu,
        env.standalone, instanceId,
        [this, alive = std::weak_ptr<bool>(mAlive)](const std::optional<guitarfx::uiclient::AvailableUpdate>& update) {
            if (!alive.lock() || !update)
            {
                return;
            }

            mUpdate = *update;
            ShowToast("Update Available", "Version " + update->version + " is available.", false);
            Mark(kTopBar | kRail);
        });
}

Ptr Shell::BuildUpdateButton()
{
    if (!mUpdate)
    {
        return nullptr;
    }

    auto button = std::make_shared<IconButton>(mTheme, mIcons, "ui-download", IconButton::Look::Chip);
    button->Size(84, 26).IconSize(12).Label("Update", 11.0f).Ink(mTheme.Accent());
    button->onClick = [this] { OpenUpdateDialog(); };
    return button;
}

void Shell::OpenUpdateDialog()
{
    if (!mUpdate)
    {
        return;
    }

    const auto update = *mUpdate;
    constexpr float kWidth = 520.0f;
    constexpr float kTextWidth = kWidth - 40.0f;
    std::vector<Ptr> body;

    // "Soundshed Guitar X is available. You are running Y." (.update-modal-summary)
    {
        const std::string product = mState.environment.nativeEditor ? "Soundshed Guitar" : "Soundshed Nano Q";
        const std::string current =
            mState.environment.version.empty() ? "an older version" : mState.environment.version;
        auto p = std::make_shared<Paragraph>(
            mTheme, product + " " + update.version + " is available. You are running " + current + ".", kTextWidth,
            13.0f);
        p->Colour(mTheme.ModalText());
        body.push_back(Row({el::share(el::hold_any(p)), Stretch()}));
        body.push_back(VGap(12));
    }

    // The release notes, rendered (.update-modal-notes): headings, quotes and bullets, in a box that
    // scrolls when they run long.
    std::vector<Ptr> notes;

    for (const auto& line : guitarfx::uiclient::ReleaseNoteLines(
             update.releaseNotes.empty() ? std::string("No release notes provided.") : update.releaseNotes))
    {
        using Kind = guitarfx::uiclient::ReleaseNoteLine::Kind;

        if (line.kind == Kind::Blank)
        {
            notes.push_back(VGap(8));
            continue;
        }

        const bool heading = line.kind == Kind::Heading;
        const float size = heading ? (line.level == 1 ? 16.0f : line.level == 2 ? 14.5f : 13.0f) : 12.5f;
        const float indent = line.kind == Kind::Bullet || line.kind == Kind::Quote ? 16.0f : 0.0f;
        auto p = std::make_shared<Paragraph>(mTheme, line.text, kTextWidth - 24.0f - indent, size);
        p->Weighted(heading ? Weight::SemiBold : Weight::Regular);
        p->Colour(line.kind == Kind::Quote ? mTheme.ModalTextSecondary() : mTheme.ModalText());
        std::vector<Ptr> parts;

        if (line.kind == Kind::Bullet)
        {
            auto dot = std::make_shared<Text>(mTheme, "\xE2\x80\xA2", size);
            dot->Colour(mTheme.ModalTextSecondary());
            parts.push_back(FixedWidth(indent, dot));
        }
        else if (line.kind == Kind::Quote)
        {
            parts.push_back(FixedWidth(3, Fill(mTheme.ModalRule(), std::make_shared<el::element>())));
            parts.push_back(HGap(indent - 3.0f));
        }

        parts.push_back(el::share(el::hold_any(p)));
        parts.push_back(Stretch());
        notes.push_back(Row(parts));
        notes.push_back(VGap(heading ? 6.0f : 3.0f));
    }

    body.push_back(FixedHeight(260, Rounded(mTheme.Bar(), 8.0f, Pad(12, 10, 12, 10, VScroll(Col(notes))))));

    std::vector<Ptr> actions{DialogButton("Later", [this] { CloseOverlay(); }, false)};

    if (!update.downloadUrl.empty())
    {
        actions.push_back(DialogButton(
            "Download",
            [this, url = update.downloadUrl] {
                mSession.Commands().OpenUrl(url);
                CloseOverlay();
            },
            true));
    }

    ShowDialog({.title = "Update available",
                .body = Col(body),
                .actions = std::move(actions),
                .width = kWidth,
                .height = kDialogHeaderHeight + kDialogFooterHeight + 36.0f + 40.0f + 260.0f});
}

Ptr Shell::BuildUpdateBanner(float width)
{
    // The banner over every Settings section while an update waits (.settings-update-banner): an
    // icon, "Software Update Version X is available" over the notes' first line, and Download.
    if (!mUpdate)
    {
        return nullptr;
    }

    const auto& update = *mUpdate;
    const el::color info = el::rgba(59, 130, 246, 255);
    auto icon = std::make_shared<BannerIcon>(mIcons, info);
    auto title = std::make_shared<Text>(mTheme, "Software Update Version " + update.version + " is available", 13.0f,
                                        Weight::SemiBold);
    title->Fixed(true);
    std::vector<Ptr> text{FixedHeight(18, Middle(title))};
    const auto first = guitarfx::uiclient::ReleaseNotesFirstLine(update.releaseNotes);

    if (!first.empty())
    {
        auto notes = std::make_shared<Text>(mTheme, first, 11.0f);
        notes->Colour(mTheme.TextSecondary());
        text.push_back(FixedHeight(16, Middle(notes)));
    }

    std::vector<Ptr> parts{Middle(icon), HGap(12), Middle(Col(text)), Stretch()};

    if (!update.downloadUrl.empty())
    {
        auto download = std::make_shared<Button>(
            mTheme, "Download", [this, url = update.downloadUrl] { mSession.Commands().OpenUrl(url); },
            Button::Style::Primary);
        download->TextSize(11.0f);
        parts.push_back(Middle(FixedHeight(30, download)));
    }

    (void)width;
    return Over(Pad(16, 12, 16, 12, Row(parts)), std::make_shared<BannerFace>(info));
}
} // namespace nanoq::ui
