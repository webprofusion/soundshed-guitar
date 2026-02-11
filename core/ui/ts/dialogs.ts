type DialogMode = "alert" | "confirm";

interface DialogOptions {
  title?: string;
  message: string;
  okLabel?: string;
  cancelLabel?: string;
  mode: DialogMode;
}

let dialogModal: HTMLElement | null = null;
let dialogTitle: HTMLElement | null = null;
let dialogMessage: HTMLElement | null = null;
let dialogOkButton: HTMLButtonElement | null = null;
let dialogCancelButton: HTMLButtonElement | null = null;
let dialogCloseButton: HTMLButtonElement | null = null;
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
  dialogCancelButton.style.display = options.mode === "confirm" ? "inline-flex" : "none";

  dialogModal.style.display = "flex";

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

export function showConfirm(message: string, title = "Confirm"): Promise<boolean> {
  return openDialog({
    mode: "confirm",
    title,
    message,
    okLabel: "OK",
    cancelLabel: "Cancel",
  });
}

export function initializeDialogModals(): void {
  dialogModal = document.getElementById("dialog-modal");
  dialogTitle = document.getElementById("dialog-title");
  dialogMessage = document.getElementById("dialog-message");
  dialogOkButton = document.getElementById("dialog-ok") as HTMLButtonElement | null;
  dialogCancelButton = document.getElementById("dialog-cancel") as HTMLButtonElement | null;
  dialogCloseButton = document.getElementById("dialog-close") as HTMLButtonElement | null;

  if (!dialogModal || !dialogOkButton || !dialogCancelButton) {
    console.warn("Dialog modal markup missing; alert/confirm fallbacks will be disabled.");
    return;
  }

  dialogOkButton.addEventListener("click", () => closeDialog(true));
  dialogCancelButton.addEventListener("click", () => closeDialog(false));
  dialogCloseButton?.addEventListener("click", () => closeDialog(activeMode === "alert"));

  dialogModal.addEventListener("mousedown", (event) => {
    if (event.target === dialogModal) {
      closeDialog(activeMode === "alert");
    }
  });

  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && dialogModal?.style.display !== "none") {
      closeDialog(false);
    }
  });

  window.alert = (message?: string) => {
    void showAlert(String(message ?? ""));
  };

  window.confirm = (message?: string) => {
    console.warn("window.confirm is disabled; use showConfirm() instead.");
    void showConfirm(String(message ?? ""));
    return false;
  };
}
