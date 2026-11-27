// The looper, as the web UI's looper strip has it (looper.ts, looper/strip.ts, looper/layers.ts):
// docked over the footer while it is open, with the state button (Rec, Play, Dub), Stop, Undo,
// Redo and Clear, the loop's waveform, playhead and progress (and the count-in over them), the
// loop's name and Save, its modes, and Layers (a panel under the row), Loops and Options (menus
// that drop from their buttons and leave the strip live under them).
// What a narrow strip has no room for is in Options (OpenLooperOptions).
// The engine records and plays (LooperService*.cpp); this shows what it reports
// (uiclient/Looper.h, Topic::Looper) and sends its commands. The Loop button in the footer shows
// only while the Looper feature is on and the app is the standalone one.

#include "ui/dialogs/DialogChrome.h"
#include "ui/shell/Shell.h"

#include "ui/shell/FrameParts.h"
#include "ui/performance/Waveform.h"

#include "uiclient/FeatureFlags.h"
#include "uiclient/Looper.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace nanoq::ui
{
using frame::ToolButton;
using guitarfx::uiclient::Topic;

/// Over the strip's display: the loop's progress along its foot (.looper-progress, 3 px) and,
/// while it counts in, the beats left as a big number over a veil (.looper-countin).
class LooperDisplayFace final : public el::element
{
  public:
    explicit LooperDisplayFace(const Theme& theme) : mTheme(theme)
    {
    }

    void Set(float progress, int countIn)
    {
        mProgress = std::clamp(progress, 0.0f, 1.0f);
        mCountIn = countIn;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{0.0f, 0.0f}, {el::full_extent, el::full_extent}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const el::rect bar{b.left, b.bottom - 3.0f, b.right, b.bottom};
        cnv.fill_style(mTheme.Input());
        cnv.fill_round_rect(bar, 1.5f);

        if (mProgress > 0.0f)
        {
            auto fill = bar;
            fill.right = bar.left + std::max(3.0f, bar.width() * mProgress);
            cnv.fill_style(mTheme.Text());
            cnv.fill_round_rect(fill, 1.5f);
        }

        if (mCountIn > 0)
        {
            cnv.fill_style(mTheme.Bar().opacity(0.7f));
            cnv.fill_rect(b);
            FillText(cnv, std::to_string(mCountIn), Font(Weight::Title).size(34.0f), kDub, b, HAlign::Centre);
        }
    }

    /// The web's --looper-dub.
    static constexpr el::color kDub = el::rgba(224, 161, 42, 255);

  private:
    const Theme& mTheme;
    float mProgress = 0.0f;
    int mCountIn = 0;
};

namespace
{
/// A layer's swatch: the hue at the web UI's saturation and lightness.
el::color HueColour(float hue)
{
    const float s = 0.55f, l = 0.58f;
    const float c = (1.0f - std::abs(2.0f * l - 1.0f)) * s;
    const float h = std::fmod(hue, 360.0f) / 60.0f;
    const float x = c * (1.0f - std::abs(std::fmod(h, 2.0f) - 1.0f));
    float r = 0, g = 0, b = 0;

    if (h < 1)
    {
        r = c, g = x;
    }
    else if (h < 2)
    {
        r = x, g = c;
    }
    else if (h < 3)
    {
        g = c, b = x;
    }
    else if (h < 4)
    {
        g = x, b = c;
    }
    else if (h < 5)
    {
        r = x, b = c;
    }
    else
    {
        r = c, b = x;
    }

    const float m = l - c * 0.5f;
    return el::color{r + m, g + m, b + m, 1.0f};
}

/// Its subject with no minimum width (Layout.h's Yielding drops the minimum height too, which
/// would let the frame's column squash the strip).
template <el::concepts::Element Subject> class WidthYielding : public el::proxy<Subject>
{
  public:
    explicit WidthYielding(Subject subject) : el::proxy<Subject>(std::move(subject))
    {
    }

    el::view_limits limits(const el::basic_context& ctx) const override
    {
        auto l = this->subject().limits(ctx);
        l.min.x = 0.0f;
        return l;
    }
};

std::string Seconds(double seconds)
{
    char text[24];
    std::snprintf(text, sizeof(text), "%.1f s", seconds);
    return text;
}

std::string Trimmed(const std::string& text)
{
    const auto first = text.find_first_not_of(" \t");

    if (first == std::string::npos)
    {
        return {};
    }

    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

/// One line per layer where the panel has the width for it (as the web's row), else two: its
/// name and buttons over its Level and Balance.
constexpr float kOneLineLayers = 1000.0f;
constexpr float kLayerLine = 60.0f;  // a one-line layer, with its rule
constexpr float kLayerLines = 95.0f; // a two-line layer
} // namespace

float Shell::LooperLayersHeight() const
{
    const auto count = static_cast<float>(mState.looper.layers.size());
    const float rows =
        count == 0.0f ? 40.0f : 39.0f + count * (StageWidth() >= kOneLineLayers ? kLayerLine : kLayerLines);
    // As the web's max-height: 40vh, the rest scrolled.
    return std::min(std::floor(ViewSize().y * 0.4f), rows + 10.0f);
}

bool Shell::LooperShown() const
{
    return guitarfx::uiclient::IsFeatureEnabled(mState, guitarfx::uiclient::Feature::Looper) && mState.looper.available;
}

void Shell::ToggleLooper()
{
    mLooperOpen = !mLooperOpen && LooperShown();
    mLooperShape.clear();

    if (mLooperOpen)
    {
        mSession.Commands().GetLooperProjects();
    }

    SyncLooper();
    // The strip takes its height from the stage: the chain and the editor are sized for what is
    // left (StageHeight, in their signatures). The compact rail's Looper button lights with it.
    Mark(kTransport | kChain | kPage | kRail);
}

std::string Shell::LooperShape() const
{
    // What changes the strip's controls, as opposed to what moves while it plays.
    const auto& l = mState.looper;
    const auto& s = l.settings;
    return std::string(mWide ? "w" : "c") + std::to_string(static_cast<int>(StageWidth() / 40.0f)) + "|" + l.state +
           "|" + (l.canUndo ? "u" : "") + (l.canRedo ? "r" : "") + (l.dirty ? "d" : "") + (l.click ? "c" : "") +
           (l.tempoLocked ? "t" : "") + "|" + std::to_string(l.layerCount) + "|" + s.start + s.length +
           std::to_string(s.bars) + "|" + std::to_string(static_cast<int>(l.tempoBpm)) + "|" +
           std::to_string(static_cast<int>(mState.metronome.bpm)) + "|" + l.projectTitle +
           (mLooperLayersOpen ? "|L" + std::to_string(static_cast<int>(LooperLayersHeight())) : std::string());
}

Ptr Shell::BuildLooperStrip()
{
    auto& commands = mSession.Commands();
    const auto& l = mState.looper;
    const auto& s = l.settings;
    const bool empty = l.state == "empty";

    mLooperMain = std::make_shared<Button>(
        mTheme, guitarfx::uiclient::LooperMainButtonLabel(l.state),
        [this] { mSession.Commands().LooperCommand("recPlayDub"); }, Button::Style::Primary);
    mLooperMain->MinWidth(76);
    auto stop =
        ToolButton(mTheme, mIcons, "ui-stop", [this] { mSession.Commands().LooperCommand("stop"); }, 28.0f, 13.0f);
    stop->Disabled(empty);
    // Undo also cancels a take in progress.
    auto undo =
        ToolButton(mTheme, mIcons, "ui-undo", [this] { mSession.Commands().LooperCommand("undo"); }, 28.0f, 15.0f);
    undo->Disabled(!l.canUndo && !guitarfx::uiclient::LooperTaking(l.state));
    auto redo =
        ToolButton(mTheme, mIcons, "ui-redo", [this] { mSession.Commands().LooperCommand("redo"); }, 28.0f, 15.0f);
    redo->Disabled(!l.canRedo);
    auto clear = ToolButton(mTheme, mIcons, "ui-trash", [this] { ClearLooperWithConfirm(); }, 28.0f, 14.0f);
    clear->Disabled(empty && !l.canRedo);

    mLooperWave = std::make_shared<Waveform>(mTheme, 14.0f);
    mLooperWave->Colour(el::rgba(122, 162, 200, 255));
    mLooperStatus = std::make_shared<Text>(mTheme, "", 11.0f, Weight::SemiBold);
    mLooperTime = std::make_shared<Text>(mTheme, "", 11.0f);
    mLooperTime->Colour(mTheme.TextSecondary());
    mLooperBar = std::make_shared<Text>(mTheme, "", 11.0f);
    mLooperBar->Colour(mTheme.TextSecondary()).Alignment(HAlign::Right);
    mLooperFace = std::make_shared<LooperDisplayFace>(mTheme);
    // Narrower columns on the smallest strip, so the layer count after them is not cut short.
    const bool narrow = !mWide && StageWidth() < 680.0f;
    // The readout, the waveform, and the progress bar the face draws along the foot.
    Ptr display = el::share(
        el::layer(el::hold(mLooperFace),
                  el::hold(Col({FixedHeight(14, Row({FixedWidth(narrow ? 80.0f : 96.0f, mLooperStatus),
                                                     FixedWidth(narrow ? 90.0f : 100.0f, mLooperTime), mLooperBar})),
                                VGap(2), mLooperWave, VGap(4)}))));

    const auto segment = [this](const char* text, bool on, std::function<void()> onClick) {
        auto b = std::make_shared<Button>(mTheme, text, std::move(onClick), Button::Style::Plain);
        b->MinWidth(48).TextSize(10.5f).Selected(on);
        return b;
    };
    const auto tool = [this](const char* text, std::function<void()> onClick) {
        auto b = std::make_shared<Button>(mTheme, text, std::move(onClick), Button::Style::Plain);
        b->MinWidth(58).TextSize(10.5f);
        return b;
    };

    auto layers = tool("Layers", [this] { ToggleLooperLayers(); });
    layers->Selected(mLooperLayersOpen);
    layers->AccessName(mLooperLayersOpen ? "Hide the loop's layers" : "Show the loop's layers");
    auto loops = tool("Loops", [this] { OpenLooperLoops(); });
    loops->Tooltip("Saved loops");
    auto options = tool("Options", [this] { OpenLooperOptions(); });
    options->Tooltip("Looper options");
    // Where the menus hang from.
    auto loopsAt = std::make_shared<BoundsProbe>(loops, [this](el::rect r) { mLooperLoopsRect = r; });
    auto optionsAt = std::make_shared<BoundsProbe>(options, [this](el::rect r) { mLooperOptionsRect = r; });

    // What fits: the strip must never hold the window wider than it is. Options is always on
    // it, and what a narrow strip leaves out is in Options: the modes and Click, and in the
    // compact layout the loop's name and Save, and Redo and Clear where they do not fit beside
    // Undo (the compact strip with them is about 620 px; the stage is 580 at the smallest
    // window). Each width threshold is a multiple of 40, as LooperShape rebuilds the strip in
    // 40 px steps.
    const float width = StageWidth();
    std::vector<Ptr> row{HGap(10), mLooperMain, HGap(6), stop, HGap(2), undo};
    std::vector<std::shared_ptr<Button>> faced{loops, options};
    // Touch sizes: the buttons are hit over 40 px (the icons 44 wide), drawn as before; Redo and
    // Clear then need 80 px more.
    const bool touch = metrics::Touch();

    if (mWide || width >= (touch ? 760.0f : 680.0f))
    {
        row.insert(row.end(), {HGap(2), redo, HGap(2), clear});
    }

    row.insert(row.end(), {HGap(narrow ? 8.0f : 12.0f), display, HGap(narrow ? 8.0f : 12.0f)});

    // The loop's project, as the web's: its name, the unsaved mark, Save and the saved loops.
    if (mWide)
    {
        if (!mLooperTitleBox)
        {
            auto [element, box] = el::input_box("Untitled loop", Font(Weight::Regular).size(11.0f), 1.0f);
            mLooperTitleBox = box;
            mLooperTitleField = el::share(std::move(element));
            box->on_enter = [this](std::string_view) {
                if (mState.looper.layerCount > 0 && mLooperTitleBox)
                {
                    mSession.Commands().SaveLooperProject(Trimmed(mLooperTitleBox->get_utf8()));
                }

                return true;
            };
        }

        // The engine's name, when it changes (a save, a load); what is being typed otherwise.
        if (l.projectTitle != mLooperTitleShown)
        {
            mLooperTitleShown = l.projectTitle;
            mLooperTitleBox->set_text(l.projectTitle);
        }

        auto save = tool("Save", [this] {
            mSession.Commands().SaveLooperProject(mLooperTitleBox ? Trimmed(mLooperTitleBox->get_utf8())
                                                                  : std::string());
        });
        save->Enabled(l.layerCount > 0);
        faced.push_back(save);
        row.insert(row.end(), {FixedWidth(130, Middle(FixedHeight(28, mLooperTitleField)))});

        if (l.dirty)
        {
            auto dot = std::make_shared<Text>(mTheme, "\xE2\x80\xA2", 16.0f, Weight::SemiBold);
            dot->Colour(LooperDisplayFace::kDub).Alignment(HAlign::Centre);
            row.insert(row.end(), {FixedWidth(14, dot)});
        }
        else
        {
            row.push_back(HGap(6));
        }

        row.insert(row.end(), {save, HGap(4), loopsAt, HGap(12)});
    }

    if (mWide && width >= 1120.0f)
    {
        auto click =
            segment("Click", l.click, [this] { mSession.Commands().SetMetronomeEnabled(!mState.looper.click); });
        click->Dot(true).On(l.click);
        click->Tooltip("The metronome as the loop's click: it stops with the loop and starts again in time with "
                       "it. Saved with the loop.");
        row.insert(row.end(), {click, HGap(8)});
    }

    if (mWide && width >= 1400.0f)
    {
        auto press =
            segment("Press", s.start != "armed", [this] { mSession.Commands().SetLooper({{"start", "press"}}); });
        auto armed =
            segment("Armed", s.start == "armed", [this] { mSession.Commands().SetLooper({{"start", "armed"}}); });
        auto free =
            segment("Free", s.length != "bars", [this] { mSession.Commands().SetLooper({{"length", "free"}}); });
        auto bars =
            segment("Bars", s.length == "bars", [this] { mSession.Commands().SetLooper({{"length", "bars"}}); });
        row.insert(row.end(), {press, armed, HGap(8), free, bars});

        if (s.length == "bars")
        {
            const int n = s.bars;
            auto fewer =
                segment("-", false, [this, n] { mSession.Commands().SetLooper({{"bars", std::max(1, n - 1)}}); });
            auto more =
                segment("+", false, [this, n] { mSession.Commands().SetLooper({{"bars", std::min(64, n + 1)}}); });
            fewer->MinWidth(26);
            more->MinWidth(26);
            fewer->AccessName("Fewer bars");
            more->AccessName("More bars");
            auto count = std::make_shared<Text>(mTheme, std::to_string(n), 11.0f, Weight::SemiBold);
            count->Alignment(HAlign::Centre);
            row.insert(row.end(), {HGap(4), fewer, FixedWidth(26, count), more});
        }

        // The tempo the loop plays at; locked while a Bars loop holds it.
        char bpm[24];
        std::snprintf(bpm, sizeof(bpm), "%.0f BPM", l.tempoBpm > 0.0 ? l.tempoBpm : mState.metronome.bpm);
        auto tempo = segment(bpm, l.tempoLocked, [this] { OpenMetronome(); });
        tempo->MinWidth(70);
        tempo->Tooltip(l.tempoLocked ? "Tempo locked to the loop" : "Open the metronome");
        row.insert(row.end(), {HGap(8), tempo, HGap(8)});
    }

    if (mWide)
    {
        row.insert(row.end(), {optionsAt, HGap(4), layers, HGap(10)});
    }
    else
    {
        row.insert(row.end(), {layers, HGap(4), loopsAt, HGap(4), optionsAt, HGap(8)});
    }

    SyncLooperLive();
    // Touch sizes: the row is the strip's height less 3 px each side, and the buttons are drawn
    // as tall as before in the middle of it.
    const float padY = touch ? 3.0f : 8.0f;

    if (touch)
    {
        const float face = LooperRowHeight() - 16.0f;

        for (auto& e : row)
        {
            if (auto b = std::dynamic_pointer_cast<Button>(e))
            {
                b->FaceHeight(face);
            }
        }

        for (auto& b : faced)
        {
            b->FaceHeight(face);
        }

        std::replace(row.begin(), row.end(), display, Middle(FixedHeight(face, display)));
    }

    Ptr strip = FixedHeight(LooperRowHeight(), Fill(mTheme.Bar(), PadXY(0, padY, Row(row))));

    if (mLooperLayersOpen)
    {
        strip = Col({strip, FixedHeight(LooperLayersHeight(), Fill(mTheme.Bar(), PadXY(12, 0, BuildLooperLayers())))});
    }
    else
    {
        mLooperLayerSubs.clear();
    }

    // Where a tap leaves its menus open (ShowLooperMenu).
    strip = std::make_shared<BoundsProbe>(strip, [this](el::rect r) { mLooperStripRect = r; });

    // No minimum width: a strip built for a wider window must not hold the window at that width
    // while the window shrinks; it is rebuilt for the new width at the next tick (LooperShape).
    return el::share(WidthYielding(el::hold_any(strip)));
}

void Shell::ToggleLooperLayers()
{
    // After the press: the strip it is on is rebuilt.
    mSession.View().post([this, alive = std::weak_ptr<bool>(mAlive)] {
        if (!alive.lock())
        {
            return;
        }

        mLooperLayersOpen = !mLooperLayersOpen;
        SyncLooper();
    });
}

void Shell::SyncLooperLive()
{
    if (!mLooperWave)
    {
        return;
    }

    const auto& l = mState.looper;
    const double now = guitarfx::uiclient::LooperClockNow();
    const double position = guitarfx::uiclient::LooperPositionNow(l, now);

    mLooperMain->Set(guitarfx::uiclient::LooperMainButtonLabel(l.state));
    mLooperStatus->Set(guitarfx::uiclient::LooperStatusLabel(l.state));
    mLooperFace->Set(l.lengthSec > 0.0 ? static_cast<float>(position / l.lengthSec) : 0.0f,
                     l.state == "countIn" ? l.countInBeatsLeft : 0);
    mLooperStatus->Colour(l.state == "recording" || l.state == "overdubbing" ? mTheme.Error() : mTheme.Text());
    mLooperTime->Set(guitarfx::uiclient::LooperTimeText(l, position));
    mLooperBar->Set(guitarfx::uiclient::LooperBarText(l));

    mLooperWave->Peaks(guitarfx::uiclient::LooperMixPeaks(l));
    mLooperWave->Overlay(l.livePeaks);
    std::vector<float> lines;

    for (int bar = 1; l.bars > 1 && bar < l.bars; ++bar)
    {
        lines.push_back(static_cast<float>((bar - l.clickBar) / l.bars));
    }

    mLooperWave->BarLines(std::move(lines));
    mLooperWave->Playhead(l.lengthSec > 0.0 ? static_cast<float>(position / l.lengthSec) : -1.0f);
    mLooperWave->Empty(l.state == "empty" ? "Press Rec and play" : "");
}

void Shell::SyncLooper()
{
    // The footer's Loop button comes and goes with the feature and the standalone app.
    const bool shown = LooperShown();

    if (shown != mLooperButtonShown)
    {
        mLooperButtonShown = shown;

        if (!shown)
        {
            mLooperOpen = false;
        }

        Mark(kTransport | kChain | kPage | kRail);
    }

    if (!mLooperOpen)
    {
        if (mLooperWave)
        {
            mLooperWave.reset();
            mLooperMain.reset();
            mLooperFace.reset();
            mLooperLayerSubs.clear();
            CloseLooperMenu(mLooperMenu);
            Fill(*mLooperSlot, FixedHeight(0, std::make_shared<el::element>()));
            mLooperShape.clear();
            mSession.View().layout();
            mSession.View().refresh();
        }

        return;
    }

    if (const auto shape = LooperShape(); shape != mLooperShape)
    {
        // Rebuilt at the height it had, only it is redrawn; at another (the Layers panel opened,
        // closed or grown), the stage over it is sized again.
        const bool wasOpen = static_cast<bool>(mLooperWave);
        const float height = LooperStripHeight();
        const bool resized = !wasOpen || height != mLooperBuiltHeight;
        mLooperShape = shape;
        mLooperBuiltHeight = height;
        Fill(*mLooperSlot, BuildLooperStrip());

        if (!resized)
        {
            mSession.View().layout(*mLooperSlot);
        }
        else
        {
            if (wasOpen)
            {
                Mark(kTransport | kChain | kPage | kRail);
            }

            mSession.View().layout();
            mSession.View().refresh();
        }

        return;
    }

    SyncLooperLive();
    RepaintLooperLive();
}

void Shell::RepaintLooperLive()
{
    for (el::element* e : std::initializer_list<el::element*>{mLooperWave.get(), mLooperTime.get(), mLooperStatus.get(),
                                                              mLooperBar.get(), mLooperMain.get(), mLooperFace.get()})
    {
        if (e != nullptr)
        {
            mSession.Repaint(*e);
        }
    }
}

void Shell::TickLooper()
{
    // The engine reports the position a few times a second; between, the playhead runs on here.
    if (mLooperWave && mState.looper.playing && mState.looper.lengthSec > 0.0)
    {
        SyncLooperLive();
        RepaintLooperLive();
    }
}

Ptr Shell::BuildLooperLayers()
{
    mLooperLayerSubs.clear();
    auto body = MakeSlot();
    auto signature = std::make_shared<std::string>("-");
    auto built = std::make_shared<bool>(false);
    // Each layer's level and balance sliders, kept so the engine's echo moves them in place.
    auto sliders = std::make_shared<
        std::vector<std::pair<std::string, std::pair<std::shared_ptr<ParamSlider>, std::shared_ptr<ParamSlider>>>>>();
    const bool oneLine = StageWidth() >= kOneLineLayers;

    auto rebuild = [this, body, signature, sliders, built, oneLine] {
        const auto& l = mState.looper;
        std::string s;

        for (const auto& layer : l.layers)
        {
            s += "|" + layer.id + layer.title + (layer.muted ? "m" : "") + (layer.solo ? "s" : "") +
                 (layer.recording ? "r" : "") + layer.presetName + (layer.toneChanged ? "t" : "") +
                 (layer.canRevert ? "v" : "") + std::to_string(static_cast<int>(layer.retoneProgress * 20.0));
        }

        if (s != *signature)
        {
            *signature = s;
            sliders->clear();
            std::vector<Ptr> rows;

            if (l.layers.empty())
            {
                auto none = std::make_shared<Text>(mTheme, "No layers yet. Press Rec and play.", 12.0f);
                none->Colour(mTheme.TextSecondary());
                rows.push_back(FixedHeight(40, none));
            }
            else
            {
                auto all = std::make_shared<Button>(
                    mTheme, "Re-amp all with the current rig", [this] { mSession.Commands().RetoneLooperLayers({}); },
                    Button::Style::Plain);
                all->MinWidth(220).TextSize(10.5f);
                rows.push_back(FixedHeight(34, Row({Middle(FixedHeight(28, all)), Stretch()})));
            }

            for (const auto& layer : l.layers)
            {
                const auto id = layer.id;
                const auto button = [this](const std::string& text, std::function<void()> onClick) {
                    auto b = std::make_shared<Button>(mTheme, text, std::move(onClick), Button::Style::Plain);
                    b->MinWidth(30).TextSize(10.5f);
                    return b;
                };
                auto swatch =
                    Fill(HueColour(guitarfx::uiclient::LooperLayerHue(layer)), std::make_shared<el::element>());
                auto title = button(layer.title.empty() ? id : layer.title, [this, id, current = layer.title] {
                    OpenPrompt("Rename layer", current, "Rename", [this, id](const std::string& text) {
                        mSession.Commands().SetLooperLayer(id, {{"title", text.substr(0, 80)}});
                    });
                });
                title->MinWidth(110).Alignment(HAlign::Left);
                title->AccessName("Layer name");
                const bool retoning = layer.retoneProgress >= 0.0;
                char retone[48];
                std::snprintf(retone, sizeof(retone), "Re-amping %d%%",
                              static_cast<int>(std::lround(layer.retoneProgress * 100.0)));
                auto tone = button(retoning ? std::string(retone)
                                            : (layer.presetName.empty() ? std::string("Tone") : layer.presetName) +
                                                  (layer.toneChanged ? " \xE2\x80\xA2" : ""),
                                   [this, id] { mSession.Commands().RetoneLooperLayers({id}); });
                tone->MinWidth(120).Enabled(!layer.recording && !retoning);
                tone->Tooltip("Played through this preset. Click to re-amp with the current rig.");
                std::vector<Ptr> first{Middle(FixedWidth(10, FixedHeight(18, swatch))), HGap(8), title, HGap(6), tone};

                if (layer.canRevert)
                {
                    auto revert =
                        button("\xE2\x86\xBA", [this, id] { mSession.Commands().RetoneLooperLayers({id}, true); });
                    revert->Tooltip("Back to the recorded tone");
                    first.insert(first.end(), {HGap(4), revert});
                }

                if (layer.recording)
                {
                    auto tag = std::make_shared<Text>(mTheme, "Recording", 10.5f, Weight::SemiBold);
                    tag->Colour(mTheme.Error());
                    first.insert(first.end(), {HGap(8), FixedWidth(70, tag)});
                }

                auto mute = button(
                    "M", [this, id, on = layer.muted] { mSession.Commands().SetLooperLayer(id, {{"muted", !on}}); });
                mute->Selected(layer.muted);
                mute->Tooltip("Mute");
                auto solo = button(
                    "S", [this, id, on = layer.solo] { mSession.Commands().SetLooperLayer(id, {{"solo", !on}}); });
                solo->Selected(layer.solo);
                solo->Tooltip("Solo");
                std::vector<Ptr> flags{mute, HGap(2), solo};

                if (!layer.recording)
                {
                    auto riff = button("Riff", [this, id] {
                        // The layer comes back as a take to save, in the riff dialog.
                        OpenRiffCapture(false);
                        mSession.Commands().ExportLooperLayer(id, "riff");
                    });
                    riff->Tooltip("Send this layer's DI to the riff library as a take");
                    auto wav = button("WAV", [this, id] { mSession.Commands().ExportLooperLayer(id, "wetWav"); });
                    wav->Tooltip("Save this layer's sound as a WAV file");
                    flags.insert(flags.end(), {HGap(6), riff, HGap(2), wav});
                }

                auto remove = button("\xC3\x97", [this, id] { mSession.Commands().RemoveLooperLayer(id); });
                remove->Tooltip("Remove layer");
                flags.insert(flags.end(), {HGap(6), remove});

                auto level = std::make_shared<ParamSlider>(mTheme, "Level");
                level->onChange = [this, id](double v) {
                    mSession.Commands().SetLooperLayer(id, {{"level", std::round(v * 200.0) / 100.0}});
                };
                level->onReset = [this, id] { mSession.Commands().SetLooperLayer(id, {{"level", 1.0}}); };
                auto balance = std::make_shared<ParamSlider>(mTheme, "Balance");
                balance->onChange = [this, id](double v) {
                    mSession.Commands().SetLooperLayer(id, {{"balance", std::round(v * 200.0) / 100.0 - 1.0}});
                };
                balance->onReset = [this, id] { mSession.Commands().SetLooperLayer(id, {{"balance", 0.0}}); };
                sliders->push_back({id, {level, balance}});

                if (oneLine)
                {
                    // As the web's row: the name and tone, Level and Balance, then the flags.
                    rows.push_back(FixedHeight(52, Row({Middle(FixedHeight(30, Tight(first))), HGap(16),
                                                        FixedWidth(170, level), HGap(12), FixedWidth(170, balance),
                                                        HGap(16), Middle(FixedHeight(30, Tight(flags))), Stretch()})));
                    rows.push_back(VGap(3));
                }
                else
                {
                    first.push_back(Stretch());
                    first.insert(first.end(), flags.begin(), flags.end());
                    rows.push_back(FixedHeight(30, Row(first)));
                    rows.push_back(FixedHeight(52, Row({HGap(18), level, HGap(12), balance})));
                    rows.push_back(VGap(6));
                }

                rows.push_back(FixedHeight(1, Fill(mTheme.Text().opacity(0.08f), std::make_shared<el::element>())));
                rows.push_back(VGap(oneLine ? 4.0f : 6.0f));
            }

            // A gutter at the right for the scroll bar.
            Fill(*body, VScroll(Pad(0, 0, 14, 0, Col(rows))));

            // The first build is laid out with the strip it is in.
            if (*built)
            {
                mSession.View().layout(*body);
            }
        }

        *built = true;

        // The sliders follow the engine, except the one under the pointer.
        for (const auto& [id, pair] : *sliders)
        {
            for (const auto& layer : l.layers)
            {
                if (layer.id != id)
                {
                    continue;
                }

                char text[24];
                std::snprintf(text, sizeof(text), "%.0f%%", layer.level * 100.0);

                if (!pair.first->Dragging())
                {
                    pair.first->Update(layer.level / 2.0, text);
                }

                std::snprintf(text, sizeof(text), "%+.2f", layer.balance);

                if (!pair.second->Dragging())
                {
                    pair.second->Update((layer.balance + 1.0) / 2.0, text);
                }

                mSession.Repaint(*pair.first);
                mSession.Repaint(*pair.second);
            }
        }
    };

    mLooperLayerSubs.push_back(mSession.Client().Subscribe(Topic::Looper, rebuild));
    rebuild();
    // A rule between the strip's row and its layers.
    return Col({FixedHeight(1, Fill(mTheme.Text().opacity(0.08f), std::make_shared<el::element>())), VGap(4), body});
}

bool Shell::CloseLooperMenu(int menu)
{
    const auto layer = mLooperMenuLayer.lock();

    if (!layer || layer != mOverlay)
    {
        mLooperMenu = 0;
        return false;
    }

    if (menu == 0 || mLooperMenu != menu)
    {
        return false;
    }

    CloseOverlay();
    mLooperMenu = 0;
    return true;
}

void Shell::ShowLooperMenu(int menu, Ptr body, el::rect anchor, float width, float height)
{
    // Up from its button, as the web's .looper-options-body and .looper-projects-body; a tap on
    // the strip goes to the strip and leaves it open.
    ShowPopover(PadXY(12, 10, std::move(body)), anchor, width, height, true, mLooperStripRect);
    mLooperMenu = menu;
    mLooperMenuLayer = mOverlay;
}

void Shell::OpenLooperLoops()
{
    if (CloseLooperMenu(1))
    {
        return;
    }

    mSession.Commands().GetLooperProjects();
    auto body = MakeSlot();
    auto signature = std::make_shared<std::string>("-");

    auto rebuild = [this, body, signature] {
        const auto& l = mState.looper;
        std::string s = std::string(l.recoverable ? "R" : "-") + (l.dirty ? "d" : "") + l.projectId;

        for (const auto& p : l.projects)
        {
            s += "|" + p.id + p.title + std::to_string(p.layerCount);
        }

        for (const auto& r : mState.riffs.riffs)
        {
            s += "|" + r.id + std::to_string(r.takes.size());
        }

        if (s == *signature)
        {
            return;
        }

        *signature = s;

        const auto button = [this](const std::string& text, std::function<void()> onClick) {
            auto b = std::make_shared<Button>(mTheme, text, std::move(onClick), Button::Style::Plain);
            b->MinWidth(60).TextSize(11.0f);
            return b;
        };
        const auto heading = [this](const char* text) {
            auto t = std::make_shared<Text>(mTheme, text, 10.0f, Weight::SemiBold);
            t->Colour(mTheme.TextMuted());
            return FixedHeight(24, t);
        };
        std::vector<Ptr> rows;

        if (l.recoverable)
        {
            // As the web's .looper-project-recover: the note in the dub colour, its buttons under it.
            auto note = std::make_shared<Text>(mTheme, "The last session ended with an unsaved loop.", 12.0f);
            note->Colour(LooperDisplayFace::kDub);
            rows.push_back(FixedHeight(22, note));
            rows.push_back(FixedHeight(
                36,
                Row({button("Recover",
                            [this] {
                                mSession.Commands().RecoverLooperSession();
                                CloseOverlay();
                            }),
                     HGap(4), button("Discard", [this] { mSession.Commands().DismissLooperRecovery(); }), Stretch()})));
            rows.push_back(VGap(8));
        }

        rows.push_back(heading("SAVED LOOPS"));

        if (l.projects.empty())
        {
            auto none = std::make_shared<Text>(mTheme, "No saved loops yet.", 12.0f);
            none->Colour(mTheme.TextSecondary());
            rows.push_back(FixedHeight(28, none));
        }

        for (const auto& p : l.projects)
        {
            // As the web's row: the name opens it, then what it holds, then a x deletes it.
            const std::string titleText = p.title.empty() ? std::string("Untitled loop") : p.title;
            auto open = std::make_shared<Button>(
                mTheme, titleText,
                [this, id = p.id] {
                    if (!mState.looper.dirty)
                    {
                        mSession.Commands().LoadLooperProject(id);
                        CloseOverlay();
                        return;
                    }

                    OpenConfirm("Open loop", "The loop playing now has not been saved.", "Open",
                                [this, id] { mSession.Commands().LoadLooperProject(id); });
                },
                Button::Style::Plain);
            open->Flexible(true).MinWidth(80).TextSize(11.0f).Alignment(HAlign::Left);
            open->Selected(p.id == l.projectId);
            open->Tooltip("Open");
            std::string detail = Seconds(p.lengthSec) + " \xC2\xB7 " + std::to_string(p.layerCount) +
                                 (p.layerCount == 1 ? " layer" : " layers");

            if (p.bars > 0)
            {
                detail += " \xC2\xB7 " + std::to_string(p.bars) + " bars";
            }

            auto meta = std::make_shared<Text>(mTheme, detail, 10.5f);
            meta->Colour(mTheme.TextSecondary());
            auto remove = std::make_shared<Button>(
                mTheme, "\xC3\x97",
                [this, id = p.id, titleText] {
                    OpenConfirm("Delete loop", "\"" + titleText + "\" is deleted.", "Delete", [this, id] {
                        mSession.Commands().DeleteLooperProject(id);
                        mSession.View().post([this, alive = mAlive] {
                            if (*alive)
                            {
                                OpenLooperLoops();
                            }
                        });
                    });
                },
                Button::Style::Plain);
            remove->MinWidth(28).TextSize(12.0f);
            remove->Tooltip("Delete");
            remove->AccessName("Delete loop");
            rows.push_back(FixedHeight(34, Row({Middle(FixedHeight(28, open)), HGap(8), FixedWidth(140, meta), HGap(4),
                                                Middle(FixedHeight(28, remove))})));
        }

        rows.push_back(VGap(8));
        rows.push_back(heading("ADD A RIFF AS A LAYER"));

        if (mState.riffs.riffs.empty())
        {
            auto none = std::make_shared<Text>(mTheme, "No riffs in the library.", 12.0f);
            none->Colour(mTheme.TextSecondary());
            rows.push_back(FixedHeight(28, none));
        }

        for (const auto& riff : mState.riffs.riffs)
        {
            // The preferred take, else the last, as the web UI's Loops menu picks it.
            std::string takeId;

            for (const auto& take : riff.takes)
            {
                if (take.id == riff.preferredTakeId)
                {
                    takeId = take.id;
                }
            }

            if (takeId.empty() && !riff.takes.empty())
            {
                takeId = riff.takes.back().id;
            }

            if (takeId.empty())
            {
                continue;
            }

            auto title = std::make_shared<Text>(mTheme, riff.title, 12.0f);
            auto add = button("Add", [this, takeId] {
                mSession.Commands().AddRiffAsLooperLayer(takeId);
                CloseOverlay();
            });
            rows.push_back(FixedHeight(32, Row({title, Stretch(), add})));
        }

        Fill(*body, VScroll(Col(rows)));
        mSession.View().layout();
        mSession.View().refresh();
    };

    // Up to half the window, as the web's max-height: 50vh, the rest scrolled.
    const auto& l = mState.looper;
    const float listed = (l.recoverable ? 66.0f : 0.0f) + 24.0f + std::max<float>(1.0f, l.projects.size()) * 34.0f +
                         32.0f + std::max<float>(1.0f, mState.riffs.riffs.size()) * 32.0f + 20.0f;
    ShowLooperMenu(1, body, mLooperLoopsRect, 440.0f, std::clamp(listed, 180.0f, std::floor(ViewSize().y * 0.5f)));
    mInstrumentSubs.push_back(mSession.Client().Subscribe(Topic::Looper, rebuild));
    mInstrumentSubs.push_back(mSession.Client().Subscribe(Topic::Riffs, rebuild));
    rebuild();
}

void Shell::OpenLooperOptions()
{
    if (CloseLooperMenu(2))
    {
        return;
    }

    std::vector<Ptr> page;
    std::vector<std::function<void()>> syncs;
    const auto label = [this](const char* text) {
        auto l = std::make_shared<Text>(mTheme, text, 12.0f);
        l->Colour(mTheme.TextSecondary());
        return FixedWidth(150, l);
    };
    // A row of options, one lit, each sending its value.
    const auto choice = [&](const char* title, std::vector<std::pair<std::string, nlohmann::json>> options,
                            const char* key, std::function<nlohmann::json()> current) {
        std::vector<Ptr> row{label(title)};
        auto buttons = std::make_shared<std::vector<std::pair<std::shared_ptr<Button>, nlohmann::json>>>();

        for (const auto& option : options)
        {
            const nlohmann::json value = option.second;
            auto b = std::make_shared<Button>(
                mTheme, option.first, [this, key, value] { mSession.Commands().SetLooper({{key, value}}); },
                Button::Style::Plain);
            b->MinWidth(64).TextSize(11.0f);
            buttons->push_back({b, value});
            row.push_back(b);
            row.push_back(HGap(4));
        }

        row.push_back(Stretch());
        page.push_back(FixedHeight(40, Row(row)));
        syncs.push_back([buttons, current] {
            const auto now = current();

            for (auto& entry : *buttons)
            {
                entry.first->Selected(entry.second == now);
            }
        });
    };
    const auto& s = [this]() -> const guitarfx::uiclient::LooperSettings& { return mState.looper.settings; };

    // The compact strip has no room for Save, nor always for Redo and Clear: they are here.
    if (!mWide)
    {
        const auto action = [this](const char* text, std::function<void()> onClick) {
            auto b = std::make_shared<Button>(mTheme, text, std::move(onClick), Button::Style::Plain);
            b->MinWidth(64).TextSize(11.0f);
            return b;
        };
        auto redo = action("Redo", [this] { mSession.Commands().LooperCommand("redo"); });
        auto clear = action("Clear", [this] { ClearLooperWithConfirm(); });
        auto save = action("Save...", [this] { SaveLooperWithPrompt(); });
        page.push_back(FixedHeight(40, Row({label("Loop"), redo, HGap(4), clear, HGap(4), save, Stretch()})));
        syncs.push_back([this, redo, clear, save] {
            const auto& l = mState.looper;
            redo->Enabled(l.canRedo);
            clear->Enabled(l.state != "empty" || l.canRedo);
            save->Enabled(l.layerCount > 0);
        });
    }

    choice("Start", {{"On press", "press"}, {"Armed", "armed"}}, "start", [s] { return nlohmann::json(s().start); });
    choice("Length", {{"Free", "free"}, {"Bars", "bars"}}, "length", [s] { return nlohmann::json(s().length); });
    {
        auto count = std::make_shared<Text>(mTheme, "", 12.0f, Weight::SemiBold);
        count->Alignment(HAlign::Centre);
        const auto step = [this](int by) {
            mSession.Commands().SetLooper({{"bars", std::clamp(mState.looper.settings.bars + by, 1, 64)}});
        };
        auto fewer = std::make_shared<Button>(mTheme, "-", [step] { step(-1); }, Button::Style::Plain);
        auto more = std::make_shared<Button>(mTheme, "+", [step] { step(1); }, Button::Style::Plain);
        fewer->MinWidth(30);
        more->MinWidth(30);
        page.push_back(
            FixedHeight(40, Row({label("Bars (a Bars loop)"), fewer, FixedWidth(70, count), more, Stretch()})));
        syncs.push_back([this, count] { count->Set(std::to_string(mState.looper.settings.bars)); });
    }
    {
        // The strip's Click: the metronome, which a Bars loop counts against.
        const auto onOff = [this](const char* text, bool on) {
            auto b = std::make_shared<Button>(
                mTheme, text, [this, on] { mSession.Commands().SetMetronomeEnabled(on); }, Button::Style::Plain);
            b->MinWidth(64).TextSize(11.0f);
            return b;
        };
        auto on = onOff("On", true);
        auto off = onOff("Off", false);
        page.push_back(FixedHeight(40, Row({label("Click"), on, HGap(4), off, Stretch()})));
        syncs.push_back([this, on, off] {
            on->Selected(mState.looper.click);
            off->Selected(!mState.looper.click);
        });
    }
    choice("Count-in", {{"None", 0}, {"1 bar", 1}, {"2 bars", 2}}, "countInBars",
           [s] { return nlohmann::json(s().countInBars); });
    {
        // A tick box, as the web's.
        auto box = std::make_shared<CheckBox>(mTheme, "Click while recording", 12.0f);
        box->onClick = [this] {
            mSession.Commands().SetLooper({{"clickWhileRecording", !mState.looper.settings.clickWhileRecording}});
        };
        page.push_back(FixedHeight(36, Row({Middle(box), Stretch()})));
        syncs.push_back([this, box] { box->Set(mState.looper.settings.clickWhileRecording); });
    }
    choice("After a Bars take", {{"Play", "play"}, {"Overdub", "overdub"}}, "afterRecord",
           [s] { return nlohmann::json(s().afterRecord); });
    choice("Longest loop", {{"30 s", 30}, {"60 s", 60}, {"120 s", 120}, {"300 s", 300}}, "maxSeconds",
           [s] { return nlohmann::json(s().maxSeconds); });

    auto offset = std::make_shared<Text>(mTheme, "", 12.0f, Weight::SemiBold);
    offset->Alignment(HAlign::Centre);
    const auto nudge = [this](double by) {
        const double next = std::clamp(mState.looper.settings.alignOffsetMs + by, -50.0, 50.0);
        mSession.Commands().SetLooper({{"alignOffsetMs", next}});
    };
    auto earlier = std::make_shared<Button>(mTheme, "-", [nudge] { nudge(-1.0); }, Button::Style::Plain);
    auto later = std::make_shared<Button>(mTheme, "+", [nudge] { nudge(1.0); }, Button::Style::Plain);
    earlier->MinWidth(30);
    later->MinWidth(30);
    page.push_back(FixedHeight(40, Row({label("Timing offset"), earlier, FixedWidth(70, offset), later, Stretch()})));
    auto memory = std::make_shared<Text>(mTheme, "", 11.0f);
    memory->Colour(mTheme.TextMuted());
    page.push_back(FixedHeight(24, memory));
    syncs.push_back([this, offset, memory] {
        char text[48];
        std::snprintf(text, sizeof(text), "%+.0f ms", mState.looper.settings.alignOffsetMs);
        offset->Set(text);
        std::snprintf(text, sizeof(text), "%.0f MB in use", mState.looper.memoryMb);
        memory->Set(text);
    });

    // Tall enough for every row; a shorter window scrolls it.
    ShowLooperMenu(2, VScroll(Col(page)), mLooperOptionsRect, 480.0f, mWide ? 396.0f : 436.0f);

    // Redrawn only when an option, the memory or what the loop allows moved: the looper reports
    // several times a second while it plays.
    auto shown = std::make_shared<std::string>();
    auto sync = [this, syncs, shown] {
        const auto& l = mState.looper;
        const auto& o = l.settings;
        const auto now = o.start + o.length + std::to_string(o.bars) + "|" + std::to_string(o.countInBars) +
                         (o.clickWhileRecording ? "c" : "") + o.afterRecord + std::to_string(o.maxSeconds) + "|" +
                         std::to_string(static_cast<int>(o.alignOffsetMs)) + "|" +
                         std::to_string(static_cast<int>(l.memoryMb)) + "|" + (l.click ? "k" : "") +
                         (l.canRedo ? "r" : "") + (l.state == "empty" ? "e" : "") + (l.layerCount > 0 ? "l" : "");

        if (now == *shown)
        {
            return;
        }

        *shown = now;

        for (const auto& f : syncs)
        {
            f();
        }

        mSession.View().refresh();
    };
    mInstrumentSubs.push_back(mSession.Client().Subscribe(Topic::Looper, sync));
    sync();
}

void Shell::ClearLooperWithConfirm()
{
    if (!mState.looper.dirty)
    {
        mSession.Commands().LooperCommand("clear");
        return;
    }

    OpenConfirm("Clear the loop", "This loop has not been saved.", "Clear",
                [this] { mSession.Commands().LooperCommand("clear"); });
}

void Shell::SaveLooperWithPrompt()
{
    const auto& looper = mState.looper;
    OpenPrompt("Save loop", looper.projectTitle.empty() ? std::string("Loop") : looper.projectTitle, "Save",
               [this](const std::string& title) { mSession.Commands().SaveLooperProject(title); });
}
} // namespace nanoq::ui
