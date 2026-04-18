import { postMessage } from "./bridge.js";
import { importGeneratedCustomEffect, getCustomEffectEntry } from "./customEffects.js";
import { Features, isFeatureEnabled } from "./featureFlags.js";
import { showNotification } from "./notifications.js";
import { getApiBaseUrl } from "./toneSharingPanel.js";
import type { GraphNode } from "./types.js";

type ModuleNodeContext = {
  nodeId?: string;
  nodeLabel?: string;
  nodeCategory?: string;
  currentModuleName?: string;
  currentModuleResourceId?: string;
  currentModuleVersion?: string;
  currentParams?: Record<string, number>;
  existingCustomEffectId?: string;
  existingCustomEffectName?: string;
  descriptorSummary?: Record<string, unknown>;
};

type ModuleDesignPlan = {
  title: string;
  summary: string;
  assistantMessage: string;
  category: string;
  executionReadiness?: "executable" | "plan_only";
  parameters: Record<string, number>;
  questions: string[];
  tags: string[];
  limitations: string[];
  designHighlights: string[];
  promptDigest: string;
};

type DesignerMessage = {
  id: string;
  role: "user" | "assistant";
  content: string;
  createdAt: string;
};

type DesignerRevisionSummary = {
  id: string;
  title: string;
  summary: string;
  category: string;
  createdAt: string;
};

type DesignerRevisionDetail = DesignerRevisionSummary & {
  plan: ModuleDesignPlan;
  descriptorText: string;
  specText: string;
  manifest: Record<string, unknown>;
  moduleBase64: string;
};

type DesignerSession = {
  id: string;
  status: string;
  title: string | null;
  summary: string | null;
  nodeContext: ModuleNodeContext;
  currentPlan: ModuleDesignPlan | null;
  latestRevisionId: string | null;
  createdAt: string;
  updatedAt: string;
  messages: DesignerMessage[];
  revisions: DesignerRevisionSummary[];
};

type DesignerSessionResponse = {
  session: DesignerSession;
  latestRevision?: DesignerRevisionDetail;
};

type CachedDesignerSession = {
  contextKey: string;
  response: DesignerSessionResponse;
};

const modal = document.getElementById("custom-effect-designer-modal") as HTMLDivElement | null;
const subtitleElement = document.getElementById("custom-effect-designer-subtitle") as HTMLParagraphElement | null;
const threadElement = document.getElementById("custom-effect-designer-thread") as HTMLDivElement | null;
const inputElement = document.getElementById("custom-effect-designer-input") as HTMLTextAreaElement | null;
const statusElement = document.getElementById("custom-effect-designer-status") as HTMLDivElement | null;
const planElement = document.getElementById("custom-effect-designer-plan") as HTMLDivElement | null;
const revisionElement = document.getElementById("custom-effect-designer-revision") as HTMLDivElement | null;
const footerNoteElement = document.getElementById("custom-effect-designer-footer-note") as HTMLSpanElement | null;
const sendButton = document.getElementById("custom-effect-designer-send") as HTMLButtonElement | null;
const generateButton = document.getElementById("custom-effect-designer-generate") as HTMLButtonElement | null;
const downloadButton = document.getElementById("custom-effect-designer-download") as HTMLButtonElement | null;
const saveButton = document.getElementById("custom-effect-designer-save") as HTMLButtonElement | null;
const applyButton = document.getElementById("custom-effect-designer-apply") as HTMLButtonElement | null;

const sessionCacheByNodeId = new Map<string, CachedDesignerSession>();

const state: {
  node: GraphNode | null;
  response: DesignerSessionResponse | null;
  busy: boolean;
  statusMessage: string;
} = {
  node: null,
  response: null,
  busy: false,
  statusMessage: "",
};

