import { uiState } from "./state.js";
import { setAppSetting, postMessage } from "./bridge.js";
import { appendLog } from "./logging.js";
import { showNotification } from "./notifications.js";
import { handleAppSettingUpdate } from "./tone3000.js";
import { updateSignalDiagnosticsView, updateDSPPerformancePlot } from "./views.js";
import { initTone3000Browser } from "./tone3000Browser.js";
import { getAudioFxLibrary, getIrLibrary } from "./dataLibraries.js";
import type { AppSettingValue } from "./types.js";
import { buildBlendModelMappingsFromIds } from "./blendUtils.js";
import { themeSwitcher, type ThemeName } from "./theme-switcher.js";

const API_KEY_SETTING = "tone3000.apiKey";
const DIAGNOSTICS_SETTING = "diagnostics.signalLevelsEnabled";
const INTERFACE_CALIBRATION_ENABLED_SETTING = "audio.interfaceCalibration.enabled";
const INTERFACE_CALIBRATION_REFERENCE_SETTING = "audio.interfaceCalibration.referenceDbu";

const apiKeyInput = document.getElementById("tone3000-api-key-input") as HTMLInputElement | null;
const saveButton = document.getElementById("tone3000-api-key-save");
const clearButton = document.getElementById("tone3000-api-key-clear");
const sessionStatus = document.getElementById("tone3000-session-status");
const openAudioPreferencesButton = document.getElementById("open-audio-preferences");
const diagnosticsToggle = document.getElementById("signal-diagnostics-toggle") as HTMLInputElement | null;
const interfaceCalibrationToggle = document.getElementById("interface-calibration-toggle") as HTMLInputElement | null;
const interfaceCalibrationReferenceInput = document.getElementById("interface-calibration-reference") as HTMLInputElement | null;
const equipmentTabButtons = Array.from(document.querySelectorAll(".equipment-tab-btn"));
const equipmentTabPanels = Array.from(document.querySelectorAll(".equipment-tab-panel"));
const themeSelect = document.getElementById("theme-select") as HTMLSelectElement | null;
const librarySearchInput = document.getElementById("equipment-library-search") as HTMLInputElement | null;
const libraryTypeSelect = document.getElementById("equipment-library-type") as HTMLSelectElement | null;
const librarySourceSelect = document.getElementById("equipment-library-source") as HTMLSelectElement | null;
const libraryViewSelect = document.getElementById("equipment-library-view") as HTMLSelectElement | null;
const libraryCategorySelect = document.getElementById("equipment-library-category") as HTMLSelectElement | null;
const libraryResults = document.getElementById("equipment-library-results");
const librarySummary = document.getElementById("equipment-library-summary");
const libraryTabButtons = Array.from(document.querySelectorAll(".library-tab-btn"));
const libraryTabPanels = Array.from(document.querySelectorAll(".library-tab-panel"));
let settingsInitialized = false;
let libraryFiltersInitialized = false;
let equipmentTabsInitialized = false;
let themeSelectInitialized = false;
let libraryTabsInitialized = false;

export function initSettingsPanel(): void {
  if (settingsInitialized) {
    return;
  }
  settingsInitialized = true;
  saveButton?.addEventListener("click", () => void saveApiKey());
  clearButton?.addEventListener("click", () => void clearApiKey());
  openAudioPreferencesButton?.addEventListener("click", () => {
    postMessage({ type: "openAudioPreferences" });
    appendLog("openAudioPreferences → requested");
  });
  initDiagnosticsToggle();
  initInterfaceCalibrationControls();
  initEquipmentTabs();
  initLibraryFilters();
  initThemeSelect();
  initLibraryTabs();

  refreshSettingsView();
  initTone3000Browser();
}

function initEquipmentTabs(): void {
  if (equipmentTabsInitialized) {
    return;
  }
  equipmentTabsInitialized = true;
  equipmentTabButtons.forEach((button) => {
    button.addEventListener("click", () => {
      const tabId = (button as HTMLElement).dataset.equipmentTab ?? "settings";
      activateEquipmentTab(tabId);
    });
  });
  activateEquipmentTab("settings");
}

