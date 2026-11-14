/**
 * MIDI mapping helpers shared by the MIDI & Automation panel and the right-click
 * "MIDI Learn…" menu: which automation address a control drives, which slot
 * already drives it, and how a mapping reads on screen.
 *
 * Nothing here reaches for app state or builds DOM — the caller hands in the
 * element and the lookups it needs — so it stays a leaf module and tests without
 * the app shell.
 */

import { EffectGuids } from "./effectGuids.js";
import type { EffectTypeInfo } from "./presetV2.js";
import type { AutomationMidiMap, AutomationRegistryEntry, AutomationSlot, GraphNode } from "./types.js";

/** An automation address a control drives, and a label for it. */
export interface MidiLearnTarget {
  address: string;
  label: string;
  /**
   * The node the control belongs to, for a `node.*` address. A global mapping names only
   * the effect type, since node ids differ from preset to preset; a per-preset mapping
   * names the node too (`nodeSelectorFor`), so two effects of one type each get their own.
   */
  nodeId?: string;
  /** The label for a per-preset mapping, which drives this node: it names the node. */
  nodeLabel?: string;
}

export interface MidiLearnTargetContext {
  /** The engine's `global.*` and `setlist.*` entries. An address missing from it is not offered. */
  registry: readonly AutomationRegistryEntry[];
  /** A node in the graph the engine is playing; undefined for one a `node.*` address cannot reach. */
  findNode: (nodeId: string) => GraphNode | undefined;
  getEffectInfo: (type: string) => EffectTypeInfo | undefined;
  /** The name the app shows for a node; without it, its title, else its effect's name. */
  getNodeName?: (node: GraphNode) => string;
}

/** The control-bar knobs, by `data-param`, and the global addresses they set. */
const GLOBAL_KNOB_ADDRESSES = new Map<string, string>([
  ["input", "global.inputTrim"],
  ["output", "global.outputTrim"],
]);

const NODE_PARAM_CONTROL_SELECTOR = [".node-param-knob", ".node-param-toggle", ".node-param-slider", ".node-param-blend-slider"]
  .map((selector) => `${selector}[data-node-id][data-param-key]`)
  .join(", ");

/** Wrappers holding a parameter control together with its label and value readout. */
const NODE_PARAM_GROUP_SELECTOR = ".node-param-group, .node-param-blend-group, .custom-layout-control";

/**
 * Effect types whose parameters belong to what a node has loaded — a plugin, a WASM
 * module — rather than to the type. A `node.*` address names only the type, so it
 * cannot say which of those parameters it means.
 */
const PER_NODE_PARAMETER_TYPES = new Set<string>([EffectGuids.kPluginHost, EffectGuids.kWasmHost]);

const TEXT_ENTRY_SELECTOR = "input:not([type='checkbox']):not([type='range']), textarea, select, [contenteditable=''], [contenteditable='true']";

/**
 * The automation address a right-clicked element drives, or null when it drives
 * nothing a MIDI mapping can reach. A text field is always null: cut, copy and
 * paste are the menu that belongs there.
 */
export function resolveMidiLearnTarget(element: Element | null, context: MidiLearnTargetContext): MidiLearnTarget | null {
  if (!element || element.closest(TEXT_ENTRY_SELECTOR)) {
    return null;
  }
  return resolveGlobalKnob(element, context)
    ?? resolveNodeBypass(element, context)
    ?? resolveNodeParam(element, context)
    ?? resolveSetlistControl(element, context);
}

/** The node selector that ties a per-preset mapping to one node: `id:<nodeId>`. */
export function nodeSelectorFor(nodeId: string): string {
  return `id:${nodeId}`;
}

/** The node id an `id:<nodeId>` selector names, or null for any other selector. */
export function nodeIdFromSelector(selector: string | undefined): string | null {
  return selector?.startsWith("id:") ? selector.slice(3) : null;
}

/** Whether an address enables or bypasses effects rather than setting a parameter. */
export function isBypassAddress(address: string): boolean {
  return /^node\.[^.]+\.(bypassed|bypass|enabled)$/.test(address);
}

/**
 * The label for an effect's on/off address. `enabled` turns the effect on with a high
 * value (CC 127 = on), `bypassed` turns it off with one: the two senses a mapping can have.
 */
export function bypassTargetLabel(effectLabel: string, paramId: string): string {
  return paramId === "enabled" ? `${effectLabel}: On/Off` : `${effectLabel}: Bypass`;
}

/**
 * An effect's on/off button — the bypass button on its tile in the signal chain, or the
 * switch in its panel — drives `node.<type>.enabled`, so a footswitch's 127 turns it on.
 */
