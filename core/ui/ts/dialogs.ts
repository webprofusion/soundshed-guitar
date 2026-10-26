type DialogMode = "alert" | "confirm" | "prompt";

interface DialogOptions {
  title?: string;
  message: string;
  okLabel?: string;
  cancelLabel?: string;
  mode: DialogMode;
  /** A prompt's starting text. */
  defaultValue?: string;
  /** A prompt for text that runs over several lines, edited in a textarea. */
  multiline?: boolean;
}

let dialogModal: HTMLElement | null = null;
let dialogTitle: HTMLElement | null = null;
let dialogMessage: HTMLElement | null = null;
let dialogOkButton: HTMLButtonElement | null = null;
let dialogCancelButton: HTMLButtonElement | null = null;
let dialogCloseButton: HTMLButtonElement | null = null;
let dialogInput: HTMLInputElement | null = null;
let dialogTextarea: HTMLTextAreaElement | null = null;
/** The field the open prompt is using; null for an alert or confirm. */
let activeField: HTMLInputElement | HTMLTextAreaElement | null = null;
let activeResolve: ((value: boolean) => void) | null = null;
let activeMode: DialogMode = "alert";

function resetDialogState(): void {
  if (dialogOkButton) {
    dialogOkButton.disabled = false;
  }
  if (dialogCancelButton) {
    dialogCancelButton.disabled = false;
  }
}

function closeDialog(result: boolean): void {
  if (!dialogModal) {
    return;
  }

  dialogModal.style.display = "none";
  resetDialogState();
  if (dialogInput) dialogInput.style.display = "none";
  if (dialogTextarea) dialogTextarea.style.display = "none";

  if (activeResolve) {
    activeResolve(result);
  }

  activeResolve = null;
}

function openDialog(options: DialogOptions): Promise<boolean> {
  if (!dialogModal || !dialogTitle || !dialogMessage || !dialogOkButton || !dialogCancelButton) {
    console.warn("Dialog modal elements not found; falling back to console output.");
    return Promise.resolve(options.mode === "confirm" ? false : true);
  }

  if (activeResolve) {
    activeResolve(false);
    activeResolve = null;
  }

  activeMode = options.mode;
  dialogTitle.textContent = options.title ?? (options.mode === "confirm" ? "Confirm" : "Notice");
  dialogMessage.textContent = options.message;

  dialogOkButton.textContent = options.okLabel ?? "OK";
  dialogCancelButton.textContent = options.cancelLabel ?? "Cancel";
  dialogCancelButton.style.display = options.mode === "alert" ? "none" : "inline-flex";

  activeField = options.mode === "prompt" ? (options.multiline ? dialogTextarea : dialogInput) : null;
  for (const field of [dialogInput, dialogTextarea]) {
    if (field) field.style.display = field === activeField ? "block" : "none";
  }
  if (activeField) {
    activeField.value = options.defaultValue ?? "";
  }

  dialogModal.style.display = "flex";
  if (activeField) {
    // Focused while still inside the press that opened the prompt, so iOS raises its keyboard.
    activeField.focus();
    activeField.select();
  }

  return new Promise<boolean>((resolve) => {
    activeResolve = resolve;
  });
}

export function showAlert(message: string, title = "Notice"): Promise<void> {
  return openDialog({
    mode: "alert",
    title,
    message,
    okLabel: "OK",
  }).then(() => undefined);
}

export function showConfirm(
  message: string,
  title = "Confirm",
  labels: { ok?: string; cancel?: string } = {},
): Promise<boolean> {
  return openDialog({
    mode: "confirm",
    title,
    message,
    okLabel: labels.ok ?? "OK",
    cancelLabel: labels.cancel ?? "Cancel",
  });
}

/**
 * Asks for a line of text, or several with `multiline`: the entered text on OK, null on
 * Cancel, Escape or the close button. `window.prompt()` is no use for this: a WKWebView
 * returns null from it on iOS and macOS, since JUCE's delegate never shows its panel.
 */
export async function showPrompt(
  message: string,
  defaultValue = "",
  options: { title?: string; okLabel?: string; cancelLabel?: string; multiline?: boolean } = {},
): Promise<string | null> {
  if (!dialogInput || !dialogTextarea) {
    console.warn("Dialog prompt fields not found; the prompt is treated as cancelled.");
    return null;
  }
  const field = options.multiline ? dialogTextarea : dialogInput;
  const ok = await openDialog({
    mode: "prompt",
    title: options.title ?? "Enter a value",
    message,
    okLabel: options.okLabel ?? "OK",
    cancelLabel: options.cancelLabel ?? "Cancel",
    defaultValue,
    multiline: options.multiline,
  });
  return ok ? field.value : null;
}

/** True while an alert, confirm or prompt is on screen; modals beneath it should leave the keyboard alone. */
export function isDialogOpen(): boolean {
  return Boolean(dialogModal && dialogModal.style.display !== "none");
}

export function initializeDialogModals(): void {
  dialogModal = document.getElementById("dialog-modal");
  dialogTitle = document.getElementById("dialog-title");
  dialogMessage = document.getElementById("dialog-message");
  dialogOkButton = document.getElementById("dialog-ok") as HTMLButtonElement | null;
  dialogCancelButton = document.getElementById("dialog-cancel") as HTMLButtonElement | null;
  dialogCloseButton = document.getElementById("dialog-close") as HTMLButtonElement | null;
  dialogInput = document.getElementById("dialog-input") as HTMLInputElement | null;
  dialogTextarea = document.getElementById("dialog-textarea") as HTMLTextAreaElement | null;

  if (!dialogModal || !dialogOkButton || !dialogCancelButton) {
    console.warn("Dialog modal markup missing; alert/confirm fallbacks will be disabled.");
    return;
  }

  dialogOkButton.addEventListener("click", () => closeDialog(true));
  dialogCancelButton.addEventListener("click", () => closeDialog(false));
  dialogCloseButton?.addEventListener("click", () => closeDialog(activeMode === "alert"));

  // Enter accepts a single-line prompt; in the textarea it starts a new line.
  dialogInput?.addEventListener("keydown", (event) => {
    if (event.key === "Enter" && !event.isComposing) {
      event.preventDefault();
      closeDialog(true);
    }
  });

  dialogModal.addEventListener("mousedown", (event) => {
    if (event.target === dialogModal) {
      closeDialog(activeMode === "alert");
    }
  });

  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && isDialogOpen()) {
      // Handled: a modal under the dialog must not take the same Escape as its own.
      event.preventDefault();
      closeDialog(false);
    }
  });

  window.alert = (message?: string) => {
    void showAlert(String(message ?? ""));
  };

  window.confirm = (_message?: string) => {
    void showConfirm("bug: window.confirm is disabled; use showConfirm() instead");
    return false;
  };

  window.prompt = (_message?: string) => {
    void showAlert("bug: window.prompt is disabled; use showPrompt() instead");
    return null;
  };
}
