import type {
  BlendLibrary,
  BlendModelMapping,
  GraphNode,
  ResourceLibrary,
  LibraryResource,
  BlendMode,
  BlendDefinition,
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
  { id: "tone", label: "Tone", min: 0, max: 10 },
  { id: "level", label: "Level", min: 0, max: 10 },
  { id: "custom_a", label: "Custom A", min: 0, max: 10 },
  { id: "custom_b", label: "Custom B", min: 0, max: 10 },
  { id: "custom_c", label: "Custom C", min: 0, max: 10 },
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
  private modelBrowserModal = document.getElementById("blend-model-browser-modal") as HTMLElement | null;
  private modelBrowserTitle = document.getElementById("blend-model-browser-title") as HTMLElement | null;
  private modelBrowserClose = document.getElementById("blend-model-browser-close") as HTMLButtonElement | null;
  private modelBrowserCancel = document.getElementById("blend-model-browser-cancel") as HTMLButtonElement | null;
  private modelBrowserSearch = document.getElementById("blend-model-browser-search") as HTMLInputElement | null;
  private modelBrowserScope = document.getElementById("blend-model-browser-scope") as HTMLSelectElement | null;
  private modelBrowserCategory = document.getElementById("blend-model-browser-category") as HTMLSelectElement | null;
  private modelBrowserList = document.getElementById("blend-model-browser-list") as HTMLElement | null;
  private modelBrowserGroupLabel = document.getElementById("blend-model-browser-group") as HTMLElement | null;

  private testParams: Record<string, number> = {};
  private testKnobs: Map<string, GenericKnob> = new Map();
  private activeTab: "settings" | "test" = "settings";
  private activeToneGroupId: string | null = null;
  private activeToneGroupTitle: string | null = null;
  private browserToneGroupId: string | null = null;
  private browserToneGroupTitle: string | null = null;
  private browserResourceType: string = "nam";
  private browserCurrentId: string = "";
  private browserOnSelect: ((resourceId: string) => void) | null = null;

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
    this.modeSelect?.addEventListener("change", () => {
      this.updateMatchedModel();
      this.updateTestMappedIndicators(this.collectCurrentMappings());
    });

    this.modelBrowserClose?.addEventListener("click", () => this.closeModelBrowser());
    this.modelBrowserCancel?.addEventListener("click", () => this.closeModelBrowser());
    this.modelBrowserSearch?.addEventListener("input", () => this.renderModelBrowserList());
    this.modelBrowserScope?.addEventListener("change", () => this.renderModelBrowserList());
    this.modelBrowserCategory?.addEventListener("change", () => this.renderModelBrowserList());
    this.modelBrowserList?.addEventListener("click", (event) => this.handleModelBrowserClick(event));

    this.tabButtons.forEach((button) => {
      button.addEventListener("click", () => {
        const tabId = button.dataset.blendTab === "test" ? "test" : "settings";
        this.setActiveTab(tabId);
      });
    });

    if (this.paramSelect) {
      this.paramSelect.innerHTML = PARAM_SPECS.map((spec) => `<option value="${spec.id}">${spec.label}</option>`).join("");
    }

    this.modal?.addEventListener("mousedown", (event) => {
      if (event.target === this.modal) {
        this.close();
      }
    });

    this.modelBrowserModal?.addEventListener("mousedown", (event) => {
      if (event.target === this.modelBrowserModal) {
        this.closeModelBrowser();
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
    this.setActiveToneGroupFromBlend(blend, mappings);

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

  openWithDefinition(blend: BlendDefinition): void {
    if (!this.modal) {
      return;
    }

    this.initialize();

    const category = blend.category ?? "amp";
    const mappings = blend.modelMappings?.length
      ? blend.modelMappings
      : buildBlendModelMappingsFromIds(blend.models ?? [], this.deps.getResourceLibrary());

    this.activeParams = resolveActiveParams(blend, mappings);
    this.setActiveToneGroupFromBlend(blend, mappings);

    if (this.nameInput) {
      this.nameInput.value = blend.name ?? blend.id;
    }
    if (this.modeSelect) {
      this.modeSelect.value = blend.blendMode ?? "interpolate";
    }
    if (this.categorySelect) {
      const options = Array.from(this.categorySelect.options).map((opt) => opt.value);
      this.categorySelect.value = options.includes(category) ? category : "amp";
    }

    this.modal.dataset.blendId = blend.id;
    delete this.modal.dataset.nodeId;

    this.setActiveTab("settings");
    this.renderParameterList();
    this.renderModelList(mappings, category);
    this.testParams = {};
    this.testKnobs.clear();
    if (this.testControls) {
      this.testControls.innerHTML = "";
    }
    if (this.matchName) {
      this.matchName.textContent = "—";
    }
    if (this.matchDetails) {
      this.matchDetails.textContent = "";
    }
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
    this.testKnobs.clear();
    this.activeToneGroupId = null;
    this.activeToneGroupTitle = null;
    this.closeModelBrowser();
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
      .map((row) => this.getRowModelId(row))
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

  private setActiveToneGroupFromBlend(blend: BlendDefinition | undefined, mappings: BlendModelMapping[]): void {
    const blendGroupId = blend?.toneGroupId?.trim() ?? "";
    const blendGroupTitle = blend?.toneGroupTitle?.trim() ?? "";
    if (blendGroupId) {
      this.activeToneGroupId = blendGroupId;
      this.activeToneGroupTitle = blendGroupTitle || blendGroupId;
      return;
    }

    const inferred = this.inferToneGroupFromMappings(mappings);
    this.activeToneGroupId = inferred.id;
    this.activeToneGroupTitle = inferred.title;
  }

  private inferToneGroupFromMappings(mappings: BlendModelMapping[]): { id: string | null; title: string | null } {
    const resources = this.deps.getResourceLibrary().nam ?? [];
    for (const mapping of mappings) {
      if (!mapping.id) {
        continue;
      }
      const match = resources.find((res) => res.id === mapping.id);
      const toneId = match?.metadata?.toneId ?? match?.metadata?.groupId ?? "";
      const toneTitle = match?.metadata?.toneTitle ?? match?.metadata?.groupName ?? "";
      if (toneId) {
        return { id: toneId, title: toneTitle || toneId };
      }
    }
    return { id: null, title: null };
  }

  private updateModelBrowserGroupLabel(): void {
    if (!this.modelBrowserGroupLabel) {
      return;
    }
    const label = this.browserToneGroupTitle || this.browserToneGroupId || "None";
    this.modelBrowserGroupLabel.textContent = label;
  }

  private updateModelBrowserCategoryOptions(): void {
    if (!this.modelBrowserCategory) {
      return;
    }
    const resources = this.deps.getResourceLibrary()[this.browserResourceType] ?? [];
    const categories = Array.from(new Set(
      resources.map((res) => (res.category ?? "").trim() || "Uncategorized"),
    )).sort((a, b) => a.localeCompare(b));

    const previous = this.modelBrowserCategory.value || "all";
    const options = ["<option value=\"all\">All types</option>"]
      .concat(categories.map((category) => `
        <option value="${escapeHtml(category)}">${escapeHtml(category)}</option>
      `.trim()))
      .join("");

    this.modelBrowserCategory.innerHTML = options;
    if (previous !== "all" && categories.includes(previous)) {
      this.modelBrowserCategory.value = previous;
    } else {
      this.modelBrowserCategory.value = "all";
    }
  }

  public openResourceBrowser(options: {
    resourceType: string;
    currentId?: string;
    toneGroupId?: string | null;
    toneGroupTitle?: string | null;
    onSelect: (resourceId: string) => void;
  }): void {
    this.initialize();
    if (!this.modelBrowserModal) {
      return;
    }

    this.browserResourceType = options.resourceType;
    this.browserCurrentId = options.currentId ?? "";
    this.browserOnSelect = options.onSelect;
    this.browserToneGroupId = options.resourceType === "nam" ? options.toneGroupId ?? null : null;
    this.browserToneGroupTitle = options.resourceType === "nam" ? options.toneGroupTitle ?? null : null;

    if (this.modelBrowserSearch) {
      this.modelBrowserSearch.value = "";
      this.modelBrowserSearch.placeholder = options.resourceType === "ir"
        ? "Search IRs..."
        : options.resourceType === "nam"
          ? "Search models..."
          : "Search resources...";
    }

    if (this.modelBrowserTitle) {
      this.modelBrowserTitle.textContent = options.resourceType === "ir"
        ? "Select IR"
        : options.resourceType === "nam"
          ? "Select Model"
          : "Select Resource";
    }

    if (this.modelBrowserScope) {
      const hasGroup = options.resourceType === "nam" && Boolean(this.browserToneGroupId);
      this.modelBrowserScope.disabled = !hasGroup;
      this.modelBrowserScope.value = hasGroup ? "group" : "all";
    }

    this.updateModelBrowserGroupLabel();
    this.updateModelBrowserCategoryOptions();
    this.renderModelBrowserList();
    this.modelBrowserModal.style.display = "flex";
  }

  private openModelBrowser(row: HTMLElement): void {
    this.openResourceBrowser({
      resourceType: "nam",
      currentId: this.getRowModelId(row),
      toneGroupId: this.activeToneGroupId,
      toneGroupTitle: this.activeToneGroupTitle,
      onSelect: (resourceId) => {
        this.setRowModel(row, resourceId);
        const activeNode = this.getActiveNode();
        if (activeNode) {
          this.renderTestControls(activeNode, this.collectCurrentMappings());
        }
        this.updateMatchedModel();
      },
    });
  }

  private closeModelBrowser(): void {
    if (!this.modelBrowserModal) {
      return;
    }
    this.modelBrowserModal.style.display = "none";
    this.browserOnSelect = null;
    this.browserCurrentId = "";
    this.browserToneGroupId = null;
    this.browserToneGroupTitle = null;
  }

  private renderModelBrowserList(): void {
    if (!this.modelBrowserList) {
      return;
    }

    const resources = this.deps.getResourceLibrary()[this.browserResourceType] ?? [];
    const query = (this.modelBrowserSearch?.value ?? "").trim().toLowerCase();
    const scope = this.modelBrowserScope?.value ?? "group";
    const category = this.modelBrowserCategory?.value ?? "all";
    const currentId = this.browserCurrentId;

    let filtered = resources.slice();
    if (scope === "group" && this.browserResourceType === "nam") {
      if (!this.browserToneGroupId) {
        this.modelBrowserList.innerHTML = "<div class=\"blend-model-browser-empty\">No tone group is associated with this selection.</div>";
        return;
      }
      filtered = filtered.filter((res) => {
        const toneId = res.metadata?.toneId ?? res.metadata?.groupId ?? "";
        return toneId === this.browserToneGroupId;
      });
    }

    if (category !== "all") {
      filtered = filtered.filter((res) => ((res.category ?? "").trim() || "Uncategorized") === category);
    }

    if (query) {
      filtered = filtered.filter((res) => {
        const haystack = `${res.name} ${res.id} ${res.category} ${res.description} ${res.filePath}`.toLowerCase();
        return haystack.includes(query);
      });
    }

    filtered.sort((a, b) => (a.name || a.id).localeCompare(b.name || b.id));

    if (!filtered.length) {
      this.modelBrowserList.innerHTML = "<div class=\"blend-model-browser-empty\">No models match the current filters.</div>";
      return;
    }

    this.modelBrowserList.innerHTML = filtered
      .map((res) => {
        const title = res.name?.trim() || res.id;
        const categoryLabel = (res.category ?? "").trim() || "Uncategorized";
        const selectedClass = res.id === currentId ? "blend-model-browser-item is-selected" : "blend-model-browser-item";
        return `
          <div class="${selectedClass}" data-model-id="${escapeHtml(res.id)}">
            <div>
              <div class="item-title">${escapeHtml(title)}</div>
              <div class="item-meta">
                <span>${escapeHtml(categoryLabel)}</span>
                <span>${escapeHtml(res.id)}</span>
              </div>
            </div>
          </div>
        `;
      })
      .join("");
  }

  private handleModelBrowserClick(event: Event): void {
    const target = event.target as HTMLElement | null;
    if (!target) {
      return;
    }
    const item = target.closest(".blend-model-browser-item") as HTMLElement | null;
    if (!item) {
      return;
    }
    const modelId = item.dataset.modelId ?? "";
    if (!modelId) {
      return;
    }
    this.browserOnSelect?.(modelId);
    this.closeModelBrowser();
  }

  private getRowModelId(row: Element): string {
    return (row as HTMLElement).dataset.modelId?.trim() ?? "";
  }

  private setRowModel(row: HTMLElement, modelId: string): void {
    row.dataset.modelId = modelId;
    const labelInfo = this.getModelLabel(modelId);
    const labelEl = row.querySelector(".blend-model-selected") as HTMLElement | null;
    if (labelEl) {
      labelEl.textContent = labelInfo.label;
      labelEl.title = labelInfo.title;
      labelEl.classList.toggle("is-missing", labelInfo.isMissing);
    }
  }

  private getModelLabel(modelId: string): { label: string; title: string; isMissing: boolean } {
    if (!modelId) {
      return { label: "No model selected", title: "No model selected", isMissing: false };
    }
    const match = getLibraryResource(this.deps.getResourceLibrary(), "nam", modelId);
    if (match) {
      const label = match.name?.trim() || modelId;
      return { label, title: label, isMissing: false };
    }
    const missing = `Missing: ${modelId}`;
    return { label: missing, title: missing, isMissing: true };
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
        const labelInfo = this.getModelLabel(mapping.id);
        const labelClass = labelInfo.isMissing ? "blend-model-selected is-missing" : "blend-model-selected";
        return `
          <div class="blend-model-row" data-index="${index}" data-model-id="${escapeHtml(mapping.id)}">
            <div class="blend-model-header">
              <button class="blend-model-browse" type="button">Browse</button>
              <div class="${labelClass}" title="${escapeHtml(labelInfo.title)}">${escapeHtml(labelInfo.label)}</div>
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
      const browseBtn = row.querySelector(".blend-model-browse") as HTMLButtonElement | null;
      const labelBtn = row.querySelector(".blend-model-selected") as HTMLElement | null;
      const autoBtn = row.querySelector(".blend-model-auto") as HTMLButtonElement | null;
      const removeBtn = row.querySelector(".blend-model-remove") as HTMLButtonElement | null;

      const applyAuto = (force: boolean) => {
        const currentId = this.getRowModelId(row);
        if (!currentId) {
          return;
        }
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

      browseBtn?.addEventListener("click", () => this.openModelBrowser(row as HTMLElement));
      labelBtn?.addEventListener("click", () => this.openModelBrowser(row as HTMLElement));
      autoBtn?.addEventListener("click", () => applyAuto(true));
      removeBtn?.addEventListener("click", () => {
        row.remove();
        const activeNode = this.getActiveNode();
        if (activeNode) {
          this.renderTestControls(activeNode, this.collectCurrentMappings());
        }
        this.updateMatchedModel();
      });

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
    row.dataset.modelId = "";
    row.innerHTML = `
      <div class="blend-model-header">
        <button class="blend-model-browse" type="button">Browse</button>
        <div class="blend-model-selected" title="No model selected">No model selected</div>
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
    this.testKnobs.clear();

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
              <div class="knob-mapped-points"></div>
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

      const knobInstance = new GenericKnob({
        knobElement: knob,
        paramId: `blend_test_${paramId}`,
        minValue: min,
        maxValue: max,
        defaultValue,
        displayFormat: (value) => value.toFixed(1),
        valueDisplay,
        labelElement: knob.parentElement?.querySelector(".knob-label") as HTMLElement | null,
        stepValue: 0.1,
        sensitivity,
        sendParameter: false,
        onValueChange: (value) => {
          const normalized = spec ? normalizeValue(value, spec) : value;
          const blendMode = (this.modeSelect?.value ?? "interpolate") as BlendMode;

          if (blendMode === "snap") {
            const target: Record<string, number> = { ...this.testParams, [paramId]: normalized };
            const closest = findClosestBlendMappingForParam(paramId, normalized, mappings, this.activeParams, target);
            if (closest) {
              const params = buildParameterMapFromLegacy(closest);
              this.activeParams.forEach((activeParamId) => {
                const mappedValue = params[activeParamId];
                if (typeof mappedValue === "number") {
                  this.testParams[activeParamId] = mappedValue;
                }
              });

              this.activeParams.forEach((activeParamId) => {
                const targetValue = this.testParams[activeParamId];
                const targetSpec = getParamSpec(activeParamId);
                const display = typeof targetValue === "number"
                  ? (targetSpec ? Number.parseFloat(denormalizeValue(targetValue, targetSpec)) : targetValue)
                  : 0;
                this.testKnobs.get(activeParamId)?.setValue(display);
              });

              this.updateMatchedModel();
              this.updateTestMappedIndicators(mappings);
              return;
            }
          }

          this.testParams[paramId] = normalized;
          this.updateMatchedModel();
          this.updateTestMappedIndicators(mappings);
        },
      });

      if (paramId) {
        this.testKnobs.set(paramId, knobInstance);
      }
    });

    this.updateTestMappedIndicators(mappings);
  }

  private updateTestMappedIndicators(mappings: BlendModelMapping[]): void {
    if (!this.testControls) {
      return;
    }

    const target: Record<string, number> = {};
    this.activeParams.forEach((paramId) => {
      const value = this.testParams[paramId];
      if (typeof value === "number") {
        target[paramId] = value;
      }
    });

    const knobElements = this.testControls.querySelectorAll(".blend-test-knob");
    knobElements.forEach((knobElement) => {
      const knob = knobElement as HTMLElement;
      const paramId = knob.dataset.paramId ?? "";
      const min = knob.dataset.min ? parseFloat(knob.dataset.min) : -1;
      const max = knob.dataset.max ? parseFloat(knob.dataset.max) : 1;
      const spec = getParamSpec(paramId);
      const points = buildBlendMappedPoints(paramId, mappings, this.activeParams, target, spec);
      renderMappedPointElements(knob, points, min, max);
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
      const modelId = this.getRowModelId(row);
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
    const blendPayload: BlendDefinition = {
      id: blendId,
      name,
      category,
      parameters: this.activeParams.slice(),
      models,
      modelMappings,
      blendMode,
      toneGroupId: this.activeToneGroupId ?? undefined,
      toneGroupTitle: this.activeToneGroupTitle ?? undefined,
    };

    postMessage({
      type: "saveBlendDefinition",
      blend: blendPayload,
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
      const modelId = this.getRowModelId(row);
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

const BLEND_MAPPING_EPS = 1e-4;

type BlendMappedPoint = {
  normalized: number;
  display: number;
  isSelectable: boolean;
  isSelected: boolean;
};

function hasCloseValue(values: number[], value: number, eps = BLEND_MAPPING_EPS): boolean {
  return values.some((existing) => Math.abs(existing - value) <= eps);
}

function addUniqueValue(values: number[], value: number, eps = BLEND_MAPPING_EPS): void {
  if (!hasCloseValue(values, value, eps)) {
    values.push(value);
  }
}

function buildBlendMappedPoints(
  paramId: string,
  mappings: BlendModelMapping[],
  activeParams: string[],
  target: Record<string, number>,
  spec: ParamSpec | null,
): BlendMappedPoint[] {
  if (!paramId) {
    return [];
  }

  const normalizedValues: number[] = [];
  const selectableValues: number[] = [];

  mappings.forEach((mapping) => {
    const params = buildParameterMapFromLegacy(mapping);
    const mappedValue = params[paramId];
    if (typeof mappedValue === "number") {
      addUniqueValue(normalizedValues, mappedValue);
    }
  });

  mappings.forEach((mapping) => {
    const params = buildParameterMapFromLegacy(mapping);
    const mappedValue = params[paramId];
    if (typeof mappedValue !== "number") {
      return;
    }
    let matches = true;
    activeParams.forEach((activeParamId) => {
      if (activeParamId === paramId) {
        return;
      }
      const targetValue = target[activeParamId];
      const otherValue = params[activeParamId];
      if (typeof targetValue !== "number" || typeof otherValue !== "number") {
        matches = false;
        return;
      }
      if (Math.abs(otherValue - targetValue) > BLEND_MAPPING_EPS) {
        matches = false;
      }
    });
    if (matches) {
      addUniqueValue(selectableValues, mappedValue);
    }
  });

  const targetValue = target[paramId];
  return normalizedValues
    .slice()
    .sort((a, b) => a - b)
    .map((value) => {
      const display = spec ? Number.parseFloat(denormalizeValue(value, spec)) : value;
      return {
        normalized: value,
        display,
        isSelectable: hasCloseValue(selectableValues, value),
        isSelected: typeof targetValue === "number" && Math.abs(targetValue - value) <= BLEND_MAPPING_EPS,
      };
    });
}

function findClosestBlendMappingForParam(
  paramId: string,
  targetValue: number,
  mappings: BlendModelMapping[],
  activeParams: string[],
  target: Record<string, number>,
): BlendModelMapping | null {
  let best: BlendModelMapping | null = null;
  let bestDelta = Number.POSITIVE_INFINITY;
  let bestSecondary = Number.POSITIVE_INFINITY;

  mappings.forEach((mapping) => {
    const params = buildParameterMapFromLegacy(mapping);
    const mappedValue = params[paramId];
    if (typeof mappedValue !== "number") {
      return;
    }

    const delta = Math.abs(mappedValue - targetValue);
    let secondary = 0;

    activeParams.forEach((activeParamId) => {
      if (activeParamId === paramId) {
        return;
      }
      const targetOther = target[activeParamId];
      const mappedOther = params[activeParamId];
      if (typeof targetOther !== "number" || typeof mappedOther !== "number") {
        secondary += 4;
        return;
      }
      const diff = mappedOther - targetOther;
      secondary += diff * diff;
    });

    const isBetter = delta < bestDelta - BLEND_MAPPING_EPS
      || (Math.abs(delta - bestDelta) <= BLEND_MAPPING_EPS && secondary < bestSecondary);

    if (isBetter) {
      best = mapping;
      bestDelta = delta;
      bestSecondary = secondary;
    }
  });

  return best;
}

function renderMappedPointElements(
  knob: HTMLElement,
  points: BlendMappedPoint[],
  min: number,
  max: number,
): void {
  let container = knob.querySelector(".knob-mapped-points") as HTMLElement | null;
  if (!container) {
    container = document.createElement("div");
    container.className = "knob-mapped-points";
    knob.prepend(container);
  }

  container.innerHTML = "";
  const range = max - min;
  const safeRange = range !== 0 ? range : 1;

  points.forEach((point) => {
    const angle = ((point.display - min) / safeRange) * 270 - 135;
    const el = document.createElement("span");
    el.className = "knob-mapped-point";
    if (point.isSelectable) {
      el.classList.add("is-selectable");
    }
    if (point.isSelected) {
      el.classList.add("is-selected");
    }
    el.style.setProperty("--mapped-angle", `${angle}deg`);
    container.appendChild(el);
  });

  knob.classList.toggle("has-mapped-points", points.length > 0);
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

