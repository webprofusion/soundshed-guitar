const notificationElement = document.getElementById("notification-area");
let _dismissTimer: ReturnType<typeof setTimeout> | null = null;
const SEVERITY_WORDS = new Set(["success", "warning", "error", "info"]);

export function clearNotification(): void {
  if (!notificationElement) return;
  if (_dismissTimer !== null) {
    clearTimeout(_dismissTimer);
    _dismissTimer = null;
  }
  notificationElement.textContent = "";
  notificationElement.classList.remove("visible");
}

/**
 * Show the toast for a few seconds. `detail` is appended to the message after a
 * colon ("Failed to apply preset: disk full") — it is extra information for the
 * user, not a severity. The toast has no severity styling; a message should read
 * as a success or a failure on its own ("Layout saved", "Import failed: …").
 */
export function showNotification(message: string, detail = ""): void {
  if (!notificationElement) return;
  if (SEVERITY_WORDS.has(detail.trim().toLowerCase())) {
    // Would render as "Layout saved: success". Treat it as the mistake it is.
    console.warn(`[notification] showNotification("${message}", "${detail}"): the second argument is a detail string, not a severity`);
    detail = "";
  }
  if (_dismissTimer !== null) {
    clearTimeout(_dismissTimer);
    _dismissTimer = null;
  }
  const resolvedMessage = detail ? `${message}: ${detail}` : message;
  notificationElement.textContent = resolvedMessage;
  notificationElement.classList.add("visible");
  _dismissTimer = setTimeout(() => {
    clearNotification();
  }, 4000);
}
