/**
 * The IR Cabinet's two IRs side by side: IR A's card, the blend between them, IR B's card.
 *
 * Each card holds its slot's IR picker and everything that belongs to that slot alone: Level,
 * Pan, the mic's Radial and Proximity, and Invert. The middle holds what is between the two:
 * IR Blend, L/R Split and Mic Emulation. The rest of the cabinet (Mix, Output, the tone controls)
 * stays in the groups under the Main and Advanced tabs, and the Alignment section under those.
 *
 * The controls are the panel's ordinary knobs and toggles (renderControl), so they bind, learn
 * MIDI and update as any other does. Each card's header says how much of the sound is that IR's,
 * and a control that does nothing as things stand is dimmed, not hidden, so the two cards keep
 * their shape: Pan and Blend under L/R Split, the mic knobs while Mic Emulation is off, and a
 * whole card while its slot is empty.
 *
 * Only for the standard controls: a custom layout places the cabinet's controls itself.
 */

import type { BlendParamDef } from "../../blendUtils.js";
import type { GraphNode } from "../../types.js";
import { escapeHtml } from "../../utils.js";
import { nodeParamsPanelElement } from "../state.js";
import { isIrCabNode } from "./irAlignmentPanel.js";

/** Each slot's own parameters, in the order its card shows them. */
const SLOT_KEYS = [
  ["slotAGain", "slotAPan", "micRadialA", "micProximityA", "slotAPolarity"],
  ["slotBGain", "slotBPan", "micRadialB", "micProximityB", "slotBPolarity"],
] as const;

/** Between the two cards. */
const CENTRE_KEYS = ["irBlend", "lrSplit", "micEmulation"] as const;

/** Every parameter the section draws; the groups under the tabs leave them out. */
export const IR_AB_PARAM_KEYS: readonly string[] = [...SLOT_KEYS[0], ...SLOT_KEYS[1], ...CENTRE_KEYS];

const PAN_KEYS = new Set(["slotAPan", "slotBPan"]);
const MIC_KEYS = new Set(["micRadialA", "micProximityA", "micRadialB", "micProximityB"]);

/** "IR A Level" is "Level" on IR A's card, "Mic B Radial" is "Radial" on IR B's. */
function nameInSlot(name: string): string {
  return name.replace(/^(IR|Mic) [AB] /, "");
}

function slotFilled(node: GraphNode, index: number): boolean {
  const ref = node.resources?.[index];
  return Boolean(ref && (ref.resourceId || ref.id || ref.filePath || ref.embeddedId));
}

function on(node: GraphNode, key: string): boolean {
  return (node.params?.[key] ?? 0) >= 0.5;
}

/** What share of the sound each IR is, as its card's header says it. */
function shareText(node: GraphNode, index: number): string {
  const hasA = slotFilled(node, 0);
  const hasB = slotFilled(node, 1);
  if (!slotFilled(node, index)) {
    return "Empty";
  }
  if (!hasA || !hasB) {
    return "Alone";
  }
  if (on(node, "lrSplit")) {
    return index === 0 ? "Left" : "Right";
  }
  const blend = Math.min(1, Math.max(0, node.params?.irBlend ?? 0));
  return `${Math.round((index === 0 ? 1 - blend : blend) * 100)}%`;
}

export interface IrAbSlots {
  /** Each slot's picker, as the resource selector builds it. */
  slots: [string, string];
  /** The IR list: its button closed, the list itself open. */
  list: string;
}

/**
 * The section's markup, or "" for any other effect. `defs` are the node's parameters;
 * `renderControl` draws one as the panel draws its others.
 */
