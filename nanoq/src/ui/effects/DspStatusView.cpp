// DspStatusView.cpp - the DSP status readouts (the web UI's dspStatusReadout.ts): an effect's,
// under its header while the DSP chip is on, and the app output's, from the Controls strip. Each
// is a row of tiles the user chose (a x takes one away, the + adds one, by group), averaged and
// redrawn twice a second by uiclient's DspStatusMeter. Loudness is measured only while a
// loudness tile is on screen.

#include "ui/shell/Shell.h"

#include "uiclient/Analysis.h"

#include <algorithm>

namespace nanoq::ui
{
using guitarfx::uiclient::DspStatusItem;

void Shell::FillDspStatus(const std::shared_ptr<DspStatusView>& view)
{
    const auto shown = guitarfx::uiclient::ShownDspStatusItems(mState, *view->readout);
    view->values.clear();
    std::vector<Ptr> row;

    for (const auto item : shown)
    {
        const auto& info = guitarfx::uiclient::DspStatusInfo(item);
        auto label = std::make_shared<Text>(mTheme, info.label, 9.0f, Weight::SemiBold);
        label->Colour(mTheme.TextMuted());
        auto value = std::make_shared<Text>(mTheme, view->meter.Value(item), 12.0f, Weight::SemiBold);
        value->Colour(mTheme.Ink());
        view->values[item] = value;
        auto remove = std::make_shared<IconButton>(mTheme, mIcons, "x", IconButton::Look::Ghost);
        remove->Size(16, 16).IconSize(8).Ink(mTheme.TextMuted());
        remove->onClick = [this, weak = std::weak_ptr<DspStatusView>(view), item] {
            const auto v = weak.lock();

            if (!v)
            {
                return;
            }

            auto items = guitarfx::uiclient::ShownDspStatusItems(mState, *v->readout);
            items.erase(std::remove(items.begin(), items.end(), item), items.end());
            SetDspStatusItems(v, items);
        };
        auto tile = Over(Pad(8, 4, 4, 4, Col({FixedHeight(14, Row({label, remove})), FixedHeight(18, value)})),
                         Fill(mTheme.Text().opacity(0.05f), std::make_shared<el::element>()));
        row.push_back(FixedWidth(110, tile));
        row.push_back(HGap(6));
    }

    // The + lists what is not shown, by group.
    std::vector<DspStatusItem> hidden;

    for (const auto item : view->readout->available)
    {
        if (std::find(shown.begin(), shown.end(), item) == shown.end())
        {
            hidden.push_back(item);
        }
    }

    if (!hidden.empty())
    {
        auto add = std::make_shared<IconButton>(mTheme, mIcons, "ui-new", IconButton::Look::Boxed);
        add->Size(28, 28).IconSize(12);
        add->onClick = [this, weak = std::weak_ptr<DspStatusView>(view), hidden, a = add.get()] {
            const auto v = weak.lock();

            if (!v)
            {
                return;
            }

            std::vector<MenuItem> items;
            std::string group;

            for (const auto& info : guitarfx::uiclient::DspStatusItems())
            {
                if (std::find(hidden.begin(), hidden.end(), info.id) == hidden.end())
                {
                    continue;
                }

                if (group != info.group)
                {
                    group = info.group;
                    items.push_back({group, false, true, {}});
                }

                items.push_back({info.label, false, false, [this, weak, id = info.id] {
                                     if (const auto v2 = weak.lock())
                                     {
                                         auto shownNow = guitarfx::uiclient::ShownDspStatusItems(mState, *v2->readout);
                                         shownNow.push_back(id);
                                         SetDspStatusItems(v2, shownNow);
                                     }
                                 }});
            }

            OpenDropdown(a->Bounds(), std::move(items), 200.0f);
        };
        row.push_back(FixedWidth(28, Col({VGap(5), FixedHeight(28, add), std::make_shared<el::element>()})));
    }

    row.push_back(Stretch());
    Fill(*view->slot, Row(row));
    mSession.View().layout();
    mSession.View().refresh();
}

void Shell::SetDspStatusItems(const std::shared_ptr<DspStatusView>& view, const std::vector<DspStatusItem>& items)
{
    // The choice is an app setting, shared with the web UI; mirrored at once so the tiles follow.
    const auto value = guitarfx::uiclient::DspStatusSettingValue(items);
    mSession.Client().MutableState().appSettings[view->readout->settingKey] = value;
    mSession.Commands().SetSetting(view->readout->settingKey, value);
    mSession.View().post([this, alive = std::weak_ptr<bool>(mAlive), weak = std::weak_ptr<DspStatusView>(view)] {
        if (const auto v = weak.lock(); v && alive.lock())
        {
            FillDspStatus(v);
        }
    });
}

Ptr Shell::BuildDspStatus(const guitarfx::uiclient::DspStatusReadout& readout, const std::string& nodeKey,
                          const guitarfx::uiclient::NodeAddress& address, bool overlay)
{
    auto view = std::make_shared<DspStatusView>();
    view->readout = &readout;
    view->meter = guitarfx::uiclient::DspStatusMeter(nodeKey);
    view->address = address;
    view->meter.Update(mState, UiSession::Now());
    (overlay ? mOutputDspStatus : mEffectDspStatus) = view;
    FillDspStatus(view);
    return view->slot;
}

void Shell::SyncDspStatus()
{
    auto& analysis = mSession.Client().GetAnalysis();
    const double now = UiSession::Now();

    // The effect's loudness is measured while its readout shows a loudness tile, and so is the
    // output's; neither costs anything otherwise.
    bool effectLoudness = false, outputLoudness = false;

    for (auto* slot : {&mEffectDspStatus, &mOutputDspStatus})
    {
        const auto& view = *slot;

        if (!view)
        {
            continue;
        }

        const auto shown = guitarfx::uiclient::ShownDspStatusItems(mState, *view->readout);
        const bool loud = guitarfx::uiclient::ShowsLoudness(shown);
        (view->address.scope.empty() ? outputLoudness : effectLoudness) |= loud;

        if (!view->address.scope.empty() && loud)
        {
            analysis.WatchLoudness(view->address);
        }

        view->meter.Update(mState, now);

        if (!view->meter.TakeRenderTurn(now))
        {
            continue;
        }

        for (const auto& [item, weak] : view->values)
        {
            if (const auto text = weak.lock(); text && text->Value() != view->meter.Value(item))
            {
                text->Set(view->meter.Value(item));
                mSession.Repaint(*text);
            }
        }
    }

    if (!effectLoudness)
    {
        analysis.StopLoudness();
    }

    analysis.WatchOutputLoudness(outputLoudness);
}

void Shell::OpenOutputStatus(el::rect anchor)
{
    CloseOverlay();
    auto title = std::make_shared<Text>(mTheme, "OUTPUT STATUS", 9.0f, Weight::SemiBold);
    title->Colour(mTheme.Pick(el::rgba(220, 227, 243, 173), mTheme.TextSecondary()));
    auto tiles = BuildDspStatus(guitarfx::uiclient::OutputDspStatus(), {}, {}, true);
    const float width = std::min(6.0f * 116.0f + 44.0f, ViewSize().x - 24.0f);
    ShowPopover(Pad(12, 10, 12, 10, Col({FixedHeight(14, title), VGap(6), FixedHeight(40, tiles)})), anchor, width,
                14.0f + 6.0f + 40.0f + 20.0f);
    mOnOverlayClosed = [this] { mOutputDspStatus.reset(); };
}
} // namespace nanoq::ui
