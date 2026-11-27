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

#include "ui/dialogs/DialogChrome.h"
#include "ui/input/Focus.h"
#include "ui/performance/MetronomeWidgets.h"
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
constexpr float kWaveHeight = 200.0f; // .practice-tool-waveform
constexpr float kHandleReach = 8.0f;  // how near a press must be to an edge to move it

/// The track's two channels as lanes, the loops over them, and the playhead (the web UI's
/// waveform/render.ts as the Practice Tool calls it). A press and release seeks; a sweep marks a
/// range for a new loop; the picked loop's edges (or the marked range's) drag.
class PracticeWave final : public el::element, public Drawn, public Focusable
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

                // With focus, the edge the keys move is marked.
                if (HasFocus() && ex == (mKeyEdge == 0 ? x0 : x1))
                {
                    cnv.fill_rect(el::rect{ex - 2.0f, b.bottom - 12.0f, ex + 2.0f, b.bottom});
                }

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

    // With focus, as the web's waveform (rangeSelect.ts): Left and Right move the picked loop's (or
    // the marked range's) selected edge by 0.05 s, 0.5 s with Shift; Up and Down pick the other
    // edge; Home and End take it to the track's ends.
    bool wants_focus() const override
    {
        return FocusWanted(HasAudio());
    }

    void begin_focus(focus_request) override
    {
        NoteFocus(true);
    }

    bool end_focus() override
    {
        NoteFocus(false);
        return true;
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info;
        info.role = AccessRole::Slider;
        info.name = NameOr("Waveform");
        info.value = mKeyEdge == 0 ? "Start" : "End";
        return info;
    }

    bool key(const el::context& ctx, el::key_info k) override
    {
        if (k.action != el::key_action::press && k.action != el::key_action::repeat)
        {
            return false;
        }

        const auto edit = Editable();

        if (!HasAudio() || !edit)
        {
            return false;
        }

        double start = edit->first, end = edit->second;
        auto& edge = mKeyEdge == 0 ? start : end;

        switch (k.key)
        {
        case el::key_code::left:
        case el::key_code::right: {
            const double step = (k.modifiers & el::mod_shift) != 0 ? 0.5 : 0.05;
            edge = std::clamp(edge + (k.key == el::key_code::left ? -step : step), 0.0, mDuration);
            break;
        }
        case el::key_code::up:
        case el::key_code::down:
            mKeyEdge = 1 - mKeyEdge;
            ctx.view.refresh(ctx);
            return true;
        case el::key_code::home:
            edge = 0.0;
            break;
        case el::key_code::end:
            edge = mDuration;
            break;
        default:
            return false;
        }

        if (start > end)
        {
            std::swap(start, end);
            mKeyEdge = 1 - mKeyEdge;
        }

        if (auto* active = ActiveBand())
        {
            active->start = start, active->end = end;
        }
        else
        {
            mCandidate = std::make_pair(start, end);
        }

        if (onRange)
        {
            onRange(start, end, true);
        }

        ctx.view.refresh(ctx);
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
    int mKeyEdge = 0; // the edge the keys move: 0 the start, 1 the end
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
    t->AccessHeading(2); // a section title, to a screen reader
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
                    mPractice->SelectedProjectId() + "|e" + mPracticeEditingLoop +
                    (mPracticeIncludePreset ? "|i" : "|") + mState.activePresetId;

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
    const auto field = [this](std::shared_ptr<el::basic_input_box>& box, Ptr& element, const char* placeholder) {
        if (!box)
        {
            auto [e, b] = el::input_box(placeholder, Font(Weight::Regular), 1.0f);
            box = b;
            element = el::share(std::move(e));
        }

        return Rounded(mTheme.Tertiary(), 6.0f, Pad(8, 0, 8, 0, element));
    };
    const auto hintText = [this](const std::string& text, float size = 11.0f) {
        auto t = std::make_shared<Text>(mTheme, text, size);
        t->Colour(mTheme.TextMuted());
        return t;
    };

    // ── The project bar, as the web's: the saved projects, Delete, the name, Preset and Save ──
    const auto projects = tool.Projects();
    const auto selected = std::find_if(projects.begin(), projects.end(), [&tool](const practice::Project& p) {
        return p.id == tool.SelectedProjectId();
    });
    auto title = std::make_shared<Text>(mTheme, "Practice Tool", 15.0f, Weight::SemiBold);
    title->Colour(mTheme.Text());
    auto projectButton = std::make_shared<PickerField>(mTheme, mIcons);
    projectButton->Set(selected != projects.end() ? selected->name
                       : projects.empty()         ? std::string("No saved projects")
                                                  : std::string("Saved projects"));
    projectButton->AccessName("Saved projects");
    projectButton->Enabled(!projects.empty());
    projectButton->onClick = [this, b = projectButton.get()] { OpenPracticeProjects(b->Bounds()); };
    auto remove = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
    remove->Size(64, 30).Label("Delete", 11.0f).Disabled(selected == projects.end());
    remove->Tooltip("Delete the selected project");
    remove->onClick = [this] { DeletePracticeProject(); };

    // The name follows the project picked, and keeps what is typed while the section rebuilds.
    auto nameField = field(mPracticeNameBox, mPracticeNameField, "Project name");
    const std::string shownFor = selected != projects.end() ? selected->id : std::string();

    if (mPracticeNameFor != shownFor && !mPracticeNameBox->is_focus())
    {
        mPracticeNameFor = shownFor;
        mPracticeNameBox->set_text(selected != projects.end() ? selected->name : std::string());
    }

    mPracticeNameBox->on_enter = [this](std::string_view) {
        SavePracticeProject();
        return true;
    };
    auto withPreset = std::make_shared<CheckBox>(mTheme, "Preset", 11.5f);
    withPreset->Set(mPracticeIncludePreset && !mState.activePresetId.empty());
    withPreset->Enabled(!mState.activePresetId.empty());
    withPreset->Tooltip("Also save the currently selected preset, and reload it with the project");
    withPreset->onClick = [this] {
        mPracticeIncludePreset = !mPracticeIncludePreset;
        Mark(kPage);
    };
    auto save = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
    save->Size(64, 30).Label("Save", 11.0f);
    // Save waits for a track and a name, as the web's.
    mLive.Bind(
        save,
        [this, box = std::weak_ptr<el::basic_input_box>(mPracticeNameBox)] {
            const auto b = box.lock();
            const auto text = b ? b->get_utf8() : std::string();
            return mPractice->HasAudio() && text.find_first_not_of(" \t") != std::string::npos;
        },
        [](IconButton& b, bool ready) { b.Disabled(!ready); });
    save->onClick = [this] { SavePracticeProject(); };
    const float nameWidth = std::clamp(width - 520.0f, 140.0f, 260.0f);
    std::vector<Ptr> bar{FixedWidth(200, projectButton),
                         HGap(6),
                         remove,
                         HGap(12),
                         FixedWidth(nameWidth, FixedHeight(30, nameField)),
                         HGap(8),
                         Middle(withPreset),
                         HGap(8),
                         save};

    if (width >= 820.0f)
    {
        body.push_back(FixedHeight(30, Row({Middle(title), Stretch(), Row(bar)})));
    }
    else
    {
        body.push_back(FixedHeight(24, title));
        body.push_back(VGap(6));
        body.push_back(FixedHeight(30, Row({Row(bar), Stretch()})));
    }

    // The hint under the bar (projectsPanel.ts buildHintText).
    std::string hint;

    if (selected != projects.end())
    {
        const auto& p = *selected;
        hint = p.fileTitle + " \xC2\xB7 " + std::to_string(p.loops.size()) + (p.loops.size() == 1 ? " loop" : " loops");

        if (!p.youtubeVideoId.empty() && p.filePath.empty())
        {
            hint = "YouTube: " + (p.youtubeTitle.empty() ? p.youtubeVideoId : p.youtubeTitle);
        }

        if (p.kept.contains("tab") && p.kept["tab"].is_object())
        {
            hint += " \xC2\xB7 tab \"" + p.kept["tab"].value("fileName", std::string("tab")) + "\"";
        }

        if (!p.presetName.empty())
        {
            hint += " \xC2\xB7 preset \"" + p.presetName + "\"";
        }
        else if (!p.presetId.empty())
        {
            hint += " \xC2\xB7 a saved preset";
        }
    }
    else if (projects.empty())
    {
        hint = hasAudio ? "Save the loaded track, its loops and the Volume/Balance/Speed/Pitch settings as a project "
                          "you can recall later."
                        : "Load a track to save it, its loops and its settings as a project.";
    }
    else
    {
        hint = "Pick a saved project to load it, or save the current one under the name shown.";
    }

    body.push_back(VGap(6));
    body.push_back(FixedHeight(18, hintText(hint)));
    body.push_back(VGap(10));

    // ── The waveform, with the file, + Add Loop and Browse over its top corners ──
    auto wave = std::make_shared<PracticeWave>(mTheme);
    wave->Track(track.peaksL, track.peaksR, track.durationSec);
    wave->AccessName("Waveform");
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

    auto clock =
        std::make_shared<Text>(mTheme, hasAudio ? track.title : std::string("No file loaded"), 11.0f, Weight::Medium);
    clock->Colour(hasAudio ? mTheme.Ink() : mTheme.TextMuted());

    if (hasAudio)
    {
        mLive.Bind(
            clock, [position] { return static_cast<long>(std::floor(position())); },
            [title = track.title, total = practice::FormatClock(track.durationSec)](Text& t, long seconds) {
                t.Set(title + "   " + practice::FormatClock(static_cast<double>(seconds)) + " / " + total);
            });
    }

    auto browse = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
    browse->Size(104, 26).Label("Browse File...", 11.0f);
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
    std::vector<Ptr> overlay{
        Rounded(el::rgba(0, 0, 0, 110), 11.0f,
                Pad(8, 3, 8, 3, FixedWidth(std::min(360.0f, width * 0.45f), FixedHeight(18, clock)))),
        Stretch()};

    if (hasAudio && tool.Candidate() && !tool.ActiveLoop())
    {
        auto add = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Accent);
        add->Size(90, 26).Label("+ Add Loop", 11.0f);
        add->onClick = [this] {
            const auto id = mPractice->AddLoop();

            if (!id.empty())
            {
                EditPracticeLoop(id, true);
            }
        };
        overlay.push_back(add);
        overlay.push_back(HGap(6));
    }

    overlay.push_back(browse);
    body.push_back(
        FixedHeight(kWaveHeight, Over(Col({Pad(10, 8, 8, 0, FixedHeight(26, Row(overlay))), VGap(0)}), wave)));
    body.push_back(VGap(12));

    // ── Tablature: drawn in Soundshed Guitar; a project's tab is kept as it was ──
    {
        std::string tab = "Tablature (Guitar Pro, MusicXML and more, synced to the track) opens in Soundshed Guitar.";

        if (selected != projects.end() && selected->kept.contains("tab") && selected->kept["tab"].is_object())
        {
            tab = "Tab \"" + selected->kept["tab"].value("fileName", std::string("tab")) +
                  "\" is kept with this project; it shows in Soundshed Guitar.";
        }

        body.push_back(FixedHeight(20, Row({FixedWidth(90, Heading(mTheme, "Tablature")), hintText(tab, 11.5f)})));
        body.push_back(VGap(12));
    }

    // ── The loops ──
    std::vector<Ptr> loops;
    // The web's grid: the loops 1.1 to the transport's 0.9, set here so a row can size its parts.
    const float loopsWidth = width >= 620.0f ? std::floor((width - 16.0f) * 0.55f) : width;
    {
        auto newLoop = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        newLoop->Size(90, 26).Label("+ New Loop", 11.0f).Disabled(!hasAudio);
        newLoop->onClick = [this] {
            const auto id = mPractice->AddLoop();

            if (!id.empty())
            {
                EditPracticeLoop(id, true);
            }
        };
        loops.push_back(FixedHeight(26, Row({Middle(Heading(mTheme, "Loops")), Stretch(), newLoop})));
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
            loops.push_back(FixedHeight(
                28, hintText(hasAudio ? "No loops saved for this file yet." : "Load a track to mark loops on it.")));
        }

        for (const auto& loop : tool.Loops())
        {
            const bool active = tool.ActiveLoop() && tool.ActiveLoop()->id == loop.id;
            const bool playingThis = active && track.playing;
            const bool editing = loop.id == mPracticeEditingLoop;
            const auto id = loop.id;
            auto pick = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Ghost);
            // U+25CF / U+25CB, as the web UI's row.
            pick->Size(26, 26).Label(active ? "\xE2\x97\x8F" : "\xE2\x97\x8B", 12.0f);
            pick->Tooltip(active ? "Active loop: click to deactivate" : "Select loop");

            if (active)
            {
                pick->Ink(mTheme.Accent());
            }

            pick->onClick = [this, id] { mPractice->SelectLoop(id); };
            auto playLoop = std::make_shared<IconButton>(mTheme, mIcons, playingThis ? "ui-stop" : "ui-play",
                                                         IconButton::Look::Ghost);
            playLoop->Size(26, 26).IconSize(11);
            playLoop->onClick = [this, id] { mPractice->ToggleLoopPlayback(id); };
            auto erase = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Ghost);
            erase->Size(26, 26).Label("\xE2\x9C\x95", 11.0f);
            erase->AccessName("Delete loop");
            erase->Tooltip("Delete");
            erase->onClick = [this, id] {
                if (mPracticeEditingLoop == id)
                {
                    mPracticeEditingLoop.clear();
                }

                mPractice->DeleteLoop(id);
            };
            Ptr row;

            if (editing)
            {
                // The web's .is-editing row: the name (with the song sections to pick from) and
                // the times in seconds; Enter keeps them, Esc drops them.
                auto names = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Ghost);
                names->Size(22, 26).Caret(true);
                names->AccessName("Song sections");
                names->onClick = [this, n = names.get()] { OpenPracticeLoopNames(n->Bounds()); };
                auto keep = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Accent);
                keep->Size(48, 24).Label("Done", 10.5f);
                keep->onClick = [this] { CommitPracticeLoopEdit(true); };
                auto dash = std::make_shared<Text>(mTheme, "\xE2\x80\x93", 11.0f);
                dash->Colour(mTheme.TextSecondary());
                auto unit = std::make_shared<Text>(mTheme, "s", 11.0f);
                unit->Colour(mTheme.TextSecondary());
                row = Row({pick, playLoop, HGap(4),
                           Stretch(FixedHeight(26, field(mPracticeLoopName, mPracticeLoopNameField, "Loop name"))),
                           names, HGap(4),
                           FixedWidth(64, FixedHeight(26, field(mPracticeLoopStart, mPracticeLoopStartField, "0.00"))),
                           HGap(3), FixedWidth(10, Middle(dash)), HGap(3),
                           FixedWidth(64, FixedHeight(26, field(mPracticeLoopEnd, mPracticeLoopEndField, "0.00"))),
                           HGap(3), FixedWidth(10, Middle(unit)), HGap(6), Middle(keep), erase});

                // Just opened: the name, selected, so typing replaces it.
                if (std::exchange(mPracticeFocusLoopName, false))
                {
                    mSession.View().post([this, box = std::weak_ptr<el::basic_input_box>(mPracticeLoopName)] {
                        if (const auto b = box.lock(); b && FocusInView(mSession.View(), *b))
                        {
                            b->select_all();
                            mSession.View().refresh();
                        }
                    });
                }
            }
            else
            {
                auto name =
                    std::make_shared<Text>(mTheme, loop.name, 11.5f, active ? Weight::SemiBold : Weight::Regular);
                name->Colour(active ? mTheme.Ink() : mTheme.Text());
                auto range = std::make_shared<Text>(mTheme, LoopRange(loop), 10.5f);
                range->Colour(mTheme.TextSecondary()).Fixed(true);
                // A click on the row's name or times picks the loop, as the web's row.
                auto main = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Ghost);
                main->AccessName("Select " + loop.name);
                // A button is as wide as its Size: the row's room, past the four 26 px buttons.
                main->Size(std::max(60.0f, loopsWidth - 4.0f * 26.0f - 8.0f), 26);
                main->onClick = [this, id] { mPractice->SelectLoop(id); };
                auto edit = std::make_shared<IconButton>(mTheme, mIcons, "ui-edit", IconButton::Look::Ghost);
                edit->Size(26, 26).IconSize(12);
                edit->AccessName("Edit loop name and time");
                edit->Tooltip("Edit name/time");
                edit->onClick = [this, id] { EditPracticeLoop(id, true); };
                row = Row({pick, playLoop, HGap(4),
                           Stretch(Over(Row({Middle(name), HGap(6), Middle(range), Stretch()}), main)), edit, erase});
            }

            loops.push_back(
                FixedHeight(30, active || editing ? Rounded(mTheme.Accent().opacity(0.10f), 6.0f, row) : row));
        }
    }

    // ── The transport and the faders: the default in the middle, double click to reset, the value typed ──
    std::vector<Ptr> side;
    {
        const auto round = [this](const std::string& icon, bool enabled, std::function<void()> click,
                                  const std::string& name) {
            // The web's round transport buttons (.practice-tool-btn).
            auto b = std::make_shared<metronome::RoundButton>(mTheme, mIcons, 36.0f, "");
            b->Icon(icon);
            b->Enabled(enabled);
            b->AccessName(name);
            b->onClick = std::move(click);
            return b;
        };
        auto eq = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        // Lit only when the EQ is on and doing something.
        eq->Size(48, 32).Label("EQ", 11.0f).Lit(practice::IsEqShaping(tool.GetEq()));
        eq->Tooltip("Backing track EQ");

        if (practice::IsEqShaping(tool.GetEq()))
        {
            eq->Ink(mTheme.Accent());
        }

        eq->onClick = [this] { OpenPracticeEq(); };
        std::vector<Ptr> transport{round(
                                       track.playing ? "ui-pause" : "ui-play", hasAudio,
                                       [this] { mPractice->PlayPause(); }, track.playing ? "Pause" : "Play"),
                                   HGap(8),
                                   round(
                                       "ui-stop", hasAudio, [this] { mPractice->Stop(); }, "Stop"),
                                   HGap(8),
                                   round(
                                       "ui-jump-back", hasAudio, [this] { mPractice->JumpBack(); }, "Jump back"),
                                   HGap(12),
                                   Middle(eq),
                                   HGap(12)};

        if (const auto* active = tool.ActiveLoop(); active && hasAudio)
        {
            auto status = std::make_shared<Text>(mTheme, "Looping \"" + active->name + "\"", 11.0f, Weight::Medium);
            status->Colour(mTheme.Accent());
            transport.push_back(Middle(status));
        }
        else
        {
            transport.push_back(Stretch());
        }

        side.push_back(FixedHeight(36, Row(transport)));
        side.push_back(VGap(8));
    }

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
        // The web's value field: an exact value typed ("85%", "L 30", "-2 st").
        slider->onEditValue = [this, fader] {
            const auto& spec = practice::SpecFor(fader);
            OpenPrompt(spec.label, practice::FormatFader(fader, practice::FaderValue(mPractice->Faders(), fader)),
                       "Set", {.placeholder = practice::FormatFader(fader, spec.def)},
                       [this, fader](const std::string& text) {
                           if (const auto value = practice::ParseFader(fader, text))
                           {
                               mPractice->SetFader(fader, *value, true);
                           }
                           else
                           {
                               ShowToast("Value not set", "\"" + text + "\" is not a value it takes.", true);
                           }
                       });
        };
        side.push_back(slider);
    }

    side.push_back(
        FixedHeight(20, hintText("Speed and pitch are independent: changing one never affects the other.", 10.5f)));

    if (width >= 620.0f)
    {
        // The web's grid: the loops 1.1 to the transport's 0.9.
        body.push_back(
            Row({FixedWidth(loopsWidth, Col(loops)), HGap(16), FixedWidth(width - 16.0f - loopsWidth, Col(side))}));
    }
    else
    {
        body.push_back(Col(loops));
        body.push_back(VGap(14));
        body.push_back(Col(side));
    }

    // ── Links: always shown, kept with the picked project ──
    {
        const bool canAdd = selected != projects.end();
        body.push_back(VGap(16));
        body.push_back(FixedHeight(22, Heading(mTheme, "Links")));
        body.push_back(VGap(4));

        if (canAdd && selected->links.empty())
        {
            body.push_back(FixedHeight(24, hintText("No links for this project yet.", 11.5f)));
        }

        if (canAdd)
        {
            const auto projectId = selected->id;

            for (const auto& link : selected->links)
            {
                const auto url = link.url;
                const auto linkId = link.id;
                const auto described = practice::DescribeLink(link);
                // The title opens it, as the web's link; then the address.
                auto open = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Ghost);
                open->Label(described, 11.5f).Ink(mTheme.Accent());
                open->Tooltip(url);
                open->onClick = [this, url] { mSession.Commands().OpenUrl(url); };
                auto address = std::make_shared<Text>(mTheme, url, 10.5f);
                address->Colour(mTheme.TextMuted());
                auto erase = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Ghost);
                erase->Size(26, 26).Label("\xE2\x9C\x95", 11.0f);
                erase->AccessName("Remove link");
                erase->Tooltip("Remove link");
                erase->onClick = [this, projectId, linkId, described] {
                    OpenConfirm("Remove link", "Remove the link \"" + described + "\" from this project?",
                                "Remove link", [this, projectId, linkId] { mPractice->RemoveLink(projectId, linkId); });
                };
                body.push_back(FixedHeight(28, Row({FixedWidth(std::min(240.0f, width * 0.35f), open), HGap(8),
                                                    Stretch(Middle(address)), erase})));
            }
        }

        auto titleField = field(mPracticeLinkTitle, mPracticeLinkTitleField, "Title (optional)");
        auto urlField = field(mPracticeLinkUrl, mPracticeLinkUrlField, "https://... e.g. the tablature");
        mPracticeLinkUrl->on_enter = [this](std::string_view) {
            AddPracticeLinkInline();
            return true;
        };
        auto add = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        add->Size(84, 30).Label("Add Link", 11.0f).Disabled(!canAdd);
        add->onClick = [this] { AddPracticeLinkInline(); };
        body.push_back(VGap(4));
        body.push_back(FixedHeight(30, Row({FixedWidth(std::min(200.0f, width * 0.3f), FixedHeight(30, titleField)),
                                            HGap(6), Stretch(FixedHeight(30, urlField)), HGap(6), add})));
        body.push_back(VGap(6));
        body.push_back(FixedHeight(
            18, hintText(canAdd ? "Links are kept with \"" + selected->name +
                                      "\" as soon as they're added, and open in your web browser."
                                : "Save the track as a project, or pick a saved one, to keep links with it: the tab, "
                                  "a lesson, the lyrics.")));
    }

    body.push_back(VGap(20));
    return Col(body);
}

