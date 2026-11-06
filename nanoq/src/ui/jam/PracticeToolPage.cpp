// The Jam tab's Practice Tool, as the web UI draws it (the practice-tool section of
// ui-components/panels/jam-panel.html, css/practice-tool.css): the project bar, the track's
// waveform with its loops, the transport, the loop list beside the four faders, and the links kept
// with a saved project. The EQ opens in its own dialog, as the web UI's modal.
//
// The engine plays the track (PracticeToolService); loops, faders, the EQ and projects are the
// shared uiclient::PracticeTool's, which keeps them in the web UI's own settings. The playhead,
// the clock and the faders are bound (mLive) and move in place; the section is rebuilt only
// when what it lists changes, and never under a drag on the waveform or a fader.

#include "ui/shell/Shell.h"

#include "uiclient/Looper.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace nanoq::ui
{
namespace practice = guitarfx::uiclient::practice;
using practice::Fader;

namespace
{
constexpr float kWaveHeight = 128.0f;
constexpr float kHandleReach = 8.0f; // how near a press must be to an edge to move it

/// The track's two channels as lanes, the loops over them, and the playhead (the web UI's
/// waveform/render.ts as the Practice Tool calls it). A press and release seeks; a sweep marks a
/// range for a new loop; the picked loop's edges (or the marked range's) drag.
class PracticeWave final : public el::element, public Drawn
{
  public:
    struct Band
    {
        double start = 0.0, end = 0.0;
        bool active = false;
        bool operator==(const Band&) const = default;
    };

    explicit PracticeWave(const Theme& theme) : mTheme(theme)
    {
    }

    void Track(std::vector<float> left, std::vector<float> right, double duration)
    {
        mLeft = std::move(left);
        mRight = std::move(right);
        mDuration = duration;
    }

    /// The loops, and the range marked for the next one. Ignored while a drag is changing them.
    void Ranges(std::vector<Band> bands, std::optional<std::pair<double, double>> candidate)
    {
        if (mMode != Mode::None)
        {
            return;
        }

        mBands = std::move(bands);
        mCandidate = std::move(candidate);
    }

    void Playhead(double seconds)
    {
        mPlayhead = seconds;
    }

    void Marker(std::optional<double> seconds)
    {
        mMarker = seconds;
    }

    [[nodiscard]] bool Dragging() const
    {
        return mMode != Mode::None;
    }

    std::function<void(double seconds)> onSeek;
    /// The picked loop's edges or the marked range moved; `done` once the drag ends.
    std::function<void(double start, double end, bool done)> onRange;
    /// A sweep begun outside the picked loop: it lets the loop go first.
    std::function<void()> onSweepOutside;

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{120.0f, kWaveHeight}, {el::full_extent, kWaveHeight}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        cnv.fill_style(mTheme.Pick(el::rgba(0, 0, 0, 80), el::rgba(0, 0, 0, 18)));
        cnv.fill_round_rect(b, 8.0f);

        if (!HasAudio())
        {
            FillText(cnv, "Drop a WAV, AIFF, or MP3 file here, or use Browse File...",
                     Font(Weight::Regular).size(12.0f), mTheme.TextMuted(), b, HAlign::Centre);
            return;
        }

        const auto x = [&](double seconds) {
            return b.left + static_cast<float>(std::clamp(seconds / mDuration, 0.0, 1.0)) * b.width();
        };

        // The loops not picked, faintly; the picked one tinted, the marked range lighter.
        for (const auto& band : mBands)
        {
            if (!band.active)
            {
                cnv.fill_style(mTheme.Text().opacity(0.05f));
                cnv.fill_rect(el::rect{x(band.start), b.top, x(band.end), b.bottom});
            }
        }

        // Two lanes, each mirrored about its own middle (a backing track is genuinely stereo).
        const auto lane = [&](const std::vector<float>& peaks, float top, float bottom) {
            if (peaks.empty())
            {
                return;
            }

            const float mid = (top + bottom) * 0.5f, half = (bottom - top) * 0.5f - 2.0f;
            const float step = b.width() / static_cast<float>(peaks.size());
            cnv.fill_style(mTheme.Accent().opacity(0.8f));

            for (std::size_t i = 0; i < peaks.size(); ++i)
            {
                const float px = b.left + step * static_cast<float>(i);
                const float a = std::max(0.5f, std::clamp(peaks[i], 0.0f, 1.0f) * half);
                cnv.fill_rect(el::rect{px, mid - a, px + std::max(1.0f, step - 0.4f), mid + a});
            }
        };
        const float split = b.top + b.height() * 0.5f;
        lane(mLeft, b.top + 3.0f, split - 1.0f);
        lane(mRight.empty() ? mLeft : mRight, split + 1.0f, b.bottom - 3.0f);

        const auto range = [&](double start, double end, bool active) {
            const float x0 = x(start), x1 = x(end);
            cnv.fill_style(active ? mTheme.Accent().opacity(0.20f) : mTheme.Text().opacity(0.10f));
            cnv.fill_rect(el::rect{x0, b.top, x1, b.bottom});

            // Outside a picked loop is shaded, so the part that plays stands out.
            if (active)
            {
                cnv.fill_style(el::rgba(0, 0, 0, 70));
                cnv.fill_rect(el::rect{b.left, b.top, x0, b.bottom});
                cnv.fill_rect(el::rect{x1, b.top, b.right, b.bottom});
            }

            const auto edge = active ? mTheme.Accent() : mTheme.Text().opacity(0.75f);

            for (const float ex : {x0, x1})
            {
                cnv.fill_style(edge);

                if (active)
                {
                    cnv.fill_rect(el::rect{ex - 1.0f, b.top, ex + 1.0f, b.bottom});
                }
                else
                {
                    // Dashed while it is only a marked range.
                    for (float y = b.top; y < b.bottom; y += 8.0f)
                    {
                        cnv.fill_rect(el::rect{ex - 0.75f, y, ex + 0.75f, std::min(b.bottom, y + 4.0f)});
                    }
                }

                cnv.fill_round_rect(el::rect{ex - 4.0f, b.top, ex + 4.0f, b.top + 12.0f}, 2.0f);
            }
        };

        if (const auto* active = ActiveBand())
        {
            range(active->start, active->end, true);
        }
        else if (mCandidate)
        {
            range(mCandidate->first, mCandidate->second, false);
        }

        // Where Jump Back returns to, when a click set it.
        if (mMarker)
        {
            const float mx = std::round(x(*mMarker)) + 0.5f;
            cnv.fill_style(mTheme.Text().opacity(0.55f));

            for (float y = b.top + 2.0f; y < b.bottom; y += 6.0f)
            {
                cnv.fill_rect(el::rect{mx - 0.5f, y, mx + 0.5f, std::min(b.bottom, y + 3.0f)});
            }
        }

        const float px = std::round(x(mPlayhead)) + 0.5f;
        cnv.fill_style(mTheme.Ink());
        cnv.fill_rect(el::rect{px - 0.75f, b.top + 1.0f, px + 0.75f, b.bottom - 1.0f});
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (btn.state != el::mouse_button::left)
        {
            return false;
        }

        if (!HasAudio())
        {
            return true;
        }

        const double seconds = At(ctx, btn.pos);

        if (btn.down)
        {
            mAnchor = seconds;
            mAnchorX = btn.pos.x;
            mMode = Mode::Pending;

            // An edge of what can be edited, near enough, is taken by the press.
            if (const auto edit = Editable())
            {
                const float x0 = X(ctx, edit->first), x1 = X(ctx, edit->second);
                const float d0 = std::abs(btn.pos.x - x0), d1 = std::abs(btn.pos.x - x1);

                if (std::min(d0, d1) <= kHandleReach)
                {
                    mMode = Mode::Edge;
                    mEdge = d0 <= d1 ? 0 : 1;
                    mStart = edit->first;
                    mEnd = edit->second;
                }
            }

            return true;
        }

        const auto mode = mMode;
        mMode = Mode::None;

        switch (mode)
        {
        case Mode::Pending:

            if (onSeek)
            {
                onSeek(seconds);
            }

            break;
        case Mode::Sweep:
        case Mode::Edge:

            if (onRange)
            {
                onRange(mStart, mEnd, true);
            }

            break;
        default:
            break;
        }

        ctx.view.refresh(ctx);
        return true;
    }

    void drag(const el::context& ctx, el::mouse_button btn) override
    {
        if (!HasAudio())
        {
            return;
        }

        const double seconds = At(ctx, btn.pos);

        if (mMode == Mode::Pending && std::abs(btn.pos.x - mAnchorX) > 4.0f)
        {
            const auto* active = ActiveBand();

            if (active && mAnchor >= active->start && mAnchor <= active->end)
            {
                // A sweep inside the picked loop is not a request for a new one.
                mMode = Mode::Ignored;
            }
            else
            {
                if (active)
                {
                    for (auto& band : mBands)
                    {
                        band.active = false;
                    }

                    if (onSweepOutside)
                    {
                        onSweepOutside();
                    }
                }

                mMode = Mode::Sweep;
            }
        }

        if (mMode == Mode::Sweep)
        {
            mStart = std::min(mAnchor, seconds);
            mEnd = std::max(mAnchor, seconds);
            mCandidate = std::make_pair(mStart, mEnd);

            if (onRange)
            {
                onRange(mStart, mEnd, false);
            }
        }
        else if (mMode == Mode::Edge)
        {
            // An edge dragged past the other swaps with it.
            (mEdge == 0 ? mStart : mEnd) = seconds;

            if (mStart > mEnd)
            {
                std::swap(mStart, mEnd);
                mEdge = 1 - mEdge;
            }

            if (auto* active = ActiveBand())
            {
                active->start = mStart, active->end = mEnd;
            }
            else
            {
                mCandidate = std::make_pair(mStart, mEnd);
            }

            if (onRange)
            {
                onRange(mStart, mEnd, false);
            }
        }

        ctx.view.refresh(ctx);
    }

    bool wants_control() const override
    {
        return true;
    }

  private:
    enum class Mode
    {
        None,
        Pending, // pressed, not moved yet: a seek on release
        Sweep,   // marking a range for a new loop
        Edge,    // moving an edge of the picked loop or the marked range
        Ignored, // a sweep inside the picked loop
    };

    [[nodiscard]] bool HasAudio() const
    {
        return mDuration > 0.0 && !mLeft.empty();
    }

    [[nodiscard]] Band* ActiveBand()
    {
        const auto it = std::find_if(mBands.begin(), mBands.end(), [](const Band& band) { return band.active; });
        return it != mBands.end() ? &*it : nullptr;
    }

    [[nodiscard]] std::optional<std::pair<double, double>> Editable()
    {
        if (const auto* active = ActiveBand())
        {
            return std::make_pair(active->start, active->end);
        }

        return mCandidate;
    }

    [[nodiscard]] double At(const el::context& ctx, el::point p) const
    {
        return std::clamp(static_cast<double>((p.x - ctx.bounds.left) / std::max(1.0f, ctx.bounds.width())), 0.0, 1.0) *
               mDuration;
    }

    [[nodiscard]] float X(const el::context& ctx, double seconds) const
    {
        return ctx.bounds.left + static_cast<float>(std::clamp(seconds / mDuration, 0.0, 1.0)) * ctx.bounds.width();
    }

    const Theme& mTheme;
    std::vector<float> mLeft, mRight;
    double mDuration = 0.0;
    std::vector<Band> mBands;
    std::optional<std::pair<double, double>> mCandidate;
    double mPlayhead = 0.0;
    std::optional<double> mMarker;
    Mode mMode = Mode::None;
    int mEdge = 0;
    double mAnchor = 0.0, mStart = 0.0, mEnd = 0.0;
    float mAnchorX = 0.0f;
};

/// What the waveform shows over the track, as its binding compares it each tick: the playhead is
/// cut to 1/2000 of the track, so the wave is redrawn only when something on it moves a pixel.
struct WaveShown
{
    double head = 0.0;
    std::vector<PracticeWave::Band> bands;
    std::optional<std::pair<double, double>> candidate;
    std::optional<double> marker;
    bool operator==(const WaveShown&) const = default;
};

Ptr Heading(const Theme& theme, const std::string& text)
{
    auto t = std::make_shared<Text>(theme, Upper(text), 9.5f, Weight::SemiBold);
    t->Colour(theme.TextSecondary());
    return t;
}

std::string LoopRange(const practice::Loop& loop)
{
    return practice::FormatClock(loop.startSec) + "\xE2\x80\x93" + practice::FormatClock(loop.endSec);
}
} // namespace

