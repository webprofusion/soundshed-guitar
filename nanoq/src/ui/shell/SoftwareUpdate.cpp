// SoftwareUpdate.cpp - the web UI's update check (updateCheck.ts): once a session, five seconds
// after the first state and unless app.updateCheckEnabled is off, ask the Soundshed service for
// its newest release (uiclient's ToneService::CheckForUpdate). A newer one is announced with a
// toast and an Update button beside the gear (the compact top bar's end), which shows its notes
// and opens the download.

#include "ui/shell/Shell.h"

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
    std::string detail = "Version " + update.version + " is available (this is " + mState.environment.version + ").";

    if (!update.releaseNotes.empty())
    {
        detail += "\n\n" + update.releaseNotes;
    }

    if (update.downloadUrl.empty())
    {
        OpenConfirm("Update available", detail, "OK", [] {});
        return;
    }

    OpenConfirm("Update available", detail, "Download",
                [this, url = update.downloadUrl] { mSession.Commands().OpenUrl(url); });
}
} // namespace nanoq::ui