function escapeHtml(value: string): string {
  return value
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/\"/g, "&quot;")
    .replace(/'/g, "&#39;");
}

function normaliseText(value: unknown, maxLength: number): string {
  if (typeof value !== "string") {
    return "";
  }
  return value.replace(/[\u0000-\u001f\u007f]+/g, " ").replace(/\s+/g, " ").trim().slice(0, maxLength);
}

function describeError(error: unknown): string {
  if (error instanceof Error) {
    return error.message;
  }
  return typeof error === "string" ? error : "Unexpected error";
}

function currentNodeLinkedEntry(node: GraphNode): ReturnType<typeof getCustomEffectEntry> {
  const customEffectId = node.config?.customEffectId ?? "";
  return customEffectId ? getCustomEffectEntry(customEffectId) : undefined;
}

function resolveCurrentModuleName(node: GraphNode): string {
  const resource = node.resources?.[0];
  if (!resource) {
    return node.displayName || node.id;
  }
  if (resource.resourceId) {
    return resource.resourceId;
  }
  if (resource.id) {
    return resource.id;
  }
  if (resource.filePath) {
    const parts = resource.filePath.replace(/\\/g, "/").split("/");
    return parts[parts.length - 1] ?? resource.filePath;
  }
  return node.displayName || node.id;
}

function buildNodeContext(node: GraphNode): ModuleNodeContext {
  const linkedEntry = currentNodeLinkedEntry(node);
  const descriptorSummary = linkedEntry?.descriptorSummary ?? {};
  return {
    nodeId: node.id,
    nodeLabel: node.displayName || node.id,
    nodeCategory: node.category || "utility",
    currentModuleName: resolveCurrentModuleName(node),
    currentModuleResourceId: node.resources?.[0]?.resourceId || node.resources?.[0]?.id || "",
    currentModuleVersion: typeof descriptorSummary.version === "string" ? descriptorSummary.version : "",
    currentParams: { ...node.params },
    existingCustomEffectId: linkedEntry?.id,
    existingCustomEffectName: linkedEntry?.name,
    descriptorSummary: { ...descriptorSummary },
  };
}

function buildNodeContextKey(node: GraphNode): string {
  return JSON.stringify(buildNodeContext(node));
}

async function designerApiFetch<T>(path: string, init: RequestInit = {}): Promise<T> {
  const apiBase = getApiBaseUrl().trim().replace(/\/+$/, "");
  if (!apiBase) {
    throw new Error("Tone Sharing API base URL is empty");
  }

  const headers = new Headers(init.headers ?? {});
  headers.set("accept", "application/json");
  if (init.body && !headers.has("content-type")) {
    headers.set("content-type", "application/json");
  }

  const response = await fetch(`${apiBase}${path}`, {
    ...init,
    headers,
    credentials: "include",
  });

  let payload: { ok?: boolean; data?: T; error?: { message?: string } } | null = null;
  try {
    payload = (await response.json()) as { ok?: boolean; data?: T; error?: { message?: string } };
  } catch {
    payload = null;
  }

  if (!response.ok || !payload?.ok) {
    throw new Error(payload?.error?.message ?? `Request failed (${response.status})`);
  }

  return payload.data as T;
}

function latestRevision(): DesignerRevisionDetail | null {
  return state.response?.latestRevision ?? null;
}

function getPendingPrompt(): string {
  return normaliseText(inputElement?.value ?? "", 480);
}

function slugifyFileStem(value: string): string {
  const slug = value
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, "-")
    .replace(/^-+|-+$/g, "")
    .slice(0, 64);
  return slug || "custom-effect";
}

function arrayBufferToBase64(buffer: ArrayBuffer): string {
  let binary = "";
  const bytes = new Uint8Array(buffer);
  const chunkSize = 0x8000;
  for (let index = 0; index < bytes.length; index += chunkSize) {
    binary += String.fromCharCode(...bytes.subarray(index, index + chunkSize));
  }
  return btoa(binary);
}

function formatParamRows(parameters: Record<string, number>): string {
  const entries = Object.entries(parameters);
  if (entries.length === 0) {
    return '<div class="custom-effect-designer-empty">No parameters in this plan yet.</div>';
  }
  return entries
    .map(([key, value]) => `
      <div class="custom-effect-designer-param-row">
        <span>${escapeHtml(key)}</span>
        <strong>${Number.isFinite(value) ? value.toFixed(2) : "0.00"}</strong>
      </div>
    `)
    .join("");
}