std::string Shell::PracticeToolSignature() const
{
    if (!mPractice)
    {
        return {};
    }

    const auto& t = mPractice->Track();
    std::string s = t.filePath + "|" + std::to_string(t.loads) + (t.playing ? "|p" : "|") +
                    (mPractice->Candidate() ? "c" : "") + (practice::IsEqShaping(mPractice->GetEq()) ? "e" : "") + "|" +
                    mPractice->SelectedProjectId();

    for (const auto& loop : mPractice->Loops())
    {
        s += "|" + loop.id + ":" + loop.name + ":" + LoopRange(loop);
    }

    if (const auto* active = mPractice->ActiveLoop())
    {
        s += "|a" + active->id;
    }

    if (const auto& deleted = mPractice->DeletedLoop())
    {
        s += "|d" + deleted->id;
    }

    for (const auto& project : mPractice->Projects())
    {
        s += "|P" + project.id + ":" + project.name;

        for (const auto& link : project.links)
        {
            s += "," + link.id;
        }
    }

    return s;
}

Ptr Shell::BuildPracticeTool(float width)
{
    auto& tool = *mPractice;
    const auto& track = tool.Track();
    const bool hasAudio = tool.HasAudio();
    const auto position = [this] { return mPractice->PositionNow(guitarfx::uiclient::LooperClockNow()); };
    std::vector<Ptr> body;

    // ── The project bar: the saved projects, Save, and Browse File ──
    const auto projects = tool.Projects();
    const auto selected = std::find_if(projects.begin(), projects.end(), [&tool](const practice::Project& p) {
        return p.id == tool.SelectedProjectId();
    });
    auto projectButton = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
    projectButton->Size(180, 30)
        .Label(selected != projects.end() ? selected->name
               : projects.empty()         ? std::string("No saved projects")
                                          : std::string("Projects"),
               11.0f)
        .Caret(true);
    projectButton->onClick = [this, b = projectButton.get()] { OpenPracticeProjects(b->Bounds()); };
    auto save = std::make_shared<IconButton>(mTheme, mIcons, "ui-save", IconButton::Look::Boxed);
    save->Size(80, 30).IconSize(13).Label("Save", 11.0f).Disabled(!hasAudio);
    save->onClick = [this, b = save.get()] {
        // Under the selected project's name (saving over it), or the track's title.
        const auto saveAs = [this](bool withPreset) {
            const auto projects = mPractice->Projects();
            const auto it = std::find_if(projects.begin(), projects.end(), [this](const practice::Project& p) {
                return p.id == mPractice->SelectedProjectId();
            });
            PostPrompt(withPreset ? "Save project with the preset" : "Save project",
                       it != projects.end() ? it->name : mPractice->SuggestedProjectName(), "Save",
                       [this, withPreset](const std::string& text) {
                           if (!mPractice->SaveProject(text, withPreset))
                           {
                               ShowToast("Project not saved", "Give it a name, with a track loaded.", true);
                           }
                           else
                           {
                               ShowToast("Project saved", text, false);
                           }
                       });
        };
        OpenDropdown(b->Bounds(),
                     {{"Save project...", false, false, [saveAs] { saveAs(false); }},
                      {"Save with the current preset...", false, false, [saveAs] { saveAs(true); }}},
                     250.0f);
    };
    auto browse = std::make_shared<IconButton>(mTheme, mIcons, "ui-folder", IconButton::Look::Boxed);
    browse->Size(120, 30).IconSize(13).Label("Browse File...", 11.0f);
    browse->onClick = [this] {
        // A new track resets the faders: ask first when one is loaded.
        if (!mPractice->HasAudio())
        {
            mPractice->Browse();
            return;
        }

        OpenConfirm("Load a new file",
                    "Volume, Balance, Speed and Pitch go back to their defaults. Save a project first to keep them.",
                    "Load New File", [this] { mPractice->Browse(); });
    };
    body.push_back(FixedHeight(30, Row({projectButton, HGap(6), save, Stretch(), browse})));
    body.push_back(VGap(10));

    // ── The waveform ──
    auto wave = std::make_shared<PracticeWave>(mTheme);
    wave->Track(track.peaksL, track.peaksR, track.durationSec);
    wave->onSeek = [this](double seconds) { mPractice->ClickAt(seconds); };
    wave->onSweepOutside = [this] { mPractice->ReleaseActiveLoop(); };
    wave->onRange = [this](double start, double end, bool done) { mPractice->SetRange(start, end, done); };
    mLive.Bind(
        wave,
        [this, position, step = std::max(1.0, track.durationSec) / 2000.0] {
            WaveShown shown{
                std::floor(position() / step) * step, {}, mPractice->Candidate(), mPractice->JumpBackMarker()};
            const auto* active = mPractice->ActiveLoop();

            for (const auto& loop : mPractice->Loops())
            {
                shown.bands.push_back({loop.startSec, loop.endSec, active && active->id == loop.id});
            }

            return shown;
        },
        [](PracticeWave& w, const WaveShown& shown) {
            w.Ranges(shown.bands, shown.candidate);
            w.Playhead(shown.head);
            w.Marker(shown.marker);
        });
    body.push_back(FixedHeight(kWaveHeight, wave));
    body.push_back(VGap(6));

    // ── The track and where it is, then + Add Loop for a marked range ──
    auto clock =
        std::make_shared<Text>(mTheme, hasAudio ? track.title : std::string("No file loaded"), 11.5f, Weight::Medium);
    clock->Colour(hasAudio ? mTheme.Ink() : mTheme.TextMuted());

    if (hasAudio)
    {
        mLive.Bind(
            clock, [position] { return static_cast<long>(std::floor(position())); },
            [title = track.title, total = practice::FormatClock(track.durationSec)](Text& t, long seconds) {
                t.Set(title + "   " + practice::FormatClock(static_cast<double>(seconds)) + " / " + total);
            });
    }

    std::vector<Ptr> info{clock};

    if (hasAudio && tool.Candidate() && !tool.ActiveLoop())
    {
        auto add = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Accent);
        add->Size(90, 24).Label("+ Add Loop", 10.5f);
        add->onClick = [this] { mPractice->AddLoop(); };
        info.push_back(add);
    }

    body.push_back(FixedHeight(24, Row(info)));
    body.push_back(VGap(8));

    // ── The transport ──
    auto play =
        std::make_shared<IconButton>(mTheme, mIcons, track.playing ? "ui-pause" : "ui-play", IconButton::Look::Boxed);
    play->Size(40, 32).IconSize(14).Disabled(!hasAudio);
    play->onClick = [this] { mPractice->PlayPause(); };
    auto stop = std::make_shared<IconButton>(mTheme, mIcons, "ui-stop", IconButton::Look::Boxed);
    stop->Size(40, 32).IconSize(13).Disabled(!hasAudio);
    stop->onClick = [this] { mPractice->Stop(); };
    auto back = std::make_shared<IconButton>(mTheme, mIcons, "ui-jump-back", IconButton::Look::Boxed);
    back->Size(40, 32).IconSize(14).Disabled(!hasAudio);
    back->onClick = [this] { mPractice->JumpBack(); };
    auto eq = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
    // Lit only when the EQ is on and doing something.
    eq->Size(48, 32).Label("EQ", 11.0f).Lit(practice::IsEqShaping(tool.GetEq()));

    if (practice::IsEqShaping(tool.GetEq()))
    {
        eq->Ink(mTheme.Accent());
    }

    eq->onClick = [this] { OpenPracticeEq(); };
    std::vector<Ptr> transport{play, HGap(6), stop, HGap(6), back, HGap(12), eq, HGap(12)};

    if (const auto* active = tool.ActiveLoop(); active && hasAudio)
    {
        auto status = std::make_shared<Text>(mTheme, "Looping \"" + active->name + "\"", 11.0f, Weight::Medium);
        status->Colour(mTheme.Accent());
        transport.push_back(status);
    }
    else
    {
        transport.push_back(Stretch());
    }

    body.push_back(FixedHeight(32, Row(transport)));
    body.push_back(VGap(14));

    // ── The loops ──
    std::vector<Ptr> loops;
    {
        auto newLoop = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        newLoop->Size(90, 24).Label("+ New Loop", 10.5f).Disabled(!hasAudio);
        newLoop->onClick = [this] { mPractice->AddLoop(); };
        loops.push_back(FixedHeight(24, Row({Heading(mTheme, "Loops"), Stretch(), newLoop})));
        loops.push_back(VGap(6));

        if (const auto& deleted = tool.DeletedLoop())
        {
            auto text = std::make_shared<Text>(mTheme, "Deleted \"" + deleted->name + "\".", 11.0f);
            text->Colour(mTheme.TextSecondary());
            auto undo = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
            undo->Size(56, 22).Label("Undo", 10.5f);
            undo->onClick = [this] { mPractice->UndoDelete(); };
            loops.push_back(
                FixedHeight(28, Rounded(mTheme.Text().opacity(0.05f), 6.0f, Pad(8, 3, 4, 3, Row({text, undo})))));
            loops.push_back(VGap(4));
        }

        if (tool.Loops().empty())
        {
            auto none = std::make_shared<Text>(
                mTheme, hasAudio ? "No loops saved for this file yet." : "Load a track to mark loops on it.", 11.0f);
            none->Colour(mTheme.TextMuted());
            loops.push_back(FixedHeight(28, none));
        }

        for (const auto& loop : tool.Loops())
        {
            const bool active = tool.ActiveLoop() && tool.ActiveLoop()->id == loop.id;
            const bool playingThis = active && track.playing;
            const auto id = loop.id;
            auto pick = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Ghost);
            // U+25CF / U+25CB, as the web UI's row.
            pick->Size(26, 26).Label(active ? "\xE2\x97\x8F" : "\xE2\x97\x8B", 12.0f);

            if (active)
            {
                pick->Ink(mTheme.Accent());
            }

            pick->onClick = [this, id] { mPractice->SelectLoop(id); };
            auto playLoop = std::make_shared<IconButton>(mTheme, mIcons, playingThis ? "ui-stop" : "ui-play",
                                                         IconButton::Look::Ghost);
            playLoop->Size(26, 26).IconSize(11);
            playLoop->onClick = [this, id] { mPractice->ToggleLoopPlayback(id); };
            auto name = std::make_shared<Text>(mTheme, loop.name, 11.5f, active ? Weight::SemiBold : Weight::Regular);
            name->Colour(active ? mTheme.Ink() : mTheme.Text());
            auto range = std::make_shared<Text>(mTheme, LoopRange(loop), 10.5f);
            range->Colour(mTheme.TextSecondary()).Fixed(true);
            auto edit = std::make_shared<IconButton>(mTheme, mIcons, "ui-edit", IconButton::Look::Ghost);
            edit->Size(26, 26).IconSize(12);
            edit->onClick = [this, id, e = edit.get()] { OpenPracticeLoopMenu(id, e->Bounds()); };
            auto remove = std::make_shared<IconButton>(mTheme, mIcons, "ui-trash", IconButton::Look::Ghost);
            remove->Size(26, 26).IconSize(12);
            remove->onClick = [this, id] { mPractice->DeleteLoop(id); };
            auto row = Row({pick, playLoop, HGap(4), name, HGap(6), range, HGap(4), edit, remove});
            loops.push_back(FixedHeight(30, active ? Rounded(mTheme.Accent().opacity(0.10f), 6.0f, row) : row));
        }
    }

    // ── The faders: the default in the middle, double click to reset ──
    std::vector<Ptr> faders{FixedHeight(24, Heading(mTheme, "Track")), VGap(2)};

    for (const auto fader : {Fader::Volume, Fader::Balance, Fader::Speed, Fader::Pitch})
    {
        const auto& spec = practice::SpecFor(fader);
        auto slider = std::make_shared<ParamSlider>(mTheme, spec.label);
        // It follows a loop picked or a project recalled; a dragged one leads.
        mLive.Bind(
            slider, [this, fader] { return practice::FaderValue(mPractice->Faders(), fader); },
            [fader](ParamSlider& s, double value) {
                s.Update(practice::FaderToSlider(fader, value), practice::FormatFader(fader, value));
            });
        slider->onChange = [this, fader, s = slider.get()](double position) {
            const double v = practice::SliderToFader(fader, position);
            mPractice->SetFader(fader, v, false);
            s->Update(position, practice::FormatFader(fader, v));
        };
        slider->onRelease = [this, fader] {
            mPractice->SetFader(fader, practice::FaderValue(mPractice->Faders(), fader), true);
        };
        slider->onReset = [this, fader] { mPractice->SetFader(fader, practice::SpecFor(fader).def, true); };
        faders.push_back(slider);
    }

    auto hint = std::make_shared<Text>(mTheme, "Speed and pitch are independent.", 10.5f);
    hint->Colour(mTheme.TextMuted());
    faders.push_back(FixedHeight(20, hint));

    if (width >= 620.0f)
    {
        const float faderWidth = std::min(300.0f, width * 0.42f);
        body.push_back(Row({Col(loops), HGap(18), FixedWidth(faderWidth, Col(faders))}));
    }
    else
    {
        body.push_back(Col(loops));
        body.push_back(VGap(14));
        body.push_back(Col(faders));
    }

    // ── The links kept with the saved project ──
    if (selected != projects.end())
    {
        const auto projectId = selected->id;
        body.push_back(VGap(16));
        auto add = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        add->Size(90, 24).Label("Add Link...", 10.5f);
        add->onClick = [this, projectId] { AddPracticeLink(projectId); };
        body.push_back(FixedHeight(24, Row({Heading(mTheme, "Links"), Stretch(), add})));
        body.push_back(VGap(4));

        if (selected->links.empty())
        {
            auto none = std::make_shared<Text>(
                mTheme, "Keep the tablature, a lesson or the lyrics with \"" + selected->name + "\".", 11.0f);
            none->Colour(mTheme.TextMuted());
            body.push_back(FixedHeight(26, none));
        }

        for (const auto& link : selected->links)
        {
            const auto url = link.url;
            const auto linkId = link.id;
            auto open = std::make_shared<IconButton>(mTheme, mIcons, "ui-share", IconButton::Look::Ghost);
            open->Size(26, 26).IconSize(12);
            open->onClick = [this, url] { mSession.Commands().OpenUrl(url); };
            auto title = std::make_shared<Text>(mTheme, practice::DescribeLink(link), 11.5f);
            title->Colour(mTheme.Ink());
            auto address = std::make_shared<Text>(mTheme, url, 10.5f);
            address->Colour(mTheme.TextMuted());
            auto remove = std::make_shared<IconButton>(mTheme, mIcons, "ui-trash", IconButton::Look::Ghost);
            remove->Size(26, 26).IconSize(12);
            remove->onClick = [this, projectId, linkId] { mPractice->RemoveLink(projectId, linkId); };
            body.push_back(FixedHeight(28, Row({open, HGap(4), FixedWidth(std::min(220.0f, width * 0.35f), title),
                                                HGap(8), address, remove})));
        }
    }

    body.push_back(VGap(20));
    return Col(body);
}

