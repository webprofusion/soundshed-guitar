#include "nativeui/views/EffectPage.h"

#include "nativeui/views/NodeCard.h"
#include "uiclient/ChainLayout.h"
#include "uiclient/NodeLabels.h"
#include "uiclient/ParamFormat.h"

namespace soundshed::nano
{
using namespace guitarfx::uiclient;

namespace
{
/// The one control the input and output nodes carry: a trim in dB (PresetTypes.h,
/// kBoundaryGainParam), which the effect catalog does not list since they are not effects.
EffectParamInfo boundaryTrim()
{
    EffectParamInfo trim;
    trim.key = guitarfx::kBoundaryGainParam;
    trim.name = "Trim";
    trim.unit = "dB";
    trim.minValue = guitarfx::kBoundaryGainMinDb;
    trim.maxValue = guitarfx::kBoundaryGainMaxDb;
    trim.defaultValue = 0.0;
    return trim;
}
} // namespace

// ── The visual: what kind of effect, what it plays, how hot it runs ─────────────

class EffectPage::Visual final : public juce::Component
{
public:
    explicit Visual (NanoContext& contextIn) : context (contextIn), meter (contextIn, {})
    {
        setComponentID ("effect-visual");
        meter.setVertical (true);
        addAndMakeVisible (meter);
    }

    void setNode (const guitarfx::GraphNode* nodeIn)
    {
        node = nodeIn != nullptr ? std::optional<guitarfx::GraphNode> (*nodeIn) : std::nullopt;
        repaint();
    }

    void update (double now)
    {
        if (! node)
            return;

        const auto* level = nodeLevel (context, node->id);
        meter.setLevel (level != nullptr ? level->peakDb : -120.0, level != nullptr && level->clipped, now);

        const auto& perf = context.state().telemetry.nodeProcessingUs;
        const auto it = perf.find (context.state().activePresetId + "::" + node->id);
        const double us = it != perf.end() ? it->second : -1.0;

        if (std::abs (us - processingUs) > 0.5)
        {
            processingUs = us;
            repaint (getLocalBounds().removeFromBottom (24));
        }
    }

    void resized() override
    {
        meter.setBounds (getLocalBounds().reduced (10).removeFromRight (14));
    }

    void paint (juce::Graphics& g) override
    {
        const auto& theme = context.theme;
        const auto bounds = getLocalBounds().toFloat();

        if (! node)
            return;

        const auto& state = context.state();
        const auto category = NodeCategory (state, *node);
        const auto* look = context.presentation.Category (category);
        const auto accent = juce::Colour (context.presentation.CategoryColour (category));
        auto from = look != nullptr && look->visualBackground ? juce::Colour (look->visualBackground->first) : accent.darker (0.6f);
        auto to = look != nullptr && look->visualBackground ? juce::Colour (look->visualBackground->second) : theme.card();

        if (theme.isLight())
        {
            from = from.brighter (0.2f);
            to = to.brighter (0.4f);
        }

        g.setGradientFill (juce::ColourGradient (from, bounds.getTopLeft(), to, bounds.getBottomRight(), false));
        g.fillRoundedRectangle (bounds, 12.0f);

        const bool bypassed = ! node->enabled && ! IsBoundaryNode (*node);
        auto area = bounds.reduced (16.0f, 12.0f).withTrimmedRight (22.0f);
        const float iconSize = juce::jlimit (24.0f, 48.0f, juce::jmin (area.getWidth(), area.getHeight()) * 0.3f);
        area.removeFromTop (juce::jmax (0.0f, area.getHeight() * 0.08f));
        const auto icon = IsBoundaryNode (*node) ? juce::String (node->id == kInputNodeId ? "guitar" : "output")
                                                 : juce::String (context.presentation.IconFor (node->type, category));
        const auto onDark = juce::Colours::white.withAlpha (bypassed ? 0.45f : 0.92f);
        context.icons->draw (g, icon, area.removeFromTop (iconSize).withSizeKeepingCentre (iconSize, iconSize), onDark);

        area.removeFromTop (10.0f);
        g.setColour (onDark);
        g.setFont (context.font (juce::jmin (NanoTheme::textHeading, area.getHeight() * 0.3f), FontWeight::semibold));
        g.drawFittedText (juce::String::fromUTF8 (NodeDisplayName (state, *node).c_str()),
                          area.removeFromTop (juce::jmin (48.0f, area.getHeight() * 0.5f)).toNearestInt(),
                          juce::Justification::centredTop, 2);

        const auto* type = state.FindEffectType (node->type);
        juce::String detail = bypassed ? "Bypassed" : juce::String::fromUTF8 (type != nullptr ? type->name.c_str() : "");

        if (processingUs >= 0.0)
            detail << (detail.isEmpty() ? juce::String() : juce::String::fromUTF8 ("  \xc2\xb7  ")) << juce::String (processingUs, 0) << juce::String::fromUTF8 (" \xc2\xb5s");

        g.setColour (juce::Colours::white.withAlpha (0.6f));
        g.setFont (context.font (NanoTheme::textCaption));
        g.drawFittedText (detail, area.toNearestInt(), juce::Justification::centredTop, 1);
    }

private:
    NanoContext& context;
    std::optional<guitarfx::GraphNode> node;
    LevelMeter meter;
    double processingUs = -1.0;
};

// ── The controls: one ParamControl per parameter, with group headings ──────────

class EffectPage::Controls final : public juce::Component
{
public:
    explicit Controls (NanoContext& contextIn) : context (contextIn) {}

