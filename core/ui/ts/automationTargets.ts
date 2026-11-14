/**
 * The catalogue of addresses the MIDI panel's target picker offers a custom slot, and the
 * label a slot gets from its address. Split from automationPanel.ts, which renders the panel.
 */

import { bypassTargetLabel } from "./midiMapping.js";
import type { AutomationTargetOption } from "./midiMapping.js";
import { BUILTIN_EFFECTS, EffectTypeRegistry } from "./presetV2.js";
import type { AutomationRegistryEntry } from "./types.js";

/** Every address a custom slot can point at, grouped for the picker's optgroups. */
export function buildTargetOptions(registry: AutomationRegistryEntry[]): AutomationTargetOption[] {
  const opts: AutomationTargetOption[] = [];

  for (const e of registry) {
    let group = "Other";
    if (e.address.startsWith("global.")) group = "Global";
    else if (e.address.startsWith("setlist.")) group = "Setlist";
    opts.push({ address: e.address, label: e.label, group });
  }

  // The chain's Input and Output nodes are hidden from the effect catalog, but their trim
  // is a node address like any other and "MIDI Learn…" on the knob points a slot at it —
  // which the picker has to be able to show, or editing that slot's target would silently
  // retarget it. Their gain is all they have; there is no bypassing a boundary node.
  for (const routing of BUILTIN_EFFECTS) {
    for (const param of routing.parameters) {
      opts.push({
        address: `node.${routing.type}.${param.key}`,
        label: `${routing.displayName || routing.type}: ${param.name || param.key}`,
        group: "Signal Chain",
      });
    }
  }

  const effects = EffectTypeRegistry.getAll()
    .filter((t) => t.catalogHidden !== true && t.parameters.length > 0);

  effects.sort((a, b) => {
    const c = (a.category || "").localeCompare(b.category || "");
    return c !== 0 ? c : (a.displayName || a.type).localeCompare(b.displayName || b.type);
  });

  for (const eff of effects) {
    const group = eff.category || "Effects";
    const effLabel = eff.displayName || eff.type;
    // Both senses of on/off: "enabled" turns the effect on with 127, "bypassed" turns it off.
    opts.push({ address: `node.${eff.type}.enabled`, label: bypassTargetLabel(effLabel, "enabled"), group });
    opts.push({ address: `node.${eff.type}.bypassed`, label: bypassTargetLabel(effLabel, "bypassed"), group });
    for (const p of eff.parameters) {
      const address = `node.${eff.type}.${p.key}`;
      const paramLabel = p.name || p.key;
      opts.push({ address, label: `${effLabel}: ${paramLabel}`, group });
    }
  }

  return opts;
}

/** The label a slot pointed at `address` gets when the user gives it none. */
export function deriveLabel(address: string, registry: AutomationRegistryEntry[]): string {
  const entry = registry.find((e) => e.address === address);
  if (entry) return entry.label;

  const nodeMatch = address.match(/^node\.([^.]+)\.(.+)$/);
  if (nodeMatch) {
    const eff = EffectTypeRegistry.get(nodeMatch[1]);
    const effName = eff?.displayName || nodeMatch[1];
    if (nodeMatch[2] === "bypassed" || nodeMatch[2] === "bypass" || nodeMatch[2] === "enabled") {
      return bypassTargetLabel(effName, nodeMatch[2]);
    }
    const param = eff?.parameters.find((p) => p.key === nodeMatch[2]);
    const paramName = param?.name || nodeMatch[2];
    return `${effName}: ${paramName}`;
  }
  return "Custom";
}
