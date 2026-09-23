#include "nativeui/NanoShell.h"

#include "nativeui/sheets/ControlsSheet.h"
#include "nativeui/sheets/Dialogs.h"
#include "nativeui/sheets/EffectPickerSheet.h"
#include "nativeui/sheets/MetronomeSheet.h"
#include "nativeui/sheets/TunerSheet.h"
#include "nativeui/views/ChainView.h"
#include "nativeui/views/EffectPage.h"
#include "nativeui/views/NavBar.h"
#include "nativeui/views/PresetsPage.h"
#include "nativeui/views/SceneStrip.h"
#include "nativeui/views/SettingsPage.h"
#include "nativeui/views/TopBar.h"
#include "nativeui/views/TransportBar.h"
#include "nativeui/widgets/Sheet.h"
#include "uiclient/ChainLayout.h"
#include "uiclient/NodeLabels.h"

namespace soundshed::nano
{
namespace
{
using guitarfx::uiclient::Topic;

constexpr const char* kUiScaleSetting = "nativeUi.scale";

// Wide enough that the scenes fit in the top bar beside the preset name.
constexpr int kScenesInTopBarWidth = 900;

const char* pageName (Page page)
{
    switch (page)
    {
        case Page::Chain: return "chain";
        case Page::Effect: return "effect";
        case Page::Presets: return "presets";
        case Page::Settings: return "settings";
        case Page::Device: return "device";
    }

    return "effect";
}

Page pageFromName (const std::string& name)
{
    for (const auto page : { Page::Chain, Page::Effect, Page::Presets, Page::Settings })
        if (name == pageName (page))
            return page;

    return Page::Effect;
}
} // namespace

NanoShell::NanoShell (NanoContext& contextIn) : context (contextIn)
{
    setComponentID ("shell");
    installActions();

    // Before the views subscribe, so the selection is settled before they redraw for a preset.
    presetSubscription = context.client.Subscribe (Topic::ActivePreset, [this] { onActivePreset(); });
    sessionSubscription = context.client.Subscribe (Topic::Session, [this] { onSession(); });
    notificationSubscription = context.client.Subscribe (Topic::Notifications, [this] {
        for (const auto& notification : context.client.TakeNotifications())
            if (toasts != nullptr)
                toasts->show (juce::String::fromUTF8 (notification.title.c_str()), juce::String::fromUTF8 (notification.detail.c_str()),
                              notification.kind == guitarfx::uiclient::Notification::Kind::Error);
    });

    context.showToast = [this] (const juce::String& title, const juce::String& detail, bool isError) {
        if (toasts != nullptr)
            toasts->show (title, detail, isError);
    };

    appliedTheme = context.state().theme;
    buildViews();
}

NanoShell::~NanoShell()
{
    context.showToast = nullptr;
    sheet = nullptr;
    retiredSheets.clear();
}

void NanoShell::installActions()
{
    actions.showPage = [this] (Page newPage) { showPage (newPage); };
    actions.currentPage = [this] { return page; };
    actions.selectNode = [this] (const std::string& nodeId, bool openEffect) { selectNode (nodeId, openEffect); };
    actions.selectedNode = [this] { return selectedNodeId; };
    actions.openEffectPicker = [this] (const std::string& afterNodeId) { openEffectPicker (afterNodeId); };
    actions.openNodeMenu = [this] (const std::string& nodeId, juce::Component* target) { showNodeMenu (nodeId, target); };
    actions.openSheet = [this] (const juce::String& title, std::unique_ptr<juce::Component> content, juce::Point<int> size) {
        openSheet (title, std::move (content), size);
    };
    actions.closeSheet = [this] { closeSheet(); };
    actions.promptText = [this] (const juce::String& title, const juce::String& initial, std::function<void (const juce::String&)> onOk) {
        openSheet (title, std::make_unique<PromptContent> (context, initial, std::move (onOk), [this] { closeSheet(); }), { 440, 190 });
    };
    actions.confirm = [this] (const juce::String& title, const juce::String& message, const juce::String& confirmText,
                              std::function<void()> onConfirm) {
        openSheet (title, std::make_unique<ConfirmContent> (context, message, confirmText, std::move (onConfirm), [this] { closeSheet(); }),
                   { 440, 210 });
    };
}

void NanoShell::buildViews()
{
    // Everything is rebuilt, so every view picks up the theme's colours from scratch.
    closeSheet();
    removeAllChildren();

    topBar = std::make_unique<TopBar> (context);
    sceneStrip = std::make_unique<SceneStrip> (context, actions);
    chainStrip = std::make_unique<ChainPanel> (context, actions, ChainView::Mode::Strip);
    chainPage = std::make_unique<ChainPanel> (context, actions, ChainView::Mode::Full);
    effectPage = std::make_unique<EffectPage> (context, actions);
    presetsPage = std::make_unique<PresetsPage> (context, actions);
    settingsPage = std::make_unique<SettingsPage> (context, actions);
    devicePage = std::make_unique<DevicePage> (context, actions);
    transport = std::make_unique<TransportBar> (context, actions);
    navBar = std::make_unique<NavBar> (context, actions);
    toasts = std::make_unique<ToastOverlay> (context);

    chainStrip->setComponentID ("chain-strip");
    chainPage->setComponentID ("page-chain");

    topBar->onOpenPresets = [this] { showPage (Page::Presets); };
    topBar->onOpenTuner = [this] { openTuner(); };
    topBar->onMoreMenu = [this] { showMoreMenu(); };
    transport->onOpenMetronome = [this] { openMetronome(); };
    transport->onOpenControls = [this] { openControls(); };

    for (juce::Component* child : std::initializer_list<juce::Component*> { topBar.get(), chainStrip.get(), chainPage.get(), effectPage.get(),
                                                                            presetsPage.get(), settingsPage.get(), transport.get(), navBar.get() })
        addChildComponent (child);

    // The scene strip is placed by resized(), in the top bar or on a row of its own.
    addChildComponent (devicePage.get());
    addChildComponent (*toasts);
    topBar->setVisible (true);
    transport->setVisible (true);
    navBar->setVisible (true);

    ensureSelection();
    showPage (page);
}

void NanoShell::onSession()
{
    const auto& state = context.state();

    if (! pageRestored && state.haveState)
    {
        const auto native = state.uiSettings.value ("native", nlohmann::json::object());

        if (native.is_object())
            showPage (pageFromName (native.value ("page", std::string ("effect"))));

        pageRestored = true;
    }

    const auto scaleIt = state.appSettings.find (kUiScaleSetting);
    const float newScale = scaleIt != state.appSettings.end() && scaleIt->is_number()
                               ? juce::jlimit (0.8f, 1.5f, scaleIt->get<float>())
                               : 1.0f;

    if (std::abs (newScale - scale) > 0.001f)
    {
        scale = newScale;

        if (onScaleChanged)
            onScaleChanged();
    }

    if (state.theme != appliedTheme)
    {
        appliedTheme = state.theme;
        context.theme.setTheme (appliedTheme);
        context.lookAndFeel->refreshColours();

        // Not now: the theme button that asked for this is still inside its click.
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<NanoShell> (this)] {
            if (safe != nullptr)
            {
                safe->buildViews();
                safe->resized();
                safe->repaint();
            }
        });
    }
}

