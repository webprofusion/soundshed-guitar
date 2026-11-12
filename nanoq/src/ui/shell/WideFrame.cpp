// The full layout, as the web UI draws it above its compact breakpoint (ui-components/
// header-icon-bar.html, control-bar.html with input-control-group.html, preset-group.html and
// output-control-group.html, signal-path-bar.html, footer-bar.html): the PLAY / TONES tabs, the
// control bar, the chain with the selected effect's editor under it, and the full footer. Also
// what decides which layout the window gets, and the pieces both footers share.

#include "ui/shell/Shell.h"

#include "ui/chain/ChainWires.h"
#include "ui/shell/FrameParts.h"
#include "ui/widgets/DragScroll.h"

#include "uiclient/FeatureFlags.h"
#include "uiclient/PresetBrowse.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace nanoq::ui
{
using namespace frame;

namespace
{
/// Whether the control bar's toolbar has room for NEW and PUBLISH spelt out: 406 px with them,
/// and the middle of the bar is the window less the meters and the six cells either side of it
/// (636 px). At 1024 wide (an iPad) that leaves 388, and the toolbar's end, the preset menu, lay
/// under the EQ cell where no press reached it. The web UI drops the same two labels there.
bool ToolbarLabelsFit(float viewWidth)
{
    constexpr float kHeaderSides = 2.0f * (28.0f + 30.0f + 14.0f) + 5.0f * 84.0f + 72.0f;
    // With touch sizes every button is hit over 44 px, and the toolbar is 596 px spelt out.
    const float labelledToolbar = metrics::Touch() ? 596.0f : 406.0f;
    return viewWidth - kHeaderSides >= labelledToolbar + 8.0f;
}
} // namespace

// ── Which layout ────────────────────────────────────────────────────────────────

el::extent Shell::ViewSize() const
{
    auto& view = mSession.View();
    const auto size = view.size();
    const float scale = view.scale() > 0.0f ? view.scale() : 1.0f;
    return {size.x / scale, size.y / scale};
}

bool Shell::WantsWide() const
{
    // The web UI's rule (compactMode.ts): compact at or below 980 wide or 620 high, in layout
    // pixels (the zoom divided out), unless the density setting says always one or the other.
    const auto density = StringSetting(mState.appSettings, kDensitySetting, "auto");

    if (density == "full")
    {
        return true;
    }

    if (density == "compact")
    {
        return false;
    }

    // A touch-first device (an iPad, the kiosk) keeps the compact frame, split, up to an iPad Pro
    // 11 in landscape: the full layout's controls are sized for a mouse.
    const auto size = ViewSize();

    if (TouchFirst())
    {
        return size.x > metrics::kTouchCompactMaxWidth && size.y > metrics::kTouchCompactMaxHeight;
    }

    return size.x > metrics::kCompactMaxWidth && size.y > metrics::kCompactMaxHeight;
}

bool Shell::WantsSplit() const
{
    return ViewSize().y >= metrics::kCompactSplitMinHeight;
}

bool Shell::TouchSizes() const
{
    const auto touch = StringSetting(mState.appSettings, kTouchControlsSetting, "auto");
    return touch == "on" || (touch != "off" && TouchFirst());
}

Ptr Shell::BuildFrame()
{
    if (mWide)
    {
        auto rule = [this] {
            return FixedHeight(1, Fill(mTheme.Text().opacity(0.06f), std::make_shared<el::element>()));
        };
        return Col({FixedHeight(metrics::kTabBarHeight, Yielding(mRailSlot)), rule(),
                    FixedHeight(metrics::HeaderHeight() - 1.0f, Yielding(mTopSlot)), rule(), Yielding(mPageSlot),
                    mLooperSlot, FixedHeight(metrics::kWideFooterHeight, Yielding(mTransportSlot))});
    }

    // The rail down the left; beside it the preset bar, the stage and the footer. The regions
    // yield their minimum sizes: each is built for the window's size and rebuilt when it changes
    // (OnTick), and the window's own floor is the root's (MinSize).
    return Row({FixedWidth(metrics::kRailWidth, Yielding(mRailSlot)),
                Col({FixedHeight(metrics::TopBarHeight(), Yielding(mTopSlot)), Yielding(mPageSlot), mLooperSlot,
                     FixedHeight(metrics::FooterHeight(), Yielding(mTransportSlot))})});
}

std::string Shell::TopBarSignature() const
{
    const auto* summary = mState.FindPresetSummary(mState.activePresetId);
    std::string s =
        std::string(mWide    ? "w"
                    : mSplit ? "s"
                             : "c") +
        std::to_string(static_cast<int>(mPage)) + "|" + mState.activePresetId + "|" + PresetTitle(mState) + "|" +
        (mState.activePresetDirty ? "d" : "") + (mState.activePreset ? "p" : "") +
        (guitarfx::uiclient::IsPresetFavorite(mState, mState.activePresetId) ? "f" : "") + "|" +
        (summary != nullptr ? summary->source : std::string()) + "|" + (mPresetTrail.CanGoBack() ? "<" : "") +
        (mPresetTrail.CanGoForward() ? ">" : "") + (mUpdate ? "|u" : "") +
        (mWide && ToolbarLabelsFit(ViewSize().x) ? "|l" : "") +
        (metrics::Touch() ? "|t" + std::to_string(mWide ? 0 : TouchBarExtras(StageWidth(), static_cast<bool>(mUpdate)))
                          : "");
    return s;
}

// ── The presets loaded, for back and forward ────────────────────────────────────

void Shell::NotePresetTrail()
{
    mPresetTrail.Note(mState.activePresetId);
    Mark(kTopBar);
}

void Shell::StepPresetTrail(int direction)
{
    const auto id = mPresetTrail.Step(
        direction, [this](const std::string& presetId) { return mState.FindPresetSummary(presetId) != nullptr; });
    Mark(kTopBar);

    if (id)
    {
        LoadWithConfirm(*id);
    }
}

// ── The tab row ─────────────────────────────────────────────────────────────────

Ptr Shell::BuildTabBar()
{
    auto tab = [this](const char* label, bool active, Page page) {
        auto t = std::make_shared<TextTab>(mTheme, label, active, 12.0f, 28.0f, metrics::kTabBarHeight);
        t->onClick = [this, page] { ShowPage(page); };
        return t;
    };
    const bool play = mPage == Page::Chain || mPage == Page::Effect || mPage == Page::Pads;

    auto settings = std::make_shared<IconButton>(mTheme, mIcons, "ui-gear", IconButton::Look::Boxed);
    settings->Size(28, 28).IconSize(15).Lit(mPage == Page::Settings);
    settings->Ink(mPage == Page::Settings ? mTheme.Accent() : mTheme.TextSecondary());
    settings->onClick = [this] { ShowPage(mPage == Page::Settings ? Page::Chain : Page::Settings); };

    // The tabs sit in the middle of the row whatever is beside them.
    std::vector<Ptr> tabItems{tab("PLAY", play, play ? mPage : Page::Chain)};

    if (guitarfx::uiclient::IsFeatureEnabled(mState, guitarfx::uiclient::Feature::ToneSharing))
    {
        tabItems.push_back(tab("TONES", mPage == Page::Tones, Page::Tones));
    }

    if (JamShown())
    {
        tabItems.push_back(tab("JAM", mPage == Page::Jam, Page::Jam));
    }

    // No Presets tab, as in the web UI: the preset pill opens the presets.
    auto tabs = Tight(tabItems);
    // A newer release's Update button sits beside the gear, as the web UI's header pill does.
    std::vector<Ptr> row{HGap(38), Stretch(), tabs, Stretch()};

    if (auto update = BuildUpdateButton())
    {
        row.insert(row.end(), {Pad(0, 5, 0, 5, update), HGap(8)});
    }

    row.insert(row.end(), {Pad(0, 4, 0, 4, settings), HGap(10)});
    return Fill(mTheme.Background(), Row(row));
}

// ── The control bar ─────────────────────────────────────────────────────────────

Ptr Shell::BuildHeader()
{
    auto& commands = mSession.Commands();

    // The global chain at either end, the same cells as the compact Controls sheet.
    auto cells = BuildControlCells();

    // ── The preset toolbar ──
    const bool dirty = mState.activePresetDirty;
    const bool havePreset = mState.activePreset.has_value();
    const std::string presetId = mState.activePresetId;
    const bool favourite = guitarfx::uiclient::IsPresetFavorite(mState, presetId);
    const auto* summary = mState.FindPresetSummary(presetId);

    auto boxed = [this](const char* icon, std::function<void()> onClick) {
        auto b = std::make_shared<IconButton>(mTheme, mIcons, icon, IconButton::Look::Boxed);
        b->Size(30, 28).IconSize(14);
        b->onClick = std::move(onClick);
        return b;
    };
    auto divider = [this] {
        return Pad(7, 6, 7, 6, FixedWidth(1, Fill(mTheme.Text().opacity(0.1f), std::make_shared<el::element>())));
    };

    // NEW and PUBLISH are spelt out only where the bar has room for them (ToolbarLabelsFit).
    const bool labelled = ToolbarLabelsFit(ViewSize().x);

    auto create = std::make_shared<IconButton>(mTheme, mIcons, "ui-new", IconButton::Look::Boxed);
    create->IconSize(13).Caret(true);

    if (labelled)
    {
        create->Size(72, 28).Label("NEW", 10.5f);
    }
    else
    {
        create->Size(44, 28);
    }

    create->onClick = [this, b = create.get()] { OpenNewPresetMenu(b->Bounds()); };

    // Edit and Save only for a preset that may be overwritten, as the web UI's toolbar.
    const bool canModify = havePreset && guitarfx::uiclient::CanModifyActivePreset(mState);
    auto edit = boxed("ui-edit", [this] { EditActivePreset(); });
    edit->Disabled(!canModify);
    auto save = boxed("ui-save", [this] { SaveActivePreset(false); });
    save->Disabled(!canModify);

    if (dirty)
    {
        save->Ink(mTheme.Accent());
    }

    auto random = boxed("ui-random", [this] { LoadRandomPreset(); });

    // Back and forward through the presets loaded (#preset-undo, #preset-redo).
    auto back = ToolButton(mTheme, mIcons, "ui-undo", [this] { StepPresetTrail(-1); }, 28.0f, 15.0f);
    back->Disabled(!mPresetTrail.CanGoBack());
    auto forward = ToolButton(mTheme, mIcons, "ui-redo", [this] { StepPresetTrail(1); }, 28.0f, 15.0f);
    forward->Disabled(!mPresetTrail.CanGoForward());

    auto more = std::make_shared<IconButton>(mTheme, mIcons, "ui-more", IconButton::Look::Boxed);
    more->Size(30, 28).IconSize(14).Caret(true);
    more->onClick = [this, b = more.get()] { OpenPresetMenu(b->Bounds()); };

    // Publish (#preset-publish): publishing is Soundshed Guitar's, and this says so.
    auto publish = std::make_shared<IconButton>(mTheme, mIcons, "ui-share", IconButton::Look::Accent);
    publish->IconSize(13);

    if (labelled)
    {
        publish->Size(84, 28).Label("PUBLISH", 10.5f);
    }
    else
    {
        publish->Size(30, 28);
    }

    publish->onClick = [this] { OpenPublishInfo(); };

    auto toolbar = Tight({create, HGap(6), edit, HGap(6), save, divider(), random, divider(), back, HGap(2), forward,
                          divider(), publish, divider(), more});

    // ── The preset selector ──
    auto prev = std::make_shared<IconButton>(mTheme, mIcons, "ui-prev", IconButton::Look::Soft);
    prev->Size(30, 30).IconSize(16);
    prev->onClick = [this] { StepPresetWithConfirm(-1); };
    auto next = std::make_shared<IconButton>(mTheme, mIcons, "ui-next", IconButton::Look::Soft);
    next->Size(30, 30).IconSize(16);
    next->onClick = [this] { StepPresetWithConfirm(1); };

    auto pill = std::make_shared<PresetPill>(mTheme, mIcons, PresetTitle(mState), favourite, dirty);
    pill->Height(36).Badge(summary != nullptr ? SourceLabel(summary->source) : std::string());
    pill->onClick = [this, p = pill.get()] { OpenPresetChooser(p->Bounds()); };
    pill->onFavourite = [&commands, presetId, favourite] {
        if (!presetId.empty())
        {
            commands.SetPresetFavorite(presetId, !favourite);
        }
    };

    // The middle gets what the knobs leave, up to the web UI's 680 px.
    auto centre =
        WidthRange(330.0f, 680.0f,
                   Col({VGap(27), FixedHeight(28, Row({Stretch(), toolbar, Stretch()})), VGap(9),
                        FixedHeight(36, Row({Pad(0, 3, 0, 3, prev), HGap(8), pill, HGap(8), Pad(0, 3, 0, 3, next)})),
                        std::make_shared<el::element>()}));

    auto meterColumn = [](Ptr meter, Ptr below) {
        std::vector<Ptr> parts{VGap(26), FixedHeight(84, std::move(meter))};

        if (below)
        {
            parts.push_back(VGap(2));
            parts.push_back(
                FixedHeight(metrics::Touch() ? 36.0f : 18.0f, Row({Stretch(), std::move(below), Stretch()})));
        }

        parts.push_back(std::make_shared<el::element>());
        return FixedWidth(30, Col(parts));
    };
    auto cellsAt = [](std::vector<Ptr> list) {
        auto row = std::make_shared<el::htile_composite>();

        for (auto& c : list)
        {
            row->push_back(Pad(0, 28, 0, 0, c));
        }

        return Ptr(row);
    };

    auto bar = Row({HGap(28), meterColumn(cells.inMeter, cells.levelling), HGap(14),
                    cellsAt({cells.in, cells.gate, cells.transpose}), Stretch(), centre, Stretch(),
                    cellsAt({cells.eq, cells.doubler, cells.out}), HGap(14), meterColumn(cells.outMeter, cells.mute),
                    HGap(28)});
    return Fill(mTheme.Tertiary(), bar);
}

// ── The play stage ──────────────────────────────────────────────────────────────

Ptr Shell::BuildPlayStage()
{
    // The chain, as tall as its lines (BuildChain), with the scene chips over it and the add
    // button at its right; the selected effect's editor takes the rest. The compact frame's
    // split stage is this one too, between its rail, preset bar and footer.
    auto add = std::make_shared<RoundAdd>(mTheme, 24.0f);
    add->onClick = [this, a = add.get()] { OpenAddMenu(a->Bounds()); };
    auto chain = Over(
        Col({mSceneSlot, mChainSlot, FixedHeight(AddRowHeight(), Pad(0, 0, 0, 6, Row({Stretch(), add, HGap(14)})))}),
        std::make_shared<StageBackdrop>(mTheme));
    // The bar between them is dragged to give the chain more room or less (kept in Nano Q's
    // own UI settings, as chainAreaHeight). Dragged 12 px past the cards' least height the
    // chain turns to icon tiles, and 12 px down from them back to cards, as the web UI's
    // splitter switches its two modes (signalPath/layout.ts, chainIcons here).
    auto handle = std::make_shared<SplitHandle>(mTheme);
    handle->onDrag = [this](float dy, bool done) {
        constexpr float kModeGesture = 12.0f;

        if (mChainDragStart < 0.0f)
        {
            mChainDragStart = mChainAreaHeight - SceneRowHeight() - AddRowHeight();
            mChainDragIcons = IconChain();
        }

        const float wanted = mChainDragStart + dy;
        const bool icons = mChainDragIcons ? dy < kModeGesture : wanted < kCardHeight + 24.0f - kModeGesture;
        mChainIcons = icons ? 1 : 0;

        if (!icons)
        {
            mChainAreaLimit = std::max(1.0f, wanted);
        }

        mChainSignature.clear();
        mEffectSignature.clear();
        Mark(kChain | kPage);

        if (done)
        {
            mChainDragStart = -1.0f;
            nlohmann::json patch{{"chainIcons", icons}};

            if (!icons)
            {
                patch["chainAreaHeight"] = std::round(mChainAreaLimit);
            }

            mSession.Commands().PatchNativeUiSettings(patch);
        }
    };
    // Where the editor is, for the FX Library to open over.
    return Col({chain, FixedHeight(10, handle),
                std::make_shared<BoundsProbe>(mEffectSlot, [this](el::rect r) { mEditorArea = r; })});
}

// ── The footers' shared pieces ──────────────────────────────────────────────────

void Shell::PlayOrStopDemo()
{
    auto& commands = mSession.Commands();

    if (mState.demo.playing)
    {
        commands.StopDemo();
    }
    else if (const auto* c = SelectedDemoClip())
    {
        if (c->takeId.empty())
        {
            commands.PreviewDemoClip(c->id, BoolSetting(mState.appSettings, kDemoRepeatSetting, false));
        }
        else
        {
            commands.PreviewRiff(c->takeId);
        }
    }
}

Ptr Shell::BuildDemoControls(float chooserWidth, bool withRepeat)
{
    const auto* clip = SelectedDemoClip();
    const bool playing = mState.demo.playing;

    auto chooser = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
    chooser->Size(chooserWidth, 26).Label(clip != nullptr ? clip->title : "Demo", 11.0f).Caret(true);
    chooser->Disabled(clip == nullptr);
    chooser->onClick = [this] { OpenDemoClips(); };
    // A long clip name keeps to the chooser's width.
    auto chooserBox = FixedWidth(chooserWidth, chooser);

    auto play = ToolButton(mTheme, mIcons, playing ? "ui-stop" : "ui-play", [this] { PlayOrStopDemo(); }, 26.0f, 12.0f);
    play->Lit(playing).Disabled(clip == nullptr);
    play->AccessName(playing ? "Stop demo" : "Play demo");

    std::vector<Ptr> parts{chooserBox, HGap(4), play};

    if (withRepeat)
    {
        const bool repeat = BoolSetting(mState.appSettings, kDemoRepeatSetting, false);
        auto loop = ToolButton(
            mTheme, mIcons, "ui-loop", [this, repeat] { mSession.Commands().SetSetting(kDemoRepeatSetting, !repeat); },
            26.0f, 14.0f);
        loop->Lit(repeat).AccessName("Repeat demo");
        parts.push_back(HGap(2));
        parts.push_back(loop);
    }

    return Tight(parts);
}

Ptr Shell::BuildTempoControls(bool withTap)
{
    auto& commands = mSession.Commands();
    char bpm[32];
    std::snprintf(bpm, sizeof(bpm), "%.1f BPM", mState.metronome.bpm);
    auto tempo = std::make_shared<IconButton>(mTheme, mIcons, "");
    tempo->Size(64, 26).Label(bpm, 11.0f).Ink(mTheme.Text());
    tempo->onClick = [this] { OpenMetronome(); };

    std::vector<Ptr> parts{tempo};

    if (withTap)
    {
        auto tap = std::make_shared<IconButton>(mTheme, mIcons, "");
        tap->Size(40, 26).Label("TAP", 10.0f).Ink(mTheme.TextSecondary());
        tap->onClick = [this] { TapTempo(); };
        auto metronome = ToolButton(mTheme, mIcons, "ui-metronome", [this] { OpenMetronome(); }, 26.0f, 15.0f);
        metronome->Ink(mTheme.TextSecondary());
        parts.push_back(HGap(2));
        parts.push_back(tap);
        parts.push_back(HGap(2));
        parts.push_back(metronome);
    }

    const bool ticking = mState.metronome.enabled;
    auto tick = ToolButton(
        mTheme, mIcons, ticking ? "ui-stop" : "ui-play",
        [&commands, ticking] { commands.SetMetronomeEnabled(!ticking); }, 26.0f, 12.0f);
    tick->Lit(ticking);
    tick->AccessName(ticking ? "Stop metronome" : "Start metronome");
    tempo->AccessName(std::string("Tempo ") + bpm);

    if (!withTap)
    {
        // Compact: the metronome's glyph stands in for the TAP and the dialog button.
        auto metronome = ToolButton(mTheme, mIcons, "ui-metronome", [this] { OpenMetronome(); }, 26.0f, 15.0f);
        metronome->Ink(mTheme.TextSecondary());
        parts.push_back(metronome);
    }

    parts.push_back(HGap(2));
    parts.push_back(tick);
    return Tight(parts);
}

Ptr Shell::ZoomButton()
{
    const float zoom = mSession.getZoom ? mSession.getZoom() : 1.0f;
    const int percent = static_cast<int>(std::lround(zoom * 100.0f));
    auto button = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
    button->Size(72, 28).Label(std::to_string(percent) + "%", 11.0f).Caret(true);
    button->onClick = [this, b = button.get(), percent] {
        std::vector<MenuItem> items;

        // The web UI's choices (settings/keys.ts ZOOM_LEVEL_OPTIONS); QPlug allows 50% to 200%.
        for (const int p : {75, 90, 100, 110, 125, 150, 175, 200})
        {
            items.push_back({std::to_string(p) + "%", p == percent, false, [this, p] {
                                 if (mSession.setZoom)
                                 {
                                     mSession.setZoom(static_cast<float>(p) / 100.0f);
                                 }

                                 Mark(kTransport);
                             }});
        }

        OpenDropdown(b->Bounds(), std::move(items), 110.0f);
    };
    return button;
}

/// The footers' Rec (the riff capture dialog, counting in at once: the standalone app with the
/// riff library on, as the web UI's) and Loop (the looper strip) buttons, those that apply.
std::vector<Ptr> Shell::BuildCaptureButtons(float size, bool labelled)
{
    std::vector<Ptr> buttons;

    if (mState.environment.standalone &&
        guitarfx::uiclient::IsFeatureEnabled(mState, guitarfx::uiclient::Feature::RiffLibrary))
    {
        auto rec = ToolButton(mTheme, mIcons, "ui-record", [this] { OpenRiffCapture(true); }, size, 14.0f);
        rec->Ink(mTheme.Error()).AccessName("Record riff");

        if (labelled)
        {
            rec->Label("Rec", 11.0f);
        }

        buttons.push_back(rec);
    }

    if (LooperShown())
    {
        auto loop = ToolButton(mTheme, mIcons, "ui-loop", [this] { ToggleLooper(); }, size, 16.0f);
        loop->Lit(mLooperOpen).AccessName("Looper");

        if (labelled)
        {
            loop->Label("Loop", 11.0f);
        }

        if (!mLooperOpen)
        {
            loop->Ink(mTheme.TextSecondary());
        }

        buttons.push_back(loop);
    }

    return buttons;
}

// ── The full footer ─────────────────────────────────────────────────────────────

Ptr Shell::BuildWideFooter()
{
    const float width = ViewSize().x;
    auto divider = [this] {
        return Pad(10, 8, 10, 8, FixedWidth(1, Fill(mTheme.Text().opacity(0.08f), std::make_shared<el::element>())));
    };

    auto pads = ToolButton(mTheme, mIcons, "ui-pads", [this] { TogglePads(); }, 28.0f, 16.0f);
    pads->Lit(mPage == Page::Pads);

    if (mPage != Page::Pads)
    {
        pads->Ink(mTheme.TextSecondary());
    }

    auto tuner = ToolButton(mTheme, mIcons, "ui-tuner", [this] { OpenTuner(); }, 28.0f, 16.0f);
    tuner->Ink(mTheme.TextSecondary());
    auto midi =
        ToolButton(mTheme, mIcons, "ui-midi", [this] { OpenMidiAutomation(); }, 28.0f, 16.0f); // MIDI & Automation
    midi->Ink(mTheme.TextSecondary());
    bool toolLabels = false;
    const bool muted = mState.outputMuted;
    auto mute = ToolButton(
        mTheme, mIcons, muted ? "ui-mute" : "ui-speaker", [this, muted] { mSession.Commands().SetOutputMuted(!muted); },
        28.0f, 16.0f);
    mute->Lit(muted);

    if (!muted)
    {
        mute->Ink(mTheme.TextSecondary());
    }

    // The tools spelt out beside their icons once there is room, as the web UI's do when its
    // tools group is 330 px wide (footer.css): the footer's four groups share what the history
    // controls leave, so from a window about 1,540 px wide.
    if ((width - 220.0f) / 4.0f >= 330.0f)
    {
        pads->Label("Setlists", 11.0f);
        tuner->Label("Tuner", 11.0f);
        midi->Label("MIDI", 11.0f);
        mute->Label(muted ? "Muted" : "Mute", 11.0f);
        toolLabels = true;
    }

    // Render the clip through the preset: the web UI's camera (#footer-demo-render-btn).
    auto render = ToolButton(mTheme, mIcons, "ui-more", [this] { OpenDemoClips(); }, 26.0f, 14.0f);
    render->Ink(mTheme.TextSecondary());

    auto themeButton = BuildThemeButton();

    std::vector<Ptr> tools;

    for (auto& b : BuildCaptureButtons(28.0f, toolLabels))
    {
        tools.insert(tools.end(), {b, HGap(6)});
    }

    tools.insert(tools.end(), {pads, HGap(6), tuner, HGap(6), midi, HGap(6), mute});
    std::vector<Ptr> row{HGap(12),  BuildHistoryControls(),
                         divider(), BuildDemoControls(width >= 1180.0f ? 150.0f : 120.0f, true),
                         HGap(2),   render,
                         Stretch(), Tight(tools),
                         Stretch(), BuildTempoControls(true),
                         divider()};

    if (mSession.getZoom && mSession.setZoom)
    {
        row.push_back(ZoomButton());
        row.push_back(HGap(8));
    }

    row.push_back(themeButton);
    row.push_back(HGap(12));
    // Touch sizes: the buttons are hit over the footer's whole height.
    return Fill(mTheme.Footer(), PadXY(0, metrics::Touch() ? 0 : 8, Row(row)));
}
} // namespace nanoq::ui