function resolveNodeBypass(element: Element, context: MidiLearnTargetContext): MidiLearnTarget | null {
  const button = element.closest<HTMLElement>(".signal-node-bypass, .node-bypass-btn[data-node-id]");
  const nodeId = button?.dataset.nodeId ?? button?.closest<HTMLElement>(".signal-node[data-node-id]")?.dataset.nodeId;
  if (!nodeId) {
    return null;
  }

  const node = context.findNode(nodeId);
  const info = node ? context.getEffectInfo(node.type) : undefined;
  if (!node || !info) {
    return null;
  }

  return {
    address: `node.${info.type}.enabled`,
    label: bypassTargetLabel(info.displayName || info.type, "enabled"),
    nodeId,
    nodeLabel: bypassTargetLabel(nodeLabel(node, info, context), "enabled"),
  };
}

/** What the user calls a node: the name the app shows for it, or its title, or its effect's name. */
function nodeLabel(node: GraphNode, info: EffectTypeInfo, context: MidiLearnTargetContext): string {
  return context.getNodeName?.(node) || node.title || node.displayName || info.displayName || info.type;
}

function registryTarget(address: string, context: MidiLearnTargetContext): MidiLearnTarget | null {
  const entry = context.registry.find((candidate) => candidate.address === address);
  return entry ? { address, label: entry.label } : null;
}

function resolveGlobalKnob(element: Element, context: MidiLearnTargetContext): MidiLearnTarget | null {
  const knob = element.closest<HTMLElement>(".knob[data-param]")
    ?? element.closest(".knob-control")?.querySelector<HTMLElement>(".knob[data-param]");
  const address = GLOBAL_KNOB_ADDRESSES.get(knob?.dataset.param ?? "");
  return address ? registryTarget(address, context) : null;
}

function resolveNodeParam(element: Element, context: MidiLearnTargetContext): MidiLearnTarget | null {
  const control = findNodeParamControl(element);
  const nodeId = control?.dataset.nodeId;
  const paramKey = control?.dataset.paramKey;
  if (!nodeId || !paramKey) {
    return null;
  }

  const node = context.findNode(nodeId);
  const info = node ? context.getEffectInfo(node.type) : undefined;
  if (!info || PER_NODE_PARAMETER_TYPES.has(info.type)) {
    return null;
  }

  // Only a parameter the type declares has a range the engine can map a MIDI value onto.
  const param = info.parameters.find((candidate) => candidate.key === paramKey);
  if (!param) {
    return null;
  }

  return {
    address: `node.${info.type}.${paramKey}`,
    label: `${info.displayName || info.type}: ${param.name || paramKey}`,
    nodeId,
    nodeLabel: `${nodeLabel(node!, info, context)}: ${param.name || paramKey}`,
  };
}

function findNodeParamControl(element: Element): HTMLElement | null {
  const direct = element.closest<HTMLElement>(NODE_PARAM_CONTROL_SELECTOR);
  if (direct) {
    return direct;
  }
  // A label, a value readout or a toggle's slider sits beside the control, not inside
  // it. The surrounding group names the control only when it holds just the one.
  const controls = element.closest(NODE_PARAM_GROUP_SELECTOR)?.querySelectorAll<HTMLElement>(NODE_PARAM_CONTROL_SELECTOR);
  return controls?.length === 1 ? controls[0] : null;
}

function resolveSetlistControl(element: Element, context: MidiLearnTargetContext): MidiLearnTarget | null {
  const slot = element.closest<HTMLElement>(".performance-setlist-pad[data-slot-index], .setlist-slot[data-slot-index]");
  if (slot) {
    // setlist.presetN is the Nth slot of whichever setlist is active — the same slot the pad shows.
    const index = Number.parseInt(slot.dataset.slotIndex ?? "", 10);
    return index >= 0 ? registryTarget(`setlist.preset${index + 1}`, context) : null;
  }

  const action = element.closest<HTMLElement>("[data-performance-action]")?.dataset.performanceAction;
  if (action === "bank-up") {
    return registryTarget("setlist.bankUp", context);
  }
  if (action === "bank-down") {
    return registryTarget("setlist.bankDown", context);
  }
  if (element.closest(".performance-bank-current:not(.performance-bank-editor)")) {
    return registryTarget("setlist.bankSelect", context);
  }
  return null;
}

/** An address the automation target picker offers, and the optgroup it sits in. */
export interface AutomationTargetOption {
  address: string;
  label: string;
  group: string;
}

/** The optgroup holding an address the catalog itself does not offer. */
export const CURRENT_TARGET_GROUP = "Current";

