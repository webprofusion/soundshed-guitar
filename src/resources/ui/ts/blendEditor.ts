import type {
  BlendLibrary,
  BlendModelMapping,
  GraphNode,
  ResourceLibrary,
  LibraryResource,
  BlendMode,
} from "./types.js";
import { uiState } from "./state.js";
import { postMessage } from "./bridge.js";
import { GenericKnob } from "./controls.js";
import { buildBlendModelMappingsFromIds, inferParamValueFromName } from "./blendUtils.js";
import { arrayBufferToBase64, buildArchiveFileName, generateResourceId, requestResourceData, sanitizeFilename } from "./archiveUtils.js";
import { sha256HexFromBase64 } from "./utils.js";

type BlendEditorDependencies = {
  getBlendLibrary: () => BlendLibrary;
  getResourceLibrary: () => ResourceLibrary;
};

type ParamSpec = {
  id: string;
  label: string;
  min: number;
  max: number;
};

const PARAM_SPECS: ParamSpec[] = [
  { id: "gain", label: "Gain", min: 0, max: 10 },
  { id: "drive", label: "Drive", min: 0, max: 10 },
  { id: "contour", label: "Contour", min: 0, max: 10 },
  { id: "treble", label: "Treble", min: 0, max: 10 },
  { id: "middle", label: "Middle", min: 0, max: 10 },
  { id: "bass", label: "Bass", min: 0, max: 10 },
  { id: "presence", label: "Presence", min: 0, max: 10 },
];

export class BlendEditorModal {
  private readonly deps: BlendEditorDependencies;
  private initialized = false;
  private activeParams: string[] = [];

  private modal = document.getElementById("blend-editor-modal") as HTMLElement | null;
  private nameInput = document.getElementById("blend-editor-name-input") as HTMLInputElement | null;
  private modeSelect = document.getElementById("blend-editor-mode-select") as HTMLSelectElement | null;
  private categorySelect = document.getElementById("blend-editor-category-select") as HTMLSelectElement | null;
  private modelList = document.getElementById("blend-model-list");
  private addModelBtn = document.getElementById("blend-add-model-btn") as HTMLButtonElement | null;
  private saveBtn = document.getElementById("blend-editor-save") as HTMLButtonElement | null;
  private cancelBtn = document.getElementById("blend-editor-cancel") as HTMLButtonElement | null;
  private closeBtn = document.getElementById("blend-editor-modal-close") as HTMLButtonElement | null;
  private paramList = document.getElementById("blend-parameter-list");
  private paramSelect = document.getElementById("blend-parameter-add-select") as HTMLSelectElement | null;
  private paramAddBtn = document.getElementById("blend-parameter-add-btn") as HTMLButtonElement | null;
  private exportBtn = document.getElementById("blend-editor-export") as HTMLButtonElement | null;
  private importBtn = document.getElementById("blend-editor-import") as HTMLButtonElement | null;
  private importInput = document.getElementById("blend-import-input") as HTMLInputElement | null;
  private paramAutoBtn = document.getElementById("blend-parameter-auto-btn") as HTMLButtonElement | null;
  private matchName = document.getElementById("blend-match-name") as HTMLElement | null;
  private matchDetails = document.getElementById("blend-match-details") as HTMLElement | null;
  private tabButtons = Array.from(document.querySelectorAll(".blend-tab-btn")) as HTMLButtonElement[];
  private tabPanels = Array.from(document.querySelectorAll(".blend-tab-panel")) as HTMLElement[];
  private testControls = document.getElementById("blend-test-controls") as HTMLElement | null;

  private testParams: Record<string, number> = {};
  private activeTab: "settings" | "test" = "settings";

  constructor(deps: BlendEditorDependencies) {
    this.deps = deps;
  }

