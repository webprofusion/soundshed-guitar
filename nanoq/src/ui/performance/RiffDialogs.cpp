// Riff capture and the riff library, as the web UI's riffLibrary.ts has them (the riff capture
// and save modals, and the Jam panel's Riff Library). The engine records, trims, previews and
// keeps the riffs (PluginControllerRiffs.cpp); these dialogs drive it and show what it reports
// (uiclient/Riffs.h, Topic::Riffs).
//
// Capture counts in, then records from the first note it hears, up to 64 bars; Stop ends the
// take. The take is then trimmed with the markers on its waveform, previewed, and saved as a
// new riff (or as another take of the riff being edited).

#include "ui/dialogs/DialogChrome.h"
#include "ui/shell/Shell.h"

#include "ui/performance/Waveform.h"

#include "uiclient/FeatureFlags.h"
#include "uiclient/Looper.h"
#include "uiclient/Riffs.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace nanoq::ui
{
using guitarfx::uiclient::Topic;

namespace
{
using guitarfx::uiclient::RiffCapture;

std::string TrimText(const std::string& text)
{
    const auto first = text.find_first_not_of(" \t");
    const auto last = text.find_last_not_of(" \t");
    return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
}

std::string JoinList(const std::vector<std::string>& items)
{
    std::string out;

    for (const auto& item : items)
    {
        out += (out.empty() ? "" : ", ") + item;
    }

    return out;
}
} // namespace

void Shell::OpenRiffCapture(bool arm, const std::string& appendRiffId)
{
    CloseOverlay();
    auto& commands = mSession.Commands();

    if (mRiffTempo <= 0.0)
    {
        mRiffTempo = mState.metronome.bpm;
    }

    const auto* editing = appendRiffId.empty() ? nullptr : mState.riffs.Find(appendRiffId);
    // The markers (0..1 of the take), the take they were set for, and the preview's repeat.
    auto marks = std::make_shared<std::pair<float, float>>(0.0f, 1.0f);
    auto markedSerial = std::make_shared<int>(mState.riffs.capture.serial);
    auto repeat = std::make_shared<bool>(true);
    auto lastRegionSend = std::make_shared<double>(0.0);
    const int savedSerial = mState.riffs.savedSerial;

    const auto label = [this](const char* text) {
        auto t = std::make_shared<Text>(mTheme, text, 10.0f, Weight::SemiBold);
        t->Colour(mTheme.TextMuted());
        return t;
    };
    const auto small = [this](const std::string& text) {
        auto b = std::make_shared<Button>(mTheme, text, std::function<void()>{}, Button::Style::Plain);
        b->MinWidth(34).TextSize(11.0f);
        return b;
    };

    // ── The take's settings: tempo, count-in, click ──
    auto tempo = std::make_shared<Text>(mTheme, "", 12.0f, Weight::SemiBold);
    tempo->Alignment(HAlign::Centre);
    auto slower = small("-"), faster = small("+");
    auto fromMetronome = small("Metronome");
    fromMetronome->MinWidth(90);
    std::vector<std::shared_ptr<Button>> countIn;

    for (const char* text : {"None", "1 bar", "2 bars"})
    {
        countIn.push_back(small(text));
    }

    auto click = std::make_shared<CheckBox>(mTheme, "Click");

    auto record = std::make_shared<Button>(mTheme, "Record", std::function<void()>{}, Button::Style::Primary);
    record->MinWidth(110);
    auto status = std::make_shared<Text>(mTheme, "", 11.0f);
    auto wave = std::make_shared<Waveform>(mTheme, 96.0f);

    // ── Trim and preview ──
    auto startText = std::make_shared<Text>(mTheme, "", 11.0f);
    auto endText = std::make_shared<Text>(mTheme, "", 11.0f);
    endText->Alignment(HAlign::Right);
    startText->Colour(mTheme.TextSecondary());
    endText->Colour(mTheme.TextSecondary());
    auto preview = std::make_shared<Button>(mTheme, "Play", std::function<void()>{}, Button::Style::Plain);
    preview->MinWidth(80);
    auto repeatButton = std::make_shared<CheckBox>(mTheme, "Repeat");
    auto crop = std::make_shared<Button>(mTheme, "Crop to Markers", std::function<void()>{}, Button::Style::Plain);
    crop->MinWidth(130);

    // ── Save ──
    auto [titleElement, titleBox] = el::input_box("Title", Font(Weight::Regular), 1.0f);
    auto [tagsElement, tagsBox] = el::input_box("Tags, comma separated", Font(Weight::Regular), 1.0f);
    std::shared_ptr<el::basic_input_box> title = titleBox, tags = tagsBox;
    auto favourite = std::make_shared<CheckBox>(mTheme, "Favourite");
    auto favouriteOn = std::make_shared<bool>(editing != nullptr && editing->favorite);

    if (editing != nullptr)
    {
        title->set_text(editing->title);
        tags->set_text(JoinList(editing->tags));
    }
    else
    {
        title->set_text("riff-" + std::to_string(mState.riffs.riffs.size() + 1));

        // As the web UI does: a new riff starts with the tags of the one saved last.
        if (!mState.riffs.riffs.empty())
        {
            tags->set_text(JoinList(mState.riffs.riffs.back().tags));
        }
    }

    auto save = std::make_shared<Button>(mTheme, editing != nullptr ? "Save Take" : "Save Riff",
                                         std::function<void()>{}, Button::Style::Primary);
    save->MinWidth(110);
    auto close = std::make_shared<Button>(mTheme, "Close", [this] { CloseOverlay(); }, Button::Style::Plain);
    close->MinWidth(88);

    const auto sampleRate = [this] { return mState.telemetry.sampleRate; };
    const auto previewing = [this] {
        const auto& c = mState.riffs.capture;
        return mState.demo.playing && !c.takeId.empty() && mState.demo.previewId == c.takeId;
    };

    auto sync = [this, tempo, countIn, click, record, status, wave, startText, endText, preview, repeatButton, crop,
                 favourite, favouriteOn, save, title, marks, markedSerial, repeat, savedSerial, sampleRate,
                 previewing] {
        const auto& riffs = mState.riffs;
        const auto& c = riffs.capture;

        // Saved: the toast says so, and the dialog is done.
        if (riffs.savedSerial != savedSerial)
        {
            mSession.View().post([this, alive = mAlive] {
                if (*alive)
                {
                    CloseOverlay();
                }
            });
            return;
        }

        char text[48];
        std::snprintf(text, sizeof(text), "%.0f BPM", mRiffTempo);
        tempo->Set(text);

        for (std::size_t i = 0; i < countIn.size(); ++i)
        {
            countIn[i]->Selected(static_cast<int>(i) == mRiffCountIn);
        }

        click->Set(mRiffClick);

        const bool complete = c.phase == RiffCapture::Phase::Complete;
        const bool taking = c.phase == RiffCapture::Phase::Armed || c.phase == RiffCapture::Phase::Recording;
        record->Set(c.phase == RiffCapture::Phase::Armed       ? "Cancel"
                    : c.phase == RiffCapture::Phase::Recording ? "Stop"
                    : complete                                 ? "Record Again"
                                                               : "Record");
        status->Set(guitarfx::uiclient::RiffCaptureStatus(c, sampleRate()));
        status->Colour(c.phase == RiffCapture::Phase::Recording ? mTheme.Error() : mTheme.Text());

        // A new take: the markers go back to its ends, or to where its first bar starts.
        if (c.serial != *markedSerial)
        {
            *markedSerial = c.serial;
            *marks = {0.0f, 1.0f};

            if (c.capturedSamples > 0 && c.barAlignOffsetSamples > 0 && c.barAlignOffsetSamples * 2 < c.capturedSamples)
            {
                marks->first = static_cast<float>(static_cast<double>(c.barAlignOffsetSamples) /
                                                  static_cast<double>(c.capturedSamples));
            }
        }

        if (c.phase == RiffCapture::Phase::Recording)
        {
            const auto fill = static_cast<float>(guitarfx::uiclient::RiffCaptureFill(c, sampleRate()));
            wave->Peaks(c.peaks, fill);
            wave->Playhead(fill);
            wave->RangeEnabled(false);
        }
        else
        {
            wave->Peaks(complete ? c.peaks : std::vector<float>{});
            wave->Playhead(-1.0f);
            wave->RangeEnabled(complete && c.hasAudio);
            wave->Range(marks->first, marks->second);
        }

        wave->Empty(c.phase == RiffCapture::Phase::Armed ? "Waiting for signal..." : "Press Record and play");

        const double seconds = guitarfx::uiclient::RiffCaptureSeconds(c, sampleRate());
        startText->Set(complete ? "Start " + guitarfx::uiclient::FormatRiffSeconds(seconds * marks->first)
                                : std::string());
        endText->Set(complete ? "End " + guitarfx::uiclient::FormatRiffSeconds(seconds * marks->second)
                              : std::string());
        preview->Set(previewing() ? "Stop" : "Play").Enabled(complete && c.hasAudio && !taking);
        repeatButton->Set(*repeat);
        crop->Enabled(complete && c.hasAudio && (marks->first > 0.0f || marks->second < 1.0f));
        favourite->Set(*favouriteOn);
        save->Enabled(complete && c.hasAudio && !TrimText(title->get_utf8()).empty());

        // Each control by where it was drawn: the waveform moves four times a second while a take
        // records, and the rest of the window has nothing new.
        for (el::element* e : std::initializer_list<el::element*>{
                 tempo.get(), click.get(), record.get(), status.get(), wave.get(), startText.get(), endText.get(),
                 preview.get(), repeatButton.get(), crop.get(), favourite.get(), save.get()})
        {
            mSession.Repaint(*e);
        }

        for (const auto& b : countIn)
        {
            mSession.Repaint(*b);
        }
    };

    slower->OnClick([this, sync] {
        mRiffTempo = std::clamp(std::round(mRiffTempo) - 1.0, 30.0, 300.0);
        sync();
    });
    faster->OnClick([this, sync] {
        mRiffTempo = std::clamp(std::round(mRiffTempo) + 1.0, 30.0, 300.0);
        sync();
    });
    fromMetronome->OnClick([this, sync] {
        mRiffTempo = mState.metronome.bpm;
        sync();
    });

    for (std::size_t i = 0; i < countIn.size(); ++i)
    {
        countIn[i]->OnClick([this, i, sync] {
            mRiffCountIn = static_cast<int>(i);
            sync();
        });
    }

    click->onClick = [this, sync] {
        mRiffClick = !mRiffClick;
        sync();
    };

    record->OnClick([this] {
        auto& cmd = mSession.Commands();

        switch (mState.riffs.capture.phase)
        {
        case RiffCapture::Phase::Armed:
            cmd.StopRiffCapture(true);
            break;
        case RiffCapture::Phase::Recording:
            cmd.StopRiffCapture(false);
            break;
        default:
            cmd.ArmRiffCapture(mRiffTempo, mState.metronome.timeSigNum, mState.metronome.timeSigDen, mRiffCountIn,
                               mRiffClick);
            break;
        }
    });

    wave->onRange = [this, marks, repeat, lastRegionSend, sync, previewing](float start, float end, bool done) {
        *marks = {start, end};
        // A preview playing follows the markers, at most every 80 ms while they move.
        const double now = guitarfx::uiclient::LooperClockNow();

        if (previewing() && (done || now - *lastRegionSend >= 0.08))
        {
            *lastRegionSend = now;
            mSession.Commands().SetRiffPreviewRegion(start, end, *repeat);
        }

        sync();
    };
    preview->OnClick([this, marks, repeat, previewing] {
        if (previewing())
        {
            mSession.Commands().StopDemo();
        }
        else
        {
            mSession.Commands().PreviewCapturedRiff(marks->first, marks->second, *repeat);
        }
    });
    repeatButton->onClick = [this, marks, repeat, sync, previewing] {
        *repeat = !*repeat;

        if (previewing())
        {
            mSession.Commands().SetRiffPreviewRegion(marks->first, marks->second, *repeat);
        }

        sync();
    };
    crop->OnClick([this, marks] { mSession.Commands().TrimCapturedRiff(marks->first, marks->second); });
    favourite->onClick = [favouriteOn, sync] {
        *favouriteOn = !*favouriteOn;
        sync();
    };

    save->OnClick([this, title, tags, favouriteOn, marks, appendRiffId, sampleRate] {
        const auto& c = mState.riffs.capture;
        const auto name = TrimText(title->get_utf8());

        if (name.empty() || c.phase != RiffCapture::Phase::Complete || !c.hasAudio)
        {
            return;
        }

        guitarfx::uiclient::UiCommands::RiffSave riff;
        riff.riffId = appendRiffId;
        riff.title = name;
        riff.tags = guitarfx::uiclient::SplitRiffList(tags->get_utf8());
        riff.favorite = *favouriteOn;
        // A capture keeps the tempo it was armed with; an import or an edit says its own.
        riff.tempoBpm = c.tempoBpm > 0.0 ? c.tempoBpm : mRiffTempo;
        riff.timeSigNum = c.timeSigNum;
        riff.timeSigDen = c.timeSigDen;
        // Bars from the markers, as the web UI counts them.
        const double barSeconds = 60.0 / riff.tempoBpm * (4.0 / std::max(1, riff.timeSigDen)) * riff.timeSigNum;
        const double seconds = guitarfx::uiclient::RiffCaptureSeconds(c, sampleRate()) * (marks->second - marks->first);
        riff.bars = barSeconds > 0.0 ? std::max(1, static_cast<int>(std::lround(seconds / barSeconds))) : c.bars;
        riff.presetId = mState.activePresetId;
        mSession.Commands().SaveRiffTake(riff);
    });
    title->on_text = [sync](std::string_view) { sync(); };
    title->on_escape = [this] { CloseOverlay(); };
    tags->on_escape = [this] { CloseOverlay(); };

    auto hint = std::make_shared<Text>(
        mTheme, "Counts in, then records from the first note you play. Crop to Markers keeps only the marked part.",
        10.5f);
    hint->Colour(mTheme.TextSecondary());

    std::vector<Ptr> countRow{FixedWidth(80, label("COUNT-IN"))};

    for (const auto& b : countIn)
    {
        countRow.push_back(b);
        countRow.push_back(HGap(4));
    }

    countRow.push_back(HGap(12));
    countRow.push_back(Middle(click));
    countRow.push_back(Stretch());

    ShowDialog(
        {.title = editing != nullptr ? "Edit Riff Take" : "New Riff",
         .body =
             Col({FixedHeight(18, hint), VGap(10),
                  FixedHeight(34, Row({FixedWidth(80, label("TEMPO")), slower, HGap(4), FixedWidth(80, tempo), HGap(4),
                                       faster, HGap(8), fromMetronome, Stretch()})),
                  VGap(4), FixedHeight(34, Row(countRow)), VGap(10), FixedHeight(36, Row({record, HGap(12), status})),
                  VGap(8), wave, VGap(4), FixedHeight(18, Row({startText, Stretch(), endText})), VGap(6),
                  FixedHeight(34, Row({preview, HGap(6), Middle(repeatButton), HGap(6), crop, Stretch()})), VGap(12),
                  FixedHeight(18, label("SAVE")),
                  FixedHeight(40, Row({PadXY(0, 2, el::share(std::move(titleElement))), HGap(8), Middle(favourite)})),
                  FixedHeight(40, PadXY(0, 2, el::share(std::move(tagsElement))))}),
         .actions = {close, save},
         .width = 640.0f,
         .height = 570.0f,
         .padY = 14.0f});

    mInstrumentSubs.push_back(mSession.Client().Subscribe(Topic::Riffs, sync));
    mInstrumentSubs.push_back(mSession.Client().Subscribe(Topic::Demo, sync));
    // Closed while counting in or recording: the take is dropped, as in the web UI; a preview of
    // it stops too.
    mOnOverlayClosed = [this, previewing] {
        const auto phase = mState.riffs.capture.phase;

        if (phase == RiffCapture::Phase::Armed || phase == RiffCapture::Phase::Recording)
        {
            mSession.Commands().StopRiffCapture(true);
        }

        if (previewing())
        {
            mSession.Commands().StopDemo();
        }
    };
    sync();

    if (arm)
    {
        commands.ArmRiffCapture(mRiffTempo, mState.metronome.timeSigNum, mState.metronome.timeSigDen, mRiffCountIn,
                                mRiffClick);
    }
}

void Shell::OpenRiffLibrary()
{
    CloseOverlay();
    mSession.Commands().GetRiffLibrary();
    auto body = MakeSlot();
    auto signature = std::make_shared<std::string>("-");

    auto rebuild = [this, body, signature] {
        const auto& riffs = mState.riffs;
        const bool looperOn = guitarfx::uiclient::IsFeatureEnabled(mState, guitarfx::uiclient::Feature::Looper) &&
                              mState.looper.available;
        std::string s = mState.demo.previewId + (looperOn ? "|L" : "|-");

        for (const auto& r : riffs.riffs)
        {
            s += "|" + r.id + r.title + (r.favorite ? "*" : "") + (r.used ? "u" : "") + std::to_string(r.takes.size()) +
                 JoinList(r.tags);
        }

        if (s == *signature)
        {
            return;
        }

        *signature = s;

        std::vector<Ptr> rows;

        if (riffs.riffs.empty())
        {
            auto none = std::make_shared<Text>(
                mTheme, riffs.known ? "No riffs yet. Record one with New Riff." : "Loading...", 12.0f);
            none->Colour(mTheme.TextSecondary());
            rows.push_back(FixedHeight(40, none));
        }

        for (const auto& riff : riffs.riffs)
        {
            const auto* take = riff.PreferredTake();
            const std::string takeId = take != nullptr ? take->id : std::string();
            auto name = std::make_shared<Text>(mTheme, (riff.favorite ? "\xE2\x98\x85 " : "") + riff.title, 13.0f,
                                               Weight::SemiBold);
            auto meta = std::make_shared<Text>(mTheme, guitarfx::uiclient::RiffMeta(riff), 10.5f);
            meta->Colour(mTheme.TextSecondary());
            auto summary = std::make_shared<Text>(
                mTheme, take != nullptr ? guitarfx::uiclient::RiffTakeSummary(*take) : std::string("No takes"), 10.5f);
            summary->Colour(mTheme.TextMuted());

            const auto button = [this](const std::string& text, std::function<void()> onClick) {
                auto b = std::make_shared<Button>(mTheme, text, std::move(onClick), Button::Style::Plain);
                b->MinWidth(54).TextSize(11.0f);
                return b;
            };
            const bool playing = !takeId.empty() && mState.demo.playing && mState.demo.previewId == takeId;
            auto play = button(playing ? "Stop" : "Play", [this, takeId, playing] {
                if (playing)
                {
                    mSession.Commands().StopDemo();
                }
                else
                {
                    mSession.Commands().PreviewRiffTake(takeId);
                }
            });
            play->Enabled(!takeId.empty());
            auto star =
                button(riff.favorite ? "\xE2\x98\x85" : "\xE2\x98\x86",
                       [this, id = riff.id, on = riff.favorite] { mSession.Commands().SetRiffFavorite(id, !on); });
            star->MinWidth(34);
            auto used =
                button("Used", [this, id = riff.id, on = riff.used] { mSession.Commands().MarkRiffUsed(id, !on); });
            used->Dot(true).On(riff.used);
            auto edit = button("Edit", [this, id = riff.id, takeId] {
                OpenRiffCapture(false, id);
                mSession.Commands().LoadRiffTakeForEdit(takeId);
            });
            edit->Enabled(!takeId.empty());
            std::vector<Ptr> actions{play, HGap(4), star, HGap(4), used, HGap(4), edit};

            if (looperOn)
            {
                auto loop = button("Loop", [this, takeId] { mSession.Commands().AddRiffAsLooperLayer(takeId); });
                loop->Enabled(!takeId.empty());
                actions.push_back(HGap(4));
                actions.push_back(loop);
            }

            auto remove = button("Delete", [this, id = riff.id, titleText = riff.title] {
                OpenConfirm("Delete riff", "\"" + titleText + "\" and all its takes are deleted.", "Delete",
                            [this, id] {
                                mSession.Commands().DeleteRiff(id);
                                mSession.View().post([this, alive = mAlive] {
                                    if (*alive)
                                    {
                                        OpenRiffLibrary();
                                    }
                                });
                            });
            });
            actions.push_back(HGap(4));
            actions.push_back(remove);

            rows.push_back(FixedHeight(20, name));
            rows.push_back(FixedHeight(16, meta));
            rows.push_back(FixedHeight(16, summary));
            rows.push_back(VGap(4));
            rows.push_back(FixedHeight(30, Row(actions)));
            rows.push_back(VGap(6));
            rows.push_back(FixedHeight(1, Fill(mTheme.Text().opacity(0.08f), std::make_shared<el::element>())));
            rows.push_back(VGap(8));
        }

        Fill(*body, VScroll(Col(rows)));
        mSession.View().layout();
        mSession.View().refresh();
    };

    auto path = std::make_shared<Text>(
        mTheme, mState.riffs.path.empty() ? std::string() : "Kept in " + mState.riffs.path, 10.5f);
    path->Colour(mTheme.TextMuted());
    auto newRiff =
        std::make_shared<Button>(mTheme, "New Riff", [this] { OpenRiffCapture(false); }, Button::Style::Primary);
    newRiff->MinWidth(100);
    const auto view = ViewSize();
    ShowDialog({.title = "Riff Library",
                .body = Col({FixedHeight(18, path), VGap(10), body}),
                .headerExtras = {newRiff},
                .width = 640.0f,
                .height = std::min(560.0f, std::max(260.0f, view.y - 60.0f)),
                .padY = 12.0f});

    mInstrumentSubs.push_back(mSession.Client().Subscribe(Topic::Riffs, rebuild));
    mInstrumentSubs.push_back(mSession.Client().Subscribe(Topic::Demo, rebuild));
    mInstrumentSubs.push_back(mSession.Client().Subscribe(Topic::Looper, rebuild));
    rebuild();
}
} // namespace nanoq::ui
