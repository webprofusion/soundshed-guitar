import { uiState } from "./state.js";
import { requestAppInfo } from "./bridge.js";
import { updateAppSetting } from "./appSettingsStore.js";
import { appendLog } from "./logging.js";
import { showNotification } from "./notifications.js";
import { getApiBaseUrl } from "./apiConfig.js";
import { refreshSettingsUpdateBanner } from "./settings/updateBanner.js";
import { escapeHtml } from "./utils.js";

const UPDATE_CHECK_ENABLED_SETTING = "app.updateCheckEnabled";
const INSTANCE_ID_SETTING = "app.instanceId";

let hasCheckedForUpdates = false;

/**
 * Runs once per session, from the first app settings that leave the check on.
 * Called on every app settings arrival, so turning the setting back on in
 * Settings checks straight away rather than at the next launch.
 */
export function triggerUpdateCheck(): void {
  if (hasCheckedForUpdates) return;

  const rawEnabled = uiState.appSettings[UPDATE_CHECK_ENABLED_SETTING];
  const updateCheckEnabled = rawEnabled === undefined ? true : (rawEnabled === true || rawEnabled === "true");
  if (!updateCheckEnabled) return;
  hasCheckedForUpdates = true;

  // Request app info from backend first
  requestAppInfo();

  // Ensure instance ID exists
  let instanceId = uiState.appSettings[INSTANCE_ID_SETTING] as string | undefined;
  if (!instanceId) {
    instanceId = crypto.randomUUID();
    updateAppSetting(INSTANCE_ID_SETTING, instanceId);
  }

  setTimeout(() => {
    void performUpdateCheck(instanceId!);
  }, 5000); // Delay check by 5 seconds to not block startup
}

async function performUpdateCheck(instanceId: string): Promise<void> {
  try {
    const os = uiState.environment?.os ?? "Unknown";
    const cpu = uiState.environment?.cpu ?? "x64";
    const isStandalone = uiState.environment?.standalone ?? false;
    const currentVersion = uiState.environment?.version?.trim();

    if (!currentVersion) {
      console.warn("[UpdateCheck] Skipping update check because the current version is unavailable");
      return;
    }

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
      if (isVersionNewer(result.data.latest_version, currentVersion)) {
        showUpdateAvailable(result.data);
      } else if (result.data.is_update_available) {
        console.info(
          `[UpdateCheck] Ignoring update notification because ${result.data.latest_version} is not newer than ${currentVersion}`,
        );
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

function toNumericSemver(version: string | null | undefined): [number, number, number] | null {
  if (typeof version !== "string") {
    return null;
  }

  const match = version.trim().match(/^[^\d]*(\d+)(?:\.(\d+))?(?:\.(\d+))?(?:[-+].*)?$/);
  if (!match) {
    return null;
  }

  const major = Number.parseInt(match[1] ?? "0", 10);
  const minor = Number.parseInt(match[2] ?? "0", 10);
  const patch = Number.parseInt(match[3] ?? "0", 10);

  if (![major, minor, patch].every((value) => Number.isSafeInteger(value) && value >= 0)) {
    return null;
  }

  return [major, minor, patch];
}

function isVersionNewer(candidateVersion: string, currentVersion: string): boolean {
  const candidate = toNumericSemver(candidateVersion);
  const current = toNumericSemver(currentVersion);

  if (candidate === null || current === null) {
    console.warn("[UpdateCheck] Unable to compare app versions", {
      candidateVersion,
      currentVersion,
    });
    return false;
  }

  for (let index = 0; index < candidate.length; index += 1) {
    if (candidate[index] !== current[index]) {
      return candidate[index] > current[index];
    }
  }

  return false;
}

function showUpdateAvailable(data: UpdateCheckResult): void {
  // The settings banner, the header button and the dialog all read this.
  uiState.availableUpdate = {
    version: data.latest_version,
    downloadUrl: toHttpUrl(data.download_url) ?? "",
    releaseNotes: data.release_notes ?? "",
  };
  refreshSettingsUpdateBanner();
  showHeaderUpdateButton(data.latest_version);

  showNotification("Update Available", `Version ${data.latest_version} is available.`);
}

/** Reveals the header's update button; it stays up for the rest of the session. */
function showHeaderUpdateButton(version: string): void {
  const button = document.getElementById("header-update-btn") as HTMLButtonElement | null;
  if (!button) return;

  const versionEl = document.getElementById("header-update-version");
  if (versionEl) {
    versionEl.textContent = version;
  }
  const label = `Soundshed Guitar ${version} is available`;
  button.title = label;
  button.setAttribute("aria-label", label);
  button.hidden = false;

  if (!button.dataset.bound) {
    button.dataset.bound = "true";
    button.addEventListener("click", openUpdateModal);
  }
}

let updateModalBound = false;

function closeUpdateModal(): void {
  const modal = document.getElementById("update-modal");
  if (modal) {
    modal.style.display = "none";
  }
}

function bindUpdateModal(modal: HTMLElement): void {
  if (updateModalBound) return;
  updateModalBound = true;

  document.getElementById("update-modal-close")?.addEventListener("click", closeUpdateModal);
  document.getElementById("update-modal-later")?.addEventListener("click", closeUpdateModal);
  // main.ts hands the link's URL to the system browser; the dialog has done its job.
  document.getElementById("update-modal-download")?.addEventListener("click", closeUpdateModal);
  modal.addEventListener("mousedown", (event) => {
    if (event.target === modal) {
      closeUpdateModal();
    }
  });
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && modal.style.display !== "none") {
      closeUpdateModal();
    }
  });
}