void Shell::OpenPracticeProjects(el::rect anchor)
{
    const auto projects = mPractice->Projects();
    std::vector<MenuItem> items;

    if (projects.empty())
    {
        items.push_back({"No saved projects", false, true, {}});
    }

    for (const auto& project : projects)
    {
        const auto id = project.id;
        const auto name = project.name;
        MenuItem item{project.name, project.id == mPractice->SelectedProjectId(), false, [this, id, name] {
                          if (!mPractice->LoadProject(id))
                          {
                              ShowToast("Open the track first",
                                        "\"" + name +
                                            "\" was made from a dropped-in file: open it, then load the project.",
                                        true);
                          }
                      }};
        item.detail = project.fileTitle + (project.presetName.empty() ? "" : " \xC2\xB7 " + project.presetName);
        items.push_back(std::move(item));
    }

    const auto selected = std::find_if(projects.begin(), projects.end(), [this](const practice::Project& p) {
        return p.id == mPractice->SelectedProjectId();
    });

    if (selected != projects.end())
    {
        const auto id = selected->id;
        const auto name = selected->name;
        items.push_back({"Delete \"" + name + "\"...", false, false, [this, id, name] {
                             OpenConfirm("Delete project",
                                         "\"" + name + "\" and its links are deleted. The track itself stays.",
                                         "Delete", [this, id] { mPractice->DeleteProject(id); });
                         }});
    }

    OpenDropdown(anchor, std::move(items), 300.0f);
}