function renderThread(messages: DesignerMessage[]): void {
  if (!threadElement) {
    return;
  }
  threadElement.innerHTML = messages.length > 0
    ? messages
        .map((message) => `
          <div class="custom-effect-designer-bubble ${message.role === "user" ? "is-user" : "is-assistant"}">
            <div class="custom-effect-designer-bubble-role">${message.role === "user" ? "You" : "Designer"}</div>
            <div class="custom-effect-designer-bubble-text">${escapeHtml(message.content)}</div>
          </div>
        `)
        .join("")
    : '<div class="custom-effect-designer-empty">Start by describing the effect you want.</div>';
  threadElement.scrollTop = threadElement.scrollHeight;
}

function renderPlan(plan: ModuleDesignPlan | null): void {
  if (!planElement) {
    return;
  }
  if (!plan) {
    planElement.innerHTML = '<div class="custom-effect-designer-empty">Describe the sound you want, then refine it until the plan looks right.</div>';
    return;
  }

  const tags = plan.tags.length > 0
    ? `<div class="custom-effect-designer-tags">${plan.tags.map((tag) => `<span class="custom-effect-designer-tag">${escapeHtml(tag)}</span>`).join("")}</div>`
    : "";
  const highlights = plan.designHighlights.length > 0
    ? `<div class="custom-effect-designer-limitations">${plan.designHighlights.map((item) => `<p>${escapeHtml(item)}</p>`).join("")}</div>`
    : "";
  const limitations = plan.limitations.length > 0
    ? `<div class="custom-effect-designer-limitations">${plan.limitations.map((item) => `<p>${escapeHtml(item)}</p>`).join("")}</div>`
    : "";
  const readinessLabel = plan.executionReadiness === "plan_only" ? "Design Brief" : "Executable";

  planElement.innerHTML = `
    <div class="custom-effect-designer-plan-header">
      <strong>${escapeHtml(plan.title)}</strong>
      <span class="custom-effect-designer-badge">${escapeHtml(plan.category)} · ${readinessLabel}</span>
    </div>
    <p class="custom-effect-designer-plan-summary">${escapeHtml(plan.summary)}</p>
    <div class="custom-effect-designer-param-grid">${formatParamRows(plan.parameters)}</div>
    ${tags}
    ${highlights}
    ${limitations}
  `;
}

function renderRevision(revision: DesignerRevisionDetail | null): void {
  if (!revisionElement) {
    return;
  }
  if (!revision) {
    revisionElement.innerHTML = '<div class="custom-effect-designer-empty">Generate a module to create a usable Custom Effect revision.</div>';
    return;
  }

  const manifest = revision.manifest ?? {};
  const validation = typeof manifest.validation === "object" && manifest.validation !== null
    ? manifest.validation as Record<string, unknown>
    : {};
  const descriptorSummary = typeof manifest.descriptorSummary === "object" && manifest.descriptorSummary !== null
    ? manifest.descriptorSummary as Record<string, unknown>
    : {};
  const fileName = typeof manifest.fileName === "string" ? manifest.fileName : `${revision.title}.wasm`;
  const byteLength = typeof validation.byteLength === "number" ? validation.byteLength : 0;
  const version = typeof descriptorSummary.version === "string" ? descriptorSummary.version : "";
  const specText = normaliseText(revision.specText, 12_000);
  const descriptorText = normaliseText(revision.descriptorText, 12_000);
  const specBlock = specText
    ? `<div class="custom-effect-designer-limitations"><strong>Implementation Spec</strong><pre>${escapeHtml(specText)}</pre></div>`
    : "";
  const descriptorBlock = descriptorText
    ? `<div class="custom-effect-designer-limitations"><strong>Descriptor</strong><pre>${escapeHtml(descriptorText)}</pre></div>`
    : "";

  revisionElement.innerHTML = `
    <div class="custom-effect-designer-plan-header">
      <strong>${escapeHtml(revision.title)}</strong>
      <span class="custom-effect-designer-badge">${escapeHtml(revision.category)}</span>
    </div>
    <p class="custom-effect-designer-plan-summary">${escapeHtml(revision.summary)}</p>
    <div class="custom-effect-designer-revision-meta">
      <div><span>File</span><strong>${escapeHtml(fileName)}</strong></div>
      <div><span>Size</span><strong>${byteLength.toLocaleString()} bytes</strong></div>
      <div><span>Revision</span><strong>${escapeHtml(revision.id)}</strong></div>
      ${version ? `<div><span>Version</span><strong>${escapeHtml(version)}</strong></div>` : ""}
    </div>
    <div class="custom-effect-designer-param-grid">${formatParamRows(revision.plan.parameters)}</div>
    ${specBlock}
    ${descriptorBlock}
  `;
}