export function initThemeSelect(): void {
  if (!themeSelect || themeSelectInitialized) {
    return;
  }
  themeSelectInitialized = true;

  const themes: Array<{ value: ThemeName; label: string }> = [
    { value: "light", label: "Light" },
    { value: "dark", label: "Dark" },
    { value: "classic", label: "70s" },
  ];

  themeSelect.innerHTML = themes
    .map((theme) => `<option value="${theme.value}">${theme.label}</option>`)
    .join("");

  themeSelect.value = themeSwitcher.getCurrentTheme();

  themeSelect.addEventListener("change", () => {
    const value = themeSelect.value as ThemeName;
    themeSwitcher.setTheme(value);
  });

  window.addEventListener("themeChanged", ((event: CustomEvent) => {
    themeSelect.value = event.detail.theme as ThemeName;
  }) as EventListener);
}

function activateEquipmentTab(tabId: string): void {
  equipmentTabButtons.forEach((button) => {
    const isActive = (button as HTMLElement).dataset.equipmentTab === tabId;
    button.classList.toggle("active", isActive);
  });

  equipmentTabPanels.forEach((panel) => {
    const isMatch = (panel as HTMLElement).id === `equipment-tab-${tabId}`;
    panel.classList.toggle("active", isMatch);
  });

  if (tabId === "performance") {
    updateDSPPerformancePlot();
  }
}

function initLibraryFilters(): void {
  if (libraryFiltersInitialized) {
    return;
  }
  libraryFiltersInitialized = true;

  librarySearchInput?.addEventListener("input", () => renderLibraryView());
  libraryTypeSelect?.addEventListener("change", () => renderLibraryView());
  librarySourceSelect?.addEventListener("change", () => renderLibraryView());
  libraryViewSelect?.addEventListener("change", () => renderLibraryView());
  libraryCategorySelect?.addEventListener("change", () => renderLibraryView());
}

function initLibraryTabs(): void {
  if (libraryTabsInitialized) {
    return;
  }
  libraryTabsInitialized = true;

  libraryTabButtons.forEach((button) => {
    button.addEventListener("click", () => {
      const tabId = (button as HTMLElement).dataset.libraryTab ?? "tone3000";
      activateLibraryTab(tabId);
    });
  });

  activateLibraryTab("tone3000");
}

function activateLibraryTab(tabId: string): void {
  libraryTabButtons.forEach((button) => {
    const isActive = (button as HTMLElement).dataset.libraryTab === tabId;
    button.classList.toggle("active", isActive);
  });

  libraryTabPanels.forEach((panel) => {
    const isMatch = (panel as HTMLElement).id === `library-tab-${tabId}`;
    panel.classList.toggle("active", isMatch);
  });
}

export function initDiagnosticsToggle(): void {
  if (!diagnosticsToggle) {
    return;
  }

  if (diagnosticsToggle.dataset.bound === "true") {
    return;
  }

  diagnosticsToggle.dataset.bound = "true";
  diagnosticsToggle.addEventListener("change", () => void updateDiagnosticsSetting());
}

export function refreshSettingsView(): void {
  const stored = getSettingValue(API_KEY_SETTING);
  if (apiKeyInput) {
    apiKeyInput.value = "";
    apiKeyInput.placeholder = stored ? "API key stored" : "Enter your Tone3000 API key";
  }
  if (diagnosticsToggle) {
    diagnosticsToggle.checked = Boolean(getSettingValue(DIAGNOSTICS_SETTING));
  }
  const interfaceEnabledSetting = getSettingValue(INTERFACE_CALIBRATION_ENABLED_SETTING);
  const interfaceEnabled = interfaceEnabledSetting === null ? true : Boolean(interfaceEnabledSetting);
  if (interfaceCalibrationToggle) {
    interfaceCalibrationToggle.checked = interfaceEnabled;
  }
  if (interfaceCalibrationReferenceInput) {
    const referenceValue = Number(getSettingValue(INTERFACE_CALIBRATION_REFERENCE_SETTING));
    const resolvedValue = Number.isFinite(referenceValue) ? referenceValue : 12.0;
    interfaceCalibrationReferenceInput.value = resolvedValue.toFixed(1);
    interfaceCalibrationReferenceInput.disabled = !interfaceEnabled;
  }
  if (themeSelect) {
    themeSelect.value = themeSwitcher.getCurrentTheme();
  }
  updateSessionStatus();
  updateSignalDiagnosticsView();
  renderLibraryView();
}

