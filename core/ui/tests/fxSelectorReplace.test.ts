/**
 * The FX library's close button and Escape, and the library opened to replace a node
 * (double-click it in the chain): a click on an effect replaces the node, no drag needed.
 */
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type { GraphNode, Preset } from "../ts/types.js";

let livePreset: Preset | null = null;

vi.mock("../ts/state.js", async (importOriginal) => ({
  ...(await importOriginal<Record<string, unknown>>()),
  getSignalPathPreset: () => livePreset,
}));
vi.mock("../ts/compactStage.js", () => ({ revealCompactNodeDetail: vi.fn() }));
vi.mock("../ts/signalPath/nodeLabels.js", () => ({
  getNodeDisplayName: (node: GraphNode) => node.displayName ?? node.type,
  getNodeResourceDisplayName: () => "",
}));
vi.mock("../ts/signalPath/bypass.js", () => ({
  isProtectedSignalPathNode: (node: GraphNode) => node.type === "test-splitter",
}));

// fxSelector looks its elements up as it loads.
document.body.innerHTML = `
  <div class="signal-path-stage">
    <div id="signal-path-bar"><div class="signal-node" tabindex="0" id="node-tile"></div></div>
    ${readFileSync(join(__dirname, "..", "ui-components", "fx-selector-panel.html"), "utf8")}
  </div>
  <div class="modal"><input id="modal-input" /></div>`;

const { EffectTypeRegistry } = await import("../ts/presetV2.js");
const fxSelector = await import("../ts/fxSelector.js");
const { openReplaceChooser, syncReplaceChooser } = await import("../ts/signalPath/replaceChooser.js");
const { beginPointerDrag, resetPointerDragForTests } = await import("../ts/pointerDrag.js");

for (const [type, displayName] of [["test-drive-a", "Drive A"], ["test-drive-b", "Drive B"], ["test-drive-c", "Drive C"]]) {
  EffectTypeRegistry.register(type, { type, displayName, category: "drive", requiresResource: false, parameters: [] });
}
fxSelector.initFxSelector();

const panel = document.getElementById("fx-selector-panel")!;
const title = (): string => document.getElementById("fx-selector-title")!.textContent ?? "";
const item = (type: string): HTMLElement => panel.querySelector<HTMLElement>(`.fx-item[data-effect-type="${type}"]`)!;
const isOpen = (): boolean => !panel.classList.contains("collapsed");

function node(id: string, type: string, displayName: string): GraphNode {
  return { id, type, displayName, category: "drive", params: {} } as unknown as GraphNode;
}

function preset(nodes: GraphNode[], id = "preset-1"): Preset {
  return { id, name: "Test", graph: { nodes, edges: [] } } as unknown as Preset;
}

const replaceNode = vi.fn();

function openToReplace(target: GraphNode, onPreset: Preset): void {
  livePreset = onPreset;
  openReplaceChooser(target, onPreset, replaceNode);
}

/** jsdom has no PointerEvent; only the geometry matters to the drag. */
function pointerEvent(type: string, x: number, y: number): PointerEvent {
  const event = new MouseEvent(type, { bubbles: true, cancelable: true, clientX: x, clientY: y, button: 0 });
  Object.defineProperty(event, "pointerId", { value: 1 });
  Object.defineProperty(event, "isPrimary", { value: true });
  return event as PointerEvent;
}

const keydown = (target: EventTarget, key: string): void => {
  target.dispatchEvent(new KeyboardEvent("keydown", { key, bubbles: true, cancelable: true }));
};

beforeEach(() => {
  replaceNode.mockClear();
  fxSelector.setFxSelectorCollapsed(true);
});

afterEach(() => {
  resetPointerDragForTests();
});

describe("closing the FX library", () => {
  it("has the dialogs' close button, named for a screen reader", () => {
    fxSelector.expandFxSelector();
    const close = document.getElementById("fx-selector-close")!;
    expect(close.classList.contains("modal-close")).toBe(true);
    expect(close.getAttribute("aria-label")).toContain("Close");
    close.click();
    expect(isOpen()).toBe(false);
  });

  it("closes on Escape from the library or the chain, not from a dialog", () => {
    fxSelector.expandFxSelector({ focusSearch: true });
    keydown(document.getElementById("modal-input")!, "Escape");
    expect(isOpen()).toBe(true);

    keydown(document.getElementById("node-tile")!, "Escape");
    expect(isOpen()).toBe(false);

    fxSelector.expandFxSelector({ focusSearch: true });
    keydown(document.getElementById("fx-search-input")!, "Escape");
    expect(isOpen()).toBe(false);
    expect(document.activeElement).not.toBe(document.getElementById("fx-search-input"));
  });
});

