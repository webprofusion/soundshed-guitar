/**
 * Walking a Tone3000 result set after the modal has closed.
 *
 * When a model is chosen from the Tone3000 tab, the list it came from is kept
 * here so the node's prev/next controls keep moving through it. Stepping to a
 * neighbour is not just an index change: that model may never have been
 * downloaded, so the step fetches the tone's models, imports the one it lands
 * on, and only then reports the resource id back.
 */

import { postMessage } from "../bridge.js";
import { showNotification } from "../notifications.js";
import { uiState } from "../state.js";
import type { Tone3000Architecture, Tone3000Model, Tone3000Tone } from "../tone3000ApiTypes.js";
import { findAdjacentTone3000Model, locateTone3000Position } from "../tone3000Navigation.js";
import type { Tone3000NavigationPosition } from "../tone3000Navigation.js";
import { isTone3000AuthReady, tone3000AuthenticatedFetch } from "../tone3000.js";
import { fetchTone3000Models, getTone3000ImageUrl } from "../tone3000Shared.js";
import { arrayBufferToBase64, findResourceById } from "../utils.js";
import { sanitizeFilename } from "./helpers.js";
import { DEFAULT_RESOURCE_CONTEXT_KEY } from "./settings.js";
import { slotImportCategory } from "./slot.js";
import type { NavigationCacheOptions, ResourceBrowserOptions, ResourceNavigationResult, ResourceType, Tone3000NavigationState } from "./types.js";

let nextImportRequestId = 0;

function importRemoteResource(payload: Record<string, unknown>): Promise<void> {
  const requestId = `tone3000-import-${++nextImportRequestId}`;
  return new Promise((resolve, reject) => {
    const cleanup = (): void => {
      clearTimeout(timeout);
      document.removeEventListener("resource-browser:resource-imported", onImported);
      document.removeEventListener("resource-browser:resource-import-failed", onFailed);
    };
    const onImported = (event: Event): void => {
      const detail = (event as CustomEvent<{ requestId?: string }>).detail;
      if (detail?.requestId !== requestId) return;
      cleanup();
      resolve();
    };
    const onFailed = (event: Event): void => {
      const detail = (event as CustomEvent<{ requestId?: string; message?: string }>).detail;
      if (detail?.requestId !== requestId) return;
      cleanup();
      reject(new Error(detail.message || "Resource import failed"));
    };
    const timeout = setTimeout(() => {
      cleanup();
      reject(new Error("Timed out waiting for resource import"));
    }, 120_000);
    document.addEventListener("resource-browser:resource-imported", onImported);
    document.addEventListener("resource-browser:resource-import-failed", onFailed);
    try {
      postMessage({ ...payload, type: "importRemoteResource", requestId });
    } catch (error) {
      cleanup();
      reject(error instanceof Error ? error : new Error(String(error)));
    }
  });
}

/**
 * What the navigator needs from the Tone3000 tab: the result set the user was
 * last looking at, and the models already fetched for those tones.
 */
export interface Tone3000NavigatorHost {
  getOptions(): ResourceBrowserOptions | null;
  getSelectedArchitecture(): Tone3000Architecture | null;
  getContextKey(): string;
  rememberNavigationView(contextKey: string, view: "library" | "folder" | "tone3000"): void;
  getNavigationView(contextKey: string): "library" | "folder" | "tone3000" | undefined;
  /** The tones currently listed in the tab. */
  getTones(): Tone3000Tone[];
  /** Models already fetched for a tone, keyed by tone id. */
  getModelsCache(): Map<string, Tone3000Model[]>;
}

export class Tone3000Navigator {
  constructor(private readonly host: Tone3000NavigatorHost) {}

  tone3000NavigationStates: Map<string, Tone3000NavigationState> = new Map();

  /// Imports land in the library asynchronously, so remember what navigation has
  /// already pulled down ("<resourceType>:<modelId>" -> resource id) and never
  /// re-download a model while stepping back and forth over a result set.
  tone3000ImportedResourceIds: Map<string, string> = new Map();

