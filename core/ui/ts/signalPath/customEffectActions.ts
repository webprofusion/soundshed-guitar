import type {
  GraphNode,
} from "../types.js";
import { showPrompt } from "../dialogs.js";
import { showNotification } from "../notifications.js";
import { getNodeEffectInfo } from "../presetV2.js";
import { saveCurrentCustomEffect } from "../customEffects.js";
import { getNodeCategory } from "./nodeTypes.js";
import { getLinkedCustomEffectEntry, getNodeDisplayName, hasCustomEffectModuleSelection } from "./nodeLabels.js";
export async function promptSaveCurrentCustomEffect(node: GraphNode, applyToNode: boolean): Promise<void> {
  if (!hasCustomEffectModuleSelection(node)) {
    showNotification("Custom Effect save failed", "Select a WASM module first");
    return;
  }

  const linkedEntry = getLinkedCustomEffectEntry(node);
  const typeInfo = getNodeEffectInfo(node);

  const suggestedName = linkedEntry?.name
    || getNodeDisplayName(node)
    || typeInfo?.displayName
    || "Custom Effect";
  const rawName = await showPrompt("Name", suggestedName, { title: "Save Custom Effect" });
  if (rawName === null) {
    return;
  }

  const name = rawName.trim();
  if (!name) {
    showNotification("Custom Effect save failed", "A name is required");
    return;
  }

  const suggestedCategory = linkedEntry?.category
    || getNodeCategory(node)
    || typeInfo?.category
    || "utility";
  const rawCategory = await showPrompt("Category", suggestedCategory, { title: "Save Custom Effect" });
  if (rawCategory === null) {
    return;
  }

  const descriptionDefault = linkedEntry?.description ?? typeInfo?.description ?? "";
  const rawDescription = await showPrompt("Description", descriptionDefault, { title: "Save Custom Effect", multiline: true });
  if (rawDescription === null) {
    return;
  }

  saveCurrentCustomEffect(node.id, {
    ...(linkedEntry?.id ? { id: linkedEntry.id } : {}),
    name,
    category: rawCategory.trim() || suggestedCategory,
    description: rawDescription.trim(),
    origin: linkedEntry?.origin ?? "imported",
  }, applyToNode);
}
