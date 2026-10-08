import type {
  GraphNode,
  Preset,
} from "../types.js";
import { EffectTypeRegistry, getNodeEffectInfo } from "../presetV2.js";
import { EffectGuids } from "../effectGuids.js";
import { getNodeCategory } from "./nodeTypes.js";
import { hasCabIrInSameSignalPath } from "./chainRules.js";
export function resolveResourceBrowserTone3000CategoryFilter(
  node: GraphNode,
  preset: Preset,
): "pedal" | "amp" | "full-rig" | undefined {
  const resolvedType = EffectTypeRegistry.resolve(node.type);

  if (resolvedType === EffectGuids.kFxNam) {
    return "pedal";
  }

  if (resolvedType === EffectGuids.kAmpNamOptimized) {
    return hasCabIrInSameSignalPath(node.id, preset) ? "amp" : "full-rig";
  }

  return undefined;
}

/// The library category an IR slot browses: what the engine files its IRs under. "ir" is the
/// resource type, not a category, since reverb IRs are type "ir" too.
export function resolveResourceBrowserLibraryCategoryHint(
  node: GraphNode,
  resourceType: "nam" | "ir",
): "cab" | "reverb" | undefined {
  if (resourceType !== "ir") {
    return undefined;
  }

  const category = getNodeEffectInfo(node)?.category || getNodeCategory(node);
  if (category === "reverb") {
    return "reverb";
  }
  if (category === "cab") {
    return "cab";
  }

  return undefined;
}

export function resolveResourceNavigationCategoryHint(
  node: GraphNode,
  preset: Preset,
  resourceType: "nam" | "ir",
): string | undefined {
  if (resourceType === "nam") {
    return resolveResourceBrowserTone3000CategoryFilter(node, preset);
  }

  return resolveResourceBrowserLibraryCategoryHint(node, resourceType);
}

/// Identifies the kind of node a resource is being picked for, so folder browsing
/// and next/prev navigation are remembered per effect role rather than globally.
/// Deliberately coarser than the category hint: a NAM Amp keeps one remembered
/// folder whether or not it is currently acting as a full rig.
export function resolveResourceContextKey(node: GraphNode, resourceType: "nam" | "ir"): string {
  if (resourceType === "nam") {
    return EffectTypeRegistry.resolve(node.type) === EffectGuids.kFxNam ? "nam-fx" : "nam-amp";
  }

  return resolveResourceBrowserLibraryCategoryHint(node, resourceType) === "reverb"
    ? "ir-reverb"
    : "ir-cab";
}