/**
 * `options` with the slot's own address guaranteed to be among them. A slot can point
 * at an address the picker never builds — "MIDI Learn…" on a blend node's parameter
 * maps a catalog-hidden effect type — and a <select> with no matching <option> selects
 * its first one instead, so opening that slot's editor and pressing Apply would
 * silently repoint the slot at an unrelated parameter. Carrying the address in an
 * option of its own keeps it both selectable and selected.
 *
 * A slot pointed at nothing (a freshly added one, address "") has nothing to keep, and
 * is left to pick from the catalog as it always has.
 */
export function withCurrentTargetOption(
  options: readonly AutomationTargetOption[],
  address: string,
  label: string,
): AutomationTargetOption[] {
  if (!address || options.some((option) => option.address === address)) {
    return [...options];
  }
  return [{ address, label, group: CURRENT_TARGET_GROUP }, ...options];
}

/**
 * The slot that drives `address` in every preset. A default slot wins over a custom slot
 * pointed at the same address: it is the one the DAW exposes under a stable name.
 */
export function findSlotForAddress(slots: readonly AutomationSlot[], address: string): AutomationSlot | undefined {
  return slots.find((slot) => slot.isDefault && slot.address === address)
    ?? slots.find((slot) => !slot.presetId && slot.address === address);
}

/**
 * Whether a control can have a mapping of its own in each preset: the signal chain's
 * effect parameters and its input and output levels. The setlist pads and bank controls
 * pick the bank and preset rather than shape the preset that is playing, so they are not.
 */
export function allowsPresetMapping(address: string): boolean {
  return address.startsWith("node.") || [...GLOBAL_KNOB_ADDRESSES.values()].includes(address);
}

/**
 * The mapping `presetId` holds for `address` itself, live only while that preset is active.
 * With a `nodeId`, the mapping for that node: one tied to it, else one from before mappings
 * named their node (no selector), which a new learn then ties to it. A mapping tied to
 * another node of the same type is that node's, not this one's.
 */
export function findPresetSlotForAddress(
  slots: readonly AutomationSlot[],
  presetId: string,
  address: string,
  nodeId?: string,
): AutomationSlot | undefined {
  const candidates = slots.filter((slot) => slot.presetId === presetId && slot.address === address);
  if (!nodeId) {
    return candidates[0];
  }
  return candidates.find((slot) => nodeIdFromSelector(slot.nodeSelector) === nodeId)
    ?? candidates.find((slot) => !slot.nodeSelector);
}

export function countCustomSlots(slots: readonly AutomationSlot[]): number {
  return slots.filter((slot) => !slot.isDefault && !slot.presetId).length;
}

export function countPresetSlots(slots: readonly AutomationSlot[], presetId: string): number {
  return slots.filter((slot) => slot.presetId === presetId).length;
}

function nextNumberedSlotId(slots: readonly AutomationSlot[], prefix: string): string {
  const pattern = new RegExp(`^${prefix}\\.(\\d+)$`);
  let highest = 0;
  for (const slot of slots) {
    const match = pattern.exec(slot.slotId);
    if (match) {
      highest = Math.max(highest, Number(match[1]));
    }
  }
  return `${prefix}.${highest + 1}`;
}

/** A `custom.N` id no slot uses yet. */
export function nextCustomSlotId(slots: readonly AutomationSlot[]): string {
  return nextNumberedSlotId(slots, "custom");
}

/** A `preset.N` id no slot uses yet. */
export function nextPresetSlotId(slots: readonly AutomationSlot[]): string {
  return nextNumberedSlotId(slots, "preset");
}

/**
 * MIDI channels are stored 0-15 on the wire (and -1 for "any"), but musicians
 * and hardware label them 1-16 — always display the 1-based number.
 */
export function formatMidiChannel(channel: number): string {
  return channel < 0 ? "any" : String(channel + 1);
}

const MIDI_EVENT_NAMES = ["CC", "PC", "NoteOn", "NoteOff", "PBend"];
const MIDI_MODE_NAMES = ["Abs", "Rel", "Toggle", "Pickup"];

/** `MidiControlMap::Mode::Absolute`: the control's value sets the target (on an on/off, a latch). */
export const MIDI_MODE_ABSOLUTE = 0;
/** `MidiControlMap::Mode::Toggle`: each press switches the target. */
export const MIDI_MODE_TOGGLE = 2;

/**
 * The modes a MIDI mapping can take, in the engine's order (`MidiControlMap::Mode`), with
 * what each does. On an effect's on/off, Absolute is the latch: a CC's value sets the state,
 * so a pedal that sends 127 while held and 0 on release switches the effect only while it is
 * held. Toggle switches it on each press (a value of 64 or more), from the state it is in.
 */
