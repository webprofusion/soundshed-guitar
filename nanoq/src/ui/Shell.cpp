#include "ui/Shell.h"

#include "WindowChrome.h"
#include "ui/DrawProbe.h"
#include "ui/ShellIcons.h"
#include "uiclient/NodeLabels.h"

#include <algorithm>
#include <cstdio>

namespace nanoq::ui
{
using guitarfx::uiclient::Topic;

Shell::Shell(UiSession& session)
    : mSession(session), mState(session.Client().MutableState()), mTheme(session.GetTheme()),
      mIcons(session.Link().AssetRoot() / "ui" / "images" / "icons"), mImages(session.Link().AssetRoot() / "ui"),
      mRemoteImages([] {
          std::error_code ec;
          const auto temp = std::filesystem::temp_directory_path(ec);
          return (ec ? std::filesystem::path(".") : temp) / "soundshed-nanoq-images";
      }()),
      mLayoutImages(std::make_unique<LayoutImages>(
          [&link = session.Link()](const std::string& imageId) { return link.ResolveLayoutImageFile(imageId); }))
{
    // The effect icons, colours and artwork: one table for every UI (core/ui/data/effect-presentation.json).
    mPresentation = guitarfx::uiclient::EffectPresentation::Load(session.Link().AssetRoot() / "ui" / "data" /
                                                                 "effect-presentation.json");
    AddShellIcons(mIcons);
    ApplyElementsTheme();
    BuildRoot();
    Subscribe();

    // The tone service's news (a picture arrived, a download moved on): the Tones page's cards
    // read it as they draw, so a redraw shows it. Its messages show as toasts.
    mToneListener = mSession.Tones().AddListener([this] {
        if (mPage == Page::Tones)
        {
            Mark(kPage); // rebuilt only if a card's state changed (TonesPageSignature)
            mSession.View().refresh();
        }
    });
    mSession.onToast = [this](const std::string& title, const std::string& detail, bool isError) {
        ShowToast(title, detail, isError);
    };

    // The Practice Tool's session lives as long as the window: its track plays on while other
    // pages are shown (JamPage.cpp).
    mPractice = std::make_unique<guitarfx::uiclient::PracticeTool>(mSession.Client(), mSession.Commands());
    mPractice->onChanged = [this] {
        if (mPage == Page::Jam)
        {
            Mark(kPage); // rebuilt only if what it lists changed (JamPageSignature)
        }
    };
    mPractice->onNotice = [this](const std::string& title, const std::string& detail) {
        ShowToast(title, detail, false);
    };
}

Shell::~Shell()
{
    *mAlive = false;

    if (mKeyboard)
    {
        mKeyboard->Detach();
    }

    mAccess.reset(); // before the view's window goes: it unhooks from it
    mSession.Tones().RemoveListener(mToneListener);
    mSession.onToast = nullptr;

    // A dialog still open goes with the window, and so must what it asked of the engine: the
    // tuner left on with the window closed kept the output muted, with nothing left to turn it
    // off. The session, and so the engine, is still there (NanoQPresenter::on_detach).
    mInstrumentSubs.clear();

    if (auto closed = std::move(mOnOverlayClosed))
    {
        closed();
    }

    CloseFilePicker({}); // the engine is told it was cancelled
    mSubscriptions.clear();
}

void Shell::Subscribe()
{
    auto& client = mSession.Client();
    auto on = [this, &client](Topic topic, unsigned bits) {
        mSubscriptions.push_back(client.Subscribe(topic, [this, bits] { Mark(bits); }));
    };

    // A settings change is patched in place; a new theme is noticed in Rebuild and redraws everything.
    // Settings include the feature switches, which show or hide the Tones tab.
    on(Topic::Session, kTransport | kValues | kRail);
    on(Topic::ActivePreset, kTopBar | kScenes | kChain | kPage | kValues);
    on(Topic::PresetLibrary, kTopBar | kPage);
    // Choosing a setlist moves only the cursor (nothing loads), so no other topic redraws the
    // pads for it.
    on(Topic::Setlists, kPage);
    on(Topic::Catalog, kChain | kPage);
    on(Topic::Resources, kChain | kPage);
    on(Topic::Tones, kPage);
    on(Topic::GlobalChain, kTransport);
    on(Topic::Mixer, kTransport | kScenes | kChain);
    on(Topic::Demo, kTransport);
    // The layout library and its pictures, and the setting that picks a layout per effect: the
    // effect page is rebuilt only when what it shows changes (EffectPageSignature).
    mSubscriptions.push_back(client.Subscribe(Topic::EffectLayouts, [this] {
        mLayoutImages->Clear();

        if (PlayShown())
        {
            Mark(kPage);
        }
    }));
    mSubscriptions.push_back(client.Subscribe(Topic::Session, [this] {
        if (PlayShown())
        {
            Mark(kPage);
        }
    }));
    // The looper strip over the footer, and the footer's Loop button (LooperStrip.cpp).
    mSubscriptions.push_back(client.Subscribe(Topic::Looper, [this] { SyncLooper(); }));
    mSubscriptions.push_back(client.Subscribe(Topic::Session, [this] { SyncLooper(); }));
    // The live sections' feeds (the spatial panner, the analyzer).
    mSubscriptions.push_back(client.Subscribe(Topic::Analysis, [this] {
        if (!mSpatialPanner.expired() || !mAnalyzerPanel.expired())
        {
            SyncLiveSection();
        }
    }));

    // MIDI and key learns, the slots and the MIDI Log (Automation.cpp).
    mSubscriptions.push_back(client.Subscribe(Topic::Automation, [this] { SyncAutomation(); }));

    // Every change the engine reports to the preset may be an edit for the chain's history.
    mSubscriptions.push_back(client.Subscribe(Topic::ActivePreset, [this] {
        NoteChainEdit();
        NotePresetTrail();
    }));
}

void Shell::BuildRoot()
{
    mRailSlot = MakeSlot();
    mTopSlot = MakeSlot();
    mSceneSlot = MakeSlot();
    mChainSlot = MakeSlot();
    mPageSlot = MakeSlot();
    mTransportSlot = MakeSlot();
    mLooperSlot = MakeSlot();
    Fill(*mLooperSlot, FixedHeight(0, std::make_shared<el::element>()));
    mEffectSlot = MakeSlot();

    // The frame (BuildFrame) holds the regions as the layout in force places them; it is
    // swapped when the window crosses the compact breakpoint. The background is its own slot
    // because a theme change repaints it.
    mBgSlot = MakeSlot();
    Fill(*mBgSlot, el::share(el::box(mTheme.Background())));
    mFrameSlot = MakeSlot();
    mWide = WantsWide();
    Fill(*mFrameSlot, BuildFrame());
    Ptr frame = mFrameSlot;

    // How much drawing the UI does, for performance work (DrawProbe.h); off unless asked for.
    if (const auto stats = DrawProbe::PathFromEnvironment(); !stats.empty())
    {
        frame = std::make_shared<DrawProbe>(frame, stats);
    }

    if (OnScreenKeyboard::Wanted())
    {
        mKeyboard = std::make_shared<OnScreenKeyboard>(
            mTheme, [this](std::function<void()> fn) { mSession.PostToUi(std::move(fn)); });
    }

    mKeys = std::make_shared<KeyRouter>(Over(MinSize(metrics::kMinWidth, metrics::kMinHeight, frame), mBgSlot));
    mKeys->keyboard = mKeyboard;
    mKeys->onKey = [this](const el::key_info& k) { return HandleKey(k); };
    mKeys->ringColour = [this] { return mTheme.Accent(); };
    mKeys->onPointerDown = [this] { mChainFocused = false; };
    mKeys->onTrackDrop = [this](const el::drop_info& info, bool leaving) { TrackFileDrop(info, leaving); };
    mKeys->onDrop = [this](const el::drop_info& info) { return DropFiles(info); };
    mRoot = mKeys;
}

void Shell::SyncWindowChrome()
{
    const auto rgb = [](el::color c) {
        const auto channel = [](float v) {
            return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        return (channel(c.red) << 16) | (channel(c.green) << 8) | channel(c.blue);
    };
    // The compact preset bar or the full layout's tab row: what sits under the caption.
    const auto caption = rgb(mWide ? mTheme.Background() : mTheme.Tertiary());

    if (caption == mChromeCaption)
    {
        return;
    }

    mChromeCaption = caption;
    nanoq::ThemeStandaloneWindow(static_cast<void*>(mSession.View().host()), caption, rgb(mTheme.Text()), caption);
}

void Shell::Rebuild()
{
    if (mAppliedTheme != mState.theme)
    {
        mAppliedTheme = mState.theme;
        mTheme.Set(mState.theme);
        ApplyElementsTheme();
        Fill(*mBgSlot, el::share(el::box(mTheme.Background())));
        mDirty |= kAll;
        mChainSignature.clear();
        mEffectSignature.clear();
        mSceneSignature.clear();
        mTopSignature.clear();
    }

    if (!mState.haveState)
    {
        if (!mShowingStarting)
        {
            Fill(*mRailSlot, Fill(mTheme.Bar(), std::make_shared<el::element>()));
            Fill(*mTopSlot, std::make_shared<el::element>());
            Fill(*mSceneSlot, std::make_shared<el::element>());
            Fill(*mChainSlot, std::make_shared<el::element>());
            Fill(*mTransportSlot, std::make_shared<el::element>());
            Fill(*mPageSlot, BuildStarting());
            mShowingStarting = true;
            mSession.View().layout();
            mSession.View().refresh();
        }

        mDirty = 0;
        return;
    }

    // The first state has arrived: every region is built.
    if (mShowingStarting)
    {
        mShowingStarting = false;
        mDirty |= kAll;
    }

    // The full layout's play page holds the chain and the editor in one stage: it is put back
    // when the page comes back to it, and its regions built afresh with it.
    const bool playStage = PlayStageShown();

    if (!playStage)
    {
        mPlayStageShown = false;
    }
    else if (!mPlayStageShown)
    {
        mDirty |= kPage | kChain | kScenes;
        mEffectSignature.clear();
        mChainSignature.clear();
        mSceneSignature.clear();
    }

    EnsureSelection();
    SelectAddedNode();

    // Nothing is rebuilt under a drag on a bound widget (a knob, a fader, the waveform): a rebuild
    // would drop the drag, so it waits for the drop.
    const unsigned held = mLive.Dragging() ? (mDirty & kRebuilds) : 0u;
    mDirty &= ~held;

    // The chain strip is rebuilt only when what it shows changed: a parameter edit marks the
    // chain dirty too, and a rebuild under the cursor would cancel a click in progress.
    if (mDirty & kChain)
    {
        const auto signature = ChainSignature();

        if (signature == mChainSignature)
        {
            mDirty &= ~kChain;
        }
        else
        {
            mChainSignature = signature;
        }
    }

    // A page of sliders is patched in place when only values moved: rebuilding it under a drag
    // would drop the drag.
    if ((mDirty & kPage) && PlayShown() && mState.activePreset)
    {
        const auto signature = EffectPageSignature();

        if (signature == mEffectSignature)
        {
            mDirty &= ~kPage;
        }
        else
        {
            mEffectSignature = signature;
        }
    }
    else if ((mDirty & kPage) && mPage == Page::Tones)
    {
        // The Tones page scrolls; it is rebuilt (back to the top) only when what it lists changed.
        // The Jam page is forgotten, so coming back to it builds it again.
        mEffectSignature.clear();
        mJamSignature.clear();
        const auto signature = TonesPageSignature();

        if (signature == mTonesSignature)
        {
            mDirty &= ~kPage;
        }
        else
        {
            mTonesSignature = signature;
        }
    }
    else if ((mDirty & kPage) && mPage == Page::Jam)
    {
        // As the Tones page: the playhead and the faders move in place (SyncJam).
        mEffectSignature.clear();
        mTonesSignature.clear();
        const auto signature = JamPageSignature();

        if (signature == mJamSignature)
        {
            mDirty &= ~kPage;
        }
        else
        {
            mJamSignature = signature;
        }
    }
    else if (mDirty & kPage)
    {
        mEffectSignature.clear();
        mTonesSignature.clear();
        mJamSignature.clear();
    }

    // The bar is rebuilt only when what it shows changed: every edit to the preset marks it, and
    // the full layout's bar holds knobs that a rebuild under a drag would drop.
    if (mDirty & kTopBar)
    {
        const auto signature = TopBarSignature();

        if (signature == mTopSignature)
        {
            mDirty &= ~kTopBar;
        }
        else
        {
            mTopSignature = signature;
        }
    }

    if (mDirty & kScenes)
    {
        const auto signature = SceneSignature();

        if (signature == mSceneSignature)
        {
            mDirty &= ~kScenes;
        }
        else
        {
            mSceneSignature = signature;
        }
    }

    if (mDirty & kRail)
    {
        Fill(*mRailSlot, BuildRail());
    }

    if (mDirty & kTopBar)
    {
        Fill(*mTopSlot, BuildTopBar());
    }

    if (mDirty & kScenes)
    {
        Fill(*mSceneSlot, BuildScenes());
    }

    if (mDirty & kChain)
    {
        Fill(*mChainSlot, BuildChain());
    }

    if ((mDirty & kPage) && playStage)
    {
        if (!mPlayStageShown)
        {
            Fill(*mPageSlot, BuildPlayStage());
            mPlayStageShown = true;
        }

        Fill(*mEffectSlot, mPage == Page::Pads ? BuildPadsView() : BuildEffectPage());
    }
    else if (mDirty & kPage)
    {
        switch (mPage)
        {
        case Page::Chain:
            Fill(*mPageSlot, BuildChainStage());
            break;
        case Page::Effect:
            Fill(*mPageSlot, BuildEffectPage());
            break;
        case Page::Tones:
            Fill(*mPageSlot, BuildTonesPage());
            break;
        case Page::Jam:
            Fill(*mPageSlot, BuildJamPage());
            break;
        case Page::Pads:
            Fill(*mPageSlot, BuildPadsView());
            break;
        case Page::Settings:
            Fill(*mPageSlot, BuildSettingsPage());
            break;
        }
    }

    if (mDirty & kTransport)
    {
        Fill(*mTransportSlot, BuildTransport());
    }

    if ((mDirty & kValues) && PlayShown())
    {
        SyncEffectValues();
    }

    const unsigned rebuilt = mDirty & kRebuilds;
    mDirty = held;

    // The footer, the bar and the rail keep their size whatever they hold, so when only they
    // were rebuilt only they are redrawn. A parameter edit commits to the chain's history, which
    // rebuilds the footer for its Undo, and redrawing the whole window for it made a knob drag
    // cost whole-window paints (dear on iOS, where Quartz draws on the CPU).
    if (rebuilt != 0 && (rebuilt & ~(kTransport | kTopBar | kRail)) == 0)
    {
        auto& view = mSession.View();
        view.layout(rebuilt & kTransport ? static_cast<el::element&>(*mTransportSlot)
                    : rebuilt & kTopBar  ? static_cast<el::element&>(*mTopSlot)
                                         : static_cast<el::element&>(*mRailSlot));

        if ((rebuilt & kTopBar) && (rebuilt & kTransport))
        {
            mSession.Repaint(*mTopSlot);
        }

        if ((rebuilt & kRail) && (rebuilt & (kTransport | kTopBar)))
        {
            mSession.Repaint(*mRailSlot);
        }
    }
    else if (rebuilt != 0)
    {
        mSession.View().layout();
        mSession.View().refresh();
    }
}

void Shell::OnTick()
{
    KeyRouter::Retake(mSession.View());
    TickKeyboard();
    TickLooper();
    TickFilePicker();
    {
        const auto wheel = mState.appSettings.find("ui.mouseWheelAdjustsControls");
        ParamKnob::sWheelAdjusts = wheel == mState.appSettings.end() || !wheel->is_boolean() || wheel->get<bool>();
    }

    // A window resized (or zoomed) across the breakpoint gets the other frame, every region
    // built for it.
    if (const bool wide = WantsWide(); wide != mWide)
    {
        mWide = wide;
        mPlayStageShown = false;
        CloseOverlay();
        CloseDropdown();
        Fill(*mFrameSlot, BuildFrame());
        mDirty |= kAll;
        mChainSignature.clear();
        mEffectSignature.clear();
        mSceneSignature.clear();
        mTopSignature.clear();
        mTonesSignature.clear();
        mJamSignature.clear();
        mShowingStarting = false;
    }

    // A new size: the regions laid out for the old one are looked at again (the chain and the
    // pages only rebuild where their signatures see the change).
    if (const auto size = ViewSize(); size.x != mLastSize.x || size.y != mLastSize.y)
    {
        mLastSize = size;
        mDirty |= kTransport | kChain | kPage;
    }

    if (mHistoryCommitAt > 0.0 && UiSession::Now() >= mHistoryCommitAt)
    {
        CommitChainHistory();
    }

    if (mDirty != 0)
    {
        Rebuild();
    }

    SyncWindowChrome();

    // Errors and confirmations the engine sent (a save that failed, a preset saved).
    for (auto& n : mSession.Client().TakeNotifications())
    {
        ShowToast(n.title, n.detail, n.kind == guitarfx::uiclient::Notification::Kind::Error);
    }

    // The cables light with the signal while Animate signal flow is on (web: signalFlow.ts).
    if (guitarfx::uiclient::IsSignalFlowAnimationEnabled(mState))
    {
        mSignalFlow.Update(mState, UiSession::Now());
    }

    SyncEqPlotFeeds();
    SyncIrExport();
    SyncDspStatus();
    SyncUpdateCheck();
    SyncDevicePage();
    SyncTonePreview();
    SyncJam();
    mLive.Sync();

    // Keyboard focus that a rebuild took from its control goes to the same one in the new
    // region; switches slide; fields closed without a context are redrawn; and a screen reader
    // is told what changed (Focus.h, SwitchMotion.h, AccessBridge.h).
    auto& view = mSession.View();

    if (auto* router = KeyRouter::Focused(view))
    {
        router->KeepFocus(view);
    }

    FlushRepaints(view);
    SwitchMotion::Tick(view);

    if (!mAccessTried && view.host() != nullptr)
    {
        mAccessTried = true;
        mAccess = AccessBridge::Create(view);
    }

    if (mAccess)
    {
        mAccess->Tick();
    }
}

// ── Selection and pages ─────────────────────────────────────────────────────────

const guitarfx::GraphNode* Shell::SelectedNode() const
{
    if (!mState.activePreset || mSelectedNode.empty())
    {
        return nullptr;
    }

    return mState.activePreset->graph.FindNode(mSelectedNode);
}

void Shell::EnsureSelection()
{
    if (!mState.activePreset)
    {
        mSelectedNode.clear();
        return;
    }

    if (SelectedNode() != nullptr)
    {
        return;
    }

    // The first thing worth editing: an effect, not the input or output.
    const auto layout = guitarfx::uiclient::BuildChainLayout(mState.activePreset->graph);

    for (const auto& id : layout.DrawnNodeIds())
    {
        if (const auto* node = mState.activePreset->graph.FindNode(id);
            node && !guitarfx::uiclient::IsBoundaryNode(*node))
        {
            mSelectedNode = id;
            return;
        }
    }

    mSelectedNode.clear();
}

void Shell::SelectAddedNode()
{
    // After adding an effect, the node that appears next is the one to show.
    if (mSelectAddedUntil <= 0.0)
    {
        return;
    }

    if (UiSession::Now() > mSelectAddedUntil || !mState.activePreset)
    {
        mSelectAddedUntil = 0.0;
        return;
    }

    for (const auto& node : mState.activePreset->graph.nodes)
    {
        if (mNodesBeforeAdd.count(node.id) == 0 && !guitarfx::uiclient::IsBoundaryNode(node))
        {
            mSelectAddedUntil = 0.0;
            mSelectedNode = node.id;
            LeavePads();
            mPage = Page::Effect;
            mDirty |= kChain | kPage | kTopBar | kRail;
            mChainSignature.clear();
            return;
        }
    }
}

void Shell::SelectNode(const std::string& nodeId)
{
    mSelectedNode = nodeId;
    LeavePads();
    mPage = Page::Effect;
    Mark(kChain | kPage | kTopBar | kRail);
    mChainSignature.clear();
}

void Shell::LeavePads()
{
    if (mPage == Page::Pads)
    {
        mEffectSignature.clear();
        mPadsEditingId.clear();
        mPadsNameBox.reset();
        mDirty |= kTransport;
    }
}

void Shell::ShowPage(Page page)
{
    if (page == mPage)
    {
        return;
    }

    // The pads and the editor share the play stage's lower half: going between them refills it
    // even when the editor's signature has not moved.
    if (page == Page::Pads)
    {
        mEffectSignature.clear();
    }

    LeavePads();

    mPage = page;
    Mark(kTopBar | kPage | kRail | kTransport);
}

// ── Starting ────────────────────────────────────────────────────────────────────

Ptr Shell::BuildStarting()
{
    auto text = std::make_shared<Text>(mTheme, "Starting the engine...", 16.0f, Weight::Medium);
    text->Colour(mTheme.TextMuted()).Alignment(Text::Align::Centre);
    return Fill(mTheme.Background(), el::share(el::align_center(el::align_middle(el::hold_any(text)))));
}

std::string Shell::SceneSignature() const
{
    if (!mState.activePreset)
    {
        return {};
    }

    std::string s = mState.activeSceneId + "|" + MixerSignature();

    for (const auto& scene : mState.activePreset->scenes)
    {
        s += "|" + scene.id + ":" + scene.title;
    }

    return s;
}
} // namespace nanoq::ui
