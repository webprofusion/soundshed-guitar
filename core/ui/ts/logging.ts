import { LOG_ENTRY_LIMIT, uiState } from "./state.js";
import { escapeHtml } from "./utils.js";

const logPanelElement = document.getElementById("log-panel");

export function renderLogEntries(): void {
  if (!logPanelElement) {
    return;
  }

  if (!uiState.logs.length) {
    logPanelElement.innerHTML = '<p class="empty">No log entries yet.</p>';
    return;
  }

  const items = uiState.logs
    .map((entry) => {
      const timestamp = entry.timestamp instanceof Date
        ? entry.timestamp.toLocaleTimeString()
        : entry.timestamp;
      return `
        <li class="log-entry">
          <time>${escapeHtml(timestamp)}</time>
          <span>${escapeHtml(entry.message)}</span>
        </li>
      `;
    })
    .join("");

  logPanelElement.innerHTML = `<ul class="log-list">${items}</ul>`;
  logPanelElement.scrollTop = logPanelElement.scrollHeight;
}

export function appendLog(message: string): void {
  const entry = {
    timestamp: new Date(),
    message,
  };

  uiState.logs.push(entry);
  if (uiState.logs.length > LOG_ENTRY_LIMIT) {
    uiState.logs.splice(0, uiState.logs.length - LOG_ENTRY_LIMIT);
  }

  renderLogEntries();
}

export function installFetchLogger(): void {
  const nativeFetch = typeof window.fetch === "function" ? window.fetch.bind(window) : null;

  if (!nativeFetch) {
    return;
  }

  window.fetch = async (...args) => {
    const [input] = args;
    const url = typeof input === "string"
      ? input
      : input instanceof Request
        ? input.url
        : input instanceof URL
          ? input.toString()
          : "(unknown)";
    appendLog(`fetch → ${url}`);
    try {
      const response = await nativeFetch(...args);
      appendLog(`response ${response.status} ← ${url}`);
      return response;
    } catch (error) {
      appendLog(`error ← ${url} (${error instanceof Error ? error.message : String(error)})`);
      throw error;
    }
  };
}
