// The compact frame around the stage, as the web UI draws it at its compact density
// (ui-components/header-icon-bar.html, preset-selector-row.html, preset-toolbar-row.html,
// footer-bar.html; css/compact/rail.css): the rail, the preset bar, the scene chips over the chain,
// and the footer and its Options sheet.

#include "ui/shell/Shell.h"

#include "ui/shell/FrameParts.h"

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

    auto tab = [this](const char* icon, const char* label, Page page) {
        auto t = std::make_shared<RailTab>(mTheme, mIcons, icon, label, mPage == page);
        t->onClick = [this, page] { ShowPage(page); };
        return Row({HGap(5), t, Stretch()});
    };

    auto settings = std::make_shared<IconButton>(mTheme, mIcons, "ui-gear", IconButton::Look::Boxed);
    settings->Size(42, 42).IconSize(20).Lit(mPage == Page::Settings);
    settings->Ink(mPage == Page::Settings ? mTheme.Accent() : mTheme.TextSecondary());
    settings->onClick = [this] { ShowPage(mPage == Page::Settings ? Page::Chain : Page::Settings); };

    std::vector<Ptr> items{VGap(6), FixedHeight(46, tab("ui-chain", "Chain", Page::Chain)),
                           VGap(4), FixedHeight(46, tab("ui-effect", "Effect", Page::Effect)),
                           VGap(4), FixedHeight(46, tab("ui-pads", "Setlist", Page::Pads)),
                           VGap(4)};

    // Tone sharing can be switched off in Settings > Features, as in the web UI.
    if (guitarfx::uiclient::IsFeatureEnabled(mState, guitarfx::uiclient::Feature::ToneSharing))
    {
        items.push_back(FixedHeight(46, tab("ui-tones", "Tones", Page::Tones)));
        items.push_back(VGap(4));
    }

    if (JamShown())
    {
        items.push_back(FixedHeight(46, tab("ui-jam", "Jam", Page::Jam)));
        items.push_back(VGap(4));
    }

    // No Presets tab, as in the web UI's rail: the preset pill opens the presets.
    items.push_back(std::make_shared<el::element>());
    items.push_back(FixedHeight(42, Row({HGap(9), settings, Stretch()})));
    items.push_back(VGap(6));
    auto rail = Col(items);
    return Fill(mTheme.Bar(), rail);
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

    // The name opens the presets; the heart in it favourites the preset.
    auto pill = std::make_shared<PresetPill>(mTheme, mIcons, PresetTitle(mState), favourite, dirty);
    pill->onClick = [this, p = pill.get()] { OpenPresetChooser(p->Bounds()); };
    pill->onFavourite = [&commands, presetId, favourite] {
        if (!presetId.empty())
        {
            commands.SetPresetFavorite(presetId, !favourite);
        }
    };

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

    std::vector<Ptr> items{HGap(15), prev, HGap(6), pill,   HGap(6), next,    HGap(10), create, HGap(4), edit,
                           HGap(4),  save, HGap(8), random, HGap(8), publish, HGap(4),  more,   HGap(19)};

    if (auto update = BuildUpdateButton())
    {
        items.insert(items.end(), {Pad(0, 1, 0, 1, update), HGap(8)});
    }

    items.insert(items.end(), {controls, HGap(6)});
    auto bar = Row(items);
    return FixedHeight(metrics::kTopBarHeight, Fill(mTheme.Tertiary(), PadXY(0, 6, bar)));
}

MenuItem Shell::NewPresetItem()
{
    return {"New preset", false, false, [this] {
                if (mState.activePresetDirty)
                {
                    OpenConfirm("Unsaved changes", "Start a new preset and lose the changes to this one?", "New preset",
                                [this] { mSession.Commands().NewPreset(); });
                }
                else
                {
                    mSession.Commands().NewPreset();
                }
            }};
}

void Shell::OpenNewPresetMenu(el::rect anchor)
{
    std::vector<MenuItem> items;
    items.push_back(NewPresetItem());

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

        for (const auto& scene : preset->scenes)
        {
            const auto id = scene.id;
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
        auto add = std::make_shared<SceneChip>(mTheme, "+ Scene", false);
        add->onClick = [&commands] { commands.AddScene(); };
        chips.push_back(add);
    }

    auto wrap = std::make_shared<IconButton>(mTheme, mIcons, "ui-wrap");
    wrap->Size(24, 16).IconSize(12).Ink(mWrapChain ? mTheme.Accent() : mTheme.TextMuted());
    wrap->onClick = [this] {
        mWrapChain = !mWrapChain;
        mChainSignature.clear();
        Mark(kChain | kScenes);
        mSceneSignature.clear();
    };

    // The chips sit in a tray as wide as they are (.signal-path-scene-badges): Row() would
    // stretch it, so the tray is a plain tile.
    std::vector<Ptr> row{HGap(26)};

    if (!chips.empty())
    {
        auto tray = std::make_shared<el::htile_composite>();
        tray->push_back(HGap(2));

        for (auto& chip : chips)
        {
            tray->push_back(chip);
        }

        tray->push_back(HGap(2));
        row.push_back(
            Rounded(mTheme.Pick(el::rgba(24, 24, 30, 184), mTheme.Text().opacity(0.06f)), 11.0f, PadXY(0, 2, tray)));
    }

    row.push_back(Stretch());
    row.push_back(Pad(0, 0, 0, 6, wrap));
    row.push_back(HGap(6));
    Ptr scenes = FixedHeight(24, Pad(0, 2, 0, 0, Row(row)));

    // With two or more presets in the mix, their tabs and the Mix tab come first.
    if (auto tabs = BuildMixerTabs())
    {
        return Col({FixedHeight(26, Pad(0, 4, 0, 0, tabs)), scenes});
    }

    return scenes;
}