function renderDesigner(): void {
  if (!modal) {
    return;
  }

  const response = state.response;
  const plan = response?.session.currentPlan ?? null;
  const revision = latestRevision();
  const pendingPrompt = getPendingPrompt();
  const canGenerate = Boolean(response) && (Boolean(pendingPrompt) || (plan?.executionReadiness !== "plan_only" && Boolean(plan)));

  subtitleElement!.textContent = state.node
    ? `${state.node.displayName || state.node.id} · ${resolveCurrentModuleName(state.node)}`
    : "Describe the Custom Effect you want to build.";
  statusElement!.textContent = state.statusMessage || (response ? `Session ${response.session.id}` : "");
  footerNoteElement!.textContent = plan?.limitations[0] ?? "Generated modules stay local until you explicitly share or publish them.";

  renderThread(response?.session.messages ?? []);
  renderPlan(plan);
  renderRevision(revision);

  const hasRevision = Boolean(revision);
  if (inputElement) {
    inputElement.disabled = state.busy;
  }
  if (sendButton) {
    sendButton.disabled = state.busy || !response;
  }
  if (generateButton) {
    generateButton.disabled = state.busy || !canGenerate;
    if (pendingPrompt) {
      generateButton.textContent = plan ? "Generate From Prompt" : "Design + Generate";
    } else {
      generateButton.textContent = plan?.executionReadiness === "plan_only" ? "Plan Only" : "Generate Module";
    }
  }
  if (downloadButton) {
    downloadButton.disabled = state.busy || !hasRevision;
  }
  if (saveButton) {
    saveButton.disabled = state.busy || !hasRevision;
  }
  if (applyButton) {
    applyButton.disabled = state.busy || !hasRevision;
  }
}

function setBusy(busy: boolean, statusMessage = ""): void {
  state.busy = busy;
  state.statusMessage = statusMessage;
  renderDesigner();
}

function closeDesigner(): void {
  if (!modal) {
    return;
  }
  modal.style.display = "none";
}

async function createSessionForNode(node: GraphNode): Promise<void> {
  const response = await designerApiFetch<DesignerSessionResponse>("/module-sessions", {
    method: "POST",
    body: JSON.stringify({
      nodeContext: buildNodeContext(node),
    }),
  });
  state.response = response;
  sessionCacheByNodeId.set(node.id, {
    contextKey: buildNodeContextKey(node),
    response,
  });
}

async function submitPrompt(content: string, options: { clearInput?: boolean } = {}): Promise<DesignerSessionResponse | null> {
  const sessionId = state.response?.session.id ?? "";
  if (!content || !sessionId) {
    if (!content) {
      showNotification("Custom Effect Designer", "Enter a prompt first");
    }
    return null;
  }

  const response = await designerApiFetch<DesignerSessionResponse>(`/module-sessions/${encodeURIComponent(sessionId)}/messages`, {
    method: "POST",
    body: JSON.stringify({ content }),
  });
  state.response = response;
  if (state.node) {
    sessionCacheByNodeId.set(state.node.id, {
      contextKey: buildNodeContextKey(state.node),
      response,
    });
  }
  if (options.clearInput !== false && inputElement) {
    inputElement.value = "";
  }
  return response;
}