export const MIDI_MODE_OPTIONS: readonly { mode: number; label: string; title: string }[] = [
  { mode: 0, label: "Absolute", title: "The control's value sets the target. On an effect's on/off: 127 one way, 0 the other (latch)." },
  { mode: 2, label: "Toggle", title: "Each press (value 64 or more) switches the target; releases are ignored." },
  { mode: 1, label: "Relative", title: "An endless encoder: values above 64 step up, below 64 step down." },
  { mode: 3, label: "Pickup", title: "The target only follows once the control reaches its current value." },
];

/** "CC 7 ch1 Abs". The mode is left off when there is none, as in a learn capture. */
export function describeMidiMap(map: Pick<AutomationMidiMap, "eventType" | "channel" | "controller"> & { mode?: number }): string {
  const text = `${MIDI_EVENT_NAMES[map.eventType] || "CC"} ${map.controller} ch${formatMidiChannel(map.channel)}`;
  return map.mode === undefined ? text : `${text} ${MIDI_MODE_NAMES[map.mode] || "Abs"}`;
}

export type MidiLearnMenuAction = "learn" | "learnPreset" | "learnPresetToggle" | "cancel" | "clear";

export interface MidiLearnMenuItem {
  action: MidiLearnMenuAction;
  label: string;
  hint: string;
  disabled: boolean;
}

export interface MidiLearnMenuState {
  /** The address the right-clicked control drives. */
  address: string;
  /** The slot that drives the right-clicked control in every preset, if one does. */
  slot: AutomationSlot | undefined;
  /** The active preset's own mapping for the control, if it has one. */
  presetSlot: AutomationSlot | undefined;
  /** The active preset; null when none is loaded, which leaves out the per-preset learn. */
  presetId: string | null;
  armedSlotId: string | null;
  customSlotCount: number;
  maxCustomSlots: number;
  /** How many per-preset mappings the active preset already holds. */
  presetSlotCount: number;
  maxPresetSlots: number;
}

/**
 * What the right-click menu offers: MIDI Learn, and MIDI Learn for this preset on a signal
 * chain control (Cancel MIDI Learn instead, while either of the control's slots is
 * listening), and Clear Mapping when there is a mapping to clear — the active preset's own
 * first, then the global one. A per-preset mapping on any other control can still be cleared.
 * On an effect's on/off button the per-preset learn comes twice: as a latch (Absolute: 127 on,
 * 0 off) and as a toggle (Toggle: each press switches the effect).
 */
export function buildMidiLearnMenuItems(state: MidiLearnMenuState): MidiLearnMenuItem[] {
  const { slot, presetSlot, presetId } = state;
  const items: MidiLearnMenuItem[] = [];
  const listening = [slot, presetSlot].some((candidate) => candidate !== undefined && candidate.slotId === state.armedSlotId);

  if (listening) {
    items.push({ action: "cancel", label: "Cancel MIDI Learn", hint: "Listening…", disabled: false });
  } else {
    // A control no slot drives yet needs a free slot to learn into.
    const noCustomSlot = !slot && state.customSlotCount >= state.maxCustomSlots;
    items.push({
      action: "learn",
      label: "MIDI Learn…",
      hint: noCustomSlot ? `All ${state.maxCustomSlots} custom slots in use` : "",
      disabled: noCustomSlot,
    });
    if (presetId && allowsPresetMapping(state.address)) {
      const noPresetSlot = !presetSlot && state.presetSlotCount >= state.maxPresetSlots;
      const hint = noPresetSlot ? `All ${state.maxPresetSlots} preset slots in use` : "";
      if (isBypassAddress(state.address)) {
        items.push({ action: "learnPreset", label: "MIDI Learn latch for this preset…", hint: hint || "127 on, 0 off", disabled: noPresetSlot });
        items.push({
          action: "learnPresetToggle",
          label: "MIDI Learn toggle for this preset…",
          hint: hint || "Each press switches it",
          disabled: noPresetSlot,
        });
      } else {
        items.push({ action: "learnPreset", label: "MIDI Learn for this preset…", hint, disabled: noPresetSlot });
      }
    }
  }

  if (presetSlot?.midiMap) {
    items.push({ action: "clear", label: "Clear Mapping", hint: `${describeMidiMap(presetSlot.midiMap)} · this preset`, disabled: false });
  } else if (slot?.midiMap) {
    items.push({ action: "clear", label: "Clear Mapping", hint: describeMidiMap(slot.midiMap), disabled: false });
  }

  return items;
}
