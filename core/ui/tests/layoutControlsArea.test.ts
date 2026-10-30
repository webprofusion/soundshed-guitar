import { afterEach, describe, expect, it } from "vitest";
import { CanvasProperties } from "../ts/layoutDesigner/canvasProperties.js";
import { defaultControlsPreviewHtml, renderDefaultControlsPreview } from "../ts/layoutDesigner/controlPreview.js";
import { normalizeImportedLayout } from "../ts/layoutDesigner/layoutImport.js";
import type { LayoutPropertiesHost } from "../ts/layoutDesigner/propertiesHost.js";
import { fitDefaultControls, usableControlsArea } from "../ts/layoutControlsArea.js";
import { renderCustomLayoutBackdrop } from "../ts/layoutRenderer.js";
import { layoutRequirements } from "../ts/layoutRequirements.js";
import type { EffectLayout } from "../ts/layoutTypes.js";
import type { GraphNode } from "../ts/types.js";

// A default-controls layout can keep its controls inside a box on the background.

const node = { id: "n", type: "fx", params: {} } as unknown as GraphNode;

function layout(extra: Partial<EffectLayout> = {}): EffectLayout {
  return {
    effectType: "fx", version: 1, dimensions: { width: 400, height: 280 }, backgrounds: [], controls: [], textLabels: [],
    useDefaultControls: true,
    ...extra,
  };
}

function mount(html: string): HTMLElement {
  const host = document.createElement("div");
  host.innerHTML = html;
  document.body.replaceChildren(host);
  return host;
}

/**
 * Lays `wrapper` out as ten 100 px controls in 80 px rows, wrapping at its width, which jsdom
 * does not: as many to a row as its width holds.
 */
function layOutAsTenControls(wrapper: HTMLElement): void {
  Object.defineProperty(wrapper, "offsetHeight", {
    get: () => {
      const perRow = Math.max(1, Math.floor(parseFloat(wrapper.style.width) / 100));
      return Math.ceil(10 / perRow) * 80;
    },
  });
}

function transformScale(wrapper: HTMLElement): [number, number] {
  const match = /scale\(([\d.]+), ([\d.]+)\)/.exec(wrapper.style.transform);
  return match ? [parseFloat(match[1]), parseFloat(match[2])] : [NaN, NaN];
}

afterEach(() => document.body.replaceChildren());

describe("the area", () => {
  it("is a positive, finite size, rounded", () => {
    expect(usableControlsArea({ width: 300.4, height: 199.6 })).toEqual({ width: 300, height: 200 });
    expect(usableControlsArea({ width: 0, height: 200 })).toBeUndefined();
    expect(usableControlsArea({ width: "300px", height: 200 })).toBeUndefined();
    expect(usableControlsArea({ width: Infinity, height: 200 })).toBeUndefined();
    expect(usableControlsArea(undefined)).toBeUndefined();
  });

  it("places the panel's controls at the offset, wrapping at its width under their scale", () => {
    const host = mount(renderCustomLayoutBackdrop(node, layout({
      defaultControlsOffset: { x: 20, y: 30 }, defaultControlsScale: { x: 0.5, y: 0.5 }, defaultControlsArea: { width: 300, height: 200 },
    }), "<div class=\"params-controls\"></div>"));
    const wrapper = host.querySelector<HTMLElement>(".custom-layout-default-controls")!;
    expect(wrapper.style.position).toBe("absolute");
    expect([wrapper.style.left, wrapper.style.top, wrapper.style.width]).toEqual(["20px", "30px", "600px"]);
    expect({ ...wrapper.dataset }).toEqual({ areaW: "300", areaH: "200", scaleX: "0.5", scaleY: "0.5" });
  });

  it("leaves a layout without one as it was: the canvas width, no fitting", () => {
    const wrapper = mount(renderCustomLayoutBackdrop(node, layout(), "")).querySelector<HTMLElement>(".custom-layout-default-controls")!;
    expect(wrapper.style.width).toBe("400px");
    expect(wrapper.dataset.areaW).toBeUndefined();
  });
});