  async importTone3000Resource(
    tone: Tone3000Tone,
    modelId: string,
    modelName: string,
    architectureVersion: string,
    buffer: ArrayBuffer,
    isZip: boolean,
    resourceType: "nam" | "ir",
    /// The library category to file it under; Tone3000's gear when not given. An IR
    /// picked for a slot goes to that slot ("cab" or "reverb"), whatever its gear says.
    category?: string,
  ): Promise<string> {
    const gearFolder = sanitizeFilename(tone.gear ?? "other");
    const toneFolder = sanitizeFilename(tone.title ?? tone.name ?? "tone");
    const subfolder = `${gearFolder}/${toneFolder}`;
    
    if (isZip) {
      // Handle zip file
      const zipLib = window.JSZip;
      if (!zipLib) {
        throw new Error("JSZip not loaded");
      }
      
      const zip = await zipLib.loadAsync(buffer);
      const entries = Object.values(zip.files) as JSZipObject[];
      let firstImportedId = "";
      
      for (const entry of entries) {
        if (entry.dir) continue;
        const lowerName = entry.name.toLowerCase();
        const isNam = lowerName.endsWith(".nam") || lowerName.endsWith(".json");
        const isIr = lowerName.endsWith(".wav") || lowerName.endsWith(".ir");
        
        if ((resourceType === "nam" && !isNam) || (resourceType === "ir" && !isIr)) {
          continue;
        }
        
        const fileBuffer = await entry.async("arraybuffer");
        const data = arrayBufferToBase64(fileBuffer);
        const fileName = sanitizeFilename(entry.name.split("/").pop() ?? modelName);
        const resourceId = `tone3000:${modelId}:${sanitizeFilename(entry.name)}`;
        
        await importRemoteResource({
          provider: "tone3000",
          resourceType,
          resourceId,
          name: `${tone.title} - ${entry.name}`,
          description: tone.description ?? "",
          category: category ?? tone.gear ?? "",
          subfolder,
          fileName,
          metadata: {
            provider: "tone3000",
            toneId: String(tone.id),
            toneTitle: tone.title ?? "",
            groupId: String(tone.id),
            groupName: tone.title ?? tone.name ?? "",
            gear: tone.gear ?? "",
            platform: tone.platform ?? "",
            modelId: String(modelId),
            modelName: modelName ?? "",
            imageUrl: getTone3000ImageUrl(tone) ?? "",
            architectureVersion: architectureVersion ?? "",
            entryName: entry.name,
            sourceUrl: `https://www.tone3000.com/tones/${tone.slug ?? tone.id}`,
            creatorId: tone.user?.id != null ? String(tone.user.id) : "",
            creatorName: tone.user?.display_name ?? tone.user?.name ?? tone.user?.username ?? "",
            authorUsername: tone.user?.username ?? "",
          },
          data,
        });
        
        if (!firstImportedId) {
          firstImportedId = resourceId;
        }
      }
      
      if (!firstImportedId) {
        throw new Error("No supported files found in archive");
      }
      
      return firstImportedId;
    } else {
      // Single file
      const data = arrayBufferToBase64(buffer);
      const extension = resourceType === "ir" ? ".wav" : ".nam";
      const fileName = `${sanitizeFilename(modelName)}${extension}`;
      const resourceId = `tone3000:${modelId}`;
      
      await importRemoteResource({
        provider: "tone3000",
        resourceType,
        resourceId,
        name: `${tone.title} - ${modelName}`,
        description: tone.description ?? "",
        category: category ?? tone.gear ?? "",
        subfolder,
        fileName,
        metadata: {
          provider: "tone3000",
          toneId: String(tone.id),
          toneTitle: tone.title ?? "",
          groupId: String(tone.id),
          groupName: tone.title ?? tone.name ?? "",
          gear: tone.gear ?? "",
          platform: tone.platform ?? "",
          modelId: String(modelId),
          modelName: modelName ?? "",
          imageUrl: getTone3000ImageUrl(tone) ?? "",
          architectureVersion: architectureVersion ?? "",
          sourceUrl: `https://www.tone3000.com/tones/${tone.slug ?? tone.id}`,
          creatorId: tone.user?.id != null ? String(tone.user.id) : "",
          creatorName: tone.user?.display_name ?? tone.user?.name ?? tone.user?.username ?? "",
          authorUsername: tone.user?.username ?? "",
        },
        data,
      });
      
      return resourceId;
    }
  }

  /// Remembers the Tone3000 result set the user just picked from. Called on a
  /// successful select so navigation only follows a list the user actually chose
  /// from, not merely browsed.
  captureTone3000NavigationState(): void {
    const options = this.host.getOptions();
    if (!options || !this.host.getTones().length) {
      return;
    }

    this.tone3000NavigationStates.set(this.host.getContextKey(), {
      resourceType: options.resourceType,
      tones: [...this.host.getTones()],
      architecture: this.host.getSelectedArchitecture(),
      modelsByToneId: new Map(this.host.getModelsCache()),
      importCategory: options.resourceType === "ir" ? slotImportCategory(options, "") : undefined,
    });
    this.host.rememberNavigationView(this.host.getContextKey(), "tone3000");
    document.dispatchEvent(new CustomEvent("resource-browser:navigation-cache-updated", {
      detail: {
        resourceType: options.resourceType,
        view: "tone3000",
      },
    }));
  }

  /// True while next/prev for this context should walk a Tone3000 result set.
  /// Those steps are asynchronous (models are fetched and imported on demand),
  /// so callers must use `stepTone3000Resource` rather than the synchronous
  /// `getAdjacentResourceSelection`.
  public isTone3000NavigationActive(resourceType: ResourceType, options?: NavigationCacheOptions): boolean {
    const contextKey = options?.contextKey || DEFAULT_RESOURCE_CONTEXT_KEY;
    if (this.host.getNavigationView(contextKey) !== "tone3000") {
      return false;
    }

    const state = this.tone3000NavigationStates.get(contextKey);
    return Boolean(state && state.resourceType === resourceType && state.tones.length);
  }