describe("the FX library opened to replace a node", () => {
  const driveA = node("n1", "test-drive-a", "Drive A");

  it("names the node and marks the effect in it now", () => {
    openToReplace(driveA, preset([driveA]));

    expect(isOpen()).toBe(true);
    expect(title()).toBe("Replace Drive A");
    expect(item("test-drive-a").classList.contains("is-current")).toBe(true);
    expect(item("test-drive-a").textContent).toContain("Current");
    expect(item("test-drive-b").getAttribute("role")).toBe("button");
    expect(item("test-drive-b").classList.contains("is-current")).toBe(false);
  });

  it("replaces the node with a clicked effect and closes", () => {
    const onPreset = preset([driveA]);
    openToReplace(driveA, onPreset);
    item("test-drive-b").click();

    expect(replaceNode).toHaveBeenCalledTimes(1);
    const [replaced, inPreset, payload] = replaceNode.mock.calls[0];
    expect(replaced).toBe(driveA);
    expect(inPreset).toBe(onPreset);
    expect(payload).toMatchObject({ effectType: "test-drive-b" });
    expect(isOpen()).toBe(false);
    expect(title()).toBe("FX Library");
  });

  it("closes without a change when the effect already in the node is clicked", () => {
    openToReplace(driveA, preset([driveA]));
    item("test-drive-a").click();

    expect(replaceNode).not.toHaveBeenCalled();
    expect(isOpen()).toBe(false);
  });

  it("chooses with Enter on a focused effect", () => {
    openToReplace(driveA, preset([driveA]));
    keydown(item("test-drive-c"), "Enter");

    expect(replaceNode.mock.calls[0]?.[2]).toMatchObject({ effectType: "test-drive-c" });
  });

  it("does not also choose the effect a drag started from", () => {
    openToReplace(driveA, preset([driveA]));
    const onDrop = vi.fn();
    const startDrag = (event: Event): void => {
      const { source, pointerEvent: down } = (event as CustomEvent).detail;
      beginPointerDrag(down, { source, sourceSelector: ".fx-item", previewClass: "preview", resolveTarget: () => null, setTargetHighlight: () => {}, onDrop });
    };
    document.addEventListener("fx-pointer-drag-start", startDrag);
    try {
      const source = item("test-drive-b");
      source.dispatchEvent(pointerEvent("pointerdown", 10, 10));
      document.dispatchEvent(pointerEvent("pointermove", 80, 10));
      document.dispatchEvent(pointerEvent("pointerup", 80, 10));
      source.click();
    } finally {
      document.removeEventListener("fx-pointer-drag-start", startDrag);
    }

    expect(onDrop).toHaveBeenCalledTimes(1);
    expect(replaceNode).not.toHaveBeenCalled();
    expect(isOpen()).toBe(true);
  });

  it("opens as a plain library from a splitter, and from Add FX", () => {
    const splitter = node("s1", "test-splitter", "Split");
    openToReplace(splitter, preset([splitter]));
    expect(title()).toBe("FX Library");

    openToReplace(driveA, preset([driveA]));
    fxSelector.expandFxSelector();
    expect(title()).toBe("FX Library");
    item("test-drive-b").click();
    expect(replaceNode).not.toHaveBeenCalled();
    expect(isOpen()).toBe(true);
  });

  it("follows the node as the chain re-renders, and lets go once it is gone", () => {
    openToReplace(driveA, preset([driveA]));

    const swapped = node("n1", "test-drive-b", "Drive B");
    syncReplaceChooser(preset([swapped]));
    expect(title()).toBe("Replace Drive B");
    expect(item("test-drive-b").classList.contains("is-current")).toBe(true);

    syncReplaceChooser(preset([swapped], "preset-2"));
    expect(title()).toBe("FX Library");
    expect(isOpen()).toBe(true);
    item("test-drive-c").click();
    expect(replaceNode).not.toHaveBeenCalled();
  });
});