async function sendPrompt(): Promise<void> {
  const content = getPendingPrompt();

  try {
    setBusy(true, "Updating design...");
    await submitPrompt(content);
  } catch (error) {
    showNotification("Custom Effect Designer", describeError(error));
  } finally {
    setBusy(false);
    inputElement?.focus();
  }
}

async function generateRevision(): Promise<void> {
  const sessionId = state.response?.session.id ?? "";
  if (!sessionId) {
    showNotification("Custom Effect Designer", "Open a designer session first");
    return;
  }

  try {
    const pendingPrompt = getPendingPrompt();
    setBusy(true, pendingPrompt ? "Designing and generating..." : "Generating module...");
    if (pendingPrompt) {
      await submitPrompt(pendingPrompt);
    }

    const plan = state.response?.session.currentPlan ?? null;
    if (!plan) {
      showNotification("Custom Effect Designer", "Describe the effect you want first");
      return;
    }
    if (plan.executionReadiness === "plan_only") {
      showNotification("Custom Effect Designer", plan.limitations[0] ?? "This design is not executable in the current generation backend yet.");
      return;
    }

    const response = await designerApiFetch<DesignerSessionResponse>(`/module-sessions/${encodeURIComponent(sessionId)}/generate`, {
      method: "POST",
      body: JSON.stringify({}),
    });
    state.response = response;
    if (state.node) {
      sessionCacheByNodeId.set(state.node.id, {
        contextKey: buildNodeContextKey(state.node),
        response,
      });
    }
  } catch (error) {
    showNotification("Custom Effect Designer", describeError(error));
  } finally {
    setBusy(false);
  }
}

async function downloadLatestRevision(): Promise<void> {
  const revision = latestRevision();
  if (!revision) {
    showNotification("Custom Effect Designer", "Generate a revision first");
    return;
  }

  const zipLib = window.JSZip;
  if (!zipLib) {
    showNotification("Custom Effect Designer", "Archive library is not available in this build");
    return;
  }

  try {
    setBusy(true, "Preparing bundle download...");
    const manifest = revision.manifest ?? {};
    const zip = new zipLib();
    const folderName = `${slugifyFileStem(revision.title)}-${revision.id}`;
    const fileName = typeof manifest.fileName === "string" && manifest.fileName.trim()
      ? manifest.fileName.trim()
      : "module.wasm";

    zip.file(`${folderName}/${fileName}`, revision.moduleBase64, { base64: true });
    zip.file(`${folderName}/manifest.json`, JSON.stringify(manifest, null, 2));
    zip.file(`${folderName}/revision.json`, JSON.stringify({
      id: revision.id,
      title: revision.title,
      summary: revision.summary,
      category: revision.category,
      createdAt: revision.createdAt,
      plan: revision.plan,
    }, null, 2));
    if (revision.descriptorText.trim()) {
      zip.file(`${folderName}/descriptor.txt`, revision.descriptorText);
    }
    if (revision.specText.trim()) {
      zip.file(`${folderName}/spec.txt`, revision.specText);
    }

    const validation = manifest.validation;
    if (validation && typeof validation === "object") {
      zip.file(`${folderName}/validation-report.json`, JSON.stringify(validation, null, 2));
    }

    const blob = await zip.generateAsync({ type: "blob" });
    postMessage({
      type: "exportGeneratedCustomEffectBundle",
      fileName: `${folderName}.custom-effect.zip`,
      data: arrayBufferToBase64(await blob.arrayBuffer()),
    });
  } catch (error) {
    showNotification("Custom Effect Designer", describeError(error));
  } finally {
    setBusy(false);
  }
}