// ── The chain stage ─────────────────────────────────────────────────────────────

std::shared_ptr<IconButton> Shell::BuildThemeButton()
{
    auto themeButton = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
    const std::string themeName = mState.theme == "light" ? "Light" : mState.theme == "classic" ? "Classic" : "Dark";
    themeButton->Size(84, 28).Label(themeName, 11.0f).Caret(true);
    themeButton->onClick = [this, t = themeButton.get()] {
        std::vector<MenuItem> items;

        // A plain array and no structured binding: MSVC 19.52 rejects the braced range and the
        // binding's capture here.
        static const std::pair<const char*, const char*> kChoices[] = {
            {"dark", "Dark"}, {"light", "Light"}, {"classic", "Classic"}};

        for (const auto& choice : kChoices)
        {
            const std::string id = choice.first;
            items.push_back(
                {choice.second, mState.theme == id, false, [this, id] { mSession.Commands().SetTheme(id); }});
        }

        OpenDropdown(t->Bounds(), std::move(items), 150.0f);
    };
    return themeButton;
}

Ptr Shell::BuildChainStage()
{
    // The add button floats at the bottom right (.signal-path-add-menu-trigger): it adds at the
    // end of the chain, before the output.
    auto add = std::make_shared<RoundAdd>(mTheme, 24.0f);
    add->onClick = [this, a = add.get()] { OpenAddMenu(a->Bounds()); };

    auto body = Col({mSceneSlot, mChainSlot, FixedHeight(24, Row({Stretch(), add, HGap(12)})), VGap(20)});
    return Over(body, std::make_shared<StageBackdrop>(mTheme));
}

// ── The footer ──────────────────────────────────────────────────────────────────

Ptr Shell::BuildHistoryControls()
{
    // The chain's history: undo, redo, the A/B pair and copying the live chain to the other.
    using Slot = guitarfx::uiclient::ChainHistory::Slot;
    const bool seeded = mHistory.IsSeeded();
    auto undo = ToolButton(mTheme, mIcons, "ui-undo", [this] { UndoChain(); }, 26.0f, 15.0f);
    undo->Disabled(!mHistory.CanUndo());
    auto redo = ToolButton(mTheme, mIcons, "ui-redo", [this] { RedoChain(); }, 26.0f, 15.0f);
    redo->Disabled(!mHistory.CanRedo());
    auto a = std::make_shared<AbButton>(mTheme, "A", mHistory.ActiveSlot() == Slot::A);
    a->onClick = [this] { SetChainSlot(Slot::A); };
    auto b = std::make_shared<AbButton>(mTheme, "B", mHistory.ActiveSlot() == Slot::B);
    b->onClick = [this] { SetChainSlot(Slot::B); };
    a->Enabled(seeded);
    b->Enabled(seeded);
    auto ab =
        Over(PadXY(2, 2, Row({a, HGap(2), b})),
             el::share(Surface(mTheme.Bar(), mTheme.Pick(el::rgba(58, 58, 72, 255), mTheme.BorderStrong()), 6.0f)));
    auto copy = ToolButton(mTheme, mIcons, "ui-swap", [this] { CopyChainSlot(); }, 26.0f, 15.0f);
    copy->Disabled(!seeded);
    return Tight({undo, HGap(2), redo, HGap(6), FixedWidth(54, FixedHeight(25, ab)), HGap(4), copy});
}

