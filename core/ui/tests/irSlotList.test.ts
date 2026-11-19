/**
 * The IR Cabinet's two slots step through lists of their own: IR B is a context of its own, so
 * a folder IR A was picked from leaves IR B on the library. The IR list between the slots shows
 * either slot's list, loads a row into either slot, and hands that slot the row's list.
 */
import { beforeEach, describe, expect, it, vi } from "vitest";
import type { GraphNode, Preset, ResourceRef } from "../ts/types";

vi.mock("../ts/notifications.js", () => ({ clearNotification: vi.fn(), showNotification: vi.fn() }));

// Module-level DOM roots are looked up at import, so the page exists first.
document.body.innerHTML = `
  <div id="signal-path-bar"><div class="signal-path-scroll"><div id="signal-path-nodes"></div></div></div>
  <div id="node-params-panel"></div>
`;
HTMLCanvasElement.prototype.getContext = (() => null) as typeof HTMLCanvasElement.prototype.getContext;
type Sent = { type: string; nodeId?: string; resourceId?: string; filePath?: string; resourceIndex?: number; exposedResourceId?: string };
let sent: Sent[] = [];
window.IPlugSendMsg = (payload: string) => {
  sent.push(JSON.parse(payload) as Sent);
};

const { EffectGuids } = await import("../ts/effectGuids.js");
const { EffectTypeRegistry } = await import("../ts/presetV2.js");
const { uiState } = await import("../ts/state.js");
const { showNodeParamsPanel } = await import("../ts/signalPath/paramsPanel/panel.js");
const { setNodeParamsPanelRenderer } = await import("../ts/signalPath/render.js");
const { resolveResourceContextKey } = await import("../ts/signalPath/resourceContext.js");
const { resourceBrowserModal } = await import("../ts/resourceBrowser.js");
const { resetIrSlotListForTests } = await import("../ts/signalPath/paramsPanel/irSlotList.js");

EffectTypeRegistry.register(EffectGuids.kCabIr, {
  type: EffectGuids.kCabIr,
  displayName: "IR Cabinet",
  category: "cab",
  requiresResource: true,
  resourceType: "ir",
  parameters: [{ key: "mix", name: "Mix", default: 1, min: 0, max: 1, unit: "amount" }],
  // As the engine describes it (IRCabEffect.h): two slots of the node itself.
  exposedResources: [
    { resourceId: "irA", displayName: "IR A", nodeId: "", resourceType: "ir", resourceIndex: 0, allowBrowseFile: true },
    { resourceId: "irB", displayName: "IR B", nodeId: "", resourceType: "ir", resourceIndex: 1, allowBrowseFile: true },
  ],
});

const FOLDER = ["C:/irs/v30/421.wav", "C:/irs/v30/57.wav", "C:/irs/v30/906.wav"];

function cab(resources: ResourceRef[]): GraphNode {
  return { id: "cab", type: EffectGuids.kCabIr, displayName: "IR Cabinet", category: "cab", bypassed: false, params: {}, config: {}, resources };
}

function presetWith(node: GraphNode): Preset {
  return { id: "user-test", name: "Test", graph: { nodes: [node], edges: [] } } as Preset;
}

// What the browser remembers per context, seeded as a folder pick would leave it.
const browser = resourceBrowserModal as unknown as {
  folderTab: { folderNavigationStates: Map<string, { resourceType: string; items: Array<{ resourceId?: string; filePath?: string }> }> };
  lastNavigationViewByContext: Map<string, string>;
};

function pickedFromFolder(contextKey: string): void {
  browser.folderTab.folderNavigationStates.set(contextKey, { resourceType: "ir", items: FOLDER.map((filePath) => ({ filePath })) });
  browser.lastNavigationViewByContext.set(contextKey, "folder");
}

function panel(): HTMLElement {
  return document.getElementById("node-params-panel") as HTMLElement;
}

let node: GraphNode;
let preset: Preset;

function render(): void {
  showNodeParamsPanel(node, preset);
}

function click(selector: string): void {
  const element = panel().querySelector<HTMLElement>(selector);
  expect(element, selector).not.toBeNull();
  element!.click();
}

