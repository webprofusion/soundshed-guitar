import { uiState } from "./state.js";

export const FEATURE_FLAGS_CHANGED_EVENT = "featureFlagsChanged";

export const Features = {
  Tone3000: "tone3000",
  ResourceLibrary: "resourceLibrary",
  RiffLibrary: "riffLibrary",
  ToneSharing: "toneSharing",
  Jam: "jam",
  CustomEffects: "customEffects",
  MultiRig: "multiRig",
  CompositeEffects: "compositeEffects",
  BlendTools: "blendTools",
  EffectLayout: "effectLayouts",
  ResourceCleanup: "resourceCleanup",
  FactoryPresetArchives: "factoryPresetArchives",
} as const;

export type FeatureId = typeof Features[keyof typeof Features];

export interface FeatureDefinition {
  id: FeatureId;
  key: string;
  label: string;
  description: string;
  defaultEnabled: boolean;
  legacyAdvanced?: boolean;
}

export interface FeatureGroupDefinition {
  id: "core" | "power";
  title: string;
  description: string;
  featureIds: FeatureId[];
}

const LEGACY_ADVANCED_OPTIONS_SETTING = "ui.advancedOptionsEnabled";

export const FEATURE_DEFINITIONS: FeatureDefinition[] = [
  {
    id: Features.Tone3000,
    key: "features.tone3000.enabled",
    label: "Tone3000 Integration",
    description: "Shows Tone3000 browsing and account controls.",
    defaultEnabled: true,
  },
  {
    id: Features.ResourceLibrary,
    key: "features.resourceLibrary.enabled",
    label: "Resource Library",
    description: "Shows the local model and IR library management tools.",
    defaultEnabled: true,
  },
  {
    id: Features.RiffLibrary,
    key: "features.riffLibrary.enabled",
    label: "Riff Library",
    description: "Shows riff capture, import, and take management workflows.",
    defaultEnabled: true,
  },
  {
    id: Features.ToneSharing,
    key: "features.toneSharing.enabled",
    label: "Tone Sharing",
    description: "Shows the tone-sharing panel and publishing workflows.",
    defaultEnabled: true,
  },
  {
    id: Features.Jam,
    key: "features.jam.enabled",
    label: "Jam Panel",
    description: "Shows backing-track search and the floating jam player.",
    defaultEnabled: true,
  },
  {
    id: Features.CustomEffects,
    key: "features.customEffects.enabled",
    label: "Custom Effects",
    description: "Enables the Custom Effect generator, saved custom-effect entries, and node-level designer actions.",
    defaultEnabled: false,
    legacyAdvanced: true,
  },
  {
    id: Features.MultiRig,
    key: "features.multiRig.enabled",
    label: "Multi-Rig Mixer",
    description: "Enables mixer controls, Add to Mixer actions, and Multi-Rig preset management.",
    defaultEnabled: false,
    legacyAdvanced: true,
  },
  {
    id: Features.CompositeEffects,
    key: "features.compositeEffects.enabled",
    label: "Composite Effects",
    description: "Shows composite effect authoring in the Library panel.",
    defaultEnabled: false,
    legacyAdvanced: true,
  },
  {
    id: Features.BlendTools,
    key: "features.blendTools.enabled",
    label: "Blend Tools",
    description: "Enables tone-group blend creation and the blend manager.",
    defaultEnabled: false,
    legacyAdvanced: true,
  },
  {
    id: Features.EffectLayout,
    key: "features.effectLayouts.enabled",
    label: "Effect Layout Editor",
    description: "Shows custom effect layout editing controls and the layout manager.",
    defaultEnabled: false,
    legacyAdvanced: true,
  },
  {
    id: Features.ResourceCleanup,
    key: "features.resourceCleanup.enabled",
    label: "Resource Cleanup Tools",
    description: "Enables bulk cleanup of unused imported NAM and IR resources.",
    defaultEnabled: false,
    legacyAdvanced: true,
  },
  {
    id: Features.FactoryPresetArchives,
    key: "features.factoryPresetArchives.enabled",
    label: "Factory Preset Archive Tools",
    description: "Enables factory preset archive authoring controls used during development.",
    defaultEnabled: false,
    legacyAdvanced: true,
  },
];

export const FEATURE_GROUPS: FeatureGroupDefinition[] = [
  {
    id: "core",
    title: "Core Features",
    description: "Primary user-facing workflows stay enabled by default.",
    featureIds: [Features.Tone3000, Features.ResourceLibrary, Features.RiffLibrary, Features.ToneSharing, Features.Jam],
  },
  {
    id: "power",
    title: "Power Features",
    description: "These replace the old Advanced Options toggle and default to off unless you previously enabled it.",
    featureIds: [Features.CustomEffects, Features.MultiRig, Features.CompositeEffects, Features.BlendTools, Features.EffectLayout, Features.ResourceCleanup, Features.FactoryPresetArchives],
  },
];

const FEATURE_MAP = new Map<FeatureId, FeatureDefinition>(FEATURE_DEFINITIONS.map((feature) => [feature.id, feature]));
const ADVANCED_LIBRARY_FEATURE_IDS: FeatureId[] = [Features.CompositeEffects, Features.BlendTools, Features.EffectLayout];
const LIBRARY_TAB_FEATURES: FeatureId[] = [Features.Tone3000, Features.ResourceLibrary, ...ADVANCED_LIBRARY_FEATURE_IDS];
const JAM_PANEL_FEATURE_IDS: FeatureId[] = [Features.Jam, Features.RiffLibrary];

function getFeatureDefinition(featureId: FeatureId): FeatureDefinition {
  const definition = FEATURE_MAP.get(featureId);
  if (!definition) {
    throw new Error(`Unknown feature flag: ${featureId}`);
  }
  return definition;
}

export function getFeatureSettingKey(featureId: FeatureId): string {
  return getFeatureDefinition(featureId).key;
}

export function isFeatureEnabled(featureId: FeatureId): boolean {
  const definition = getFeatureDefinition(featureId);
  const storedValue = uiState.appSettings?.[definition.key];
  if (typeof storedValue === "boolean") {
    return storedValue;
  }

  if (definition.legacyAdvanced) {
    const legacyValue = uiState.appSettings?.[LEGACY_ADVANCED_OPTIONS_SETTING];
    if (typeof legacyValue === "boolean") {
      return legacyValue;
    }
  }

  return definition.defaultEnabled;
}

export function areAnyFeaturesEnabled(featureIds: readonly FeatureId[]): boolean {
  return featureIds.some((featureId) => isFeatureEnabled(featureId));
}

export function areAdvancedLibraryFeaturesEnabled(): boolean {
  return areAnyFeaturesEnabled(ADVANCED_LIBRARY_FEATURE_IDS);
}

export function isLibraryExperienceEnabled(): boolean {
  return areAnyFeaturesEnabled(LIBRARY_TAB_FEATURES);
}

export function isJamExperienceEnabled(): boolean {
  return areAnyFeaturesEnabled(JAM_PANEL_FEATURE_IDS);
}