async function saveApiKey(): Promise<void> {
  const apiKey = apiKeyInput?.value.trim() ?? "";
  if (!apiKey) {
    showNotification("Tone3000 API key required");
    return;
  }

  uiState.appSettings[API_KEY_SETTING] = apiKey;
  setAppSetting(API_KEY_SETTING, apiKey);
  appendLog("tone3000 api key saved");

  await handleAppSettingUpdate(API_KEY_SETTING, apiKey);
  updateSessionStatus();
}

async function clearApiKey(): Promise<void> {
  uiState.appSettings[API_KEY_SETTING] = null;
  setAppSetting(API_KEY_SETTING, null);
  if (apiKeyInput) {
    apiKeyInput.value = "";
  }

  await handleAppSettingUpdate(API_KEY_SETTING, null);
  updateSessionStatus();
}

function updateDiagnosticsSetting(): void {
  const enabled = Boolean(diagnosticsToggle?.checked);
  uiState.appSettings[DIAGNOSTICS_SETTING] = enabled;
  setAppSetting(DIAGNOSTICS_SETTING, enabled);
  if (!enabled) {
    uiState.signalDiagnostics = null;
  }
  updateSignalDiagnosticsView();
}

function initInterfaceCalibrationControls(): void {
  if (interfaceCalibrationToggle && interfaceCalibrationToggle.dataset.bound !== "true") {
    interfaceCalibrationToggle.dataset.bound = "true";
    interfaceCalibrationToggle.addEventListener("change", () => updateInterfaceCalibrationSettings());
  }
  if (interfaceCalibrationReferenceInput && interfaceCalibrationReferenceInput.dataset.bound !== "true") {
    interfaceCalibrationReferenceInput.dataset.bound = "true";
    interfaceCalibrationReferenceInput.addEventListener("change", () => updateInterfaceCalibrationSettings());
  }
}

function updateInterfaceCalibrationSettings(): void {
  const enabled = Boolean(interfaceCalibrationToggle?.checked ?? true);
  const rawReference = Number(interfaceCalibrationReferenceInput?.value ?? 12);
  const reference = Number.isFinite(rawReference) ? rawReference : 12.0;

  uiState.appSettings[INTERFACE_CALIBRATION_ENABLED_SETTING] = enabled;
  uiState.appSettings[INTERFACE_CALIBRATION_REFERENCE_SETTING] = reference;
  setAppSetting(INTERFACE_CALIBRATION_ENABLED_SETTING, enabled);
  setAppSetting(INTERFACE_CALIBRATION_REFERENCE_SETTING, reference);

  if (interfaceCalibrationReferenceInput) {
    interfaceCalibrationReferenceInput.disabled = !enabled;
    interfaceCalibrationReferenceInput.value = reference.toFixed(1);
  }
}

function updateSessionStatus(): void {
  if (!sessionStatus) return;

  if (!uiState.appSettings[API_KEY_SETTING]) {
    sessionStatus.textContent = "No API key saved.";
    return;
  }

  const session = uiState.tone3000Session;
  if (!session) {
    sessionStatus.textContent = "Session not started.";
    return;
  }

  const remainingSeconds = Math.max(0, Math.floor((session.expiresAt - Date.now()) / 1000));
  sessionStatus.textContent = `Session active. Expires in ${remainingSeconds}s.`;
}

type LibraryItem = {
  type: string;
  id: string;
  name: string;
  category: string;
  description: string;
  filePath: string;
  metadata?: Record<string, string>;
};

function getLibraryItems(): LibraryItem[] {
  const items: LibraryItem[] = [];
  const builtInModels = getAudioFxLibrary();
  const builtInIrs = getIrLibrary();

  builtInModels.forEach((model) => {
    items.push({
      type: "nam",
      id: model.id,
      name: humanizeId(model.id),
      category: "Built-in",
      description: "",
      filePath: model.filePath,
      metadata: { source: "built-in" },
    });
  });

  builtInIrs.forEach((ir) => {
    items.push({
      type: "ir",
      id: ir.id,
      name: humanizeId(ir.id),
      category: "Built-in",
      description: "",
      filePath: ir.filePath,
      metadata: { source: "built-in" },
    });
  });

  const library = uiState.resourceLibrary ?? {};
  Object.entries(library).forEach(([type, resources]) => {
    if (!Array.isArray(resources)) {
      return;
    }
    resources.forEach((res) => {
      if (!res || typeof res !== "object") {
        return;
      }
      const entry = res as import("./types.js").LibraryResource;
      items.push({
        type,
        id: entry.id ?? "",
        name: entry.name ?? entry.id ?? "",
        category: entry.category ?? "Imported",
        description: entry.description ?? "",
        filePath: entry.filePath ?? "",
        metadata: entry.metadata ?? { source: "imported" },
      });
    });
  });
  return dedupeLibraryItems(items);
}