export function buildIrAbSectionHtml(
  node: GraphNode,
  defs: readonly BlendParamDef[],
  renderControl: (def: BlendParamDef) => string,
  pickers: IrAbSlots,
): string {
  if (!isIrCabNode(node)) {
    return "";
  }
  const def = (key: string): BlendParamDef | undefined => defs.find((candidate) => candidate.key === key);
  const control = (key: string, name?: string): string => {
    const found = def(key);
    if (!found) {
      return "";
    }
    // The knob or toggle as drawn anywhere else, tagged so the dimming can find it. The blend is
    // a knob here, as the cards' controls are, rather than the slider it is elsewhere.
    const html = renderControl({ ...found, name: name ?? found.name, blendAsKnob: true });
    return html.replace(`<div class="node-param-group`, `<div data-ir-ab-key="${escapeHtml(key)}" class="node-param-group`);
  };

  const card = (index: 0 | 1): string => {
    const letter = index === 0 ? "a" : "b";
    const keys = SLOT_KEYS[index];
    const named = (key: string): string => control(key, nameInSlot(def(key)?.name ?? key));
    const slotName = `IR ${letter.toUpperCase()}`;
    // Invert is a small Ø switch in the header, as a console's phase button is: the same
    // .node-param-toggle the panel binds, without the label and value a toggle cell has.
    const invertKey = keys[4];
    const inverted = on(node, invertKey);
    const invert = def(invertKey)
      ? `<label class="ir-ab-invert" data-ir-ab-key="${escapeHtml(invertKey)}" title="${escapeHtml(`Invert ${slotName}'s polarity`)}">
          <input class="node-param-toggle" type="checkbox" data-node-id="${escapeHtml(node.id)}" data-param-key="${escapeHtml(invertKey)}" aria-label="${escapeHtml(`Invert ${slotName}`)}"${inverted ? " checked" : ""}>
          <span aria-hidden="true">&Oslash;</span>
        </label>`
      : "";
    return `
      <div class="node-param-group-block ir-ab-slot ir-ab-slot-${letter}" data-slot="${index}">
        <div class="ir-ab-slot-head">
          <span class="ir-ab-key ir-ab-key-${letter}">${slotName}</span>
          <span class="ir-ab-share" data-ir-ab-share="${index}">${escapeHtml(shareText(node, index))}</span>
          ${invert}
        </div>
        ${pickers.slots[index]}
        <div class="node-param-group-items ir-ab-slot-controls">
          ${named(keys[0])}${named(keys[1])}
          <span class="ir-ab-divider" aria-hidden="true"></span>
          ${named(keys[2])}${named(keys[3])}
        </div>
      </div>`;
  };

  return `
    <section class="effect-inline-resource-selectors ir-ab${pickers.list.includes("is-open") ? " has-slot-list" : ""}" data-node-id="${escapeHtml(node.id)}" aria-label="IR A and IR B">
      <div class="ir-ab-grid">
      ${card(0)}
      <div class="node-param-group-block ir-ab-centre">
        <div class="node-param-group-items ir-ab-centre-controls">
          ${control("irBlend", "Blend")}
          ${control("lrSplit", "L/R Split")}
          ${control("micEmulation", "Mics")}
        </div>
      </div>
      ${card(1)}
      ${pickers.list}
      </div>
    </section>`;
}

/** Brings the headers and the dimming up to date with the node's settings and slots. */
export function updateIrAbVisualization(node: GraphNode): void {
  const root = nodeParamsPanelElement?.querySelector<HTMLElement>(".ir-ab");
  if (!root || root.dataset.nodeId !== node.id) {
    return;
  }
  const split = on(node, "lrSplit");
  const mics = on(node, "micEmulation");
  const both = slotFilled(node, 0) && slotFilled(node, 1);
  root.querySelectorAll<HTMLElement>("[data-ir-ab-share]").forEach((share) => {
    share.textContent = shareText(node, Number(share.dataset.irAbShare));
  });
  root.querySelectorAll<HTMLElement>(".ir-ab-slot").forEach((slot) => {
    slot.classList.toggle("is-empty", !slotFilled(node, Number(slot.dataset.slot)));
  });
  // Auto Align can flip IR B from the Alignment section.
  root.querySelectorAll<HTMLInputElement>(".ir-ab-invert input").forEach((toggle) => {
    toggle.checked = on(node, toggle.dataset.paramKey ?? "");
  });
  const reasons: Array<[(key: string) => boolean, string]> = [
    [(key) => PAN_KEYS.has(key) && split, "L/R Split plays A on the left and B on the right, so Pan does nothing"],
    [(key) => MIC_KEYS.has(key) && !mics, "Turn on Mics to move this IR's mic"],
    [(key) => key === "irBlend" && split, "L/R Split plays both IRs in full, one each side"],
    [(key) => (key === "irBlend" || key === "lrSplit") && !both, "Load both IRs to blend them"],
  ];
  root.querySelectorAll<HTMLElement>("[data-ir-ab-key]").forEach((cell) => {
    const key = cell.dataset.irAbKey ?? "";
    const reason = reasons.find(([applies]) => applies(key))?.[1];
    cell.classList.toggle("is-inactive", Boolean(reason));
    if (reason) {
      cell.title = reason;
    } else {
      cell.removeAttribute("title");
    }
  });
}