    void build (const std::vector<EffectParamInfo>& params, bool sliders,
                std::function<void (const std::string&, double)> onChange)
    {
        controls.clear();
        headings.clear();
        order.clear();
        useSliders = sliders;
        std::string group;

        for (const auto& param : params)
        {
            if (param.group != group && ! param.group.empty())
            {
                auto heading = std::make_unique<juce::Label>();
                heading->setText (juce::String::fromUTF8 (param.group.c_str()).toUpperCase(), juce::dontSendNotification);
                heading->setFont (context.font (NanoTheme::textOverline, FontWeight::semibold).withExtraKerningFactor (0.06f));
                heading->setColour (juce::Label::textColourId, context.theme.textMuted());
                addAndMakeVisible (*heading);
                order.push_back ({ heading.get(), true });
                headings.push_back (std::move (heading));
            }

            group = param.group;
            auto control = std::make_unique<ParamControl> (context, param, sliders ? ParamControl::Style::SliderRow
                                                                                   : ParamControl::Style::Knob);
            const auto key = param.key;
            control->onChange = [onChange, key] (double value) { onChange (key, value); };
            addAndMakeVisible (*control);
            order.push_back ({ control.get(), false });
            controls.push_back (std::move (control));
        }
    }

    void setValues (const guitarfx::GraphNode& node)
    {
        for (auto& control : controls)
            control->setValue (NodeParamValue (node, control->getInfo()));
    }

    int layoutFor (int width, bool apply)
    {
        int y = 4;

        if (useSliders)
        {
            for (auto& [component, heading] : order)
            {
                const int h = heading ? 22 : static_cast<ParamControl*> (component)->preferredHeight (width);

                if (apply)
                    component->setBounds (8, y, width - 16, h);

                y += h;
            }

            return y + 8;
        }

        // Knobs flow in rows; a group heading starts a new row.
        const int cellWidth = context.touch ? 84 : 74;
        const int columns = juce::jmax (1, (width - 8) / cellWidth);
        const int cell = (width - 8) / columns;
        int column = 0;
        int rowHeight = 0;

        for (auto& [component, heading] : order)
        {
            if (heading)
            {
                if (column > 0)
                {
                    y += rowHeight;
                    column = 0;
                }

                if (apply)
                    component->setBounds (8, y + 4, width - 16, 18);

                y += 24;
                rowHeight = 0;
                continue;
            }

            auto* control = static_cast<ParamControl*> (component);
            const int h = control->preferredHeight (cell);

            if (apply)
                control->setBounds (4 + column * cell, y, cell, h);

            rowHeight = juce::jmax (rowHeight, h);

            if (++column == columns)
            {
                column = 0;
                y += rowHeight + 4;
                rowHeight = 0;
            }
        }

        return y + rowHeight + 8;
    }