function renderLibraryView(): void {
  if (!libraryResults) {
    return;
  }

  const allItems = getLibraryItems();
  const query = (librarySearchInput?.value ?? "").trim().toLowerCase();
  const typeFilter = libraryTypeSelect?.value ?? "all";
  const sourceFilter = librarySourceSelect?.value ?? "all";
  const viewMode = libraryViewSelect?.value ?? "list";

  const typeFilteredItems = typeFilter === "all"
    ? allItems
    : allItems.filter((item) => item.type === typeFilter);

  updateCategoryOptions(typeFilteredItems);
  const categoryFilter = libraryCategorySelect?.value ?? "all";

  let filtered = typeFilteredItems;
  if (categoryFilter !== "all") {
    filtered = filtered.filter((item) => item.category === categoryFilter);
  }

  if (sourceFilter !== "all") {
    filtered = filtered.filter((item) => {
      const origin = inferResourceOrigin(item.filePath).toLowerCase();
      return sourceFilter === "imported" ? origin === "imported" : origin === "built-in";
    });
  }

  if (query) {
    filtered = filtered.filter((item) => {
      const haystack = `${item.name} ${item.id} ${item.category} ${item.description} ${item.filePath}`.toLowerCase();
      return haystack.includes(query);
    });
  }

  if (viewMode === "grouped") {
    renderGroupedLibraryView(filtered, allItems.length);
    return;
  }

  renderListLibraryView(filtered, allItems.length);
}

function renderListLibraryView(filtered: LibraryItem[], totalCount: number): void {
  if (!libraryResults) {
    return;
  }

  if (librarySummary) {
    librarySummary.textContent = `Showing ${filtered.length} of ${totalCount} resources`;
  }

  if (!filtered.length) {
    libraryResults.innerHTML = `<div class="equipment-library-empty">No resources match the current filters.</div>`;
    return;
  }

  libraryResults.innerHTML = filtered
    .map((item) => renderLibraryItemRow(item))
    .join("");
}

function renderGroupedLibraryView(filtered: LibraryItem[], totalCount: number): void {
  if (!libraryResults) {
    return;
  }

  const grouped = groupLibraryItemsByTone(filtered);
  if (librarySummary) {
    librarySummary.textContent = `Showing ${grouped.length} of ${totalCount} resources (grouped)`;
  }

  if (!grouped.length) {
    libraryResults.innerHTML = `<div class="equipment-library-empty">No grouped resources match the current filters.</div>`;
    return;
  }

  libraryResults.innerHTML = grouped
    .map((group) => {
      const typeLabel = group.types.size === 1
        ? Array.from(group.types)[0]
        : "Mixed";
      const categoryLabel = group.categories.size === 1
        ? Array.from(group.categories)[0]
        : "Multiple";
      const originLabel = group.origins.size === 1
        ? Array.from(group.origins)[0]
        : "Mixed";
      const modelCountLabel = `${group.count} models`;
      return `
        <div class="equipment-library-item equipment-library-group" draggable="true" data-group-id="${escapeHtml(group.groupId)}">
          <div class="equipment-library-item-main">
            <div class="equipment-library-item-title">${escapeHtml(group.title)}</div>
            <div class="equipment-library-item-meta">
              <span>${escapeHtml(typeLabel)}</span>
              <span>${escapeHtml(categoryLabel)}</span>
              <span>${escapeHtml(originLabel)}</span>
              <span>${escapeHtml(group.groupId)}</span>
              <span>${escapeHtml(modelCountLabel)}</span>
            </div>
          </div>
          <div class="equipment-library-item-actions">
            <button class="equipment-library-create-blend" data-group-id="${escapeHtml(group.groupId)}">Create Blend</button>
          </div>
        </div>
      `;
    })
    .join("");

  bindBlendCreateButtons(grouped);
  bindBlendGroupDragHandlers(grouped);
}

