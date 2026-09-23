/**
 * Incoming messages from the native host — the routing table and nothing else.
 *
 * Every message type maps to one handler, and the handlers are grouped by what
 * they change in ./messages/. Keeping the table here means the whole protocol
 * surface is readable in one place: if a message type is not in this map, the UI
 * does not react to it.
 */

import { Features, isFeatureEnabled } from "./featureFlags.js";
import { onAudioDeviceLevels, onAudioDeviceState } from "./messages/audioDeviceHandlers.js";
import { onDemoAudioRenderFailed, onDemoAudioRenderSaved, onMetronomeBeat, onPracticeToolFileLoaded, onPracticeToolPlaybackEnded, onPracticeToolTransportState, onPreviewComplete, onPreviewStarted, onPreviewStopped, onRiffCaptureCanceled, onRiffCaptureProgress, onRiffCaptureStarted, onRiffCaptureStopped, onRiffLibraryState, onRiffSaved } from "./messages/captureHandlers.js";
import { onAutomation, onMidiLearnCapture, onMidiLog } from "./messages/controlSurfaceHandlers.js";
import { DEBUG_SNAPSHOT_SKIP_TYPES, onCaptureDebugSnapshot, onDebugSnapshotWritten, scheduleUiDebugSnapshot } from "./messages/debugSnapshot.js";
import { onCompositeDefinitionAdded, onCompositeDefinitionRemoved, onCompositeEditModeExited, onCompositeEditState, onCompositeLibrary, onCompositePresetList, onCompositePresetLoaded, onCompositePresetSaved, onCustomEffectLibrary, onCustomEffectSaved, onEffectCatalog, onEffectResponse, onGeneratedCustomEffectBundleExportFailed, onGeneratedCustomEffectBundleExportSaved, onSimpleCabIrMatch } from "./messages/effectHandlers.js";
import { onLayoutExportFailed, onLayoutExportSaved, onLayoutImageSelected, onLayoutImagesLoaded, onLayoutLibraryLoaded, onLayoutSaved } from "./messages/layoutHandlers.js";
import { onNavigateToToneSharingDeepLink } from "./messages/mixerHandlers.js";
import { onEffectPresets, onPresetArchiveSessionEnded, onPresetArchiveSessionFailed, onPresetArchiveSessionStarted, onPresetData, onPresetDirtyChanged, onPresetExportFailed, onPresetExportSaved, onPresetFavorites, onPresetFolders, onPresetList, onPresetLoaded, onPresetRatings, onPresetRecents, onPresetSaved, onSetlistCursorChanged, onSetlists } from "./messages/presetHandlers.js";
import { onBlendExportFailed, onBlendExportSaved, onHostedPluginResourceLoadCompleted, onHostedPluginResourceLoadFailed, onIrLoaded, onLibraryExportFailed, onLibraryExportSaved, onModelLoaded, onNodeResourceBrowseCancelled, onResourceCleanupResult, onResourceData, onResourceDataFailed, onResourceDeleteFailed, onResourceFolderListing, onResourceFolderListingFailed, onResourceFolderMetadata, onResourceFolderPicked, onResourceImported, onResourceImportFailed, onResourceRemoved, onResourceUsageInfo, onToneSharingPackDeleteFailed } from "./messages/resourceHandlers.js";
import { onSharedSyncState, onSharedSyncUpdated } from "./messages/sharedSync.js";
import { onGlobalSignalChainChanged, onSignalPathNodeConfigUpdated, onSignalPathNodeParamUpdated, onSignalPathTestResult, onSpatialPosition } from "./messages/signalPathHandlers.js";
import { onAmpCabStateChanged, onAppInfo, onAppSettingChanged, onError, onInputModeChanged, onOutputMutedChanged, onState, onTheme } from "./messages/stateHandlers.js";
import { onDspPerformance, onSld, onSldA, onSldRoster, onSldS } from "./messages/telemetry.js";
import { onTunerLiveModeChanged, onTunerReferenceChanged, onTunerStarted, onTunerStopped, onTunerUpdate } from "./messages/tunerHandlers.js";
import type { MessageHandler } from "./messages/types.js";