void NanoShell::onActivePreset()
{
    const auto& state = context.state();
    const auto scenes = state.activePreset ? state.activePreset->scenes.size() : 0;

    if ((scenes > 1) != (sceneCount > 1))
    {
        sceneCount = scenes;
        resized();
    }

    sceneCount = scenes;

    if (selectAddedUntil > 0.0 && state.activePreset)
    {
        if (NanoContext::now() > selectAddedUntil)
        {
            selectAddedUntil = 0.0;
        }
        else
        {
            for (const auto& node : state.activePreset->graph.nodes)
            {
                if (nodeIdsBeforeAdd.count (node.id) == 0 && ! guitarfx::uiclient::IsBoundaryNode (node))
                {
                    selectAddedUntil = 0.0;
                    selectedNodeId = node.id;
                    showPage (Page::Effect);
                    refreshSelection();
                    return;
                }
            }
        }
    }

    ensureSelection();
}

void NanoShell::ensureSelection()
{
    const auto& state = context.state();

    if (! state.activePreset)
        return;

    const auto& graph = state.activePreset->graph;

    if (! selectedNodeId.empty() && graph.FindNode (selectedNodeId) != nullptr)
        return;

    // The amp if there is one (what most people reach for first), else the first effect.
    const auto layout = guitarfx::uiclient::BuildChainLayout (graph);
    const auto drawn = layout.DrawnNodeIds();
    std::string choice;

    for (const auto& id : drawn)
    {
        const auto* node = graph.FindNode (id);

        if (node == nullptr || guitarfx::uiclient::IsSplitterType (node->type) || guitarfx::uiclient::IsMixerType (node->type))
            continue;

        if (choice.empty())
            choice = id;

        if (guitarfx::uiclient::NodeCategory (state, *node) == "amp")
        {
            choice = id;
            break;
        }
    }

    // No refresh: the views subscribed after the shell, so they redraw for this preset anyway.
    selectedNodeId = choice;
}