beforeEach(() => {
  sent = [];
  resetIrSlotListForTests();
  browser.folderTab.folderNavigationStates.clear();
  browser.lastNavigationViewByContext.clear();
  uiState.resourceLibrary = {
    nam: [],
    ir: [
      { id: "lib-a", name: "Alpha Cab", category: "cab", filePath: "C:/lib/a.wav" },
      { id: "lib-b", name: "Bravo Cab", category: "cab", filePath: "C:/lib/b.wav" },
    ],
  };
  node = cab([{ resourceType: "ir", filePath: FOLDER[0] }, { resourceType: "ir", resourceId: "lib-a" }]);
  preset = presetWith(node);
  setNodeParamsPanelRenderer(showNodeParamsPanel);
});

describe("the IR Cabinet's slots", () => {
  it("browse as two contexts", () => {
    expect(resolveResourceContextKey(node, "ir", 0)).toBe("ir-cab");
    expect(resolveResourceContextKey(node, "ir", 1)).toBe("ir-cab#1");
  });

  it("step through the list each was last picked from", () => {
    pickedFromFolder("ir-cab");
    render();

    click('.resource-nav-next-btn[data-resource-index="0"]');
    click('.resource-nav-next-btn[data-resource-index="1"]');

    const updates = sent.filter((message) => message.type === "updateNodeResource");
    expect(updates).toEqual([
      expect.objectContaining({ resourceIndex: 0, exposedResourceId: "irA", filePath: FOLDER[1] }),
      expect.objectContaining({ resourceIndex: 1, exposedResourceId: "irB", resourceId: "lib-b" }),
    ]);
  });
});

describe("the IR list between the slots", () => {
  beforeEach(() => {
    render();
  });

  it("is a button between IR A and IR B until opened", () => {
    // Side by side: IR A's card, the blend, IR B's card, and the list's button (drawn in the middle).
    const children = Array.from(panel().querySelector(".ir-ab-grid")!.children);
    expect(children.map((child) => child.className.split(" ").find((name) => name.startsWith("ir-ab-") || name === "ir-slot-list")))
      .toEqual(["ir-ab-slot", "ir-ab-centre", "ir-ab-slot", "ir-slot-list"]);
    expect(panel().querySelectorAll(".ir-ab-slot .node-resource-selector")).toHaveLength(2);
    expect(panel().querySelector(".ir-slot-list-rows")).toBeNull();
  });

  it("opens on the slot browsed from a folder, marking what each slot has loaded", () => {
    pickedFromFolder("ir-cab");
    click(".ir-slot-list-toggle");

    expect(panel().querySelector(".ir-ab")!.classList.contains("has-slot-list")).toBe(true);
    expect(panel().querySelector(".ir-slot-list-source.is-active")?.textContent).toBe("IR A · Folder");
    const rows = Array.from(panel().querySelectorAll(".ir-slot-list-row"));
    expect(rows.map((row) => row.querySelector(".ir-slot-list-row-name")?.textContent)).toEqual(["421", "57", "906"]);
    expect(rows[0].querySelector(".ir-slot-list-row-loaded")?.textContent).toBe("A");

    click('.ir-slot-list-source[data-source-slot="1"]');
    expect(Array.from(panel().querySelectorAll(".ir-slot-list-row-name")).map((name) => name.textContent)).toEqual(["Alpha Cab", "Bravo Cab"]);
  });

  it("filters its rows", () => {
    pickedFromFolder("ir-cab");
    click(".ir-slot-list-toggle");
    const search = panel().querySelector<HTMLInputElement>(".ir-slot-list-search")!;
    search.value = "90";
    search.dispatchEvent(new Event("input"));

    expect(Array.from(panel().querySelectorAll(".ir-slot-list-row-name")).map((name) => name.textContent)).toEqual(["906"]);
  });

  it("loads a row into the other slot, which then steps through that list", async () => {
    pickedFromFolder("ir-cab");
    click(".ir-slot-list-toggle");
    const row = Array.from(panel().querySelectorAll<HTMLElement>(".ir-slot-list-row"))[2];
    row.querySelector<HTMLButtonElement>('.ir-slot-list-assign[data-slot="1"]')!.click();
    await Promise.resolve();

    expect(sent.filter((message) => message.type === "updateNodeResource")).toEqual([
      expect.objectContaining({ resourceIndex: 1, exposedResourceId: "irB", filePath: FOLDER[2] }),
    ]);
    expect(resourceBrowserModal.navigationLists.effectiveView("ir", { contextKey: "ir-cab#1" })).toBe("folder");
    expect(resourceBrowserModal.getAdjacentResourceSelection("ir", "", FOLDER[2], 1, { contextKey: "ir-cab#1" }))
      .toEqual(expect.objectContaining({ filePath: FOLDER[0] }));
  });
});