  initialize(): void {
    if (this.initialized) {
      return;
    }
    this.initialized = true;

    this.closeBtn?.addEventListener("click", () => this.close());
    this.cancelBtn?.addEventListener("click", () => this.close());
    this.addModelBtn?.addEventListener("click", () => this.addModelRow());
    this.saveBtn?.addEventListener("click", () => this.save());
    this.paramAddBtn?.addEventListener("click", () => this.addParameter());
    this.paramAutoBtn?.addEventListener("click", () => this.autoMapParameters());
    this.exportBtn?.addEventListener("click", () => void this.exportBlendArchive());
    this.importBtn?.addEventListener("click", () => this.importInput?.click());
    this.importInput?.addEventListener("change", () => void this.importBlendArchive());
    this.modeSelect?.addEventListener("change", () => this.updateMatchedModel());

    this.tabButtons.forEach((button) => {
      button.addEventListener("click", () => {
        const tabId = button.dataset.blendTab === "test" ? "test" : "settings";
        this.setActiveTab(tabId);
      });
    });

    if (this.paramSelect) {
      this.paramSelect.innerHTML = PARAM_SPECS.map((spec) => `<option value="${spec.id}">${spec.label}</option>`).join("");
    }

    this.modal?.addEventListener("click", (event) => {
      if (event.target === this.modal) {
        this.close();
      }
    });
  }

  open(node: GraphNode): void {
    const blendId = (node as unknown as { config?: Record<string, string> }).config?.blendId;
    if (!blendId || !this.modal) {
      return;
    }

    this.initialize();

    const blend = this.deps.getBlendLibrary().find((entry) => entry.id === blendId);
    const category = blend?.category ?? node.category ?? "amp";
    const mappings = blend?.modelMappings?.length
      ? blend.modelMappings
      : buildBlendModelMappingsFromIds(blend?.models ?? [], this.deps.getResourceLibrary());

    this.activeParams = resolveActiveParams(blend, mappings);

    if (this.nameInput) {
      this.nameInput.value = blend?.name ?? blendId;
    }
    if (this.modeSelect) {
      this.modeSelect.value = blend?.blendMode ?? "interpolate";
    }
    if (this.categorySelect) {
      const options = Array.from(this.categorySelect.options).map((opt) => opt.value);
      this.categorySelect.value = options.includes(category) ? category : "amp";
    }

    this.modal.dataset.blendId = blendId;
    this.modal.dataset.nodeId = node.id;

    this.setActiveTab("settings");

    this.renderParameterList();
    this.renderModelList(mappings, category);
    this.syncTestParams(node, mappings);
    this.renderTestControls(node, mappings);
    this.updateMatchedModel();
    this.modal.style.display = "flex";
  }

  private close(): void {
    if (!this.modal) {
      return;
    }
    this.modal.style.display = "none";
    delete this.modal.dataset.blendId;
    delete this.modal.dataset.nodeId;
    if (this.modelList) {
      this.modelList.innerHTML = "";
    }
    this.activeParams = [];
    this.testParams = {};
  }

  private getActiveNode(): GraphNode | null {
    if (!this.modal) {
      return null;
    }
    const nodeId = this.modal.dataset.nodeId ?? "";
    if (!nodeId) {
      return null;
    }
    const presetId = uiState.activePresetId ?? "";
    const preset = presetId ? uiState.presetCache.get(presetId) : undefined;
    return preset?.graph?.nodes.find((entry) => entry.id === nodeId) ?? null;
  }

  private setActiveTab(tabId: "settings" | "test"): void {
    this.activeTab = tabId;
    this.tabButtons.forEach((button) => {
      const isActive = (button.dataset.blendTab ?? "settings") === tabId;
      button.classList.toggle("active", isActive);
    });
    this.tabPanels.forEach((panel) => {
      const isActive = (panel.dataset.blendTabPanel ?? "settings") === tabId;
      panel.classList.toggle("active", isActive);
    });
    this.updateMatchedModel();
  }