void Shell::OpenPracticeProjects(el::rect anchor)
{
    const auto projects = mPractice->Projects();
    std::vector<MenuItem> items;

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

    OpenDropdown(anchor, std::move(items), 300.0f);
}

void Shell::SavePracticeProject()
{
    const auto text = mPracticeNameBox ? mPracticeNameBox->get_utf8() : std::string();
    const auto first = text.find_first_not_of(" \t");
    const auto name =
        first == std::string::npos ? std::string() : text.substr(first, text.find_last_not_of(" \t") - first + 1);

    if (!mPractice->HasAudio())
    {
        ShowToast("Nothing to save", "Load a backing track first", true);
        return;
    }

    if (name.empty())
    {
        ShowToast("Name the project", "Type a name before saving", true);
        return;
    }

    const bool withPreset = mPracticeIncludePreset && !mState.activePresetId.empty();
    const auto save = [this, name, withPreset](bool existed) {
        if (!mPractice->SaveProject(name, withPreset))
        {
            ShowToast("Too many saved projects", "Delete one before saving another", true);
            return;
        }

        mPracticeNameFor = "-"; // the name box follows the project saved
        ShowToast(existed ? "Project \"" + name + "\" updated" : "Project \"" + name + "\" saved", "", false);
    };
    // Saving over the same name updates that project, once asked (projectsPanel.ts).
    const auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    const auto projects = mPractice->Projects();
    const auto existing = std::find_if(projects.begin(), projects.end(),
                                       [&](const practice::Project& p) { return lower(p.name) == lower(name); });

    if (existing == projects.end())
    {
        save(false);
        return;
    }

    OpenConfirm("Overwrite project", "Overwrite the saved project \"" + existing->name + "\"?", "Overwrite project",
                [save] { save(true); });
}