void Shell::OpenPracticeLoopMenu(const std::string& loopId, el::rect anchor)
{
    const auto& loops = mPractice->Loops();
    const auto it =
        std::find_if(loops.begin(), loops.end(), [&loopId](const practice::Loop& l) { return l.id == loopId; });

    if (it == loops.end())
    {
        return;
    }

    const auto loop = *it;
    std::vector<MenuItem> items{
        {"Rename...", false, false,
         [this, loop] {
             PostPrompt("Rename loop", loop.name, "Rename",
                        [this, id = loop.id](const std::string& name) { mPractice->RenameLoop(id, name); });
         }},
        {"Set times...", false, false,
         [this, loop] {
             char text[64];
             std::snprintf(text, sizeof(text), "%.2f - %.2f", loop.startSec, loop.endSec);
             PostPrompt("Start - end, in seconds", text, "Set", [this, id = loop.id](const std::string& value) {
                 double start = 0.0, end = 0.0;
                 char dash = 0;

                 if (std::sscanf(value.c_str(), " %lf %c %lf", &start, &dash, &end) == 3 && end > start)
                 {
                     mPractice->SetLoopTimes(id, start, end);
                 }
                 else
                 {
                     ShowToast("Times not set", "Write them as start - end, such as 12.5 - 20.", true);
                 }
             });
         }},
        {"Name it", false, true, {}}};
    // The song sections, numbered past those already used.
    std::vector<std::string> names;

    for (const auto& l : loops)
    {
        names.push_back(l.name);
    }

    for (const auto& base : practice::kLoopNameTemplates)
    {
        const auto name = practice::SuggestLoopTemplateName(base, names);
        items.push_back({name, false, false, [this, id = loop.id, name] { mPractice->RenameLoop(id, name); }});
    }

    OpenDropdown(anchor, std::move(items), 180.0f);
}

