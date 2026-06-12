const notificationElement = document.getElementById("notification-area");
let _dismissTimer: ReturnType<typeof setTimeout> | null = null;

export function clearNotification(): void {
  if (!notificationElement) return;
  if (_dismissTimer !== null) {
    clearTimeout(_dismissTimer);
    _dismissTimer = null;
  }
  notificationElement.textContent = "";
  notificationElement.classList.remove("visible");
}

export function showNotification(message: string, detail = ""): void {
  if (!notificationElement) return;
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
