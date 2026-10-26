/**
 * showPrompt: the in-page prompt that stands in for window.prompt(), which a WKWebView
 * answers with null on iOS and macOS.
 */
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { beforeAll, beforeEach, describe, expect, it } from "vitest";
import { initializeDialogModals, isDialogOpen, showConfirm, showPrompt } from "../ts/dialogs";

const markup = readFileSync(join(__dirname, "../ui-components/modals/dialog-modal.html"), "utf8");

const byId = <T extends HTMLElement>(id: string): T => document.getElementById(id) as T;

beforeAll(() => {
  document.body.innerHTML = markup;
  initializeDialogModals();
});

beforeEach(() => {
  // Leave no dialog open from the test before.
  if (isDialogOpen()) byId<HTMLButtonElement>("dialog-cancel").click();
});

describe("showPrompt", () => {
  it("opens with the default text selected in a single-line field, and answers it on OK", async () => {
    const answer = showPrompt("Name", "Clean Boost", { title: "Save Custom Effect" });
    const input = byId<HTMLInputElement>("dialog-input");

    expect(isDialogOpen()).toBe(true);
    expect(byId("dialog-title").textContent).toBe("Save Custom Effect");
    expect(byId("dialog-message").textContent).toBe("Name");
    expect(input.style.display).toBe("block");
    expect(byId("dialog-textarea").style.display).toBe("none");
    expect(byId("dialog-cancel").style.display).not.toBe("none");
    expect(input.value).toBe("Clean Boost");
    expect(document.activeElement).toBe(input);
    expect(input.selectionStart).toBe(0);
    expect(input.selectionEnd).toBe("Clean Boost".length);

    input.value = "Edge Boost";
    byId<HTMLButtonElement>("dialog-ok").click();
    await expect(answer).resolves.toBe("Edge Boost");
    expect(isDialogOpen()).toBe(false);
    expect(input.style.display).toBe("none");
  });

  it("answers null for Cancel, the close button and Escape", async () => {
    const cancelled = showPrompt("Name", "x");
    byId<HTMLButtonElement>("dialog-cancel").click();
    await expect(cancelled).resolves.toBeNull();

    const closed = showPrompt("Name", "x");
    byId<HTMLButtonElement>("dialog-close").click();
    await expect(closed).resolves.toBeNull();

    const escaped = showPrompt("Name", "x");
    document.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape", bubbles: true }));
    await expect(escaped).resolves.toBeNull();
  });

  it("answers an empty string, not null, for an emptied field", async () => {
    const answer = showPrompt("Description", "old");
    byId<HTMLInputElement>("dialog-input").value = "";
    byId<HTMLButtonElement>("dialog-ok").click();
    await expect(answer).resolves.toBe("");
  });

  it("accepts on Enter in a single-line field", async () => {
    const answer = showPrompt("Name", "Lead");
    byId("dialog-input").dispatchEvent(new KeyboardEvent("keydown", { key: "Enter", bubbles: true }));
    await expect(answer).resolves.toBe("Lead");
  });

  it("edits multi-line text in the textarea, where Enter is a new line", async () => {
    const json = JSON.stringify({ a: "1" }, null, 2);
    const answer = showPrompt("Metadata JSON", json, { multiline: true });
    const textarea = byId<HTMLTextAreaElement>("dialog-textarea");

    expect(textarea.style.display).toBe("block");
    expect(byId("dialog-input").style.display).toBe("none");
    expect(textarea.value).toBe(json);
    textarea.dispatchEvent(new KeyboardEvent("keydown", { key: "Enter", bubbles: true }));
    expect(isDialogOpen()).toBe(true);

    byId<HTMLButtonElement>("dialog-ok").click();
    await expect(answer).resolves.toBe(json);
  });

  it("is cancelled by a dialog opened over it, which hides its field", async () => {
    const answer = showPrompt("Name", "x");
    const confirmed = showConfirm("Sure?");
    await expect(answer).resolves.toBeNull();
    expect(byId("dialog-input").style.display).toBe("none");
    byId<HTMLButtonElement>("dialog-ok").click();
    await expect(confirmed).resolves.toBe(true);
  });

  it("leaves window.prompt() reporting the bug instead of returning a value", async () => {
    expect(window.prompt("Name", "x")).toBeNull();
    expect(byId("dialog-message").textContent).toContain("showPrompt()");
    byId<HTMLButtonElement>("dialog-ok").click();
  });
});
