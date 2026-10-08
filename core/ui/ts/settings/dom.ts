/**
 * Every element the settings panel talks to, looked up once.
 *
 * The lookups run at import time, which is safe: dist/main.js is a module script
 * at the end of <body>, so the document is already parsed and all of this markup
 * is static, assembled from ui-components/.
 */

export const apiKeyInput = document.getElementById("tone3000-api-key-input") as HTMLInputElement | null;

export const saveButton = document.getElementById("tone3000-api-key-save");

export const clearButton = document.getElementById("tone3000-api-key-clear");

export const userInputCalibrationToolbarTrigger = document.getElementById("user-input-calibration-toolbar-trigger") as HTMLButtonElement | null;

export const userInputCalibrationToolbarMenu = document.getElementById("user-input-calibration-toolbar-menu") as HTMLDivElement | null;

export const userInputCalibrationProfileSelect = document.getElementById("user-input-calibration-profile") as HTMLSelectElement | null;

export const userInputCalibrationTrainButton = document.getElementById("user-input-calibration-train") as HTMLButtonElement | null;

export const userInputCalibrationDeleteButton = document.getElementById("user-input-calibration-delete") as HTMLButtonElement | null;

export const userInputCalibrationSummary = document.getElementById("user-input-calibration-summary") as HTMLElement | null;

export const userInputCalibrationModal = document.getElementById("user-input-calibration-modal") as HTMLDivElement | null;

export const userInputCalibrationCloseButton = document.getElementById("user-input-calibration-close") as HTMLButtonElement | null;

export const userInputCalibrationCancelButton = document.getElementById("user-input-calibration-cancel") as HTMLButtonElement | null;

export const userInputCalibrationSaveButton = document.getElementById("user-input-calibration-save") as HTMLButtonElement | null;

export const userInputCalibrationResetButton = document.getElementById("user-input-calibration-reset") as HTMLButtonElement | null;

export const userInputCalibrationNameInput = document.getElementById("user-input-calibration-name") as HTMLInputElement | null;

export const userInputCalibrationDescriptionInput = document.getElementById("user-input-calibration-description") as HTMLTextAreaElement | null;

export const userInputCalibrationLivePeak = document.getElementById("user-input-calibration-live-peak") as HTMLElement | null;

export const userInputCalibrationCapturedPeak = document.getElementById("user-input-calibration-captured-peak") as HTMLElement | null;

export const userInputCalibrationRecommendedTrim = document.getElementById("user-input-calibration-recommended-trim") as HTMLElement | null;

export const userInputCalibrationStatus = document.getElementById("user-input-calibration-status") as HTMLElement | null;

export const equipmentTabButtons = Array.from(document.querySelectorAll(".equipment-tab-btn"));

export const equipmentTabPanels = Array.from(document.querySelectorAll(".equipment-tab-panel"));

export const equipmentLibraryTabButton = document.querySelector('.equipment-tab-btn[data-equipment-tab="library"]') as HTMLElement | null;

export const themeSelect = document.getElementById("theme-select") as HTMLSelectElement | null;

export const zoomLevelSelect = document.getElementById("zoom-level-select") as HTMLSelectElement | null;

export const densityLevelSelect = document.getElementById("density-level-select") as HTMLSelectElement | null;

export const librarySearchInput = document.getElementById("equipment-library-search") as HTMLInputElement | null;

export const libraryTypeSelect = document.getElementById("equipment-library-type") as HTMLSelectElement | null;

export const librarySourceSelect = document.getElementById("equipment-library-source") as HTMLSelectElement | null;

export const libraryViewSelect = document.getElementById("equipment-library-view") as HTMLSelectElement | null;

export const libraryCategorySelect = document.getElementById("equipment-library-category") as HTMLSelectElement | null;

export const libraryCreatorSelect = document.getElementById("equipment-library-creator") as HTMLSelectElement | null;

export const libraryTagFilterBar = document.getElementById("equipment-library-tag-filters");

export const libraryCleanupSelect = document.getElementById("equipment-library-cleanup-scope") as HTMLSelectElement | null;

export const libraryCleanupButton = document.getElementById("equipment-library-cleanup-btn") as HTMLButtonElement | null;

export const libraryCleanupRow = document.getElementById("equipment-library-cleanup-row") as HTMLElement | null;

export const libraryResults = document.getElementById("equipment-library-results");

export const librarySummary = document.getElementById("equipment-library-summary");

export const libraryTabButtons = Array.from(document.querySelectorAll(".library-tab-btn"));

export const libraryTabPanels = Array.from(document.querySelectorAll(".library-tab-panel"));

export const libraryExportButton = document.getElementById("library-export-btn");

export const libraryExportResourcesSelect = document.getElementById("library-export-resources") as HTMLSelectElement | null;

export const featureGroupsContainer = document.getElementById("settings-feature-groups") as HTMLElement | null;