function importLatestRevision(applyToNode: boolean): void {
  const node = state.node;
  const response = state.response;
  const revision = latestRevision();
  if (!node || !response || !revision) {
    showNotification("Custom Effect Designer", "Generate a revision first");
    return;
  }

  const linkedEntry = currentNodeLinkedEntry(node);
  const manifest = revision.manifest ?? {};
  const descriptorSummary = typeof manifest.descriptorSummary === "object" && manifest.descriptorSummary !== null
    ? manifest.descriptorSummary as Record<string, unknown>
    : {};
  const generationBackend = typeof manifest.generationBackend === "object" && manifest.generationBackend !== null
    ? manifest.generationBackend as Record<string, unknown>
    : {};
  const fileName = typeof manifest.fileName === "string" ? manifest.fileName : `${revision.title}.wasm`;
  const generationBackendType = typeof generationBackend.type === "string" ? generationBackend.type : "";
  const baseEntryId = linkedEntry?.id || revision.title.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "") || "custom-effect";
  const moduleResourceId = `custom-effect:${baseEntryId}:${revision.id}`;

  importGeneratedCustomEffect(node.id, {
    ...(linkedEntry?.id ? { id: linkedEntry.id } : {}),
    name: revision.title,
    category: revision.category,
    description: revision.summary,
    origin: "generated",
    latestRevisionId: revision.id,
    tags: revision.plan.tags,
    defaultParams: revision.plan.parameters,
    descriptorSummary,
    moduleData: revision.moduleBase64,
    moduleFileName: fileName,
    moduleResourceId,
    moduleSubfolder: `custom-effects/generated/${response.session.id}/${revision.id}`,
    moduleMetadata: {
      customEffectRevisionId: revision.id,
      customEffectSessionId: response.session.id,
      ...(generationBackendType ? { customEffectGenerationBackend: generationBackendType } : {}),
      customEffectSource: "module-generation-api",
    },
    descriptorText: revision.descriptorText,
    specText: revision.specText,
    manifest,
    sessionId: response.session.id,
  }, applyToNode);

  sessionCacheByNodeId.delete(node.id);

  showNotification(applyToNode ? "Applying generated Custom Effect" : "Saving generated Custom Effect", revision.title);
}

export async function openCustomEffectDesigner(node: GraphNode): Promise<void> {
  if (!isFeatureEnabled(Features.CustomEffects)) {
    showNotification("Custom Effect Designer", "Custom Effects are disabled in Settings > Features");
    return;
  }

  if (!modal) {
    showNotification("Custom Effect Designer", "Designer UI is unavailable");
    return;
  }

  state.node = node;
  modal.style.display = "flex";

  const cached = sessionCacheByNodeId.get(node.id);
  const contextKey = buildNodeContextKey(node);
  if (cached && cached.contextKey === contextKey) {
    state.response = cached.response;
    state.statusMessage = "";
    renderDesigner();
    inputElement?.focus();
    return;
  }
  if (cached) {
    sessionCacheByNodeId.delete(node.id);
  }

  try {
    state.response = null;
    setBusy(true, "Opening designer...");
    await createSessionForNode(node);
  } catch (error) {
    showNotification("Custom Effect Designer", describeError(error));
  } finally {
    setBusy(false);
    inputElement?.focus();
  }
}

export function initializeCustomEffectDesignerModal(): void {
  if (!modal) {
    return;
  }

  const closeButton = document.getElementById("custom-effect-designer-close") as HTMLButtonElement | null;
  closeButton?.addEventListener("click", closeDesigner);
  modal.addEventListener("mousedown", (event) => {
    if (event.target === modal) {
      closeDesigner();
    }
  });

  sendButton?.addEventListener("click", () => {
    void sendPrompt();
  });
  generateButton?.addEventListener("click", () => {
    void generateRevision();
  });
  downloadButton?.addEventListener("click", () => {
    void downloadLatestRevision();
  });
  saveButton?.addEventListener("click", () => {
    importLatestRevision(false);
  });
  applyButton?.addEventListener("click", () => {
    importLatestRevision(true);
  });
  inputElement?.addEventListener("keydown", (event) => {
    if (event.key === "Enter" && !event.shiftKey) {
      event.preventDefault();
      void sendPrompt();
    }
  });
  inputElement?.addEventListener("input", () => {
    renderDesigner();
  });

  renderDesigner();
}