void Shell::AddPracticeLink(const std::string& projectId)
{
    OpenPrompt("Link address (the tablature, a lesson, the lyrics)", "", "Next",
               [this, projectId](const std::string& url) {
                   if (!practice::NormalizeLinkUrl(url))
                   {
                       ShowToast("Not a web address", "Links open in the browser: http or https only.", true);
                       return;
                   }

                   PostPrompt("Title (optional)", "", "Add Link", [this, projectId, url](const std::string& title) {
                       const auto result = mPractice->AddLink(projectId, title, url);

                       if (result == "too-many")
                       {
                           ShowToast("Link not added", "A project keeps up to 30 links.", true);
                       }
                       else if (result != "added")
                       {
                           ShowToast("Link not added", "Save the project first.", true);
                       }
                   });
               });
}

bool Shell::DropPracticeTrack(const std::vector<std::string>& paths)
{
    if (mPage != Page::Jam || mJamSection != JamSection::Practice || !mPractice)
    {
        return false;
    }

    for (const auto& path : paths)
    {
        const auto lower = Lower(path);

        for (const char* ext : {".wav", ".aiff", ".aif", ".mp3"})
        {
            if (lower.size() > std::strlen(ext) &&
                lower.compare(lower.size() - std::strlen(ext), std::strlen(ext), ext) == 0)
            {
                // The path straight through: unlike the web view, a native drop has it. A new
                // track resets the faders: ask first when one is loaded, as Browse does.
                if (!mPractice->HasAudio())
                {
                    mSession.Commands().LoadPracticeToolFile(path);
                }
                else
                {
                    OpenConfirm(
                        "Load a new file",
                        "Volume, Balance, Speed and Pitch go back to their defaults. Save a project first to keep them.",
                        "Load New File", [this, path] { mSession.Commands().LoadPracticeToolFile(path); });
                }

                return true;
            }
        }
    }

    return false;
}
} // namespace nanoq::ui