  private renderParameterList(): void {
    if (!this.paramList) {
      return;
    }
    this.paramList.innerHTML = this.activeParams
      .map((paramId) => {
        const spec = getParamSpec(paramId);
        const label = spec?.label ?? paramId;
        return `
          <span class="blend-param-chip" data-param-id="${paramId}">
            ${escapeHtml(label)}
            <button type="button" class="blend-param-remove">×</button>
          </span>
        `;
      })
      .join("");

    const chips = this.paramList.querySelectorAll(".blend-param-remove");
    chips.forEach((btn) => {
      btn.addEventListener("click", () => {
        const chip = (btn as HTMLElement).closest(".blend-param-chip") as HTMLElement | null;
        const paramId = chip?.dataset.paramId ?? "";
        if (!paramId) {
          return;
        }
        this.activeParams = this.activeParams.filter((entry) => entry !== paramId);
        if (!this.activeParams.length) {
          this.activeParams = ["gain"];
        }
        this.renderParameterList();
        this.renderModelList(this.collectCurrentMappings(), this.categorySelect?.value ?? "amp");
        const activeNode = this.getActiveNode();
        if (activeNode) {
          this.renderTestControls(activeNode, this.collectCurrentMappings());
        }
        this.updateMatchedModel();
      });
    });
  }

  private addParameter(): void {
    const next = this.paramSelect?.value ?? "";
    if (!next || this.activeParams.includes(next)) {
      return;
    }
    this.activeParams = [...this.activeParams, next];
    this.renderParameterList();
    this.renderModelList(this.collectCurrentMappings(), this.categorySelect?.value ?? "amp");
    const activeNode = this.getActiveNode();
    if (activeNode) {
      this.renderTestControls(activeNode, this.collectCurrentMappings());
    }
    this.updateMatchedModel();
  }

  private autoMapParameters(): void {
    if (!this.modelList) {
      return;
    }

    const rows = Array.from(this.modelList.querySelectorAll(".blend-model-row"));
    const modelIds = rows
      .map((row) => (row.querySelector(".blend-model-select") as HTMLSelectElement | null)?.value ?? "")
      .filter((id) => Boolean(id));

    if (!modelIds.length) {
      return;
    }

    const resources = this.deps.getResourceLibrary().nam ?? [];
    const paramSet = new Set<string>();
    const mappings: BlendModelMapping[] = modelIds.map((id) => {
      const match = resources.find((res) => res.id === id);
      const parameters: Record<string, number> = {};
      PARAM_SPECS.forEach((spec) => {
        const value = inferParamValueFromName(match?.name ?? "", spec.id);
        if (value !== null) {
          parameters[spec.id] = value;
          paramSet.add(spec.id);
        }
      });
      return { id, parameters };
    });

    const params = Array.from(paramSet);
    this.activeParams = params.length ? params : ["gain"];
    this.renderParameterList();
    this.renderModelList(mappings, this.categorySelect?.value ?? "amp");
    const activeNode = this.getActiveNode();
    if (activeNode) {
      this.renderTestControls(activeNode, mappings);
    }
    this.updateMatchedModel();
  }

  private async exportBlendArchive(): Promise<void> {
    if (!this.modal) {
      return;
    }
    const blendId = this.modal.dataset.blendId ?? "";
    const blend = this.deps.getBlendLibrary().find((entry) => entry.id === blendId);
    if (!blend) {
      return;
    }

    const zipLib = window.JSZip;
    if (!zipLib) {
      return;
    }

    const zip = new zipLib();
    const resourcesFolder = zip.folder("resources");
    if (!resourcesFolder) {
      return;
    }

    const resources = this.deps.getResourceLibrary().nam ?? [];
    const exportResources: Array<{ id: string; name: string; category: string; type: string; fileName: string; hash?: string }> = [];

    for (const modelId of blend.models ?? []) {
      const resource = resources.find((res) => res.id === modelId);
      if (!resource) {
        continue;
      }
      const fileName = buildArchiveFileName(resource, "nam");
      const data = await requestResourceData("nam", resource.id);
      if (!data) {
        continue;
      }
      const hash = await sha256HexFromBase64(data);
      resourcesFolder.file(fileName, data, { base64: true });
      exportResources.push({
        id: resource.id,
        name: resource.name,
        category: resource.category,
        type: "nam",
        fileName,
        hash,
      });
    }

    const archive = {
      formatVersion: 1,
      blend: {
        ...blend,
      },
      resources: exportResources,
    };

    zip.file("blend.json", JSON.stringify(archive, null, 2));
    const blob = await zip.generateAsync({ type: "blob" });
    const buffer = await blob.arrayBuffer();
    const data = arrayBufferToBase64(buffer);
    postMessage({
      type: "saveBlendArchive",
      fileName: `${sanitizeFilename(blend.name || blend.id || "blend")}.namz`,
      data,
    });
  }

