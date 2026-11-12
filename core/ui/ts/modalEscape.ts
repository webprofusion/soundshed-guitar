/**
 * Escape closes the top-most open modal, for the dialogs that have no Escape
 * handling of their own (save preset, the blend editor, the custom effect
 * designer, the Practice Tool and Global EQ, the tone builder, consent, ...). It
 * matters on an iPad with a keyboard, where there is no other way out but a reach
 * for the ×. It does what the modal's own × does, so each dialog closes the way
 * it always has (a dirty form still asks, a build in progress still refuses).
 *
 * The modal on top is decided when the key goes down, before any handler runs,
 * and closed once the event has been through everything else: a field inside the
 * dialog that uses Escape itself (an inline edit, a picker) stops it or marks it
 * handled, and a modal whose own handler already closed it is left alone. When
 * the key is pressed outside the dialog (focus on the page behind), it is closed
 * at once and the event goes no further, so the library or menu under the dialog
 * does not close as well.
 */

/** Modals that handle Escape themselves, and the riff capture, which a stray key must not end. */
const SELF_HANDLED = new Set([
  "dialog-modal",
  "tuner-modal",
  "metronome-modal",
  "midi-modal",
  "resource-browser-modal",
  "update-modal",
  "tone3000-details-modal",
  "layout-designer-modal",
  "user-input-calibration-modal",
  "riff-capture-modal",
]);

function isShown(el: HTMLElement): boolean {
  if (!el.isConnected || el.closest("[hidden]")) {
    return false;
  }
  const style = getComputedStyle(el);
  return style.display !== "none" && style.visibility !== "hidden";
}

/** Modals are shown and hidden through their inline display. */
function isOpen(modal: HTMLElement): boolean {
  return modal.style.display !== "none" && isShown(modal);
}

function zIndexOf(modal: HTMLElement): number {
  const z = Number.parseInt(getComputedStyle(modal).zIndex, 10);
  return Number.isFinite(z) ? z : 0;
}

/** The open modal drawn over the others: the highest z-index, and the later one in the page on a tie. */
export function topOpenModal(root: ParentNode = document): HTMLElement | null {
  let top: HTMLElement | null = null;
  let topZ = Number.NEGATIVE_INFINITY;
  for (const modal of Array.from(root.querySelectorAll<HTMLElement>(".modal"))) {
    if (!isOpen(modal)) {
      continue;
    }
    const z = zIndexOf(modal);
    if (z >= topZ) {
      top = modal;
      topZ = z;
    }
  }
  return top;
}

/** The modal's own × (or, failing that, its Cancel or Close button), if it has one showing. */
export function modalCloseControl(modal: HTMLElement): HTMLElement | null {
  const ownedBy = (el: Element): boolean => el.closest(".modal") === modal;
  const visible = (el: HTMLElement): boolean => !(el as HTMLButtonElement).disabled && isShown(el);
  const close = Array.from(modal.querySelectorAll<HTMLElement>(".modal-close, .eq-modal-close"))
    .find((el) => ownedBy(el) && visible(el));
  if (close) {
    return close;
  }
  return Array.from(modal.querySelectorAll<HTMLButtonElement>("button"))
    .find((el) => ownedBy(el) && visible(el) && /^(cancel|close)$/i.test(el.textContent?.trim() ?? "")) ?? null;
}

function closeModal(modal: HTMLElement): boolean {
  const control = modalCloseControl(modal);
  control?.click();
  return Boolean(control);
}

/** The modal Escape would close, or null when the top one is not ours to close. */
function escapeTarget(): HTMLElement | null {
  const modal = topOpenModal();
  return modal && !SELF_HANDLED.has(modal.id) ? modal : null;
}

let pending: HTMLElement | null = null;

function onKeyDownCapture(event: KeyboardEvent): void {
  pending = null;
  if (event.key !== "Escape" || event.defaultPrevented || event.isComposing) {
    return;
  }
  const modal = escapeTarget();
  if (!modal) {
    return;
  }
  const target = event.target as Node | null;
  if (target && modal.contains(target)) {
    // Let the dialog's own fields see it first.
    pending = modal;
    return;
  }
  if (closeModal(modal)) {
    event.preventDefault();
    event.stopPropagation();
  }
}

function onKeyDownBubble(event: KeyboardEvent): void {
  const modal = pending;
  pending = null;
  if (!modal || event.key !== "Escape" || event.defaultPrevented || !isOpen(modal)) {
    return;
  }
  if (closeModal(modal)) {
    event.preventDefault();
  }
}

let installed = false;

export function initializeModalEscape(): void {
  if (installed) {
    return;
  }
  installed = true;
  window.addEventListener("keydown", onKeyDownCapture, true);
  window.addEventListener("keydown", onKeyDownBubble);
}
