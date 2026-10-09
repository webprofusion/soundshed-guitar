#include "ui/Shell.h"

#include "uiclient/NodeLabels.h"

#include <algorithm>
#include <cstdio>

namespace nanoq::ui
{
using guitarfx::uiclient::Topic;

namespace
{
constexpr float kBarHeight = 56.0f;

std::string PresetTitle(const guitarfx::uiclient::ClientState& state)
{
    if (const auto* summary = state.FindPresetSummary(state.activePresetId))
        return summary->name;
    if (state.activePreset && !state.activePreset->name.empty())
        return state.activePreset->name;
    return "No preset";
}
} // namespace


Shell::Shell(UiSession& session)
    : mSession(session), mState(session.Client().MutableState()), mTheme(session.GetTheme())
{
    // The effect icons, colours and artwork: one table for every UI (core/ui/data/effect-presentation.json).
    mPresentation = guitarfx::uiclient::EffectPresentation::Load(session.GetEngine().GetBundledAssetsPath() / "ui" / "data" /
                                                                  "effect-presentation.json");
    ApplyElementsTheme();
    BuildRoot();
    Subscribe();
}

Shell::~Shell()
{
    *mAlive = false;
    mSubscriptions.clear();
}

void Shell::Subscribe()
{
    auto& client = mSession.Client();
    auto on = [this, &client](Topic topic, unsigned bits) {
        mSubscriptions.push_back(client.Subscribe(topic, [this, bits] { Mark(bits); }));
    };

    // A settings change is patched in place; a new theme is noticed in Rebuild and redraws everything.
    on(Topic::Session, kTransport | kValues | kSettings);
    on(Topic::ActivePreset, kTopBar | kScenes | kChain | kPage | kValues);
    on(Topic::PresetLibrary, kTopBar | kPage);
    on(Topic::Catalog, kChain | kPage);
    on(Topic::Resources, kChain | kPage);
    on(Topic::GlobalChain, kTransport);
    on(Topic::Mixer, kTransport);
}

void Shell::BuildRoot()
{
    mTopSlot = MakeSlot();
    mSceneSlot = MakeSlot();
    mChainSlot = MakeSlot();
    mPageSlot = MakeSlot();
    mTransportSlot = MakeSlot();

    // The page stretches between the bars at either end; the background is its own slot because
    // a theme change repaints it.
    mBgSlot = MakeSlot();
    Fill(*mBgSlot, el::share(el::box(mTheme.Background())));
    mRoot = Over(Col({mTopSlot, mSceneSlot, mChainSlot, mPageSlot, mTransportSlot}), mBgSlot);
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
    }
    if (!mState.haveState)
    {
        if (!mShowingStarting)
        {
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


    EnsureSelection();
    SelectAddedNode();

    // The chain strip is rebuilt only when what it shows changed: a parameter edit marks the
    // chain dirty too, and a rebuild under the cursor would cancel a click in progress.
    if (mDirty & kChain)
    {
        const auto signature = ChainSignature();
        if (signature == mChainSignature)
            mDirty &= ~kChain;
        else
            mChainSignature = signature;
    }

    // A page of sliders is patched in place when only values moved: rebuilding it under a drag
    // would drop the drag.
    if ((mDirty & kPage) && mPage == Page::Effect && mState.activePreset)
    {
        const auto signature = EffectPageSignature();
        if (signature == mEffectSignature)
            mDirty &= ~kPage;
        else
            mEffectSignature = signature;
    }
    else if (mDirty & kPage)
    {
        mEffectSignature.clear();
    }

    if (mDirty & kScenes)
    {
        const auto signature = SceneSignature();
        if (signature == mSceneSignature)
            mDirty &= ~kScenes;
        else
            mSceneSignature = signature;
    }

    if (mDirty & kTopBar)
        Fill(*mTopSlot, BuildTopBar());
    if (mDirty & kScenes)
        Fill(*mSceneSlot, BuildScenes());
    if (mDirty & kChain)
        Fill(*mChainSlot, BuildChain());
    if (mDirty & kPage)
    {
        switch (mPage)
        {
        case Page::Effect:
            Fill(*mPageSlot, BuildEffectPage());
            break;
        case Page::Presets:
            Fill(*mPageSlot, BuildPresetsPage());
            break;
        case Page::Settings:
            Fill(*mPageSlot, BuildSettingsPage());
            break;
        }
    }
    if (mDirty & kTransport)
        Fill(*mTransportSlot, BuildTransport());
    if ((mDirty & kValues) && mPage == Page::Effect)
        SyncEffectValues();
    if ((mDirty & kSettings) && mPage == Page::Settings)
        SyncSettings();

    const bool structural = (mDirty & (kTopBar | kScenes | kChain | kPage | kTransport)) != 0;
    mDirty = 0;

    if (structural)
    {
        mSession.View().layout();
        mSession.View().refresh();
    }
}

void Shell::OnTick()
{
    if (mDirty != 0)
        Rebuild();

    // Errors and confirmations the engine sent (a save that failed, a preset saved).
    for (auto& n : mSession.Client().TakeNotifications())
        ShowToast(n.title, n.detail, n.kind == guitarfx::uiclient::Notification::Kind::Error);

    // The meters: the telemetry feed arrives at 20 Hz; only a bar that moved is redrawn.
    if (mInMeter && mOutMeter && mState.haveState)
    {
        const auto& t = mState.telemetry;
        if (mInMeter->Set(t.input.peakDb, t.input.clipped))
            mSession.View().refresh(*mInMeter);
        if (mOutMeter->Set(t.output.peakDb, t.output.clipped))
            mSession.View().refresh(*mOutMeter);

        char text[32];
        std::snprintf(text, sizeof(text), "DSP %d%%", static_cast<int>(t.dspLoadPercent + 0.5));
        if (mLoadText && mLoadText->Value() != text)
        {
            mLoadText->Set(text);
            mSession.View().refresh(*mLoadText);
        }
    }
}

// ── Selection and pages ─────────────────────────────────────────────────────────

const guitarfx::GraphNode* Shell::SelectedNode() const
{
    if (!mState.activePreset || mSelectedNode.empty())
        return nullptr;
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
        return;

    // The first thing worth editing: an effect, not the input or output.
    const auto layout = guitarfx::uiclient::BuildChainLayout(mState.activePreset->graph);
    for (const auto& id : layout.DrawnNodeIds())
    {
        if (const auto* node = mState.activePreset->graph.FindNode(id); node && !guitarfx::uiclient::IsBoundaryNode(*node))
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
        return;
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
            mPage = Page::Effect;
            mDirty |= kChain | kPage | kTopBar;
            mChainSignature.clear();
            return;
        }
    }
}

void Shell::SelectNode(const std::string& nodeId)
{
    mSelectedNode = nodeId;
    mPage = Page::Effect;
    Mark(kChain | kPage | kTopBar);
    mChainSignature.clear();
}

void Shell::ShowPage(Page page)
{
    if (page == mPage)
        return;
    mPage = page;
    Mark(kTopBar | kPage);
}

// ── The top bar ─────────────────────────────────────────────────────────────────

Ptr Shell::BuildStarting()
{
    auto text = std::make_shared<Text>(mTheme, "Starting the engine...", 16.0f, Weight::Medium);
    text->Colour(mTheme.TextMuted()).Alignment(Text::Align::Centre);
    return Fill(mTheme.Background(), el::share(el::align_center(el::align_middle(el::hold_any(text)))));
}

Ptr Shell::BuildTopBar()
{
    auto& commands = mSession.Commands();
    const bool dirty = mState.activePresetDirty;

    auto prev = std::make_shared<Button>(mTheme, "\xE2\x80\xB9", [&commands] { commands.StepPreset(-1); }, Button::Style::Flat);
    prev->MinWidth(40).TextSize(20);
    auto next = std::make_shared<Button>(mTheme, "\xE2\x80\xBA", [&commands] { commands.StepPreset(1); }, Button::Style::Flat);
    next->MinWidth(40).TextSize(20);

    auto name = std::make_shared<Button>(mTheme, PresetTitle(mState), [this] { ShowPage(Page::Presets); },
                                         Button::Style::Flat);
    name->MinWidth(260).Selected(mPage == Page::Presets);

    // Save: a user preset is overwritten, a factory one is saved as a copy (SaveActivePreset).
    auto save = std::make_shared<Button>(mTheme, dirty ? "Save" : "Saved", [this] { SaveActivePreset(false); },
                                         dirty ? Button::Style::Primary : Button::Style::Plain);
    save->Enabled(dirty).MinWidth(72);

    auto more = std::make_shared<Button>(mTheme, "⋯", [this] { OpenPresetMenu(); }, Button::Style::Flat);
    more->MinWidth(44).TextSize(20);

    auto tab = [this](const char* label, Page page) {
        auto b = std::make_shared<Button>(mTheme, label, [this, page] { ShowPage(page); }, Button::Style::Flat);
        b->Selected(mPage == page).MinWidth(84);
        return b;
    };

    auto bar = Row({HGap(8), prev, name, next, HGap(8), save, more, Stretch(), tab("Effects", Page::Effect),
                    tab("Presets", Page::Presets), tab("Settings", Page::Settings), HGap(8)});

    return FixedHeight(kBarHeight, Fill(mTheme.Bar(), PadXY(0, 8, bar)));
}

std::string Shell::SceneSignature() const
{
    if (!mState.activePreset)
        return {};

    std::string s = mState.activeSceneId;
    for (const auto& scene : mState.activePreset->scenes)
        s += "|" + scene.id + ":" + scene.title;
    return s;
}

Ptr Shell::BuildScenes()
{
    const auto& preset = mState.activePreset;
    if (!preset || preset->scenes.size() < 2)
        return VGap(0);

    std::vector<Ptr> items{HGap(12)};
    int index = 1;
    for (const auto& scene : preset->scenes)
    {
        const auto id = scene.id;
        auto label = scene.title.empty() ? "Scene " + std::to_string(index) : scene.title;
        auto b = std::make_shared<Button>(mTheme, label, [this, id] { mSession.Commands().SelectScene(id); },
                                          Button::Style::Plain);
        b->Selected(id == mState.activeSceneId).MinWidth(96);
        items.push_back(b);
        items.push_back(HGap(6));
        ++index;
    }
    items.push_back(Stretch());

    return FixedHeight(48, Fill(mTheme.Panel(), PadXY(0, 4, Row(items))));
}

// ── The transport bar ───────────────────────────────────────────────────────────

Ptr Shell::BuildTransport()
{
    auto& commands = mSession.Commands();
    const bool muted = mState.outputMuted;

    mInMeter = std::make_shared<MeterBar>(mTheme);
    mOutMeter = std::make_shared<MeterBar>(mTheme);
    mLoadText = std::make_shared<Text>(mTheme, "DSP 0%", metrics::kSmallText, Weight::Medium);
    mLoadText->Colour(mTheme.TextSecondary());

    auto label = [this](const char* text) {
        auto t = std::make_shared<Text>(mTheme, text, metrics::kSmallText, Weight::Medium);
        t->Colour(mTheme.TextMuted()).Fixed(true);
        return t;
    };

    auto mute = std::make_shared<Button>(mTheme, muted ? "Muted" : "Mute", [&commands, muted] { commands.SetOutputMuted(!muted); },
                                         muted ? Button::Style::Primary : Button::Style::Plain);
    mute->MinWidth(80).TextSize(metrics::kSmallText);

    auto io = std::make_shared<Button>(mTheme, "Input / Output", [this] { OpenControls(); }, Button::Style::Plain);
    io->MinWidth(120).TextSize(metrics::kSmallText);
    auto tuner = std::make_shared<Button>(mTheme, "Tuner", [this] { OpenTuner(); }, Button::Style::Plain);
    tuner->MinWidth(72).TextSize(metrics::kSmallText);
    auto metronome = std::make_shared<Button>(mTheme, "Metronome", [this] { OpenMetronome(); }, Button::Style::Plain);
    metronome->MinWidth(96).TextSize(metrics::kSmallText);

    auto bar = Row({HGap(12), label("IN"), HGap(8), FixedWidth(180, PadXY(0, 16, mInMeter)), HGap(20),
                    label("OUT"), HGap(8), FixedWidth(180, PadXY(0, 16, mOutMeter)), HGap(20), FixedWidth(80, mLoadText),
                    Stretch(), io, HGap(8), tuner, HGap(8), metronome, HGap(8), mute, HGap(12)});

    return FixedHeight(48, Fill(mTheme.Bar(), PadXY(0, 4, bar)));
}
} // namespace nanoq::ui