function renderLibraryItemRow(item: LibraryItem): string {
  const typeLabel = item.type === "ir" ? "Cab IR" : item.type === "nam" ? "Amp Model" : item.type.toUpperCase();
  const categoryLabel = item.category ? item.category : "Uncategorized";
  const originLabel = inferResourceOrigin(item.filePath);
  const metadataBadges = buildMetadataBadges(item.metadata);
  return `
    <div class="equipment-library-item">
      <div class="equipment-library-item-main">
        <div class="equipment-library-item-title">${escapeHtml(item.name || item.id)}</div>
        <div class="equipment-library-item-meta">
          <span>${escapeHtml(typeLabel)}</span>
          <span>${escapeHtml(categoryLabel)}</span>
          <span>${escapeHtml(originLabel)}</span>
          <span>${escapeHtml(item.id)}</span>
          ${metadataBadges}
        </div>
      </div>
    </div>
  `;
}

type ToneGroup = {
  groupId: string;
  title: string;
  count: number;
  types: Set<string>;
  categories: Set<string>;
  origins: Set<string>;
  modelIds: string[];
  gear?: string;
};

function groupLibraryItemsByTone(items: LibraryItem[]): ToneGroup[] {
  const groups = new Map<string, ToneGroup>();
  items.forEach((item) => {
    const toneId = item.metadata?.toneId ?? item.metadata?.groupId;
    const toneTitle = item.metadata?.toneTitle ?? item.metadata?.groupName;
    if (!toneId || !toneTitle) {
      return;
    }

    const key = `${toneId}:${toneTitle}`;
    const existing = groups.get(key);
    const origin = inferResourceOrigin(item.filePath);
    const gear = item.metadata?.gear ?? item.category;
    if (existing) {
      existing.count += 1;
      existing.types.add(item.type);
      existing.categories.add(item.category || "Uncategorized");
      existing.origins.add(origin);
      existing.modelIds.push(item.id);
      if (!existing.gear && gear) {
        existing.gear = gear;
      }
    } else {
      groups.set(key, {
        groupId: toneId,
        title: toneTitle,
        count: 1,
        types: new Set([item.type]),
        categories: new Set([item.category || "Uncategorized"]),
        origins: new Set([origin]),
        modelIds: [item.id],
        gear: gear,
      });
    }
  });

  return Array.from(groups.values()).sort((a, b) => a.title.localeCompare(b.title));
}

function bindBlendCreateButtons(groups: ToneGroup[]): void {
  const buttons = libraryResults?.querySelectorAll(".equipment-library-create-blend");
  if (!buttons) {
    return;
  }

  buttons.forEach((btn) => {
    btn.addEventListener("click", () => {
      const groupId = (btn as HTMLElement).dataset.groupId ?? "";
      const group = groups.find((entry) => entry.groupId === groupId);
      if (!group) {
        return;
      }
      createBlendFromGroup(group);
    });
  });
}

function bindBlendGroupDragHandlers(groups: ToneGroup[]): void {
  const items = libraryResults?.querySelectorAll(".equipment-library-group") as NodeListOf<HTMLElement> | null;
  if (!items) {
    return;
  }

  items.forEach((item) => {
    item.addEventListener("dragstart", (event: DragEvent) => {
      const groupId = item.dataset.groupId ?? "";
      const group = groups.find((entry) => entry.groupId === groupId);
      if (!group || !event.dataTransfer) {
        return;
      }

      const payload = {
        groupId: group.groupId,
        title: group.title,
        category: normalizeBlendCategory(group.gear),
        modelIds: group.modelIds,
        modelMappings: buildBlendModelMappingsFromIds(group.modelIds, uiState.resourceLibrary),
      };
      event.dataTransfer.setData("application/x-resource-group", JSON.stringify(payload));
      event.dataTransfer.effectAllowed = "copy";
      item.classList.add("dragging");
      document.body.classList.add("fx-dragging");
    });

    item.addEventListener("dragend", () => {
      item.classList.remove("dragging");
      document.body.classList.remove("fx-dragging");
    });
  });
}