    [[nodiscard]] bool isEmpty() const noexcept { return controls.empty(); }

private:
    NanoContext& context;
    bool useSliders = false;
    std::vector<std::unique_ptr<ParamControl>> controls;
    std::vector<std::unique_ptr<juce::Label>> headings;
    std::vector<std::pair<juce::Component*, bool>> order;
};

// ── The page ───────────────────────────────────────────────────────────────────

EffectPage::EffectPage (NanoContext& contextIn, ShellActions& actionsIn)
    : context (contextIn),
      actions (actionsIn),
      presetsButton (contextIn, "effect-presets", {}, "Presets"),
      menuButton (contextIn, "effect-menu", {}, juce::String::fromUTF8 ("\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2")),
      mainTab (contextIn, "effect-tab-main", {}, "Main"),
      advancedTab (contextIn, "effect-tab-advanced", {}, "Advanced"),
      visual (std::make_unique<Visual> (contextIn)),
      controls (std::make_unique<Controls> (contextIn))
{
    setComponentID ("effect-page");
    titleLabel.setFont (context.font (16.0f, FontWeight::semibold));
    titleLabel.setComponentID ("effect-title");
    subtitleLabel.setFont (context.font (NanoTheme::textCaption));
    subtitleLabel.setColour (juce::Label::textColourId, context.theme.textMuted());
    bypassSwitch.setComponentID ("effect-bypass");
    bypassSwitch.setTooltip ("On / bypassed");
    menuButton.setFlat (true);
    mainTab.setFlat (true);
    advancedTab.setFlat (true);

    for (auto* component : std::initializer_list<juce::Component*> { &titleLabel, &subtitleLabel, &bypassSwitch, &presetsButton,
                                                                      &menuButton, &mainTab, &advancedTab, visual.get(), &controlsViewport })
        addAndMakeVisible (component);

    controlsViewport.setViewedComponent (controls.get(), false);
    controlsViewport.setScrollBarsShown (true, false);
    controlsViewport.setScrollBarThickness (6);
    controlsViewport.setScrollOnDragMode (juce::Viewport::ScrollOnDragMode::nonHover);

    bypassSwitch.onClick = [this] {
        if (const auto* node = selectedNode())
            context.commands.SetNodeBypassed (node->id, ! bypassSwitch.getToggleState());
    };

    presetsButton.onClick = [this] { showPresetMenu(); };
    menuButton.onClick = [this] {
        if (const auto* node = selectedNode(); node != nullptr && actions.openNodeMenu)
            actions.openNodeMenu (node->id, &menuButton);
    };

    mainTab.setClickingTogglesState (false);
    advancedTab.setClickingTogglesState (false);
    mainTab.onClick = [this] { showAdvanced = false; builtForNodeId.clear(); refresh(); };
    advancedTab.onClick = [this] { showAdvanced = true; builtForNodeId.clear(); refresh(); };

    presetSubscription = context.client.Subscribe (Topic::ActivePreset, [this] { refresh(); });
    catalogSubscription = context.client.Subscribe (Topic::Catalog, [this] { builtForNodeId.clear(); refresh(); });
    resourceSubscription = context.client.Subscribe (Topic::Resources, [this] { refresh(); });
    refresh();
}

EffectPage::~EffectPage() = default;

void EffectPage::setUseSliderList (bool useSliders)
{
    if (sliderList != useSliders)
    {
        sliderList = useSliders;
        builtForNodeId.clear();
        refresh();
    }
}

const guitarfx::GraphNode* EffectPage::selectedNode() const
{
    const auto& state = context.state();
    const auto id = actions.selectedNode ? actions.selectedNode() : std::string {};

    if (! state.activePreset || id.empty())
        return nullptr;

    return state.activePreset->graph.FindNode (id);
}

void EffectPage::refresh()
{
    const auto* node = selectedNode();
    const auto& state = context.state();
    visual->setNode (node);

    const bool hasNode = node != nullptr;

    for (auto* component : std::initializer_list<juce::Component*> { &subtitleLabel, &bypassSwitch, &presetsButton, &menuButton,
                                                                      &controlsViewport })
        component->setVisible (hasNode);

    visual->setVisible (hasNode && visualFits);

    if (! hasNode)
    {
        titleLabel.setText ("Choose an effect in the chain", juce::dontSendNotification);
        mainTab.setVisible (false);
        advancedTab.setVisible (false);
        builtForNodeId.clear();
        return;
    }

    const bool boundary = IsBoundaryNode (*node);
    titleLabel.setText (juce::String::fromUTF8 (NodeDisplayName (state, *node).c_str()), juce::dontSendNotification);
    const auto* type = state.FindEffectType (node->type);
    juce::String subtitle = boundary ? juce::String ("Level trim")
                                     : juce::String::fromUTF8 (type != nullptr ? type->name.c_str() : node->type.c_str());

    // Nothing under the name when it would only repeat it (an effect with no model loaded).
    if (subtitle == titleLabel.getText())
        subtitle.clear();

    if (const auto badge = NodeArchitectureBadge (state, *node); ! badge.empty())
        subtitle << (subtitle.isEmpty() ? juce::String() : juce::String::fromUTF8 ("  \xc2\xb7  ")) << "NAM " << juce::String (badge);

    subtitleLabel.setText (subtitle, juce::dontSendNotification);
    bypassSwitch.setVisible (! boundary);
    bypassSwitch.setToggleState (node->enabled, juce::dontSendNotification);
    bypassSwitch.setButtonText (node->enabled ? "On" : "Off");

    const bool hasPresets = type != nullptr && (! type->presets.empty() || state.customEffectPresets.count (node->type) > 0);
    presetsButton.setVisible (! boundary && type != nullptr);
    presetsButton.setEnabled (true);
    juce::ignoreUnused (hasPresets);
    menuButton.setVisible (! boundary);

    if (node->id != builtForNodeId || node->type != builtForType)
        rebuildControls();

    syncValues();
}

void EffectPage::rebuildControls()
{
    const auto* node = selectedNode();

    if (node == nullptr)
        return;

    builtForNodeId = node->id;
    builtForType = node->type;

    std::vector<EffectParamInfo> params;
    bool anyAdvanced = false;

    if (IsBoundaryNode (*node))
    {
        params.push_back (boundaryTrim());
    }
    else if (const auto* type = context.state().FindEffectType (node->type))
    {
        for (const auto& param : type->parameters)
        {
            anyAdvanced = anyAdvanced || param.advanced;

            if (param.advanced == showAdvanced)
                params.push_back (param);
        }
    }

    if (! anyAdvanced)
        showAdvanced = false;

    mainTab.setVisible (anyAdvanced);
    advancedTab.setVisible (anyAdvanced);
    mainTab.setToggleState (! showAdvanced, juce::dontSendNotification);
    advancedTab.setToggleState (showAdvanced, juce::dontSendNotification);

    const auto nodeId = node->id;
    controls->build (params, sliderList, [this, nodeId] (const std::string& key, double value) {
        context.commands.SetNodeParam (nodeId, key, value);
    });

    resized();
}

void EffectPage::syncValues()
{
    if (const auto* node = selectedNode())
        controls->setValues (*node);
}

void EffectPage::updateMeters (double nowSeconds)
{
    visual->update (nowSeconds);
}

void EffectPage::showPresetMenu()
{
    const auto* node = selectedNode();
    const auto* type = node != nullptr ? context.state().FindEffectType (node->type) : nullptr;

    if (type == nullptr)
        return;

    juce::PopupMenu menu;
    const auto nodeId = node->id;

    for (const auto& preset : type->presets)
        menu.addItem (juce::String::fromUTF8 (preset.name.c_str()),
                      [this, nodeId, preset] { context.commands.ApplyEffectPreset (nodeId, preset); });

    const auto custom = context.state().customEffectPresets.find (node->type);

    if (custom != context.state().customEffectPresets.end() && ! custom->second.empty())
    {
        menu.addSeparator();
        menu.addSectionHeader ("Yours");

        for (const auto& preset : custom->second)
            menu.addItem (juce::String::fromUTF8 (preset.name.c_str()),
                          [this, nodeId, preset] { context.commands.ApplyEffectPreset (nodeId, preset); });
    }

    menu.addSeparator();
    menu.addItem ("Save these settings...", [this, nodeId] {
        if (actions.promptText)
            actions.promptText ("Save effect preset", {}, [this, nodeId] (const juce::String& name) {
                context.commands.SaveEffectPreset (nodeId, name.trim().toStdString());
            });
    });

    context.showMenu (menu, &presetsButton);
}

void EffectPage::paint (juce::Graphics& g)
{
    if (selectedNode() == nullptr)
    {
        g.setColour (context.theme.textMuted());
        g.setFont (context.font (14.0f));
        g.drawFittedText ("Tap an effect in the chain above to see its controls.",
                          getLocalBounds().reduced (24).withTrimmedTop (48), juce::Justification::centredTop, 3);
    }
}

void EffectPage::resized()
{
    auto area = getLocalBounds().reduced (8, 4);
    const int buttonHeight = context.touch ? 40 : 32;
    auto header = area.removeFromTop (buttonHeight + 8);

    menuButton.setBounds (header.removeFromRight (buttonHeight).reduced (0, 4));

    if (presetsButton.isVisible())
        presetsButton.setBounds (header.removeFromRight (88).reduced (2, 4));

    if (bypassSwitch.isVisible())
        bypassSwitch.setBounds (header.removeFromRight (84).reduced (2, 4));

    // A short page (a 360-480 px tall landscape screen) puts the Main/Advanced tabs in the
    // header row, when there is room beside the name, rather than on a row of their own.
    constexpr int tabWidth = 90;
    const bool shortPage = getHeight() < 300;

    if (mainTab.isVisible() && shortPage && header.getWidth() >= 2 * tabWidth + 140)
    {
        auto tabs = header.removeFromRight (2 * tabWidth).withSizeKeepingCentre (2 * tabWidth, buttonHeight);
        mainTab.setBounds (tabs.removeFromLeft (tabWidth).reduced (2));
        advancedTab.setBounds (tabs.reduced (2));
    }
    else if (mainTab.isVisible())
    {
        auto tabs = area.removeFromTop (buttonHeight);
        mainTab.setBounds (tabs.removeFromLeft (100).reduced (2));
        advancedTab.setBounds (tabs.removeFromLeft (100).reduced (2));
        area.removeFromTop (4);
    }

    titleLabel.setBounds (header.removeFromTop (header.getHeight() * 3 / 5));
    subtitleLabel.setBounds (header);

    // Landscape: the visual beside the controls. Portrait (slider rows): above them. Too
    // short to show anything useful, it gives its room to the controls; the chain strip
    // above still shows which effect this is.
    const bool roomForVisual = sliderList ? area.getHeight() >= 320 : area.getHeight() >= 170;
    visualFits = roomForVisual;
    visual->setVisible (roomForVisual && selectedNode() != nullptr);

    if (roomForVisual && sliderList)
    {
        visual->setBounds (area.removeFromTop (juce::jlimit (90, 170, area.getHeight() / 3)));
        area.removeFromTop (6);
    }
    else if (roomForVisual)
    {
        visual->setBounds (area.removeFromLeft (juce::jlimit (140, 260, area.getWidth() / 3)));
        area.removeFromLeft (8);
    }

    controlsViewport.setBounds (area);
    const int width = area.getWidth() - (controlsViewport.isVerticalScrollBarShown() ? 6 : 0);
    const int height = controls->layoutFor (width, false);
    controls->setSize (width, height);
    controls->layoutFor (width, true);
}
} // namespace soundshed::nano