void Shell::DeletePracticeProject()
{
    const auto projects = mPractice->Projects();
    const auto it = std::find_if(projects.begin(), projects.end(),
                                 [this](const practice::Project& p) { return p.id == mPractice->SelectedProjectId(); });

    if (it == projects.end())
    {
        return;
    }

    const auto count = it->links.size();
    const std::string linkNote =
        count == 0
            ? std::string()
            : " Its " + std::to_string(count) + (count == 1 ? " saved link goes with it." : " saved links go with it.");
    OpenConfirm("Delete project",
                "Delete the saved project \"" + it->name + "\"? The track file and its loops are not affected." +
                    linkNote,
                "Delete project", [this, id = it->id] { mPractice->DeleteProject(id); });
}

void Shell::EditPracticeLoop(const std::string& loopId, bool focus)
{
    if (!mPracticeEditingLoop.empty() && mPracticeEditingLoop != loopId)
    {
        CommitPracticeLoopEdit(true);
    }

    const auto& loops = mPractice->Loops();
    const auto it =
        std::find_if(loops.begin(), loops.end(), [&loopId](const practice::Loop& l) { return l.id == loopId; });

    if (it == loops.end())
    {
        return;
    }

    const auto make = [](std::shared_ptr<el::basic_input_box>& box, Ptr& element, const char* placeholder) {
        if (!box)
        {
            auto [e, b] = el::input_box(placeholder, Font(Weight::Regular), 1.0f);
            box = b;
            element = el::share(std::move(e));
        }
    };
    make(mPracticeLoopName, mPracticeLoopNameField, "Loop name");
    make(mPracticeLoopStart, mPracticeLoopStartField, "0.00");
    make(mPracticeLoopEnd, mPracticeLoopEndField, "0.00");
    char start[32], end[32];
    std::snprintf(start, sizeof(start), "%.2f", it->startSec);
    std::snprintf(end, sizeof(end), "%.2f", it->endSec);
    mPracticeLoopName->set_text(it->name);
    mPracticeLoopStart->set_text(start);
    mPracticeLoopEnd->set_text(end);

    for (auto* box : {mPracticeLoopName.get(), mPracticeLoopStart.get(), mPracticeLoopEnd.get()})
    {
        box->on_enter = [this](std::string_view) {
            CommitPracticeLoopEdit(true);
            return true;
        };
        box->on_escape = [this] { CommitPracticeLoopEdit(false); };
    }

    mPracticeEditingLoop = loopId;
    Mark(kPage);

    // The row is built on the next rebuild; the name takes the focus there.
    mPracticeFocusLoopName = focus;
}