describe("fitting", () => {
  it("keeps controls that fit at their own scale", () => {
    const wrapper = mount(renderCustomLayoutBackdrop(node, layout({ defaultControlsArea: { width: 500, height: 200 } }), ""))
      .querySelector<HTMLElement>(".custom-layout-default-controls")!;
    layOutAsTenControls(wrapper);
    fitDefaultControls(wrapper);
    expect(transformScale(wrapper)).toEqual([1, 1]);
    expect(wrapper.style.width).toBe("500px");
  });

  it("shrinks taller controls, rewrapping them, until they are no taller than the area", () => {
    // At 400 px: four to a row, three rows, 240 px. Three rows fit 200 px at 5/6 scale.
    const wrapper = mount(renderCustomLayoutBackdrop(node, layout({ defaultControlsArea: { width: 400, height: 200 } }), ""))
      .querySelector<HTMLElement>(".custom-layout-default-controls")!;
    layOutAsTenControls(wrapper);
    fitDefaultControls(wrapper);
    const [sx, sy] = transformScale(wrapper);
    expect(sx).toBe(sy);
    expect(sx).toBeGreaterThan(0.82);
    expect(sx).toBeLessThanOrEqual(5 / 6);
    expect(wrapper.offsetHeight * sy).toBeLessThanOrEqual(200);
    expect(parseFloat(wrapper.style.width) * sx).toBeCloseTo(400, 3);
  });

  it("keeps the layout's own Scale X and Y in proportion as it shrinks", () => {
    const wrapper = mount(renderCustomLayoutBackdrop(node, layout({
      defaultControlsScale: { x: 1, y: 2 }, defaultControlsArea: { width: 500, height: 200 },
    }), "")).querySelector<HTMLElement>(".custom-layout-default-controls")!;
    layOutAsTenControls(wrapper);
    fitDefaultControls(wrapper);
    const [sx, sy] = transformScale(wrapper);
    expect(sy / sx).toBeCloseTo(2, 3);
    expect(wrapper.offsetHeight * sy).toBeLessThanOrEqual(200);
  });
});

describe("in the designer", () => {
  it("outlines the area and fits the preview into it", () => {
    const layer = document.createElement("div");
    document.body.replaceChildren(layer);
    const l = layout({ defaultControlsOffset: { x: 16, y: 24 }, defaultControlsArea: { width: 368, height: 200 } });
    renderDefaultControlsPreview(layer, l, [], 2000);
    const outline = layer.querySelector<HTMLElement>(".layout-default-controls-area")!;
    expect([outline.style.left, outline.style.top, outline.style.width, outline.style.height]).toEqual(["16px", "24px", "368px", "200px"]);
    expect(layer.querySelector<HTMLElement>(".layout-default-controls-preview")!.style.transform).toBe("scale(1, 1)");
    expect(mount(defaultControlsPreviewHtml(layout(), [], 2000)).querySelector(".layout-default-controls-area")).toBeNull();
  });

  it("the canvas panel sets it, takes the other size from the canvas, fills the canvas and clears", () => {
    const l = layout({ defaultControlsOffset: { x: 20, y: 40 } });
    const sidebar = document.createElement("div");
    document.body.replaceChildren(sidebar);
    const host: LayoutPropertiesHost = {
      getLayout: () => l, getParamDefs: () => [], getResourceCandidates: () => [], getSidebarContent: () => sidebar,
      pushUndoState: () => {}, pushSidebarUndoOnce: () => {}, renderCanvas: () => {}, selectElement: () => {},
      renderSidebar: () => properties.renderCanvasProperties(), renderImageOptionsHtml: () => "",
      browseBackgroundImage: () => {}, browseKnobImage: () => {}, isResourceControl: () => false,
    };
    const properties = new CanvasProperties(host);
    properties.renderCanvasProperties();
    const set = (id: string, value: string) => {
      const input = document.getElementById(id) as HTMLInputElement;
      input.value = value;
      input.dispatchEvent(new Event("change"));
    };
    expect(document.getElementById("prop-dc-area-clear")).toBeNull();
    set("prop-dc-area-w", "300");
    expect(l.defaultControlsArea).toEqual({ width: 300, height: 240 });
    set("prop-dc-area-h", "180");
    expect(l.defaultControlsArea).toEqual({ width: 300, height: 180 });
    document.getElementById("prop-dc-area-fill")!.click();
    expect(l.defaultControlsArea).toEqual({ width: 360, height: 200 });
    document.getElementById("prop-dc-area-clear")!.click();
    expect("defaultControlsArea" in l).toBe(false);
    set("prop-dc-area-w", "300");
    set("prop-dc-area-h", "");
    expect("defaultControlsArea" in l).toBe(false);
  });
});

describe("as stored", () => {
  it("is a newer feature only on a default-controls layout", () => {
    expect(layoutRequirements(layout({ defaultControlsArea: { width: 300, height: 200 } }))).toEqual(["default-controls-style"]);
    expect(layoutRequirements(layout({ useDefaultControls: false, defaultControlsArea: { width: 300, height: 200 } }))).toEqual([]);
  });

  it("survives import when it is a size, and is dropped when it is not", () => {
    const raw = (area: unknown) => ({ ...layout(), defaultControlsArea: area });
    expect(normalizeImportedLayout(raw({ width: 300, height: 200 }))!.defaultControlsArea).toEqual({ width: 300, height: 200 });
    expect(normalizeImportedLayout(raw({ width: "300px; background: red", height: 200 }))!.defaultControlsArea).toBeUndefined();
  });
});