  /**
   * Steps to the neighbouring model in the captured Tone3000 result set,
   * downloading and importing it if it is not in the library yet. Walks tone by
   * tone in result order and wraps around, so next/prev is always available.
   * Returns null when the step cannot be resolved (no session, fetch failure,
   * or a set with nowhere else to go).
   */
  public async stepTone3000Resource(
    resourceType: ResourceType,
    currentResourceId: string,
    offset: number,
    options?: NavigationCacheOptions,
  ): Promise<ResourceNavigationResult | null> {
    if (!this.isTone3000NavigationActive(resourceType, options)) {
      return null;
    }

    const contextKey = options?.contextKey || DEFAULT_RESOURCE_CONTEXT_KEY;
    const state = this.tone3000NavigationStates.get(contextKey);
    if (!state) {
      return null;
    }

    const direction = offset >= 0 ? 1 : -1;
    const position = this.locateTone3000Position(state, currentResourceId);
    const target = await findAdjacentTone3000Model(
      state.tones,
      position,
      direction,
      (tone) => this.ensureTone3000NavigationModels(state, tone),
    );
    if (!target || String(target.model.id) === position.modelId) {
      return null;
    }

    return this.resolveTone3000ModelResource(state, target.tone, target.model);
  }

  /// The tone/model ids of the loaded resource are recorded in import metadata;
  /// the resource id (`tone3000:<modelId>`) is the fallback for resources
  /// imported before that metadata existed.
  private locateTone3000Position(
    state: Tone3000NavigationState,
    currentResourceId: string,
  ): Tone3000NavigationPosition {
    const resource = findResourceById(uiState.resourceLibrary[state.resourceType] ?? [], currentResourceId);
    const metadata = resource?.metadata ?? {};
    const idParts = currentResourceId.startsWith("tone3000:") ? currentResourceId.split(":") : [];
    const modelId = (metadata.modelId ?? "").trim() || (idParts[1] ?? "");
    const toneId = (metadata.toneId ?? "").trim();

    return locateTone3000Position(state.tones, state.modelsByToneId, toneId, modelId);
  }

  private async ensureTone3000NavigationModels(
    state: Tone3000NavigationState,
    tone: Tone3000Tone,
  ): Promise<Tone3000Model[]> {
    const toneId = String(tone.id);
    const cached = state.modelsByToneId.get(toneId);
    if (cached) {
      return cached;
    }

    if (!isTone3000AuthReady()) {
      state.modelsByToneId.set(toneId, []);
      return [];
    }

    try {
      const models = await fetchTone3000Models(tone, state.architecture ?? undefined);
      state.modelsByToneId.set(toneId, models);
      return models;
    } catch {
      state.modelsByToneId.set(toneId, []);
      return [];
    }
  }

  /// Reuses the already-imported copy when there is one, so stepping back and
  /// forth over a set downloads each model at most once.
  private async resolveTone3000ModelResource(
    state: Tone3000NavigationState,
    tone: Tone3000Tone,
    model: Tone3000Model,
  ): Promise<ResourceNavigationResult | null> {
    const resourceType = state.resourceType;
    const modelId = String(model.id);
    const displayName = model.name?.trim() || tone.title?.trim() || modelId;

    const importedId = this.findImportedTone3000ResourceId(resourceType, modelId);
    if (importedId) {
      return { resourceId: importedId, displayName };
    }

    if (!isTone3000AuthReady()) {
      showNotification("Load failed", "No Tone3000 session");
      return null;
    }

    try {
      const response = await tone3000AuthenticatedFetch(model.model_url);
      if (!response.ok) {
        throw new Error(`Download failed: ${response.status}`);
      }

      const buffer = await response.arrayBuffer();
      const contentType = response.headers.get("content-type") ?? "";
      const isZip = contentType.includes("zip") || model.model_url.toLowerCase().endsWith(".zip");
      const resourceId = await this.importTone3000Resource(
        tone,
        modelId,
        displayName,
        model.architecture_version ?? "",
        buffer,
        isZip,
        resourceType,
        state.importCategory,
      );

      this.tone3000ImportedResourceIds.set(`${resourceType}:${modelId}`, resourceId);
      showNotification("Imported", displayName);
      return { resourceId, displayName };
    } catch (error) {
      showNotification("Load failed", error instanceof Error ? error.message : String(error));
      return null;
    }
  }

  findImportedTone3000ResourceId(resourceType: ResourceType, modelId: string): string {
    const justImported = this.tone3000ImportedResourceIds.get(`${resourceType}:${modelId}`);
    if (justImported) {
      return justImported;
    }

    const resources = uiState.resourceLibrary[resourceType] ?? [];
    const direct = findResourceById(resources, `tone3000:${modelId}`);
    if (direct) {
      return direct.id;
    }

    const match = resources.find((resource) => resource.metadata?.provider === "tone3000"
      && (resource.metadata?.modelId ?? "") === modelId);
    return match?.id ?? "";
  }
}
