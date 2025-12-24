const notificationElement = document.getElementById("notification-area");

export function clearNotification(): void {
  if (!notificationElement) return;
  notificationElement.textContent = "";
  notificationElement.classList.remove("visible");
}

export function showNotification(message: string, detail = ""): void {
  if (!notificationElement) return;
  const resolvedMessage = detail ? `${message}: ${detail}` : message;
  notificationElement.textContent = resolvedMessage;
  notificationElement.classList.add("visible");
}