void NanoShell::selectNode (const std::string& nodeId, bool openEffect)
{
    if (nodeId.empty())
        return;

    const bool changed = nodeId != selectedNodeId;
    selectedNodeId = nodeId;

    if (openEffect)
        showPage (Page::Effect);

    if (changed)
        refreshSelection();
}

void NanoShell::refreshSelection()
{
    if (chainStrip != nullptr)
        chainStrip->getChain().refresh();

    if (chainPage != nullptr)
        chainPage->getChain().refresh();

    if (effectPage != nullptr)
        effectPage->refresh();
}

void NanoShell::showPage (Page newPage)
{
    const bool changed = newPage != page;
    page = newPage;

    if (effectPage == nullptr)
        return;

    effectPage->setVisible (page == Page::Effect);
    chainStrip->setVisible (page == Page::Effect);
    chainPage->setVisible (page == Page::Chain);
    presetsPage->setVisible (page == Page::Presets);
    settingsPage->setVisible (page == Page::Settings);
    devicePage->setVisible (page == Page::Device);
    navBar->setCurrent (page);

    if (page == Page::Presets)
        presetsPage->refresh();

    if (changed && pageRestored && page != Page::Device)
        context.commands.PatchNativeUiSettings ({ { "page", pageName (page) } });

    resized();
}

void NanoShell::openSheet (const juce::String& title, std::unique_ptr<juce::Component> content, juce::Point<int> size)
{
    closeSheet();
    sheet = std::make_unique<Sheet> (context, title, std::move (content), size);
    sheet->onClose = [this] { closeSheet(); };
    addAndMakeVisible (*sheet);
    sheet->setBounds (getLocalBounds());
    toasts->toFront (false);
    sheet->grabKeyboardFocus();
}

void NanoShell::closeSheet()
{
    if (sheet == nullptr)
        return;

    // Often called from inside a button on the sheet: hide it now, delete it later.
    sheet->setVisible (false);
    removeChildComponent (sheet.get());
    retiredSheets.push_back (std::move (sheet));

    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<NanoShell> (this)] {
        if (safe != nullptr)
            safe->retiredSheets.clear();
    });
}

void NanoShell::openEffectPicker (const std::string& afterNodeId)
{
    auto content = std::make_unique<EffectPickerContent> (context, afterNodeId, [this, afterNodeId] (const std::string& effectType) {
        closeSheet();
        nodeIdsBeforeAdd.clear();

        if (const auto& preset = context.state().activePreset)
            for (const auto& node : preset->graph.nodes)
                nodeIdsBeforeAdd.insert (node.id);

        selectAddedUntil = NanoContext::now() + 3.0;
        context.commands.AddNode (effectType, afterNodeId);
    });

    openSheet ("Add effect", std::move (content), { 640, 640 });
}