function createBlendFromGroup(group: ToneGroup): void {
  const id = typeof crypto !== "undefined" && "randomUUID" in crypto
    ? crypto.randomUUID()
    : `${Date.now()}-${Math.random().toString(16).slice(2)}`;
  const name = prompt("Blend name", group.title) ?? group.title;
  if (!name.trim()) {
    return;
  }

  const snapMode = confirm("Snap between models? Click OK for snap, Cancel for interpolate.");

  const category = normalizeBlendCategory(group.gear);
  const modelMappings = buildBlendModelMappingsFromIds(group.modelIds, uiState.resourceLibrary);
  const blend = {
    id,
    name: name.trim(),
    category,
    models: modelMappings.map((mapping) => mapping.id),
    modelMappings,
    blendMode: snapMode ? "snap" : "interpolate",
  };

  postMessage({
    type: "saveBlendDefinition",
    blend,
  });
}

function normalizeBlendCategory(category?: string): string {
  const value = (category ?? "").toLowerCase();
  const allowed = new Set(["pedal", "preamp", "amp", "full-rig", "cab"]);
  if (allowed.has(value)) {
    return value;
  }
  return "amp";
}

function updateCategoryOptions(items: LibraryItem[]): void {
  if (!libraryCategorySelect) {
    return;
  }

  const categories = Array.from(
    new Set(items.map((item) => item.category).filter((value) => value && value.trim().length > 0)),
  ).sort((a, b) => a.localeCompare(b));

  const previousSelection = libraryCategorySelect.value;
  const options = ["<option value=\"all\">All Categories</option>"]
    .concat(categories.map((category) => `
      <option value="${escapeHtml(category)}">${escapeHtml(category)}</option>
    `.trim()))
    .join("");

  libraryCategorySelect.innerHTML = options;
  if (previousSelection !== "all" && categories.includes(previousSelection)) {
    libraryCategorySelect.value = previousSelection;
  } else {
    libraryCategorySelect.value = "all";
  }
}

function inferResourceOrigin(filePath: string): string {
  if (!filePath) {
    return "Unknown";
  }
  const normalized = filePath.toLowerCase().replace(/\\/g, "/");
  if (normalized.includes("/settings/") || normalized.includes("/appdata/") || normalized.includes("/documents/")) {
    return "Imported";
  }
  return "Built-in";
}

function buildMetadataBadges(metadata?: Record<string, string>): string {
  if (!metadata) {
    return "";
  }

  const badges: string[] = [];
  const provider = metadata.provider;
  const toneTitle = metadata.toneTitle;
  const groupId = metadata.groupId;
  const groupName = metadata.groupName;
  const gear = metadata.gear;
  const modelName = metadata.modelName;
  const entryName = metadata.entryName;

  if (provider) badges.push(`<span>${escapeHtml(provider)}</span>`);
  if (toneTitle) badges.push(`<span>tone: ${escapeHtml(toneTitle)}</span>`);
  if (groupName) badges.push(`<span>group: ${escapeHtml(groupName)}</span>`);
  if (groupId) badges.push(`<span>groupId: ${escapeHtml(groupId)}</span>`);
  if (gear) badges.push(`<span>gear: ${escapeHtml(gear)}</span>`);
  if (modelName) badges.push(`<span>model: ${escapeHtml(modelName)}</span>`);
  if (entryName) badges.push(`<span>file: ${escapeHtml(entryName)}</span>`);

  return badges.join("");
}

function dedupeLibraryItems(items: LibraryItem[]): LibraryItem[] {
  const map = new Map<string, LibraryItem>();
  items.forEach((item) => {
    const key = `${item.type}:${item.id}`;
    const existing = map.get(key);
    if (!existing) {
      map.set(key, item);
      return;
    }
    const existingOrigin = inferResourceOrigin(existing.filePath);
    const currentOrigin = inferResourceOrigin(item.filePath);
    if (existingOrigin === "Built-in" && currentOrigin === "Imported") {
      map.set(key, item);
    }
  });
  return Array.from(map.values());
}

function humanizeId(value: string): string {
  if (!value) {
    return "Resource";
  }
  return value
    .replace(/[_-]+/g, " ")
    .replace(/\s+/g, " ")
    .trim()
    .replace(/\b\w/g, (char) => char.toUpperCase());
}

function escapeHtml(value: string): string {
  return value
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/\"/g, "&quot;")
    .replace(/'/g, "&#039;");
}

function getSettingValue(key: string): AppSettingValue {
  return uiState.appSettings?.[key] ?? null;
}

export function updateSettingsSessionStatus(): void {
  updateSessionStatus();
}
