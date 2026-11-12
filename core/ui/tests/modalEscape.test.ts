/**
 * Escape closes the top-most open modal through its own ×, leaves alone the modals that handle
 * Escape themselves and the riff capture, lets a field inside the dialog take the key first, and
 * does not reach the page under the dialog.
 */
import { beforeEach, describe, expect, it, vi } from "vitest";

const { initializeModalEscape, topOpenModal, modalCloseControl } = await import("../ts/modalEscape.js");

function modal(id: string, extra = ""): string {
  return `<div id="${id}" class="modal" style="display: none;">
    <div class="modal-content"><button class="modal-close" id="${id}-close" type="button">×</button>${extra}</div>
  </div>`;
}

document.body.innerHTML = [
  modal("save-preset-modal", `<input id="save-name" /><input id="own-escape" />`),
  modal("blend-editor-modal"),
  modal("riff-capture-modal"),
  modal("tuner-modal"),
  `<div id="tone-builder-modal" class="modal" style="display: none;"><button type="button">Cancel</button></div>`,
  `<div id="practice-tool-eq-modal" class="modal eq-modal" style="display: none;"><button class="eq-modal-close" type="button">×</button></div>`,
  `<button id="outside" type="button">outside</button>`,
].join("");

const closes: string[] = [];
for (const el of document.querySelectorAll<HTMLElement>(".modal")) {
  el.querySelectorAll("button").forEach((button) => button.addEventListener("click", () => {
    closes.push(el.id);
    el.style.display = "none";
  }));
}
// A field that handles Escape itself, as an inline rename does.
document.getElementById("own-escape")?.addEventListener("keydown", (event) => {
  if (event.key === "Escape") event.preventDefault();
});

initializeModalEscape();

const show = (...ids: string[]): void => ids.forEach((id) => { (document.getElementById(id) as HTMLElement).style.display = "flex"; });
const escapeOn = (target: EventTarget): KeyboardEvent => {
  const event = new KeyboardEvent("keydown", { key: "Escape", bubbles: true, cancelable: true });
  target.dispatchEvent(event);
  return event;
};

beforeEach(() => {
  closes.length = 0;
  document.querySelectorAll<HTMLElement>(".modal").forEach((el) => { el.style.display = "none"; });
});

describe("Escape closes the top-most modal", () => {
  it("clicks the open modal's own close button", () => {
    show("blend-editor-modal");
    const event = escapeOn(document.body);
    expect(closes).toEqual(["blend-editor-modal"]);
    expect(event.defaultPrevented).toBe(true);
  });

  it("closes only the later of two open modals", () => {
    show("save-preset-modal", "blend-editor-modal");
    expect(topOpenModal()?.id).toBe("blend-editor-modal");
    escapeOn(document.body);
    expect(closes).toEqual(["blend-editor-modal"]);
    escapeOn(document.body);
    expect(closes).toEqual(["blend-editor-modal", "save-preset-modal"]);
  });

  it("uses an EQ close, or a Cancel button when there is no ×", () => {
    show("practice-tool-eq-modal");
    escapeOn(document.body);
    show("tone-builder-modal");
    expect(modalCloseControl(document.getElementById("tone-builder-modal") as HTMLElement)?.textContent).toBe("Cancel");
    escapeOn(document.body);
    expect(closes).toEqual(["practice-tool-eq-modal", "tone-builder-modal"]);
  });

  it("leaves the riff capture and the modals that handle Escape themselves", () => {
    show("riff-capture-modal");
    escapeOn(document.body);
    show("save-preset-modal", "tuner-modal");
    escapeOn(document.body);
    expect(closes).toEqual([]);
  });

  it("closes from a field inside the dialog, unless the field used the key", () => {
    show("save-preset-modal");
    escapeOn(document.getElementById("own-escape") as HTMLElement);
    expect(closes).toEqual([]);
    escapeOn(document.getElementById("save-name") as HTMLElement);
    expect(closes).toEqual(["save-preset-modal"]);
  });

  it("keeps the key from the page under the dialog", () => {
    show("blend-editor-modal");
    const underneath = vi.fn();
    document.addEventListener("keydown", underneath);
    escapeOn(document.getElementById("outside") as HTMLElement);
    document.removeEventListener("keydown", underneath);
    expect(closes).toEqual(["blend-editor-modal"]);
    expect(underneath).not.toHaveBeenCalled();
  });

  it("does nothing with no modal open, or for another key", () => {
    const event = escapeOn(document.body);
    expect(event.defaultPrevented).toBe(false);
    show("blend-editor-modal");
    document.body.dispatchEvent(new KeyboardEvent("keydown", { key: "Enter", bubbles: true }));
    expect(closes).toEqual([]);
  });
});
