// The compact frame around the stage, as the web UI draws it at its compact density
// (ui-components/header-icon-bar.html, preset-selector-row.html, preset-toolbar-row.html,
// footer-bar.html; css/compact/rail.css): the rail, the preset bar, the scene chips over the chain,
// and the footer and its Options sheet.

#include "ui/shell/Shell.h"

#include "ui/shell/FrameParts.h"
#include "ui/widgets/ControlSkin.h"

#include "IPluginHost.h"
#include "uiclient/FeatureFlags.h"
#include "uiclient/NodeLabels.h"
#include "uiclient/PresetBrowse.h"

#include <algorithm>
#include <cstdio>
#include <random>

namespace nanoq::ui
{
using namespace frame;

// ── The rail ────────────────────────────────────────────────────────────────────

Ptr Shell::BuildRail()
{
    if (mWide)
    {
        return BuildTabBar();
    }

    if (mStack)
    {
        return BuildStackTabs();
    }

    auto tab = [this](const char* icon, const char* label, Page page, bool lit) {
        auto t = std::make_shared<RailTab>(mTheme, mIcons, icon, label, lit);
        t->onClick = [this, page] { ShowPage(page); };
        return Row({HGap(5), t, Stretch()});
    };

    auto settings = std::make_shared<IconButton>(mTheme, mIcons, "ui-gear", IconButton::Look::Boxed);
    settings->Size(42, 42).IconSize(20).Lit(mPage == Page::Settings);
    settings->Ink(mPage == Page::Settings ? mTheme.Accent() : mTheme.TextSecondary());
    settings->onClick = [this] { ShowPage(mPage == Page::Settings ? Page::Chain : Page::Settings); };

    std::vector<Ptr> items{VGap(6)};

    // Split, the chain and the effect are one Play page, as the full layout's PLAY tab; staged,
    // they take turns.
    if (mSplit)
    {
        items.push_back(FixedHeight(46, tab("ui-chain", "Play", PlayShown() ? mPage : Page::Chain, PlayShown())));
    }
    else
    {
        items.insert(items.end(),
                     {FixedHeight(46, tab("ui-chain", "Chain", Page::Chain, mPage == Page::Chain)), VGap(4),
                      FixedHeight(46, tab("ui-effect", "Effect", Page::Effect, mPage == Page::Effect))});
    }

    items.insert(items.end(),
                 {VGap(4), FixedHeight(46, tab("ui-pads", "Setlist", Page::Pads, mPage == Page::Pads)), VGap(4)});

    // Tone sharing can be switched off in Settings > Features, as in the web UI.
    if (guitarfx::uiclient::IsFeatureEnabled(mState, guitarfx::uiclient::Feature::ToneSharing))
    {
        items.push_back(FixedHeight(46, tab("ui-tones", "Tones", Page::Tones, mPage == Page::Tones)));
        items.push_back(VGap(4));
    }

    if (JamShown())
    {
        items.push_back(FixedHeight(46, tab("ui-jam", "Jam", Page::Jam, mPage == Page::Jam)));
        items.push_back(VGap(4));
    }

    // No Presets tab, as in the web UI's rail: the preset pill opens the presets.
    items.push_back(std::make_shared<el::element>());

    // The tuner and the looper, over the gear: tools rather than places, so squares like it, not
    // tabs. The looper lights while its strip is open. At the shortest window (400 px) the five
    // tabs, these two and the gear still fit.
    auto tool = [this](const char* icon, const char* name, bool lit, std::function<void()> onClick) {
        auto b = std::make_shared<IconButton>(mTheme, mIcons, icon, IconButton::Look::Boxed);
        b->Size(42, 42).IconSize(20).Lit(lit);
        b->Ink(lit ? mTheme.Accent() : mTheme.TextSecondary());
        b->AccessName(name);
        b->onClick = std::move(onClick);
        return FixedHeight(42, Row({HGap(9), b, Stretch()}));
    };
    items.push_back(tool("ui-tuner", "Tuner", false, [this] { OpenTuner(); }));
    items.push_back(VGap(4));

    if (LooperShown())
    {
        items.push_back(tool("ui-loop", "Looper", mLooperOpen, [this] { ToggleLooper(); }));
        items.push_back(VGap(4));
    }

    items.push_back(FixedHeight(42, Row({HGap(9), settings, Stretch()})));
    items.push_back(VGap(6));
    auto rail = Col(items);
    return Dressed(mTheme, mTheme.Bar(), &Backdrops::header, rail);
}

// ── The preset bar ──────────────────────────────────────────────────────────────

Ptr Shell::BuildTopBar()
{
    if (mWide)
    {
        return BuildHeader();
    }

    auto& commands = mSession.Commands();
    const bool dirty = mState.activePresetDirty;
    const bool havePreset = mState.activePreset.has_value();
    const std::string presetId = mState.activePresetId;
    const bool favourite = guitarfx::uiclient::IsPresetFavorite(mState, presetId);

    auto prev = std::make_shared<IconButton>(mTheme, mIcons, "ui-prev", IconButton::Look::Soft);
    prev->Size(28, 28).IconSize(16);
    prev->onClick = [this] { StepPresetWithConfirm(-1); };
    auto next = std::make_shared<IconButton>(mTheme, mIcons, "ui-next", IconButton::Look::Soft);
    next->Size(28, 28).IconSize(16);
    next->onClick = [this] { StepPresetWithConfirm(1); };

    // The name opens the presets; the heart in it favourites the preset. The source badge
    // (Factory, User) says why Edit and Save are greyed; the pill shows it only where the name
    // keeps 180 px, which it works out from its own width as it draws, so the window's width
    // need not be in TopBarSignature (the source is).
    const auto* summary = mState.FindPresetSummary(presetId);
    auto pill = std::make_shared<PresetPill>(mTheme, mIcons, PresetTitle(mState), favourite, dirty);
    pill->Badge(summary != nullptr ? SourceLabel(summary->source) : std::string(), 180.0f);
    pill->onClick = [this, p = pill.get()] { OpenPresetChooser(p->Bounds()); };
    pill->onFavourite = [&commands, presetId, favourite] {
        if (!presetId.empty())
        {
            commands.SetPresetFavorite(presetId, !favourite);
        }
    };

    // Dragged onto a folder of the chooser, the setlist or a pad (presets/drag.ts).
    Ptr pillItem = pill;

    if (!presetId.empty())
    {
        LibraryDrag drag;
        drag.id = presetId;
        drag.name = PresetTitle(mState);
        pillItem = LibraryDragSource(std::move(drag), pill);
    }

    auto create = ToolButton(mTheme, mIcons, "ui-new", {});
    create->Size(30, 28).Caret(true);
    create->onClick = [this, b = create.get()] { OpenNewPresetMenu(b->Bounds()); };

    // Edit and Save, as the web UI's toolbar (presets/toolbar.ts): only for a preset that may be
    // overwritten; a factory one is copied with Save As, in the preset menu.
    const bool canModify = havePreset && guitarfx::uiclient::CanModifyActivePreset(mState);
    auto edit = ToolButton(mTheme, mIcons, "ui-edit", [this] { EditActivePreset(); });
    edit->Disabled(!canModify);
    auto save = ToolButton(mTheme, mIcons, "ui-save", [this] { SaveActivePreset(false); });
    save->Disabled(!canModify);

    if (dirty)
    {
        save->Ink(mTheme.Accent());
    }

    auto random = ToolButton(mTheme, mIcons, "ui-random", [this] { LoadRandomPreset(); });
    // Share (#preset-publish at the compact size): publishing is Soundshed Guitar's, and this says so.
    auto publish = std::make_shared<IconButton>(mTheme, mIcons, "ui-share", IconButton::Look::Accent);
    publish->Size(28, 28).IconSize(14);
    publish->onClick = [this] { OpenPublishInfo(); };

    auto more = std::make_shared<IconButton>(mTheme, mIcons, "ui-more");
    more->Size(28, 28).IconSize(14).Caret(true);
    more->onClick = [this, b = more.get()] { OpenPresetMenu(b->Bounds()); };

    auto controls = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
    controls->Size(76, 28).Label("CONTROLS", 9.0f);
    controls->Ink(mTheme.TextSecondary());
    controls->onClick = [this] { OpenControls(); };
    mLive.Bind(controls, [this] { return mControlsOpen; }, [](IconButton& b, bool open) { b.Lit(open); });

    auto update = BuildUpdateButton();

    if (metrics::Touch())
    {
        // Touch sizes: each button is hit over 44 px, which spaces their faces as the gaps did,
        // and what a narrow bar has no room for goes: Share first (only a pointer to Soundshed
        // Guitar), then Random.
        const int extras = TouchBarExtras(StageWidth(), update != nullptr);
        std::vector<Ptr> items{HGap(7), prev, pillItem, next, HGap(2), create, edit, save};

        if (extras >= 1)
        {
            items.push_back(random);
        }

        if (extras >= 2)
        {
            items.push_back(publish);
        }

        items.insert(items.end(), {more, HGap(4)});

        if (update)
        {
            items.insert(items.end(), {Pad(0, 7, 0, 7, update), HGap(4)});
        }

        items.insert(items.end(), {controls, HGap(6)});
        return FixedHeight(metrics::TopBarHeight(),
                           Dressed(mTheme, mTheme.Tertiary(), &Backdrops::controlBar, PadXY(0, 2, Row(items))));
    }

    // Even gaps, as the web UI's rail strip (css/compact/rail.css): 6 inside the preset row and the
    // toolbar and 10 between the groups once the window is 720 wide, 4 below that, where every
    // pixel of gap comes out of the preset's name.
    const bool roomy = ViewSize().x >= 720.0f;
    const float g = roomy ? 6.0f : 4.0f, group = roomy ? 10.0f : 4.0f;
    std::vector<Ptr> items{HGap(15), prev, HGap(g), pillItem, HGap(g), next,    HGap(group), create, HGap(g),    edit,
                           HGap(g),  save, HGap(g), random,   HGap(g), publish, HGap(g),     more,   HGap(group)};

    if (update)
    {
        items.insert(items.end(), {Pad(0, 1, 0, 1, update), HGap(group)});
    }

    items.insert(items.end(), {controls, HGap(6)});
    auto bar = Row(items);
    return FixedHeight(metrics::TopBarHeight(),
                       Dressed(mTheme, mTheme.Tertiary(), &Backdrops::controlBar, PadXY(0, 6, bar)));
}

int Shell::TouchBarExtras(float width, bool update)
{
    // The touch bar's buttons, the pill's least width and CONTROLS (BuildTopBar), and the update
    // button when there is one.
    constexpr float kHit = metrics::kTouchTarget;
    const float base =
        7.0f + kHit + 160.0f + kHit + 2.0f + 4.0f * kHit + 4.0f + 82.0f + 6.0f + (update ? 110.0f : 0.0f);
    return width >= base + 2.0f * kHit ? 2 : width >= base + kHit ? 1 : 0;
}

MenuItem Shell::NewPresetItem(const std::string& label, const std::string& configuration)
{
    return {label, false, false, [this, configuration] {
                if (mState.activePresetDirty)
                {
                    OpenConfirm("Unsaved changes", "Start a new preset and lose the changes to this one?", "New preset",
                                [this, configuration] { mSession.Commands().NewPreset(configuration); });
                }
                else
                {
                    mSession.Commands().NewPreset(configuration);
                }
            }};
}

void Shell::OpenNewPresetMenu(el::rect anchor)
{
    // The web's New menu (presets/newPresetMenu.ts): Empty Preset, Basic Signal Chain.
    std::vector<MenuItem> items;
    items.push_back(NewPresetItem("Empty Preset", "empty"));
    items.push_back(NewPresetItem("Basic Signal Chain", "basic"));

    if (mState.activePreset)
    {
        items.push_back({"Save as a copy...", false, false, [this] { SaveActivePreset(true); }});
    }

    OpenDropdown(anchor, std::move(items), 180.0f);
}

void Shell::LoadRandomPreset()
{
    const auto& list = mState.presetList;

    if (list.size() < 2)
    {
        return;
    }

    static std::mt19937 random{std::random_device{}()};
    std::uniform_int_distribution<std::size_t> pick(0, list.size() - 1);
    std::size_t index = pick(random);

    if (list[index].id == mState.activePresetId)
    {
        index = (index + 1) % list.size();
    }

    LoadWithConfirm(list[index].id);
}

// ── The scene chips, over the chain ─────────────────────────────────────────────

Ptr Shell::BuildScenes()
{
    auto& commands = mSession.Commands();
    const auto& preset = mState.activePreset;

    std::vector<Ptr> chips;

    if (preset && preset->scenes.size() >= 2)
    {
        int index = 1;
        // The chips in order, for a dragged one to find where it lands among the others.
        auto order = std::make_shared<std::vector<std::shared_ptr<SceneChip>>>();
        const int count = static_cast<int>(preset->scenes.size());

        for (const auto& scene : preset->scenes)
        {
            const auto id = scene.id;
            const int at = index - 1;
            auto label = scene.title.empty() ? "Scene " + std::to_string(index) : scene.title;
            auto chip = std::make_shared<SceneChip>(mTheme, label, id == mState.activeSceneId);
            chip->onClick = [&commands, id] { commands.SelectScene(id); };
            chip->onRemove = [this, id] {
                OpenConfirm("Remove scene", "Remove this scene from the preset?", "Remove",
                            [this, id] { mSession.Commands().RemoveScene(id); });
            };
            chip->onMenu = [this, id, title = scene.title] {
                OpenPrompt("Rename scene", title, "Rename",
                           [this, id](const std::string& entered) { mSession.Commands().RenameScene(id, entered); });
            };
            chip->onRename = [this, id](const std::string& entered) { mSession.Commands().RenameScene(id, entered); };
            chip->onRenaming = [this](bool open) {
                mSceneRenaming = open;

                if (!open)
                {
                    mSceneSignature.clear();
                    Mark(kScenes);
                }
            };
            // Alt+Left/Right, as the web UI's badges.
            chip->onMove = [this, id, at, count](int step) {
                const int to = std::clamp(at + step, 0, count - 1);

                if (to != at)
                {
                    mSession.Commands().MoveScene(id, to);
                }
            };
            // Dragged: the place among the others whose middles it has passed, marked as it moves.
            chip->onDrag = [this, id, at, order](float x, bool done) {
                int to = 0;
                std::shared_ptr<SceneChip> before, after;

                for (int i = 0; i < static_cast<int>(order->size()); ++i)
                {
                    if (i == at)
                    {
                        continue;
                    }

                    const auto& other = (*order)[static_cast<std::size_t>(i)];
                    const auto r = other->DrawnBounds();

                    if ((r.left + r.right) * 0.5f < x)
                    {
                        ++to;
                        after = other;
                    }
                    else if (!before)
                    {
                        before = other;
                    }
                }

                for (const auto& other : *order)
                {
                    other->Mark(done || to == at ? 0 : other == before ? -1 : (other == after && !before) ? 1 : 0);
                }

                if (done && to != at)
                {
                    mSession.Commands().MoveScene(id, to);
                }
            };
            order->push_back(chip);
            chips.push_back(chip);
            chips.push_back(HGap(4));
            ++index;
        }

        auto add = std::make_shared<SceneChip>(mTheme, "+", false, true);
        add->onClick = [&commands] { commands.AddScene(); };
        chips.push_back(add);
    }
    else if (preset)
    {
        auto add = std::make_shared<SceneChip>(mTheme, "+ Scene", false, true);
        add->onClick = [&commands] { commands.AddScene(); };
        chips.push_back(add);
    }

    const bool touch = metrics::Touch();
    auto wrap = std::make_shared<IconButton>(mTheme, mIcons, "ui-wrap");
    // .signal-path-wrap-btn: muted at .38, the accent at .85 while on.
    wrap->Size(24, 16).IconSize(12).Ink(mWrapChain ? mTheme.Accent().opacity(0.85f)
                                                   : mTheme.TextMuted().opacity(0.38f));
    wrap->onClick = [this] {
        mWrapChain = !mWrapChain;
        mChainSignature.clear();
        Mark(kChain | kScenes);
        mSceneSignature.clear();
    };

    // The chips sit in a tray as wide as they are (.signal-path-scene-badges): Row() would
    // stretch it, so the tray is a plain tile.
    // Where the chain shares the stage with the editor, its chevron folds it (.signal-path-collapse-btn).
    std::vector<Ptr> row{HGap(26)};

    if (PlayStageShown() && !touch)
    {
        auto fold = std::make_shared<CollapseToggle>(mTheme, mIcons, "SIGNAL CHAIN", false);
        fold->onClick = [this] { SetChainCollapsed(true); };
        row = {HGap(2), Pad(0, 2, 0, 4, fold)};
    }

    if (!chips.empty())
    {
        auto tray = std::make_shared<el::htile_composite>();
        tray->push_back(HGap(2));

        for (auto& chip : chips)
        {
            tray->push_back(chip);
        }

        tray->push_back(HGap(2));
        const auto trayColour = mTheme.DarkOr(el::rgba(24, 24, 30, 184), mTheme.Text().opacity(0.06f));

        if (touch)
        {
            // The chips are hit over the row's 40 px and their x zones run past their faces, so a
            // tray round them all was one long capsule with the chips far apart in it: with touch
            // sizes each chip stands on its own, the faces an even 24 px apart.
            row.push_back(tray);
        }
        else
        {
            row.push_back(Rounded(trayColour, 11.0f, PadXY(0, 2, tray)));
        }
    }

    row.push_back(Stretch());

    if (AddInSceneRow())
    {
        auto add = std::make_shared<RoundAdd>(mTheme, 24.0f);
        add->onClick = [this, a = add.get()] { OpenAddMenu(a->Bounds()); };
        row.push_back(FixedWidth(44, Centred(add)));
    }

    row.push_back(touch ? Ptr(wrap) : Pad(0, 0, 0, 6, wrap));
    row.push_back(HGap(6));
    Ptr scenes = touch ? FixedHeight(40, Row(row)) : FixedHeight(24, Pad(0, 2, 0, 0, Row(row)));

    // With two or more presets in the mix, their tabs and the Mix tab come first.
    if (auto tabs = BuildMixerTabs())
    {
        return Col({FixedHeight(26, Pad(0, 4, 0, 0, tabs)), scenes});
    }

    return scenes;
}

// ── The chain stage ─────────────────────────────────────────────────────────────

Ptr Shell::BuildChainStage()
{
    // The add button floats at the bottom right (.signal-path-add-menu-trigger): it adds at the
    // end of the chain, before the output.
    auto add = std::make_shared<RoundAdd>(mTheme, 24.0f);
    add->onClick = [this, a = add.get()] { OpenAddMenu(a->Bounds()); };

    // Touch sizes: hit over 40 px, in a row as tall with the gap under it (ChainStrip.cpp).
    const bool touch = metrics::Touch();
    auto body = Col({mSceneSlot, mChainSlot,
                     FixedHeight(touch ? 40.0f : 24.0f, Row({Stretch(), add, HGap(touch ? 4.0f : 12.0f)})),
                     VGap(touch ? 8.0f : 20.0f)});
    return Over(body, std::make_shared<StageBackdrop>(mTheme));
}

// ── The footer ──────────────────────────────────────────────────────────────────

Ptr Shell::BuildHistoryControls()
{
    // The chain's history: undo, redo, the A/B pair and copying the live chain to the other.
    using Slot = guitarfx::uiclient::ChainHistory::Slot;
    const bool seeded = mHistory.IsSeeded();
    // The full footer's are footer buttons, 28 px (.footer-btn).
    const float key = mWide ? 28.0f : 26.0f;
    auto undo = ToolButton(mTheme, mIcons, "ui-undo", [this] { UndoChain(); }, key, 15.0f);
    undo->Disabled(!mHistory.CanUndo());
    auto redo = ToolButton(mTheme, mIcons, "ui-redo", [this] { RedoChain(); }, key, 15.0f);
    redo->Disabled(!mHistory.CanRedo());
#if defined(__APPLE__)
    undo->AccessShortcut("Cmd+Z");
    redo->AccessShortcut("Cmd+Shift+Z");
#else
    undo->AccessShortcut("Ctrl+Z");
    redo->AccessShortcut("Ctrl+Y");
#endif
    auto a = std::make_shared<AbButton>(mTheme, "A", mHistory.ActiveSlot() == Slot::A);
    a->onClick = [this] { SetChainSlot(Slot::A); };
    auto b = std::make_shared<AbButton>(mTheme, "B", mHistory.ActiveSlot() == Slot::B);
    b->onClick = [this] { SetChainSlot(Slot::B); };
    a->Enabled(seeded);
    b->Enabled(seeded);
    auto ab = Over(PadXY(2, 2, Row({a, HGap(2), b})), el::share(Surface(mTheme.Bar(), mTheme.ModalRule(), 6.0f)));
    auto copy = ToolButton(mTheme, mIcons, "ui-swap", [this] { CopyChainSlot(); }, key, 15.0f);
    copy->Disabled(!seeded);

    if (mWide)
    {
        for (auto* b : {undo.get(), redo.get(), copy.get()})
        {
            b->Styled(IconButton::Look::Footer);
        }
    }

    // The footer is rebuilt when the history moves; the Options sheet is not, so its copies follow
    // through bindings.
    mLive.Bind(undo, [this] { return mHistory.CanUndo(); }, [](IconButton& w, bool can) { w.Disabled(!can); });
    mLive.Bind(redo, [this] { return mHistory.CanRedo(); }, [](IconButton& w, bool can) { w.Disabled(!can); });
    mLive.Bind(copy, [this] { return mHistory.IsSeeded(); }, [](IconButton& w, bool can) { w.Disabled(!can); });
    auto bindHalf = [this](const std::shared_ptr<AbButton>& half, Slot slot) {
        mLive.Bind(
            half, [this, slot] { return std::make_pair(mHistory.IsSeeded(), mHistory.ActiveSlot() == slot); },
            [](AbButton& w, std::pair<bool, bool> key) {
                w.Enabled(key.first);
                w.Active(key.second);
            });
    };
    bindHalf(a, Slot::A);
    bindHalf(b, Slot::B);

    if (metrics::Touch())
    {
        // Each half is hit over 40 px, its face drawn against the other's in the middle, in the
        // pair's box as ever.
        a->FaceAt(1.0f);
        b->FaceAt(0.0f);
        auto box = el::share(Surface(mTheme.Bar(), mTheme.ModalRule(), 6.0f));
        auto pair = Over(Tight({a, HGap(2), b}), Centred(FixedWidth(54, FixedHeight(25, box))));
        return Tight({undo, redo, pair, copy});
    }

    // The A/B pair (.footer-ab-toggle): 28 tall, its halves 24 wide.
    return Tight({undo, HGap(2), redo, HGap(6), FixedWidth(54, FixedHeight(28, ab)), HGap(4), copy});
}

Ptr Shell::BuildTransport()
{
    if (mWide)
    {
        return BuildWideFooter();
    }

    // The compact footer keeps the everyday tools in reach rather than behind Options, in the full
    // footer's order: the demo clip and its play button at the left, then the setlists and Rec,
    // then the tempo and the metronome, then Options for the rest. The chain's history and A/B
    // are in Options; the tuner and the looper are on the rail.
    const float width = StageWidth();
    auto pads = ToolButton(mTheme, mIcons, "ui-pads", [this] { TogglePads(); }, 26.0f, 16.0f);
    pads->Lit(mPage == Page::Pads).AccessName("Setlists");

    if (mPage != Page::Pads)
    {
        pads->Ink(mTheme.TextSecondary());
    }

    auto options = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
    options->Size(64, 26).Label("OPTIONS", 10.0f);
    options->onClick = [this, o = options.get()] { OpenOptions(o->Bounds()); };

    // Touch sizes: the buttons are hit over 44 px, which spaces them, and the demo clip needs a
    // wider footer to grow.
    const bool touch = metrics::Touch();
    const float gap = touch ? 0.0f : 4.0f;
    auto divider = [this, touch] {
        const float inset = touch ? 8.0f : 4.0f;
        return Pad(6, inset, 6, inset,
                   FixedWidth(1, Fill(mTheme.Text().opacity(0.08f), std::make_shared<el::element>())));
    };

    std::vector<Ptr> row{HGap(8), BuildDemoControls(width >= (touch ? 820.0f : 720.0f) ? 150.0f : 112.0f, false),
                         divider(), pads};

    for (auto& b : BuildCaptureButtons(26.0f, false, false))
    {
        row.insert(row.end(), {HGap(gap), b});
    }

    row.push_back(Stretch());
    row.push_back(BuildTempoControls(false));
    row.push_back(divider());
    row.push_back(options);
    row.push_back(HGap(8));
    return FixedHeight(metrics::FooterHeight(),
                       Dressed(mTheme, mTheme.Footer(), &Backdrops::footer, PadXY(0, touch ? 0 : 3, Row(row))));
}

void Shell::OpenOptions(el::rect anchor)
{
    auto& commands = mSession.Commands();

    // The chain's history and A/B (the bar has the demo clip in their place), and the demo's repeat.
    // It changes in place, through a binding, as TAP and the metronome's switch do.
    const auto repeating = [this] { return BoolSetting(mState.appSettings, kDemoRepeatSetting, false); };
    auto loop = ToolButton(
        mTheme, mIcons, "ui-loop",
        [this, repeating] { mSession.Commands().SetSetting(kDemoRepeatSetting, !repeating()); }, 28.0f, 15.0f);
    loop->Label("Repeat demo", 11.0f).AccessName("Repeat demo");
    mLive.Bind(loop, repeating,
               [this](IconButton& b, bool on) { b.Lit(on).Ink(on ? mTheme.Accent() : mTheme.TextSecondary()); });

    // The tools: setlists, tuner, input and output, mute.
    auto pads = ToolButton(
        mTheme, mIcons, "ui-pads",
        [this] {
            CloseOverlay();
            ShowPage(Page::Pads);
        },
        28.0f, 16.0f);
    auto tuner = ToolButton(
        mTheme, mIcons, "ui-tuner",
        [this] {
            CloseOverlay();
            OpenTuner();
        },
        28.0f, 16.0f);
    auto midi = ToolButton(
        mTheme, mIcons, "ui-midi",
        [this] { // MIDI & Automation
            CloseOverlay();
            OpenMidiAutomation();
        },
        28.0f, 16.0f);
    auto io = ToolButton(
        mTheme, mIcons, "ui-io",
        [this] {
            CloseOverlay();
            OpenControls();
        },
        28.0f, 16.0f);
    const bool muted = mState.outputMuted;
    auto mute = ToolButton(
        mTheme, mIcons, muted ? "ui-mute" : "ui-speaker",
        [&commands, muted, this] {
            commands.SetOutputMuted(!muted);
            CloseOverlay();
        },
        28.0f, 16.0f);
    mute->Lit(muted);

    // The metronome: its tempo, TAP (as the full footer's), the full controls, on and off. TAP and
    // the transport leave the sheet open and change in place, through bindings rather than a
    // rebuild of the sheet from a click inside it; only the Metronome dialog closes it.
    auto tempo = std::make_shared<Text>(mTheme, "", 12.0f, Weight::SemiBold);
    // A set width, so a tempo of more figures needs no new layout.
    tempo->Colour(mTheme.Text());
    mLive.Bind(
        tempo, [this] { return mState.metronome.bpm; },
        [](Text& t, double value) {
            char bpm[32];
            std::snprintf(bpm, sizeof(bpm), "%.1f BPM", value);
            t.Set(bpm);
        });
    auto tap = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
    tap->Size(44, 28).Label("TAP", 10.0f).Ink(mTheme.TextSecondary());
    tap->AccessName("Tap tempo");
    tap->onClick = [this] { TapTempo(); };
    auto metronome = ToolButton(
        mTheme, mIcons, "ui-metronome",
        [this] {
            CloseOverlay();
            OpenMetronome();
        },
        28.0f, 16.0f);
    auto tick = ToolButton(
        mTheme, mIcons, "ui-play", [&commands, this] { commands.SetMetronomeEnabled(!mState.metronome.enabled); },
        28.0f, 13.0f);
    mLive.Bind(
        tick, [this] { return mState.metronome.enabled; },
        [](IconButton& b, bool ticking) {
            b.Icon(ticking ? "ui-stop" : "ui-play").Lit(ticking);
            b.AccessName(ticking ? "Stop metronome" : "Start metronome");
        });

    // The theme.
    auto themeButton = BuildThemeButton();

    // The layout: automatic (compact at or below 980 x 620), or one of them always; and the zoom.
    const auto density = StringSetting(mState.appSettings, kDensitySetting, "auto");
    auto densityButton = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
    densityButton->Size(110, 28)
        .Label(density == "full"      ? "Full"
               : density == "compact" ? "Compact"
                                      : "Auto layout",
               11.0f)
        .Caret(true);
    densityButton->onClick = [this, d = densityButton.get(), density] {
        std::vector<MenuItem> items;

        static const std::pair<const char*, const char*> kChoices[] = {
            {"auto", "Auto layout"}, {"compact", "Compact"}, {"full", "Full"}};

        for (const auto& choice : kChoices)
        {
            const std::string id = choice.first;
            items.push_back({choice.second, density == id, false,
                             [this, id] { mSession.Commands().SetSetting(kDensitySetting, id); }});
        }

        OpenDropdown(d->Bounds(), std::move(items), 150.0f);
    };
    // The zoom, and the choice between this UI and the web one, are in the theme's flyout, as
    // the web UI's.
    std::vector<Ptr> viewRow{themeButton, HGap(8), densityButton};

    viewRow.push_back(Stretch());

    // Touch sizes: 40 px rows, and the touch sizes' own choice under the view's (no room in it).
    const bool touch = metrics::Touch();
    const float rowHeight = touch ? 40.0f : 28.0f;
    const float space = touch ? 0.0f : 10.0f;
    auto rule = [this] { return FixedHeight(1, Fill(mTheme.Text().opacity(0.07f), std::make_shared<el::element>())); };
    std::vector<Ptr> rows{FixedHeight(rowHeight, Row({BuildHistoryControls(), Stretch(), loop})),
                          VGap(8),
                          rule(),
                          VGap(8),
                          FixedHeight(rowHeight, Row({pads, HGap(space), tuner, HGap(space), midi, HGap(space), io,
                                                      HGap(space), mute, Stretch()})),
                          VGap(8),
                          rule(),
                          VGap(8),
                          FixedHeight(rowHeight, Row({HGap(8), FixedWidth(72, tempo), HGap(6), tap, HGap(space),
                                                      metronome, HGap(touch ? 0 : 6), tick, Stretch()})),
                          VGap(8),
                          rule(),
                          VGap(8),
                          FixedHeight(rowHeight, Row(viewRow))};

    if (touch)
    {
        rows.insert(rows.end(), {VGap(8), FixedHeight(rowHeight, Row({TouchControlsButton(), Stretch()}))});
    }

    ShowPopover(Pad(10, 10, 10, 10, Col(rows)), anchor, 350.0f,
                touch ? 20.0f + 5.0f * 40.0f + 3.0f * 17.0f + 8.0f : 190.0f, true);
    // Noted, so a theme chosen in the flyout over it redraws it in the theme's colours.
    mOptionsAnchor = anchor;
    mOptionsOpen = true;
    mOnOverlayClosed = [this] { mOptionsOpen = false; };
}

std::shared_ptr<IconButton> Shell::TouchControlsButton()
{
    // Touch sizes: as the device is (TouchFirst), or on or off whatever it is.
    const auto touch = StringSetting(mState.appSettings, kTouchControlsSetting, "auto");
    auto button = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
    button->Size(150, 28)
        .Label(touch == "on"    ? "Touch sizes: On"
               : touch == "off" ? "Touch sizes: Off"
                                : "Touch sizes: Auto",
               11.0f)
        .Caret(true);
    button->onClick = [this, b = button.get(), touch] {
        std::vector<MenuItem> items;

        static const std::pair<const char*, const char*> kChoices[] = {
            {"auto", "Auto (touch screens)"}, {"on", "On"}, {"off", "Off"}};

        for (const auto& choice : kChoices)
        {
            const std::string id = choice.first;
            items.push_back({choice.second, touch == id, false,
                             [this, id] { mSession.Commands().SetSetting(kTouchControlsSetting, id); }});
        }

        OpenDropdown(b->Bounds(), std::move(items), 190.0f);
    };
    return button;
}
} // namespace nanoq::ui
