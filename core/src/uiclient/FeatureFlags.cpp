#include "uiclient/FeatureFlags.h"

#include <string>

namespace guitarfx::uiclient
{
const std::vector<Feature>& AllFeatures()
{
    static const std::vector<Feature> all = {Feature::FactoryPresetPacks,
                                             Feature::Tone3000,
                                             Feature::ResourceLibrary,
                                             Feature::RiffLibrary,
                                             Feature::ToneSharing,
                                             Feature::ToneBuilder,
                                             Feature::Jam,
                                             Feature::PracticeTool,
                                             Feature::ExperimentalEffects,
                                             Feature::PickupMatch,
                                             Feature::CustomEffects,
                                             Feature::MultiRig,
                                             Feature::CompositeEffects,
                                             Feature::BlendTools,
                                             Feature::EffectLayouts,
                                             Feature::ResourceCleanup,
                                             Feature::FactoryPresetArchives,
                                             Feature::DebugStateCapture,
                                             Feature::EffectsPerformancePads,
                                             Feature::Looper};
    return all;
}

const char* FeatureLabel(Feature feature)
{
    switch (feature)
    {
    case Feature::FactoryPresetPacks:
        return "Factory Preset Packs";
    case Feature::Tone3000:
        return "Tone3000 Integration";
    case Feature::ResourceLibrary:
        return "Resource Library";
    case Feature::RiffLibrary:
        return "Riff Library";
    case Feature::ToneSharing:
        return "Tone Sharing";
    case Feature::ToneBuilder:
        return "AI Tone Builder";
    case Feature::Jam:
        return "Jam Panel";
    case Feature::PracticeTool:
        return "Practice Tool";
    case Feature::ExperimentalEffects:
        return "Experimental Effects";
    case Feature::PickupMatch:
        return "Pickup Match";
    case Feature::CustomEffects:
        return "Custom Effects";
    case Feature::MultiRig:
        return "Multi-Rig Mixer";
    case Feature::CompositeEffects:
        return "Composite Effects";
    case Feature::BlendTools:
        return "Blend Tools";
    case Feature::EffectLayouts:
        return "Effect Layout Editor";
    case Feature::ResourceCleanup:
        return "Resource Cleanup Tools";
    case Feature::FactoryPresetArchives:
        return "Factory Preset Archive Tools";
    case Feature::DebugStateCapture:
        return "Capture Debug State";
    case Feature::EffectsPerformancePads:
        return "Effects Performance Pads";
    case Feature::Looper:
        return "Looper";
    }

    return "";
}

const char* FeatureDescription(Feature feature)
{
    switch (feature)
    {
    case Feature::FactoryPresetPacks:
        return "Shows factory presets in the preset chooser. Switch off to hide them without deleting packs or changing the preset currently playing.";
    case Feature::Tone3000:
        return "Shows Tone3000 browsing and account controls.";
    case Feature::ResourceLibrary:
        return "Shows the local model and IR library management tools.";
    case Feature::RiffLibrary:
        return "Shows riff capture, import, and take management workflows.";
    case Feature::ToneSharing:
        return "Shows the tone-sharing panel and publishing workflows.";
    case Feature::ToneBuilder:
        return "Adds a Tone Builder button to Tone Sharing: it checks the community presets first, then researches a tone, picks NAM captures and IRs, and builds ready-to-play presets, which are published on Tone Sharing as Soundshed for everyone.";
    case Feature::Jam:
        return "Shows backing-track search and the floating jam player.";
    case Feature::PracticeTool:
        return "Shows the Jam panel's local backing-track player with tempo/pitch control and named loop sections.";
    case Feature::ExperimentalEffects:
        return "Shows experimental work-in-progress effects in the FX library. These effects may change or be removed over time.";
    case Feature::PickupMatch:
        return "Experimental: makes your guitar's pickup sound like another, a single coil like a humbucker or the other way round, ahead of every preset. Adds a Pickup Match tab to the input meter's flyout (the chevron under the meter). While this is off, Pickup Match is off too.";
    case Feature::CustomEffects:
        return "Enables the Custom Effect generator, saved custom-effect entries, and node-level designer actions.";
    case Feature::MultiRig:
        return "Run two or more presets at once, balance them in the signal path's Mix tab, and save the combination as a Multi-Rig preset. Adds the Add to Mixer action and the Multi-Rig tab in the preset library.";
    case Feature::CompositeEffects:
        return "Shows composite effect authoring in the Library panel.";
    case Feature::BlendTools:
        return "Enables tone-group blend creation and the blend manager.";
    case Feature::EffectLayouts:
        return "Shows custom effect layout editing controls and the layout manager.";
    case Feature::ResourceCleanup:
        return "Enables bulk cleanup of unused imported NAM and IR resources.";
    case Feature::FactoryPresetArchives:
        return "Enables factory preset archive authoring controls used during development.";
    case Feature::DebugStateCapture:
        return "Shows a footer action that writes the current UI and backend debug snapshot for agent inspection.";
    case Feature::EffectsPerformancePads:
        return "Enables experimental Performance Pad assignments for toggling individual effects. These controls may change or be removed over time.";
    case Feature::Looper:
        return "Standalone: a phrase looper docked above the footer. Record a loop, layer overdubs on it, and mix, mute or remove each layer. Each layer keeps the tone it was played with. Adds a Loop button to the footer.";
    }

    return "";
}

const char* FeatureSettingKey(Feature feature)
{
    switch (feature)
    {
    case Feature::FactoryPresetPacks:
        return "features.factoryPresetPacks.enabled";
    case Feature::Tone3000:
        return "features.tone3000.enabled";
    case Feature::ResourceLibrary:
        return "features.resourceLibrary.enabled";
    case Feature::RiffLibrary:
        return "features.riffLibrary.enabled";
    case Feature::ToneSharing:
        return "features.toneSharing.enabled";
    case Feature::Jam:
        return "features.jam.enabled";
    case Feature::PracticeTool:
        return "features.practiceTool.enabled";
    case Feature::MultiRig:
        return "features.multiRig.enabled";
    case Feature::ToneBuilder:
        return "features.toneBuilder.enabled";
    case Feature::ExperimentalEffects:
        return "features.experimentalEffects.enabled";
    case Feature::PickupMatch:
        return "features.pickupMatch.enabled";
    case Feature::CustomEffects:
        return "features.customEffects.enabled";
    case Feature::CompositeEffects:
        return "features.compositeEffects.enabled";
    case Feature::BlendTools:
        return "features.blendTools.enabled";
    case Feature::EffectLayouts:
        return "features.effectLayouts.enabled";
    case Feature::ResourceCleanup:
        return "features.resourceCleanup.enabled";
    case Feature::FactoryPresetArchives:
        return "features.factoryPresetArchives.enabled";
    case Feature::DebugStateCapture:
        return "features.debugStateCapture.enabled";
    case Feature::EffectsPerformancePads:
        return "features.effectsPerformancePads.enabled";
    case Feature::Looper:
        return "features.looper.enabled";
    }

    return "";
}

bool FeatureDefault(Feature feature)
{
    switch (feature)
    {
    case Feature::FactoryPresetPacks:
    case Feature::Tone3000:
    case Feature::ResourceLibrary:
    case Feature::RiffLibrary:
    case Feature::ToneSharing:
    case Feature::Jam:
    case Feature::PracticeTool:
    case Feature::MultiRig:
        return true;
    default:
        return false;
    }
}

bool IsFeatureEnabled(const ClientState& state, Feature feature)
{
    const auto it = state.appSettings.find(FeatureSettingKey(feature));

    if (it == state.appSettings.end() || !it->is_boolean())
    {
        return FeatureDefault(feature);
    }

    return it->get<bool>();
}
} // namespace guitarfx::uiclient
