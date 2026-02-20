import { uiState } from "./state.js";
import { setAppSetting, requestAppInfo } from "./bridge.js";
import { appendLog } from "./logging.js";
import { showNotification } from "./notifications.js";
import { getApiBaseUrl } from "./toneSharingPanel.js";

const UPDATE_CHECK_ENABLED_SETTING = "app.updateCheckEnabled";
const INSTANCE_ID_SETTING = "app.instanceId";

let hasCheckedForUpdates = false;

export function triggerUpdateCheck(): void {
  if (hasCheckedForUpdates) return;
  hasCheckedForUpdates = true;

  // Request app info from backend first
  requestAppInfo();

  // Ensure instance ID exists
  let instanceId = uiState.appSettings[INSTANCE_ID_SETTING] as string | undefined;
  if (!instanceId) {
    instanceId = crypto.randomUUID();
    uiState.appSettings[INSTANCE_ID_SETTING] = instanceId;
    setAppSetting(INSTANCE_ID_SETTING, instanceId);
  }

  const rawEnabled = uiState.appSettings[UPDATE_CHECK_ENABLED_SETTING];
  const updateCheckEnabled = rawEnabled === undefined ? true : (rawEnabled === true || rawEnabled === "true");
  
  if (updateCheckEnabled) {
    setTimeout(() => {
      void performUpdateCheck(instanceId!);
    }, 5000); // Delay check by 5 seconds to not block startup
  } else {
    console.log("[UpdateCheck] Update check is disabled in settings");
  }
}

async function performUpdateCheck(instanceId: string): Promise<void> {
  try {
    const os = uiState.environment?.os ?? "Unknown";
    const cpu = uiState.environment?.cpu ?? "x64";
    const isStandalone = uiState.environment?.standalone ?? false;
    const currentVersion = uiState.environment?.version ?? "1.0.1";

    const apiBase = getApiBaseUrl();
    const response = await fetch(`${apiBase}/app/updatecheck`, {
      method: "POST",
      headers: {
        "Content-Type": "application/json"
      },
      body: JSON.stringify({
        current_version: currentVersion,
        os,
        cpu,
        is_standalone: isStandalone,
        instance_id: instanceId
      })
    });

    if (!response.ok) {
      throw new Error(`HTTP error! status: ${response.status}`);
    }

    const result = await response.json();
    
    if (result.ok && result.data) {
      if (result.data.is_update_available) {
        showUpdateAvailable(result.data);
      }
    }
  } catch (error) {
    console.error("Failed to check for updates:", error);
    appendLog("Update check failed");
  }
}

interface UpdateCheckResult {
  is_update_available: boolean;
  latest_version: string;
  download_url: string;
  release_notes: string;
}

function showUpdateAvailable(data: UpdateCheckResult): void {
  // Create badge in UI
  const settingsBtn = document.getElementById("footer-settings-btn");
  if (settingsBtn) {
    let badge = settingsBtn.querySelector(".update-badge") as HTMLElement | null;
    if (!badge) {
      badge = document.createElement("span");
      badge.className = "update-badge";
      badge.textContent = "1";
      badge.style.cssText = "position: absolute; top: -5px; right: -5px; background: var(--accent-color, #ff4444); color: white; border-radius: 50%; padding: 2px 6px; font-size: 10px; font-weight: bold; pointer-events: none;";
      settingsBtn.style.position = "relative";
      settingsBtn.appendChild(badge);
    }
    
    // Add click handler to show modal
    settingsBtn.addEventListener("click", () => {
      createUpdateModal(data);
    });
  }

  // Show notification
  showNotification("Update Available", `Version ${data.latest_version} is available.`);
}

function createUpdateModal(data: UpdateCheckResult): void {
  let modal = document.getElementById("update-modal");
  if (!modal) {
    modal = document.createElement("div");
    modal.id = "update-modal";
    modal.className = "modal-overlay";
    modal.style.display = "none";
    
    modal.innerHTML = `
      <div class="modal-content" style="max-width: 500px;">
        <div class="modal-header">
          <h2>Update Available</h2>
          <button class="icon-btn" id="update-modal-close">&times;</button>
        </div>
        <div class="modal-body">
          <p>A new version of Soundshed Guitar is available: <strong>${data.latest_version}</strong></p>
          <div class="release-notes" style="margin-top: 15px; max-height: 200px; overflow-y: auto; background: var(--bg-color-dark, rgba(0,0,0,0.1)); padding: 10px; border-radius: 4px; font-size: 0.9em;">
            ${renderMarkdown(data.release_notes || "No release notes provided.")}
          </div>
        </div>
        <div class="modal-footer">
          <button class="btn" id="update-modal-later">Later</button>
          <a href="${data.download_url}" target="_blank" class="btn primary" id="update-modal-download">Download Update</a>
        </div>
      </div>
    `;
    document.body.appendChild(modal);

    document.getElementById("update-modal-close")?.addEventListener("click", () => {
      modal!.style.display = "none";
    });
    document.getElementById("update-modal-later")?.addEventListener("click", () => {
      modal!.style.display = "none";
    });
    document.getElementById("update-modal-download")?.addEventListener("click", () => {
      modal!.style.display = "none";
    });
  }

  // Show the modal
  modal.style.display = "flex";
}

function renderMarkdown(text: string): string {
  // Very basic markdown rendering for release notes
  return text
    .replace(/^### (.*$)/gim, '<h3>$1</h3>')
    .replace(/^## (.*$)/gim, '<h2>$1</h2>')
    .replace(/^# (.*$)/gim, '<h1>$1</h1>')
    .replace(/^\> (.*$)/gim, '<blockquote>$1</blockquote>')
    .replace(/\*\*(.*)\*\*/gim, '<strong>$1</strong>')
    .replace(/\*(.*)\*/gim, '<em>$1</em>')
    .replace(/!\[(.*?)\]\((.*?)\)/gim, "<img alt='$1' src='$2' />")
    .replace(/\[(.*?)\]\((.*?)\)/gim, "<a href='$2'>$1</a>")
    .replace(/\n$/gim, '<br />');
}
