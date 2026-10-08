import { readFileSync } from "node:fs";
import { join } from "node:path";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { ResourceBrowserModal } from "../ts/resourceBrowser.js";
import { resolveLibraryCategoryFromHint } from "../ts/resourceBrowser/helpers.js";
import { irSlotCategory, slotImportCategory, slotStateKey } from "../ts/resourceBrowser/slot.js";
import type { ResourceBrowserOptions } from "../ts/resourceBrowser/types.js";
import { uiState } from "../ts/state.js";
import type { LibraryResource } from "../ts/types.js";

const markup = readFileSync(join(__dirname, "..", "ui-components", "modals", "resource-browser-modal.html"), "utf8");

const ir = (id: string, category: string): LibraryResource => ({
  id,
  name: id,
  category,
  description: "",
  filePath: `C:/IRs/${id}.wav`,
});

const cabSlot: ResourceBrowserOptions = {
  resourceType: "ir", nodeId: "cab", libraryCategoryHint: "cab", contextKey: "ir-cab", onSelect: () => {},
};
const reverbSlot: ResourceBrowserOptions = {
  resourceType: "ir", nodeId: "verb", libraryCategoryHint: "reverb", contextKey: "ir-reverb", onSelect: () => {},
};

/// The IR Cabinet and IR Reverb both browse type "ir" files, so the slot, not the type, decides
/// what the browser shows and where an import is filed.
describe("the slot a resource browser is opened for", () => {
  it("files IR imports under the slot, and NAM imports under the slot's gear", () => {
    expect(slotImportCategory(cabSlot, "Some Folder")).toBe("cab");
    expect(slotImportCategory(reverbSlot, "Some Folder")).toBe("reverb");
    // The match-an-IR picker still asks for "ir", the hint's old name.
    expect(irSlotCategory({ resourceType: "ir", libraryCategoryHint: "ir", contextKey: "ir-match" })).toBe("cab");
    expect(slotImportCategory({ resourceType: "nam", tone3000CategoryFilter: "pedal" }, "Some Folder")).toBe("pedal");
    expect(slotImportCategory({ resourceType: "nam" }, "Some Folder")).toBe("Some Folder");
  });

  it("keeps each slot's remembered state apart", () => {
    expect(slotStateKey(cabSlot)).not.toBe(slotStateKey(reverbSlot));
    expect(slotStateKey({ resourceType: "nam", contextKey: "nam-amp", tone3000CategoryFilter: "amp" }))
      .not.toBe(slotStateKey({ resourceType: "nam", contextKey: "nam-fx", tone3000CategoryFilter: "pedal" }));
  });

  it("never picks a reverb category for the cab hint", () => {
    expect(resolveLibraryCategoryFromHint("cab", ["Reverb IRs", "cab"])).toBe("cab");
    expect(resolveLibraryCategoryFromHint("ir", ["Reverb IRs", "reverb"])).toBeNull();
    expect(resolveLibraryCategoryFromHint("ir", ["reverb", "cab"])).toBe("cab");
  });
});

describe("a resource browser opened for an IR slot", () => {
  const previousLibrary = uiState.resourceLibrary;

  beforeEach(() => {
    document.body.innerHTML = markup;
    window.IPlugSendMsg = () => {};
    vi.stubGlobal("CSS", { escape: (value: string) => value.replace(/["\\]/g, "\\$&") });
    if (!("scrollIntoView" in Element.prototype)) {
      Element.prototype.scrollIntoView = () => {};
    }
  });

  afterEach(async () => {
    await new Promise((resolve) => requestAnimationFrame(resolve));
    delete window.IPlugSendMsg;
    vi.unstubAllGlobals();
    uiState.resourceLibrary = previousLibrary;
    document.body.innerHTML = "";
  });

  const controls = () => ({
    title: document.getElementById("resource-browser-title")!.textContent,
    category: document.querySelector<HTMLSelectElement>("#resource-browser-library-category")!.value,
    gear: document.querySelector<HTMLSelectElement>("#resource-browser-tone3000-category")!.value,
    search: document.querySelector<HTMLInputElement>("#resource-browser-library-search")!,
  });

  it("opens on the slot's category and Tone3000 gear, and remembers each slot's search apart", () => {
    uiState.resourceLibrary = { ...previousLibrary, ir: [ir("v30", "cab"), ir("hall", "reverb")] };
    const browser = new ResourceBrowserModal();

    browser.open(cabSlot);
    expect([controls().title, controls().category, controls().gear]).toEqual(["Select IR Cabinet", "cab", "cab"]);
    controls().search.value = "v30";
    controls().search.dispatchEvent(new Event("input"));
    document.querySelector<HTMLButtonElement>("#resource-browser-cancel")!.click();

    browser.open(reverbSlot);
    expect([controls().title, controls().category, controls().gear]).toEqual(["Select Reverb IR", "reverb", "space"]);
    expect(controls().search.value).toBe("");
    document.querySelector<HTMLButtonElement>("#resource-browser-cancel")!.click();

    browser.open(cabSlot);
    expect(controls().search.value).toBe("v30");
  });

  it("opens on everything, not another slot's category, when the library has none of its own", () => {
    uiState.resourceLibrary = { ...previousLibrary, ir: [ir("v30", "cab")] };
    const browser = new ResourceBrowserModal();

    browser.open(cabSlot);
    expect(controls().category).toBe("cab");
    document.querySelector<HTMLButtonElement>("#resource-browser-cancel")!.click();

    browser.open(reverbSlot);
    expect(controls().category).toBe("all");
  });
});