void Shell::CommitPracticeLoopEdit(bool keep)
{
    const auto id = std::exchange(mPracticeEditingLoop, std::string());

    if (id.empty())
    {
        return;
    }

    if (keep && mPracticeLoopName && mPracticeLoopStart && mPracticeLoopEnd)
    {
        const auto name = mPracticeLoopName->get_utf8();

        if (name.find_first_not_of(" \t") != std::string::npos)
        {
            mPractice->RenameLoop(id, name);
        }

        char* stop = nullptr;
        const auto startText = mPracticeLoopStart->get_utf8(), endText = mPracticeLoopEnd->get_utf8();
        const double start = std::strtod(startText.c_str(), &stop);
        const bool startOk = stop != startText.c_str();
        const double end = std::strtod(endText.c_str(), &stop);
        const bool endOk = stop != endText.c_str();

        if (startOk && endOk && end > start)
        {
            mPractice->SetLoopTimes(id, start, end);
        }
        else
        {
            ShowToast("Times not set", "The end must come after the start, in seconds.", true);
        }
    }

    Mark(kPage);
}

void Shell::OpenPracticeLoopNames(el::rect anchor)
{
    // The song sections, numbered past those already used (the web's name datalist).
    std::vector<std::string> names;

    for (const auto& l : mPractice->Loops())
    {
        if (l.id != mPracticeEditingLoop)
        {
            names.push_back(l.name);
        }
    }

    std::vector<MenuItem> items;

    for (const auto& base : practice::kLoopNameTemplates)
    {
        const auto name = practice::SuggestLoopTemplateName(base, names);
        items.push_back({name, false, false, [this, name] {
                             if (mPracticeLoopName)
                             {
                                 mPracticeLoopName->set_text(name);
                                 mSession.View().refresh();
                             }
                         }});
    }

    OpenDropdown(anchor, std::move(items), 180.0f);
}

void Shell::AddPracticeLinkInline()
{
    const auto projectId = mPractice->SelectedProjectId();

    if (projectId.empty() || !mPracticeLinkUrl)
    {
        return;
    }

    const auto url = mPracticeLinkUrl->get_utf8();

    if (url.find_first_not_of(" \t") == std::string::npos)
    {
        FocusInView(mSession.View(), *mPracticeLinkUrl);
        return;
    }

    const auto result = mPractice->AddLink(projectId, mPracticeLinkTitle ? mPracticeLinkTitle->get_utf8() : "", url);

    if (result == "invalid-url")
    {
        ShowToast("Not a web address",
                  "Paste a link that starts with https://, or a site address like songsterr.com/...", true);
        return;
    }

    if (result == "too-many")
    {
        ShowToast("Link not added", "A project keeps up to 30 links.", true);
        return;
    }

    if (result != "added")
    {
        ShowToast("Link not added", "Save the project first.", true);
        return;
    }

    mPracticeLinkUrl->set_text("");

    if (mPracticeLinkTitle)
    {
        mPracticeLinkTitle->set_text("");
    }
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