function openUpdateModal(): void {
  const update = uiState.availableUpdate;
  const modal = document.getElementById("update-modal");
  if (!update || !modal) return;
  bindUpdateModal(modal);

  const versionEl = document.getElementById("update-modal-version");
  if (versionEl) {
    versionEl.textContent = update.version;
  }
  const currentEl = document.getElementById("update-modal-current");
  if (currentEl) {
    currentEl.textContent = uiState.environment?.version ?? "an older version";
  }
  const notesEl = document.getElementById("update-modal-notes");
  if (notesEl) {
    notesEl.innerHTML = renderMarkdown(update.releaseNotes || "No release notes provided.");
  }
  const download = document.getElementById("update-modal-download") as HTMLAnchorElement | null;
  if (download) {
    download.href = update.downloadUrl || "#";
    download.style.display = update.downloadUrl ? "" : "none";
  }

  modal.style.display = "flex";
}

/** The URL when it is an absolute http(s) one, else null: a link can never be `javascript:`. */
export function toHttpUrl(value: unknown): string | null {
  if (typeof value !== "string") {
    return null;
  }
  try {
    const url = new URL(value.trim());
    return url.protocol === "https:" || url.protocol === "http:" ? url.toString() : null;
  } catch {
    return null;
  }
}

function renderEmphasis(escaped: string): string {
  return escaped
    .replace(/\*\*(.*)\*\*/g, "<strong>$1</strong>")
    .replace(/\*(.*)\*/g, "<em>$1</em>");
}

/**
 * One line of release notes: text is escaped before any tag is added, and a link
 * or image keeps its markup only when its target is an http(s) URL.
 */
function renderMarkdownInline(line: string): string {
  let html = "";
  let last = 0;
  const pattern = /(!?)\[([^\]]*)\]\(([^)\s]*)\)/g;
  for (let match = pattern.exec(line); match; match = pattern.exec(line)) {
    html += renderEmphasis(escapeHtml(line.slice(last, match.index)));
    const [, bang, label, target] = match;
    const url = toHttpUrl(target);
    if (!url) {
      html += renderEmphasis(escapeHtml(label));
    } else if (bang) {
      html += `<img alt="${escapeHtml(label)}" src="${escapeHtml(url)}" />`;
    } else {
      html += `<a href="${escapeHtml(url)}" target="_blank" rel="noopener noreferrer">${renderEmphasis(escapeHtml(label))}</a>`;
    }
    last = match.index + match[0].length;
  }
  return html + renderEmphasis(escapeHtml(line.slice(last)));
}

/** Very basic markdown for release notes, which arrive from the update server. */
export function renderMarkdown(text: string): string {
  return text
    .split(/\r?\n/)
    .map((line) => {
      const heading = /^(#{1,3}) (.*)$/.exec(line);
      if (heading) {
        const level = heading[1].length;
        return `<h${level}>${renderMarkdownInline(heading[2])}</h${level}>`;
      }
      const quote = /^> (.*)$/.exec(line);
      return quote ? `<blockquote>${renderMarkdownInline(quote[1])}</blockquote>` : renderMarkdownInline(line);
    })
    .join("\n")
    .replace(/\n$/gm, "<br />");
}