export const factoryArchiveLoadingToggle = document.getElementById("factory-archive-loading-toggle") as HTMLInputElement | null;

export const factoryArchiveExportEffectPresetsToggle = document.getElementById("factory-archive-export-effect-presets-toggle") as HTMLInputElement | null;

export const dspNominalLevelInput = document.getElementById("dsp-nominal-level-input") as HTMLInputElement | null;

export const dspProtectionCeilingInput = document.getElementById("dsp-protection-ceiling-input") as HTMLInputElement | null;

export const dspOutputLimiterToggle = document.getElementById("dsp-output-limiter-toggle") as HTMLInputElement | null;

export const presetSwitchTailSelect = document.getElementById("preset-switch-tail-select") as HTMLSelectElement | null;

export const namSlimmableSizeInput = document.getElementById("nam-slimmable-size-input") as HTMLInputElement | null;

export const namOversamplingSelect = document.getElementById("nam-oversampling-select") as HTMLSelectElement | null;

export const namAntiAliasPhaseSelect = document.getElementById("nam-anti-alias-phase-select") as HTMLSelectElement | null;

export const namInterfaceCalibrationLevelInput = document.getElementById("nam-interface-calibration-level-input") as HTMLInputElement | null;

export const namAutoInputCalibrationToggle = document.getElementById("nam-auto-input-calibration-toggle") as HTMLInputElement | null;

export const factoryArchiveLoadingRow = document.getElementById("factory-archive-loading-row") as HTMLElement | null;

export const factoryArchiveSettingsSection = document.getElementById("factory-archive-settings-section") as HTMLElement | null;

export const updateCheckToggle = document.getElementById("update-check-toggle") as HTMLInputElement | null;

export const channelModeControlToggle = document.getElementById("channel-mode-control-toggle") as HTMLInputElement | null;

export const tone3000UseSoundshedApiToggle = document.getElementById("tone3000-use-soundshed-api-toggle") as HTMLInputElement | null;

export const tone3000ApiKeyRow = document.getElementById("tone3000-api-key-row") as HTMLElement | null;

export const tone3000ProxyInfoHint = document.getElementById("tone3000-proxy-info-hint") as HTMLElement | null;

export const tone3000ApiModeStatus = document.getElementById("tone3000-api-mode-status") as HTMLElement | null;

export const tone3000ProxyHealthRow = document.getElementById("tone3000-proxy-health-row") as HTMLElement | null;

export const tone3000ProxyHealthCheckButton = document.getElementById("tone3000-proxy-health-check") as HTMLButtonElement | null;

export const tone3000ProxyHealthStatus = document.getElementById("tone3000-proxy-health-status") as HTMLElement | null;

export const advancedTabButton = document.querySelector('.library-tab-btn[data-library-tab="advanced"]') as HTMLElement | null;

export const tone3000TabButton = document.querySelector('.library-tab-btn[data-library-tab="tone3000"]') as HTMLElement | null;

export const resourceLibraryTabButton = document.querySelector('.library-tab-btn[data-library-tab="resources"]') as HTMLElement | null;

export const compositeTabButton = document.querySelector('.advanced-sub-tab-btn[data-advanced-tab="composites"]') as HTMLElement | null;

export const blendsTabButton = document.querySelector('.advanced-sub-tab-btn[data-advanced-tab="blends"]') as HTMLElement | null;

export const layoutsTabButton = document.querySelector('.advanced-sub-tab-btn[data-advanced-tab="layouts"]') as HTMLElement | null;

export const compositeTabPanel = document.getElementById("advanced-tab-composites") as HTMLElement | null;

export const blendsTabPanel = document.getElementById("advanced-tab-blends") as HTMLElement | null;

export const layoutsTabPanel = document.getElementById("advanced-tab-layouts") as HTMLElement | null;

export const tone3000SettingsHeading = document.getElementById("settings-tone3000-heading") as HTMLElement | null;

export const tone3000SettingsSection = document.getElementById("settings-tone3000-section") as HTMLElement | null;

export const libraryToolsHeading = document.getElementById("settings-library-tools-heading") as HTMLElement | null;

export const libraryToolsSection = document.getElementById("settings-library-tools-section") as HTMLElement | null;

export const sharingPanelButton = document.querySelector('.icon-btn[data-panel="sharing"]') as HTMLElement | null;

export const jamPanelButton = document.querySelector('.icon-btn[data-panel="jam"]') as HTMLElement | null;

export const sharingPanel = document.getElementById("panel-sharing") as HTMLElement | null;

export const jamPanel = document.getElementById("panel-jam") as HTMLElement | null;

export const jamPlayerDock = document.getElementById("jam-player-dock") as HTMLElement | null;

export const jamFloatingPlayerRoot = document.getElementById("jam-floating-player-root") as HTMLElement | null;

export const footerRiffRecordButton = document.getElementById("footer-riff-record-btn") as HTMLElement | null;

export const libraryActiveTagFilters = new Set<string>();
