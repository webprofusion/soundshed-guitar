import { readFileSync } from "node:fs";
import { join } from "node:path";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { ResourceBrowserModal } from "../ts/resourceBrowser.js";
import type { ResourceBrowserOptions } from "../ts/resourceBrowser/types.js";
import { uiState } from "../ts/state.js";
import type { LibraryResource } from "../ts/types.js";

const markup = readFileSync(join(__dirname, "..", "ui-components", "modals", "resource-browser-modal.html"), "utf8");

const ir = (id: string, name: string): LibraryResource => ({
  id,
  name,
  category: "Cabs",
  description: "",
  filePath: `C:/IRs/${id}.wav`,
});

/// The Simple Cabinet's "Match an IR…" opens the browser with no node: the pick is only
/// something to match against, so nothing may be loaded onto a node along the way.
describe("a resource browser opened without a node", () => {
  const previousLibrary = uiState.resourceLibrary;
  let sent: Array<Record<string, unknown>>;

  beforeEach(() => {
    document.body.innerHTML = markup;
    sent = [];
    window.IPlugSendMsg = (message: string) => sent.push(JSON.parse(message) as Record<string, unknown>);
    uiState.resourceLibrary = { ...previousLibrary, ir: [ir("ir-a", "Alpha 4x12"), ir("ir-b", "Bravo 2x12")] };
    // jsdom has neither, and opening scrolls the selected row into view.
    vi.stubGlobal("CSS", { escape: (value: string) => value.replace(/["\\]/g, "\\$&") });
    if (!("scrollIntoView" in Element.prototype)) {
      Element.prototype.scrollIntoView = () => {};
    }
  });

  afterEach(async () => {
    // Let that scroll's frame run while the stub is still in place.
    await new Promise((resolve) => requestAnimationFrame(resolve));
    delete window.IPlugSendMsg;
    vi.unstubAllGlobals();
    uiState.resourceLibrary = previousLibrary;
    document.body.innerHTML = "";
  });

  const pickOnly = (onSelect: (resourceId: string) => void): ResourceBrowserOptions => ({
    resourceType: "ir",
    contextKey: "test-pick",
    title: "Match an IR",
    hint: "Pick one.",
    onSelect,
  });

  it("selects on click without previewing through the chain, and hands back the pick", () => {
    const onSelect = vi.fn();
    new ResourceBrowserModal().open(pickOnly(onSelect));
    const modal = document.getElementById("resource-browser-modal")!;

    modal.querySelector<HTMLElement>('.resource-browser-item[data-resource-id="ir-b"]')!.click();
    expect(document.querySelector<HTMLButtonElement>("#resource-browser-select")!.disabled).toBe(false);
    document.querySelector<HTMLButtonElement>("#resource-browser-select")!.click();

    expect(onSelect).toHaveBeenCalledWith("ir-b");
    expect(modal.style.display).toBe("none");
    expect(sent.filter((message) => message.type === "updateNodeResource")).toEqual([]);
  });

  it("shows the caller's title and hint without File…, and gives them back to a node's picker", () => {
    const browser = new ResourceBrowserModal();
    const title = document.getElementById("resource-browser-title")!;
    const hint = document.querySelector<HTMLElement>(".resource-browser-footer-hint")!;
    const file = document.getElementById("resource-browser-library-browse")!;
    const defaultHint = hint.textContent;

    browser.open(pickOnly(() => {}));
    expect([title.textContent, hint.textContent, file.hidden]).toEqual(["Match an IR", "Pick one.", true]);

    document.querySelector<HTMLButtonElement>("#resource-browser-cancel")!.click();
    browser.open({ resourceType: "ir", nodeId: "cab", resourceIndex: 0, onSelect: () => {} });
    expect([title.textContent, hint.textContent, file.hidden]).toEqual(["Select IR Cabinet", defaultHint, false]);
  });
});