Ptr Shell::BuildTransport()
{
    if (mWide)
    {
        return BuildWideFooter();
    }

    // The compact footer keeps the everyday tools in reach rather than behind Options: the
    // setlists and tuner, the demo clip and its play button, the chain's history and A/B, the
    // tempo and the metronome, then Options for the rest. What does not fit a narrow window
    // goes first (the demo clip, then the tempo).
    const float width = StageWidth();
    auto pads = ToolButton(mTheme, mIcons, "ui-pads", [this] { TogglePads(); }, 26.0f, 16.0f);
    pads->Lit(mPage == Page::Pads);

    if (mPage != Page::Pads)
    {
        pads->Ink(mTheme.TextSecondary());
    }

    auto tuner = ToolButton(mTheme, mIcons, "ui-tuner", [this] { OpenTuner(); }, 26.0f, 16.0f);
    tuner->Ink(mTheme.TextSecondary());

    auto options = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
    options->Size(64, 26).Label("OPTIONS", 10.0f);
    options->onClick = [this, o = options.get()] { OpenOptions(o->Bounds()); };

    auto divider = [this] {
        return Pad(6, 4, 6, 4, FixedWidth(1, Fill(mTheme.Text().opacity(0.08f), std::make_shared<el::element>())));
    };

    std::vector<Ptr> row{HGap(8), pads, HGap(4), tuner};

    for (auto& b : BuildCaptureButtons(26.0f))
    {
        row.insert(row.end(), {HGap(4), b});
    }

    if (width >= 760.0f)
    {
        row.push_back(divider());
        row.push_back(BuildDemoControls(width >= 860.0f ? 132.0f : 104.0f, false));
    }

    row.push_back(Stretch());
    row.push_back(BuildHistoryControls());
    row.push_back(Stretch());

    if (width >= 640.0f)
    {
        row.push_back(BuildTempoControls(false));
        row.push_back(divider());
    }

    row.push_back(options);
    row.push_back(HGap(8));
    return FixedHeight(metrics::kFooterHeight, Fill(mTheme.Footer(), PadXY(0, 3, Row(row))));
}

void Shell::OpenOptions(el::rect anchor)
{
    auto& commands = mSession.Commands();

    // Demo audio: the clip (a tap opens the list), play/stop and repeat.
    const auto* clip = SelectedDemoClip();
    const bool playing = mState.demo.playing;
    const bool repeat = BoolSetting(mState.appSettings, kDemoRepeatSetting, false);

    auto clipButton = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
    clipButton->Size(200, 28).Label(clip != nullptr ? clip->title : "Demo", 11.0f).Caret(true);
    clipButton->Disabled(clip == nullptr);
    clipButton->onClick = [this] { OpenDemoClips(); };

    auto play = ToolButton(
        mTheme, mIcons, playing ? "ui-stop" : "ui-play",
        [this] {
            PlayOrStopDemo();
            CloseOverlay();
        },
        28.0f, 13.0f);
    play->Lit(playing).Disabled(clip == nullptr);
    auto loop = ToolButton(
        mTheme, mIcons, "ui-loop",
        [this, repeat] {
            mSession.Commands().SetSetting(kDemoRepeatSetting, !repeat);
            CloseOverlay();
        },
        28.0f, 15.0f);
    loop->Lit(repeat);

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

    // The metronome: its tempo, the full controls, on and off.
    char bpm[32];
    std::snprintf(bpm, sizeof(bpm), "%.1f BPM", mState.metronome.bpm);
    auto tempo = std::make_shared<Text>(mTheme, bpm, 12.0f, Weight::SemiBold);
    tempo->Colour(mTheme.Text()).Fixed(true);
    auto metronome = ToolButton(
        mTheme, mIcons, "ui-metronome",
        [this] {
            CloseOverlay();
            OpenMetronome();
        },
        28.0f, 16.0f);
    const bool ticking = mState.metronome.enabled;
    auto tick = ToolButton(
        mTheme, mIcons, ticking ? "ui-stop" : "ui-play",
        [&commands, ticking, this] {
            commands.SetMetronomeEnabled(!ticking);
            CloseOverlay();
        },
        28.0f, 13.0f);
    tick->Lit(ticking);

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
    std::vector<Ptr> viewRow{themeButton, HGap(8), densityButton};

    if (mSession.getZoom && mSession.setZoom)
    {
        auto zoom = ZoomButton();
        viewRow.push_back(HGap(8));
        viewRow.push_back(zoom);
    }

    viewRow.push_back(Stretch());

    auto rule = [this] { return FixedHeight(1, Fill(mTheme.Text().opacity(0.07f), std::make_shared<el::element>())); };
    auto panel =
        Pad(10, 10, 10, 10,
            Col({FixedHeight(28, Row({clipButton, Stretch(), play, HGap(6), loop})), VGap(8), rule(), VGap(8),
                 FixedHeight(28, Row({pads, HGap(10), tuner, HGap(10), midi, HGap(10), io, HGap(10), mute, Stretch()})),
                 VGap(8), rule(), VGap(8),
                 FixedHeight(28, Row({HGap(8), tempo, HGap(14), metronome, HGap(6), tick, Stretch()})), VGap(8), rule(),
                 VGap(8), FixedHeight(28, Row(viewRow))}));
    ShowPopover(panel, anchor, 350.0f, 190.0f, true);
}
} // namespace nanoq::ui