export { handleMixerStateMessage } from "./messages/mixerHandlers.js";

/**
 * Every message the backend can send, and what handles it.
 *
 * This replaced a single 1,451-line `switch (type)`. Each arm is now an
 * independently readable — and independently testable — function.
 */
const MESSAGE_HANDLERS: Record<string, MessageHandler> = {
  "state": onState,
  "metronomeBeat": onMetronomeBeat,
  "riffCaptureProgress": onRiffCaptureProgress,
  "riffCaptureStarted": onRiffCaptureStarted,
  "riffCaptureStopped": onRiffCaptureStopped,
  "riffCaptureCanceled": onRiffCaptureCanceled,
  "riffSaved": onRiffSaved,
  "riffLibraryState": onRiffLibraryState,
  "practiceToolFileLoaded": onPracticeToolFileLoaded,
  "practiceToolTransportState": onPracticeToolTransportState,
  "practiceToolPlaybackEnded": onPracticeToolPlaybackEnded,
  "resourceCleanupResult": onResourceCleanupResult,
  "presetLoaded": onPresetLoaded,
  "presetDirtyChanged": onPresetDirtyChanged,
  "signalPathTestResult": onSignalPathTestResult,
  "previewStarted": onPreviewStarted,
  "previewComplete": onPreviewComplete,
  "previewStopped": onPreviewStopped,
  "demoAudioRenderSaved": onDemoAudioRenderSaved,
  "demoAudioRenderFailed": onDemoAudioRenderFailed,
  "error": onError,
  "modelLoaded": onModelLoaded,
  "irLoaded": onIrLoaded,
  "resourceImported": onResourceImported,
  "resourceImportFailed": onResourceImportFailed,
  "resourceRemoved": onResourceRemoved,
  "toneSharingPackDeleteFailed": onToneSharingPackDeleteFailed,
  "resourceDeleteFailed": onResourceDeleteFailed,
  "resourceUsageInfo": onResourceUsageInfo,
  "resourceFolderPicked": onResourceFolderPicked,
  "resourceFolderListing": onResourceFolderListing,
  "resourceFolderMetadata": onResourceFolderMetadata,
  "resourceFolderListingFailed": onResourceFolderListingFailed,
  "hostedPluginResourceLoadFailed": onHostedPluginResourceLoadFailed,
  "hostedPluginResourceLoadCompleted": onHostedPluginResourceLoadCompleted,
  "nodeResourceBrowseCancelled": onNodeResourceBrowseCancelled,
  "resourceData": onResourceData,
  "resourceDataFailed": onResourceDataFailed,
  "blendExportSaved": onBlendExportSaved,
  "blendExportFailed": onBlendExportFailed,
  "libraryExportSaved": onLibraryExportSaved,
  "libraryExportFailed": onLibraryExportFailed,
  "presetExportSaved": onPresetExportSaved,
  "presetExportFailed": onPresetExportFailed,
  "presetSaved": onPresetSaved,
  "presetArchiveSessionStarted": onPresetArchiveSessionStarted,
  "presetArchiveSessionEnded": onPresetArchiveSessionEnded,
  "presetArchiveSessionFailed": onPresetArchiveSessionFailed,
  "presetList": onPresetList,
  "appInfo": onAppInfo,
  "appSettingChanged": onAppSettingChanged,
  "outputMutedChanged": onOutputMutedChanged,
  "audioDeviceState": onAudioDeviceState,
  "audioDeviceLevels": onAudioDeviceLevels,
  "sharedSyncUpdated": onSharedSyncUpdated,
  "sharedSyncState": onSharedSyncState,
  "presetData": onPresetData,
  "presetFolders": onPresetFolders,
  "presetFavorites": onPresetFavorites,
  "presetRatings": onPresetRatings,
  "presetRecents": onPresetRecents,
  "setlists": onSetlists,
  "effectPresets": onEffectPresets,
  "setlistCursorChanged": onSetlistCursorChanged,
  "automation": onAutomation,
  "midiLog": onMidiLog,
  "midiLearnCapture": onMidiLearnCapture,
  "theme": onTheme,
  "tunerUpdate": onTunerUpdate,
  "tunerStarted": onTunerStarted,
  "tunerStopped": onTunerStopped,
  "tunerReferenceChanged": onTunerReferenceChanged,
  "tunerLiveModeChanged": onTunerLiveModeChanged,
  "captureDebugSnapshot": onCaptureDebugSnapshot,
  "debugSnapshotWritten": onDebugSnapshotWritten,
  "inputModeChanged": onInputModeChanged,
  "ampCabStateChanged": onAmpCabStateChanged,
  "dspPerformance": onDspPerformance,
  "sldRoster": onSldRoster,
  "sld": onSld,
  "sldA": onSldA,
  "sldS": onSldS,
  "spatialPosition": onSpatialPosition,
  "signalPathNodeConfigUpdated": onSignalPathNodeConfigUpdated,
  "signalPathNodeParamUpdated": onSignalPathNodeParamUpdated,
  "globalChain": onGlobalSignalChainChanged,
  "layoutLibraryLoaded": onLayoutLibraryLoaded,
  "layoutSaved": onLayoutSaved,
  "layoutImagesLoaded": onLayoutImagesLoaded,
  "layoutImageSelected": onLayoutImageSelected,
  "layoutExportSaved": onLayoutExportSaved,
  "layoutExportFailed": onLayoutExportFailed,
  "compositeLibrary": onCompositeLibrary,
  "customEffectLibrary": onCustomEffectLibrary,
  "customEffectSaved": onCustomEffectSaved,
  "generatedCustomEffectBundleExportSaved": onGeneratedCustomEffectBundleExportSaved,
  "generatedCustomEffectBundleExportFailed": onGeneratedCustomEffectBundleExportFailed,
  "effectCatalog": onEffectCatalog,
  "effectResponse": onEffectResponse,
  "simpleCabIrMatch": onSimpleCabIrMatch,
  "compositeDefinitionAdded": onCompositeDefinitionAdded,
  "compositeDefinitionRemoved": onCompositeDefinitionRemoved,
  "compositeEditState": onCompositeEditState,
  "compositeEditModeExited": onCompositeEditModeExited,
  "compositePresetList": onCompositePresetList,
  "compositePresetSaved": onCompositePresetSaved,
  "compositePresetLoaded": onCompositePresetLoaded,
  "navigateToToneSharingDeepLink": onNavigateToToneSharingDeepLink,
};

export function handleIncomingMessage(message: string): void {
  const payload = JSON.parse(message) as Record<string, unknown>;
  const type = typeof payload.type === "string" ? payload.type : "";
  // Frequent diagnostics messages; avoid spamming console.
  if (type !== "dspPerformance" && type !== "sld" && type !== "sldA" && type !== "sldS" && type !== "sldRoster" && type !== "spatialPosition" && type !== "audioDeviceLevels") {
    console.log("[JS] handleIncomingMessage received:", message.substring(0, 200));
    console.log("[JS] Parsed message type:", type);
  }

  const handler = MESSAGE_HANDLERS[type];
  if (handler) {
    handler(payload);
  } else {
    console.warn("Unknown message type", payload.type);
  }

  // Building this snapshot serializes the whole of uiState — resource library, preset cache
  // and all — which is tens of MB, on the main thread, and then ships it over the bridge for
  // the backend to write to disk. It is a diagnostic for the Debug State Capture feature, so
  // it must not run at all unless that feature is switched on.
  if (isFeatureEnabled(Features.DebugStateCapture) && !DEBUG_SNAPSHOT_SKIP_TYPES.has(type)) {
    scheduleUiDebugSnapshot(`incoming:${type}`);
  }
}
