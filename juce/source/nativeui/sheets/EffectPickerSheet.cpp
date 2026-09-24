#include "nativeui/sheets/EffectPickerSheet.h"

#include "dsp/EffectGuids.h"

#include <algorithm>
#include <set>

namespace soundshed::nano
{
using guitarfx::uiclient::EffectTypeInfo;

namespace
{
bool featureEnabled (const NanoContext& context, const char* feature)
{
    const auto& settings = context.state().appSettings;
    const auto key = std::string ("features.") + feature + ".enabled";
    return settings.contains (key) && settings[key].is_boolean() && settings[key].get<bool>();
}

std::string lower (std::string text)
{
    std::transform (text.begin(), text.end(), text.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
    return text;
}
} // namespace

std::vector<const EffectTypeInfo*> EffectPickerContent::offeredEffects (const NanoContext& context)
{
    namespace guids = guitarfx::EffectGuids;
    const std::set<std::string> experimental { guids::kTransposeStft, guids::kTransposeHybrid, guids::kGuitarToMidi };
    const bool showExperimental = featureEnabled (context, "experimentalEffects");
    const bool showCustom = featureEnabled (context, "customEffects");
    std::vector<const EffectTypeInfo*> offered;

    for (const auto& info : context.state().catalog)
    {
        // As core/ui/ts/fxSelector.ts's getCatalogEffects. The plugin host and composites need
        // pickers of their own, which live in Soundshed Guitar.
        if (info.type == guids::kMixer || info.type == guids::kAmpNamBlend || info.type == guids::kPluginHost
            || info.type.rfind ("composite:", 0) == 0)
            continue;

        if (info.type == guids::kWasmHost && ! showCustom)
            continue;

        if (experimental.count (info.type) > 0 && ! showExperimental)
            continue;

        offered.push_back (&info);
    }

    return offered;
}

EffectPickerContent::EffectPickerContent (NanoContext& contextIn, std::string afterNodeIdIn,
                                          std::function<void (const std::string&)> onPickIn)
    : context (contextIn), afterNodeId (std::move (afterNodeIdIn)), onPick (std::move (onPickIn))
{
    setComponentID ("effect-picker");
    search.setComponentID ("effect-search");
    search.setTextToShowWhenEmpty ("Search effects", context.theme.textMuted());
    search.setFont (context.font (NanoTheme::textBody));
    search.setIndents (12, 0);
    search.setJustification (juce::Justification::centredLeft);
    search.onTextChange = [this] { rebuild(); };
    addAndMakeVisible (search);

    // Categories that have something in them, in the shared table's order.
    const auto offered = offeredEffects (context);
    std::vector<std::string> categories;

    for (const auto& id : context.presentation.CategoryOrder())
        if (std::any_of (offered.begin(), offered.end(), [&id] (const EffectTypeInfo* e) { return e->category == id; }))
            categories.push_back (id);

    for (const auto* effect : offered)
        if (std::find (categories.begin(), categories.end(), effect->category) == categories.end())
            categories.push_back (effect->category);

    for (const auto& id : categories)
    {
        const auto* look = context.presentation.Category (id);
        auto chip = std::make_unique<IconButton> (context, juce::String ("fx-category:" + id), juce::String (context.presentation.CategoryIcon (id)),
                                                  look != nullptr ? juce::String::fromUTF8 (look->name.c_str()) : juce::String (id));
        chip->setClickingTogglesState (false);
        chip->onClick = [this, id] {
            category = id;
            search.setText ({}, false);
            rebuild();
        };
        chipStrip.addAndMakeVisible (*chip);
        categoryChips.push_back (std::move (chip));
    }

    if (! categories.empty())
        category = categories.front();

    chipViewport.setViewedComponent (&chipStrip, false);
    chipViewport.setScrollBarsShown (false, false, false, true);
    chipViewport.setScrollOnDragMode (juce::Viewport::ScrollOnDragMode::all);
    addAndMakeVisible (chipViewport);

    list.setRowHeight (context.touch ? 52 : 40);
    list.rowCount = [this] { return (int) shown.size(); };
    list.paintRow = [this] (juce::Graphics& g, int row, juce::Rectangle<int> bounds, bool pressed) {
        if (row < 0 || row >= (int) shown.size())
            return;

        const auto& theme = context.theme;
        const auto* effect = shown[(std::size_t) row];
        auto area = bounds.reduced (4, 2);

        if (pressed)
        {
            g.setColour (theme.pressedFill());
            g.fillRoundedRectangle (area.toFloat(), (float) NanoTheme::controlRadius);
        }

        area.reduce (10, 0);
        const auto colour = juce::Colour (context.presentation.CategoryColour (effect->category));
        context.icons->draw (g, context.presentation.IconFor (effect->type, effect->category),
                             area.removeFromLeft (28).toFloat().withSizeKeepingCentre (NanoTheme::iconSize, NanoTheme::iconSize),
                             colour.interpolatedWith (theme.text(), 0.25f));
        area.removeFromLeft (8);
        g.setColour (theme.text());
        g.setFont (context.font (NanoTheme::textBody + 1.0f));
        g.drawFittedText (juce::String::fromUTF8 (effect->name.c_str()), area, juce::Justification::centredLeft, 1);
    };
    list.rowName = [this] (int row) { return "fx:" + juce::String::fromUTF8 (shown[(std::size_t) row]->name.c_str()); };
    list.onTap = [this] (int row) {
        if (row >= 0 && row < (int) shown.size() && onPick)
            onPick (shown[(std::size_t) row]->type);
    };
    addAndMakeVisible (list);
    rebuild();
}

EffectPickerContent::~EffectPickerContent() = default;

void EffectPickerContent::rebuild()
{
    const auto text = lower (search.getText().trim().toStdString());
    shown.clear();

    for (const auto* effect : offeredEffects (context))
    {
        const bool matches = text.empty() ? effect->category == category
                                          : lower (effect->name).find (text) != std::string::npos
                                                || lower (effect->category).find (text) != std::string::npos;

        if (matches)
            shown.push_back (effect);
    }

    for (auto& chip : categoryChips)
        chip->setToggleState (text.empty() && chip->getComponentID() == juce::String ("fx-category:" + category),
                              juce::dontSendNotification);

    list.updateContent();
}

void EffectPickerContent::resized()
{
    auto area = getLocalBounds();
    const int row = context.touch ? 44 : 34;
    search.setBounds (area.removeFromTop (row).reduced (2));
    area.removeFromTop (6);

    // The categories wrap onto a second row when that fits them all; otherwise (a phone) they
    // stay on one row that scrolls sideways.
    constexpr int chipGap = 4;
    const int chipWidth = context.touch ? 128 : 120;
    const int perRow = juce::jmax (1, (area.getWidth() + chipGap) / (chipWidth + chipGap));
    const int count = (int) categoryChips.size();
    const bool wrap = count <= perRow * 2;
    const int chipRows = wrap && count > perRow ? 2 : 1;

    auto chips = area.removeFromTop (chipRows * row + (chipRows - 1) * chipGap);
    chipViewport.setBounds (chips);
    int x = 0;
    int y = 0;

    for (auto& chip : categoryChips)
    {
        if (wrap && x > 0 && x + chipWidth > chips.getWidth())
        {
            x = 0;
            y += row + chipGap;
        }

        chip->setBounds (x, y, chipWidth, row);
        x += chipWidth + chipGap;
    }

    chipStrip.setSize (wrap ? chips.getWidth() : juce::jmax (x, chips.getWidth()), chips.getHeight());
    area.removeFromTop (6);
    list.setBounds (area);
}
} // namespace soundshed::nano