void NanoShell::openTuner()
{
    openSheet ("Tuner", std::make_unique<TunerContent> (context), { 460, 380 });
}

void NanoShell::openMetronome()
{
    openSheet ("Metronome", std::make_unique<MetronomeContent> (context), { 480, 520 });
}

void NanoShell::openControls()
{
    openSheet ("Input and output", std::make_unique<ControlsContent> (context), { 520, 620 });
}

void NanoShell::idleTick (double nowSeconds)
{
    if (topBar != nullptr)
        topBar->updateMeters (nowSeconds);

    if (page == Page::Effect && effectPage != nullptr)
    {
        effectPage->updateMeters (nowSeconds);
        chainStrip->getChain().updateMeters (nowSeconds);
    }
    else if (page == Page::Chain && chainPage != nullptr)
    {
        chainPage->getChain().updateMeters (nowSeconds);
    }
}

void NanoShell::paint (juce::Graphics& g)
{
    g.fillAll (context.theme.background());
}

void NanoShell::resized()
{
    if (topBar == nullptr)
        return;

    metrics = guitarfx::uiclient::DecideLayout (getWidth(), getHeight(), context.touch);
    effectPage->setUseSliderList (metrics.sliderList);

    auto area = getLocalBounds();
    const bool rail = metrics.shell == guitarfx::uiclient::ShellLayout::Rail;
    navBar->setVertical (rail);

    if (rail)
        navBar->setBounds (area.removeFromLeft (NanoTheme::railWidth));
    else
        navBar->setBounds (area.removeFromBottom (NanoTheme::tabBarHeight));

    // The scenes join the top bar when it is wide enough, and get their own row otherwise.
    const bool embed = area.getWidth() >= kScenesInTopBarWidth;

    if (embed != scenesInTopBar || sceneStrip->getParentComponent() == nullptr)
    {
        scenesInTopBar = embed;

        if (auto* parent = sceneStrip->getParentComponent())
            parent->removeChildComponent (sceneStrip.get());

        if (embed)
            topBar->addAndMakeVisible (*sceneStrip);
        else
            addAndMakeVisible (*sceneStrip);

        topBar->setSceneStripArea (embed ? sceneStrip.get() : nullptr);
    }

    topBar->setBounds (area.removeFromTop (NanoTheme::barHeight + 8));

    // On a short screen a preset with one scene has nothing to switch to, so the strip
    // gives its row to the page until a second scene exists ("Add scene" is in the more menu).
    const bool playPage = page == Page::Effect || page == Page::Chain;
    const bool shortScreen = getHeight() < 440;
    const bool showScenes = embed || (playPage && ! (shortScreen && sceneCount <= 1));
    sceneStrip->setVisible (showScenes);

    if (! embed && showScenes)
        sceneStrip->setBounds (area.removeFromTop (40));

    transport->setBounds (area.removeFromBottom (NanoTheme::transportHeight));

    if (page == Page::Effect)
        chainStrip->setBounds (area.removeFromTop (NanoTheme::stripHeight + 4));

    for (juce::Component* pageView : std::initializer_list<juce::Component*> { effectPage.get(), chainPage.get(), presetsPage.get(),
                                                                               settingsPage.get(), devicePage.get() })
        pageView->setBounds (area);

    // A card above the transport, or above the tab bar in portrait.
    const int toastWidth = juce::jmin (520, getWidth() - 24);
    toasts->setBounds (area.getCentreX() - toastWidth / 2, transport->getY() - 72, toastWidth, 64);

    if (sheet != nullptr)
        sheet->setBounds (getLocalBounds());
}
} // namespace soundshed::nano