  private async importBlendArchive(): Promise<void> {
    const file = this.importInput?.files?.[0];
    if (!file) {
      return;
    }

    if (this.importInput) {
      this.importInput.value = "";
    }

    const zipLib = window.JSZip;
    if (!zipLib) {
      return;
    }

    const buffer = await file.arrayBuffer();
    const zip = await zipLib.loadAsync(buffer);
    const blendEntry = zip.file("blend.json");
    if (!blendEntry) {
      return;
    }

    const blendText = await blendEntry.async("text");
    const archive = JSON.parse(blendText) as {
      blend?: BlendLibrary[number];
      resources?: Array<{ id?: string; fileName?: string; name?: string; category?: string; type?: string; hash?: string }>;
    };

    const zipFiles = Object.values(zip.files) as JSZipObject[];
    const fileMap = new Map<string, JSZipObject>();
    zipFiles.forEach((entry) => {
      if (!entry.dir) {
        const name = entry.name.replace(/^resources\//, "");
        fileMap.set(name, entry);
      }
    });

    const idMap = new Map<string, string>();
    const resourcesToImport = archive.resources ?? [];

    for (const resource of resourcesToImport) {
      const fileName = resource.fileName ?? "";
      const existing = getLibraryResourceByHash(this.deps.getResourceLibrary(), "nam", resource.hash);
      if (existing) {
        idMap.set(resource.id ?? fileName, existing.id);
        continue;
      }

      const entry = fileMap.get(fileName);
      if (!entry) {
        continue;
      }
      const dataBuffer = await entry.async("arraybuffer");
      const data = arrayBufferToBase64(dataBuffer);
      const newId = generateResourceId(fileName);
      idMap.set(resource.id ?? fileName, newId);

      postMessage({
        type: "importRemoteResource",
        provider: "blendArchive",
        resourceType: "nam",
        resourceId: newId,
        name: resource.name ?? fileName,
        description: "",
        category: resource.category ?? "",
        subfolder: "blend-imports",
        fileName,
        hash: resource.hash ?? "",
        metadata: {
          provider: "blendArchive",
          sourceFile: fileName,
        },
        data,
      });
    }

    const originalBlend = archive.blend;
    if (!originalBlend) {
      return;
    }

    const newBlendId = generateResourceId(originalBlend.id || originalBlend.name || "blend");
    const remapId = (id: string) => idMap.get(id) ?? id;
    const models = (originalBlend.models ?? []).map(remapId);
    const modelMappings = (originalBlend.modelMappings ?? []).map((mapping) => ({
      ...mapping,
      id: remapId(mapping.id),
    }));

    postMessage({
      type: "saveBlendDefinition",
      blend: {
        ...originalBlend,
        id: newBlendId,
        models,
        modelMappings,
      },
    });
  }

  private renderModelList(mappings: BlendModelMapping[], category: string): void {
    if (!this.modelList) {
      return;
    }

    if (!mappings.length) {
      mappings = [{ id: "" }];
    }

    this.modelList.innerHTML = mappings
      .map((mapping, index) => {
        const parameters = mapping.parameters ?? buildParameterMapFromLegacy(mapping);
        return `
          <div class="blend-model-row" data-index="${index}">
            <div class="blend-model-header">
              <select class="blend-model-select">
                ${this.buildModelOptions(mapping.id)}
              </select>
              <button class="blend-model-auto" type="button">Auto</button>
              <button class="blend-model-remove" type="button">Remove</button>
            </div>
            <div class="blend-model-params">
              ${this.activeParams.map((paramId) => renderParamInput(paramId, parameters)).join("")}
            </div>
          </div>
        `;
      })
      .join("");

    this.bindModelRowEvents(category);
    const activeNode = this.getActiveNode();
    if (activeNode) {
      this.renderTestControls(activeNode, mappings);
    }
    this.updateMatchedModel();
  }

  private buildModelOptions(selectedId: string): string {
    const resources = this.deps.getResourceLibrary().nam ?? [];
    const options = resources
      .slice()
      .sort((a, b) => a.name.localeCompare(b.name))
      .map((res) => {
        const selected = res.id === selectedId ? "selected" : "";
        return `<option value="${res.id}" ${selected}>${escapeHtml(res.name)}</option>`;
      })
      .join("");

    const selectedMissing = selectedId && !resources.some((res) => res.id === selectedId);
    const missingOption = selectedMissing
      ? `<option value="${escapeHtml(selectedId)}" selected>Missing: ${escapeHtml(selectedId)}</option>`
      : "";

    return `
      <option value="">-- Select model --</option>
      ${missingOption}
      ${options}
    `;
  }

  private bindModelRowEvents(category: string): void {
    if (!this.modelList) {
      return;
    }

    const rows = this.modelList.querySelectorAll(".blend-model-row");
    rows.forEach((row) => {
      const select = row.querySelector(".blend-model-select") as HTMLSelectElement | null;
      const autoBtn = row.querySelector(".blend-model-auto") as HTMLButtonElement | null;
      const removeBtn = row.querySelector(".blend-model-remove") as HTMLButtonElement | null;

      const applyAuto = (force: boolean) => {
        if (!select || !select.value) {
          return;
        }
        const currentId = select.value;
        const match = getLibraryResource(this.deps.getResourceLibrary(), "nam", currentId);
        this.activeParams.forEach((paramId) => {
          const input = row.querySelector(`.blend-model-param-value[data-param-id="${paramId}"]`) as HTMLInputElement | null;
          if (!input) {
            return;
          }
          if (input.value.trim() && !force) {
            return;
          }
          const inferred = inferParamValueFromName(match?.name ?? "", paramId);
          if (inferred === null) {
            return;
          }
          const spec = getParamSpec(paramId);
          input.value = spec ? denormalizeValue(inferred, spec) : inferred.toFixed(2);
        });
      };

      select?.addEventListener("change", () => applyAuto(false));
      autoBtn?.addEventListener("click", () => applyAuto(true));
      removeBtn?.addEventListener("click", () => {
        row.remove();
        const activeNode = this.getActiveNode();
        if (activeNode) {
          this.renderTestControls(activeNode, this.collectCurrentMappings());
        }
        this.updateMatchedModel();
      });

      select?.addEventListener("change", () => this.updateMatchedModel());
      autoBtn?.addEventListener("click", () => this.updateMatchedModel());
      row.querySelectorAll(".blend-model-param-value").forEach((input) => {
        input.addEventListener("input", () => this.updateMatchedModel());
      });
    });
  }

  private addModelRow(): void {
    if (!this.modelList) {
      return;
    }

    const category = this.categorySelect?.value ?? "amp";
    const index = this.modelList.querySelectorAll(".blend-model-row").length;
    const row = document.createElement("div");
    row.className = "blend-model-row";
    row.dataset.index = index.toString();
    row.innerHTML = `
      <div class="blend-model-header">
        <select class="blend-model-select">
          ${this.buildModelOptions("")}
        </select>
        <button class="blend-model-auto" type="button">Auto</button>
        <button class="blend-model-remove" type="button">Remove</button>
      </div>
      <div class="blend-model-params">
        ${this.activeParams.map((paramId) => renderParamInput(paramId, {})).join("")}
      </div>
    `;
    this.modelList.appendChild(row);
    this.bindModelRowEvents(category);
    const activeNode = this.getActiveNode();
    if (activeNode) {
      this.renderTestControls(activeNode, this.collectCurrentMappings());
    }
    this.updateMatchedModel();
  }

  private syncTestParams(node: GraphNode, mappings: BlendModelMapping[]): void {
    const specs = new Map(PARAM_SPECS.map((spec) => [spec.id, spec]));
    const defaultNormalized = (paramId: string): number => {
      const nodeValue = node.params[paramId];
      if (typeof nodeValue === "number") {
        return nodeValue;
      }
      const values: number[] = [];
      mappings.forEach((mapping) => {
        const params = buildParameterMapFromLegacy(mapping);
        const value = params[paramId];
        if (typeof value === "number") {
          values.push(value);
        }
      });
      if (values.length) {
        const sorted = values.slice().sort((a, b) => a - b);
        const mid = Math.floor(sorted.length / 2);
        return sorted.length % 2 === 0 ? (sorted[mid - 1] + sorted[mid]) / 2 : sorted[mid];
      }
      const spec = specs.get(paramId);
      return spec ? normalizeValue((spec.min + spec.max) / 2, spec) : 0.5;
    };

    const nextParams: Record<string, number> = {};
    this.activeParams.forEach((paramId) => {
      const existing = this.testParams[paramId];
      nextParams[paramId] = typeof existing === "number" ? existing : defaultNormalized(paramId);
    });
    this.testParams = nextParams;
  }

  private renderTestControls(node: GraphNode, mappings: BlendModelMapping[]): void {
    if (!this.testControls) {
      return;
    }

    this.syncTestParams(node, mappings);

    this.testControls.innerHTML = this.activeParams
      .map((paramId) => {
        const spec = getParamSpec(paramId);
        const label = spec?.label ?? paramId;
        const normalizedValue = this.testParams[paramId] ?? 0;
        const displayValue = spec ? Number.parseFloat(denormalizeValue(normalizedValue, spec)) : normalizedValue;
        const min = spec?.min ?? -1;
        const max = spec?.max ?? 1;
        return `
          <div class="knob-control">
            <span class="knob-label">${escapeHtml(label)}</span>
            <div
              class="knob blend-test-knob"
              data-param-id="${paramId}"
              data-value="${displayValue.toFixed(2)}"
              data-min="${min}"
              data-max="${max}"
            >
              <div class="knob-indicator"></div>
            </div>
            <span class="knob-value">${displayValue.toFixed(2)}</span>
          </div>
        `;
      })
      .join("");

    const knobElements = this.testControls.querySelectorAll(".blend-test-knob");
    knobElements.forEach((knobElement) => {
      const knob = knobElement as HTMLElement;
      const valueDisplay = knob.parentElement?.querySelector(".knob-value") as HTMLElement | null;
      const paramId = knob.dataset.paramId ?? "";
      const min = knob.dataset.min ? parseFloat(knob.dataset.min) : -1;
      const max = knob.dataset.max ? parseFloat(knob.dataset.max) : 1;
      const spec = getParamSpec(paramId);
      const defaultValue: number = spec
        ? Number.parseFloat(denormalizeValue(this.testParams[paramId] ?? 0, spec))
        : Number(this.testParams[paramId] ?? 0);
      const sensitivity = (max - min) / 200;

      new GenericKnob({
        knobElement: knob,
        paramId: `blend_test_${paramId}`,
        minValue: min,
        maxValue: max,
        defaultValue,
        displayFormat: (value) => value.toFixed(1),
        valueDisplay,
        sensitivity,
        sendParameter: false,
        onValueChange: (value) => {
          const normalized = spec ? normalizeValue(value, spec) : value;
          this.testParams[paramId] = normalized;
          this.updateMatchedModel();
        },
      });
    });
  }

  private updateMatchedModel(): void {
    if (!this.matchName || !this.modal) {
      return;
    }

    const nodeId = this.modal.dataset.nodeId ?? "";
    if (!nodeId) {
      this.matchName.textContent = "—";
      if (this.matchDetails) this.matchDetails.textContent = "";
      return;
    }

    const node = this.getActiveNode();
    if (!node) {
      this.matchName.textContent = "—";
      if (this.matchDetails) this.matchDetails.textContent = "";
      return;
    }

    const target: Record<string, number> = {};
    this.activeParams.forEach((paramId) => {
      const value = this.testParams[paramId];
      if (typeof value === "number") {
        target[paramId] = value;
      }
    });

    const mappings: BlendModelMapping[] = this.collectCurrentMappings();
    if (!Object.keys(target).length || !mappings.length) {
      this.matchName.textContent = "—";
      if (this.matchDetails) this.matchDetails.textContent = "";
      return;
    }

    const blendMode = (this.modeSelect?.value ?? "interpolate") as BlendMode;

    let bestIndex = -1;
    let secondIndex = -1;
    let bestDistance = Number.POSITIVE_INFINITY;
    let secondDistance = Number.POSITIVE_INFINITY;

    mappings.forEach((mapping: BlendModelMapping, index: number) => {
      const params = buildParameterMapFromLegacy(mapping);
      let distance = 0;
      let matched = false;
      this.activeParams.forEach((paramId) => {
        if (!(paramId in target)) {
          return;
        }
        const mappedValue = params[paramId];
        if (typeof mappedValue !== "number") {
          distance += 4;
          return;
        }
        const delta = mappedValue - target[paramId];
        distance += delta * delta;
        matched = true;
      });
      if (!matched) {
        distance += 9;
      }

      if (distance < bestDistance) {
        secondDistance = bestDistance;
        secondIndex = bestIndex;
        bestDistance = distance;
        bestIndex = index;
      } else if (distance < secondDistance) {
        secondDistance = distance;
        secondIndex = index;
      }
    });

    if (bestIndex < 0) {
      this.matchName.textContent = "—";
      if (this.matchDetails) this.matchDetails.textContent = "";
      return;
    }

    const bestMapping = mappings[bestIndex] as BlendModelMapping;
    const bestResource = getLibraryResource(this.deps.getResourceLibrary(), "nam", bestMapping.id);
    const bestName = bestResource?.name ?? bestMapping.id;
    this.matchName.textContent = bestName || "—";

    if (this.matchDetails) {
      const hasSecond = secondIndex >= 0 && secondIndex !== bestIndex;
      if (blendMode === "interpolate" && hasSecond) {
        const secondMapping = mappings[secondIndex] as BlendModelMapping;
        const secondResource = getLibraryResource(this.deps.getResourceLibrary(), "nam", secondMapping.id);
        const secondName = secondResource?.name ?? secondMapping.id;
        const eps = 1e-6;
        const w1 = 1 / Math.max(bestDistance, eps);
        const w2 = 1 / Math.max(secondDistance, eps);
        const denom = Math.max(w1 + w2, eps);
        const p1 = Math.round((w1 / denom) * 100);
        const p2 = 100 - p1;
        this.matchDetails.textContent = `Mix: ${bestName} ${p1}% / ${secondName} ${p2}%`;
      } else {
        this.matchDetails.textContent = "";
      }
    }
  }

  private save(): void {
    if (!this.modal) {
      return;
    }
    const blendId = this.modal.dataset.blendId ?? "";
    if (!blendId) {
      return;
    }

    const name = (this.nameInput?.value ?? "").trim();
    if (!name) {
      return;
    }

    const category = (this.categorySelect?.value ?? "amp").trim();
    const blendMode = (this.modeSelect?.value ?? "interpolate") as "snap" | "interpolate";

    const modelMappings: BlendModelMapping[] = [];
    const rows = this.modelList?.querySelectorAll(".blend-model-row") ?? [];
    rows.forEach((row) => {
      const select = row.querySelector(".blend-model-select") as HTMLSelectElement | null;
      const modelId = select?.value?.trim() ?? "";
      if (!modelId) {
        return;
      }

      const parameters: Record<string, number> = {};
      this.activeParams.forEach((paramId) => {
        const input = row.querySelector(`.blend-model-param-value[data-param-id="${paramId}"]`) as HTMLInputElement | null;
        const raw = input?.value?.trim() ?? "";
        const numeric = raw ? Number.parseFloat(raw) : NaN;
        if (Number.isNaN(numeric)) {
          return;
        }
        const spec = getParamSpec(paramId);
        parameters[paramId] = spec ? normalizeValue(numeric, spec) : numeric;
      });

      const primaryParam = this.activeParams[0] ?? "";
      const primaryValue = primaryParam && primaryParam in parameters ? parameters[primaryParam] : undefined;

      const mapping: BlendModelMapping = {
        id: modelId,
      };
      if (primaryParam) {
        mapping.parameterId = primaryParam;
      }
      if (primaryValue !== undefined) {
        mapping.parameterValue = primaryValue;
      }
      if (Object.keys(parameters).length) {
        mapping.parameters = parameters;
      }
      modelMappings.push(mapping);
    });

    const models = modelMappings.map((mapping) => mapping.id);
    postMessage({
      type: "saveBlendDefinition",
      blend: {
        id: blendId,
        name,
        category,
        parameters: this.activeParams.slice(),
        models,
        modelMappings,
        blendMode,
      },
    });

    this.close();
  }

  private collectCurrentMappings(): BlendModelMapping[] {
    if (!this.modelList) {
      return [];
    }
    const rows = this.modelList.querySelectorAll(".blend-model-row");
    const mappings: BlendModelMapping[] = [];
    rows.forEach((row) => {
      const select = row.querySelector(".blend-model-select") as HTMLSelectElement | null;
      const modelId = select?.value?.trim() ?? "";
      if (!modelId) {
        return;
      }
      const parameters: Record<string, number> = {};
      this.activeParams.forEach((paramId) => {
        const input = row.querySelector(`.blend-model-param-value[data-param-id="${paramId}"]`) as HTMLInputElement | null;
        const raw = input?.value?.trim() ?? "";
        const numeric = raw ? Number.parseFloat(raw) : NaN;
        if (Number.isNaN(numeric)) {
          return;
        }
        const spec = getParamSpec(paramId);
        parameters[paramId] = spec ? normalizeValue(numeric, spec) : numeric;
      });
      mappings.push({ id: modelId, parameters });
    });
    return mappings;
  }
}

function resolveActiveParams(blend: BlendLibrary[number] | undefined, mappings: BlendModelMapping[]): string[] {
  const params = new Set<string>();
  if (blend?.parameters?.length) {
    blend.parameters.forEach((param) => params.add(param));
  }
  mappings.forEach((mapping) => {
    if (mapping.parameters) {
      Object.keys(mapping.parameters).forEach((param) => params.add(param));
    }
    if (mapping.parameterId) {
      params.add(mapping.parameterId);
    }
  });
  if (!params.size) {
    params.add("gain");
  }
  return Array.from(params);
}

function renderParamInput(paramId: string, parameters: Record<string, number>): string {
  const spec = getParamSpec(paramId);
  const label = spec?.label ?? paramId;
  const value = typeof parameters[paramId] === "number"
    ? (spec ? denormalizeValue(parameters[paramId], spec) : parameters[paramId].toFixed(2))
    : "";
  const step = spec ? "0.1" : "0.01";
  return `
    <label class="blend-model-param">
      <span>${escapeHtml(label)}</span>
      <input class="blend-model-param-value" data-param-id="${paramId}" type="number" step="${step}" placeholder="Value" value="${escapeHtml(value)}" />
    </label>
  `;
}

function buildParameterMapFromLegacy(mapping: BlendModelMapping): Record<string, number> {
  if (mapping.parameters) {
    return mapping.parameters;
  }
  if (mapping.parameterId && typeof mapping.parameterValue === "number") {
    return { [mapping.parameterId]: mapping.parameterValue };
  }
  return {};
}

function getLibraryResource(library: ResourceLibrary, resourceType: string, resourceId: string): LibraryResource | undefined {
  if (!resourceType || !resourceId) return undefined;
  const resources = library[resourceType] || [];
  return resources.find((res) => res.id === resourceId);
}

function getLibraryResourceByHash(library: ResourceLibrary, resourceType: string, hash?: string): LibraryResource | undefined {
  if (!resourceType || !hash) return undefined;
  const resources = library[resourceType] || [];
  const normalized = hash.toLowerCase();
  return resources.find((res) => res.hash?.toLowerCase() === normalized);
}

function getParamSpec(parameterId: string): ParamSpec | null {
  if (!parameterId) {
    return null;
  }
  return PARAM_SPECS.find((spec) => spec.id === parameterId) ?? null;
}

function normalizeValue(value: number, spec: ParamSpec): number {
  if (value < 0) {
    return value / 10;
  }
  const clamped = Math.min(spec.max, Math.max(spec.min, value));
  const range = spec.max - spec.min;
  if (range <= 0) {
    return 0;
  }
  return (clamped - spec.min) / range;
}

function denormalizeValue(value: number, spec: ParamSpec): string {
  const range = spec.max - spec.min;
  const raw = value < 0 ? value * 10 : spec.min + value * range;
  return raw.toFixed(1);
}

function escapeHtml(value: string): string {
  return value
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#039;");
}

