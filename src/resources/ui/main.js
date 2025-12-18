const presetListElement = document.getElementById("preset-list");
const presetDetailsElement = document.getElementById("preset-details");
const presetSearchElement = document.getElementById("preset-search");
const logPanelElement = document.getElementById("log-panel");
const tabButtons = Array.from(document.querySelectorAll(".tab-button"));
const tabPanels = Array.from(document.querySelectorAll(".tab-panel"));
const appRootElement = document.getElementById("app");

const notificationElement = document.createElement("div");
notificationElement.id = "notification";
notificationElement.className = "notification";
if (appRootElement && typeof appRootElement.prepend === "function") {
  appRootElement.prepend(notificationElement);
} else {
  document.body.prepend(notificationElement);
}

const REMOTE_BASE_URL = window.NAM_REMOTE_BASE_URL ?? "";

/**
 * Library of known NAM models bundled with the application.
 * Each model has a unique hash-based ID derived from the file's SHA-256 hash.
 * The filePath is relative to the application's resource directory.
 * Loaded from data/nam-models.json on startup.
 */
let NAM_MODEL_LIBRARY = [];

/**
 * Library of known Impulse Responses bundled with the application.
 * Each IR has a unique hash-based ID derived from the file's SHA-256 hash.
 * The filePath is relative to the application's resource directory.
 * Loaded from data/ir-library.json on startup.
 */
let IR_LIBRARY = [];

/**
 * Default factory presets bundled with the application.
 * Presets reference NAM models and IRs by their library IDs.
 * Loaded from data/default-presets.json on startup.
 */
let DEFAULT_PRESETS = [];

/**
 * Resolves a NAM model by its library ID.
 * @param {string} modelId - The model library ID
 * @returns {object|null} The model entry or null if not found
 */
function resolveNamModel(modelId) {
  return NAM_MODEL_LIBRARY.find((m) => m.id === modelId) ?? null;
}

/**
 * Resolves an IR by its library ID.
 * @param {string} irId - The IR library ID
 * @returns {object|null} The IR entry or null if not found
 */
function resolveIR(irId) {
  return IR_LIBRARY.find((ir) => ir.id === irId) ?? null;
}

/**
 * Builds attachments array for a preset from model and IR library IDs.
 * Falls back to custom file paths if IDs are not found in the library.
 * @param {string|null} namModelId - NAM model library ID or null
 * @param {string|null} irId - IR library ID or null
 * @param {string|null} customNamPath - Custom NAM file path (used if namModelId not in library)
 * @param {string|null} customIrPath - Custom IR file path (used if irId not in library)
 * @returns {Array} Array of attachment objects
 */
function buildAttachments(namModelId, irId, customNamPath = null, customIrPath = null) {
  const attachments = [];

  // Resolve NAM model
  if (namModelId) {
    const model = resolveNamModel(namModelId);
    if (model) {
      attachments.push({
        type: "nam",
        id: model.id,
        filePath: model.filePath,
        hash: model.hash,
      });
    } else if (customNamPath) {
      attachments.push({ type: "nam", filePath: customNamPath, hash: "" });
    }
  } else if (customNamPath) {
    attachments.push({ type: "nam", filePath: customNamPath, hash: "" });
  }

  // Resolve IR
  if (irId) {
    const ir = resolveIR(irId);
    if (ir) {
      attachments.push({
        type: "ir",
        id: ir.id,
        filePath: ir.filePath,
        hash: ir.hash,
      });
    } else if (customIrPath) {
      attachments.push({ type: "ir", filePath: customIrPath, hash: "" });
    }
  } else if (customIrPath) {
    attachments.push({ type: "ir", filePath: customIrPath, hash: "" });
  }

  return attachments;
}

/**
 * Loads the NAM model library from the JSON file.
 * @returns {Promise<Array>} Array of NAM model objects
 */
async function loadNamModelLibrary() {
  try {
    const response = await fetch("data/nam-models.json");
    if (!response.ok) {
      throw new Error(`Failed to load NAM models: ${response.status}`);
    }
    return await response.json();
  } catch (error) {
    console.error(`Error loading NAM model library: ${error.message}`);
    return [];
  }
}

/**
 * Loads the IR library from the JSON file.
 * @returns {Promise<Array>} Array of IR objects
 */
async function loadIrLibrary() {
  try {
    const response = await fetch("data/ir-library.json");
    if (!response.ok) {
      throw new Error(`Failed to load IR library: ${response.status}`);
    }
    return await response.json();
  } catch (error) {
    console.error(`Error loading IR library: ${error.message}`);
    return [];
  }
}

/**
 * Loads the default presets from the JSON file and builds attachments.
 * @returns {Promise<Array>} Array of preset objects with resolved attachments
 */
async function loadDefaultPresets() {
  try {
    const response = await fetch("data/default-presets.json");
    if (!response.ok) {
      throw new Error(`Failed to load default presets: ${response.status}`);
    }
    const presets = await response.json();
    // Build attachments for each preset based on library IDs
    return presets.map((preset) => ({
      ...preset,
      attachments: buildAttachments(preset.namModelId, preset.irId),
    }));
  } catch (error) {
    console.error(`Error loading default presets: ${error.message}`);
    return [];
  }
}

/**
 * Initializes all data libraries from JSON files.
 * Must be called before using NAM_MODEL_LIBRARY, IR_LIBRARY, or DEFAULT_PRESETS.
 */
async function initializeDataLibraries() {
  // Load model and IR libraries first (needed for preset attachments)
  const [models, irs] = await Promise.all([
    loadNamModelLibrary(),
    loadIrLibrary(),
  ]);
  NAM_MODEL_LIBRARY = models;
  IR_LIBRARY = irs;
  
  // Load presets after libraries are ready
  DEFAULT_PRESETS = await loadDefaultPresets();
  
  console.log(`Loaded ${NAM_MODEL_LIBRARY.length} NAM models, ${IR_LIBRARY.length} IRs, ${DEFAULT_PRESETS.length} default presets`);
}

const DEMO_AUDIO_SAMPLES = [
  {
    id: "guitar-riff-01",
    title: "Guitar Riff 01",
    path: "demo/guitar-riff-01.wav",
  },
  {
    id: "guitar-riff-02",
    title: "Guitar Riff 02",
    path: "demo/guitar-riff-02.wav",
  },
];

const LOG_ENTRY_LIMIT = 200;

const uiState = {
  presets: [],
  filteredPresets: [],
  activePresetId: null,
  presetCache: new Map(),
  parameters: {
    values: [],
    gateEnabled: false,
    gateThreshold: null,
    modelPath: "",
    irPath: "",
  },
  signalTest: null,
  demoAudioSelectedId: DEMO_AUDIO_SAMPLES.length ? DEMO_AUDIO_SAMPLES[0].id : null,
  demoAudioRepeat: false,
  logs: [],
};

window.NAMBridge = {
  postMessage(message) {
    if (window.IPlugSendMsg) {
      window.IPlugSendMsg(message);
    }
  },
};

/**
 * Sends a parameter change message to the C++ backend.
 * @param {string} id - Parameter ID (e.g., "mix", "drive", "tone", "output_trim")
 * @param {number} value - Parameter value
 */
function setParameter(id, value) {
  window.NAMBridge.postMessage({
    type: "setParameter",
    id,
    value,
  });
}

/**
 * Updates the display value for a control slider.
 * @param {string} controlId - The control element ID (without "control-" prefix)
 * @param {number} value - The current value
 * @param {string} format - Format type: "percent", "db", or "value"
 */
function updateControlDisplay(controlId, value, format = "percent") {
  const displayElement = document.getElementById(`control-${controlId}-value`);
  if (!displayElement) return;

  let displayText;
  switch (format) {
    case "db":
      displayText = `${value >= 0 ? "+" : ""}${value.toFixed(1)} dB`;
      break;
    case "percent":
      displayText = `${Math.round(value * 100)}%`;
      break;
    default:
      displayText = value.toFixed(2);
  }
  displayElement.textContent = displayText;
}

/**
 * Initializes the control panel sliders and their event handlers.
 */
function initializeControls() {
  const controls = [
    { id: "mix", paramId: "mix", format: "percent" },
    { id: "drive", paramId: "drive", format: "percent" },
    { id: "output-trim", paramId: "output_trim", format: "db" },
    { id: "tone", paramId: "tone", format: "percent" },
    { id: "input-trim", paramId: "input_trim", format: "db" },
  ];

  controls.forEach(({ id, paramId, format }) => {
    const slider = document.getElementById(`control-${id}`);
    if (!slider) return;

    // Update display on input (while dragging)
    slider.addEventListener("input", () => {
      const value = parseFloat(slider.value);
      updateControlDisplay(id, value, format);
    });

    // Send to backend on change (when released)
    slider.addEventListener("change", () => {
      const value = parseFloat(slider.value);
      setParameter(paramId, value);
      appendLog(`${paramId} → ${value}`);
    });
  });
}

/**
 * Updates the control sliders from the current UI state parameters.
 */
function syncControlsFromState() {
  const paramToControl = {
    mix: { id: "mix", format: "percent" },
    drive: { id: "drive", format: "percent" },
    output_trim: { id: "output-trim", format: "db" },
    tone: { id: "tone", format: "percent" },
    input_trim: { id: "input-trim", format: "db" },
  };

  // Extract values from the parameters array
  const paramValues = {};
  if (Array.isArray(uiState.parameters.values)) {
    uiState.parameters.values.forEach((param) => {
      paramValues[param.id] = param.value;
    });
  }

  Object.entries(paramToControl).forEach(([paramId, { id, format }]) => {
    const slider = document.getElementById(`control-${id}`);
    if (!slider) return;

    const value = paramValues[paramId];
    if (typeof value === "number") {
      slider.value = value;
      updateControlDisplay(id, value, format);
    }
  });
}

function clonePreset(preset) {
  return JSON.parse(JSON.stringify(preset));
}

function clearNotification() {
  notificationElement.textContent = "";
  notificationElement.classList.remove("visible");
}

function showNotification(message, detail = "") {
  const resolvedMessage = detail ? `${message}: ${detail}` : message;
  notificationElement.textContent = resolvedMessage;
  notificationElement.classList.add("visible");
}

function activateTab(tabId) {
  if (!tabButtons.length || !tabPanels.length) {
    return;
  }

  tabButtons.forEach((button) => {
    const isActive = button.dataset.tab === tabId;
    button.classList.toggle("active", isActive);
  });

  tabPanels.forEach((panel) => {
    const isDetailsPanel = panel.id === "preset-details" && tabId === "details";
    const isLogPanel = panel.id === "log-panel" && tabId === "logs";
    panel.classList.toggle("active", isDetailsPanel || isLogPanel);
  });
}

function escapeHtml(value) {
  return String(value)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#039;");
}

function renderLogEntries() {
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

function appendLog(message) {
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

const nativeFetch = typeof window.fetch === "function" ? window.fetch.bind(window) : null;

if (nativeFetch) {
  // Wrap fetch so we can display request activity inside the log tab.
  window.fetch = async (...args) => {
    const [input] = args;
    const url = typeof input === "string" ? input : input?.url ?? "(unknown)";
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

function requestSignalPathTest() {
  clearNotification();
  window.NAMBridge.postMessage({
    type: "runSignalPathTest",
    frequency: 440,
    duration: 1.0,
  });
}

function renderPresetList(presets) {
  if (!presets.length) {
    presetListElement.innerHTML = '<p class="empty">No presets available.</p>';
    return;
  }

  presetListElement.innerHTML = presets
    .map(
      (preset) => `
        <article class="preset ${preset.id === uiState.activePresetId ? "active" : ""}" data-id="${preset.id}">
          <header>
            <h3>${preset.name}</h3>
            <span>${preset.category ?? ""}</span>
          </header>
          <p>${preset.description ?? ""}</p>
        </article>
      `,
    )
    .join("");

  presetListElement.querySelectorAll("article.preset").forEach((element) => {
    element.addEventListener("click", async () => {
      const presetId = element.getAttribute("data-id");
      if (!presetId) {
        return;
      }
      await applyPresetFromLibrary(presetId);
    });
  });
}

function formatParameterValue(parameter) {
  if (typeof parameter.value === "number") {
    return parameter.value.toFixed(2);
  }
  if (typeof parameter.value === "boolean") {
    return parameter.value ? "On" : "Off";
  }
  return `${parameter.value ?? ""}`;
}

function renderParameterSection() {
  const parameterItems = (uiState.parameters.values ?? [])
    .map((parameter) => {
      const label = parameter.label ?? parameter.id ?? "";
      return `
        <li>
          <span class="parameter-label">${label}</span>
          <span class="parameter-value">${formatParameterValue(parameter)}</span>
        </li>
      `;
    })
    .join("");

  const hasParameters = Boolean(parameterItems);
  const gateInfo = typeof uiState.parameters.gateEnabled === "boolean"
    ? `
        <li>
          <span class="parameter-label">Noise Gate</span>
          <span class="parameter-value">${uiState.parameters.gateEnabled ? "On" : "Off"}${
            typeof uiState.parameters.gateThreshold === "number"
              ? ` (${uiState.parameters.gateThreshold.toFixed(1)} dB)`
              : ""
          }</span>
        </li>
      `
    : "";

  const signalPath = `
    <ul class="signal-path">
      <li><span class="parameter-label">Model</span><span class="parameter-value">${uiState.parameters.modelPath || "None"}</span></li>
      <li><span class="parameter-label">Impulse Response</span><span class="parameter-value">${uiState.parameters.irPath || "None"}</span></li>
    </ul>
  `;

  const signalTestSection = uiState.signalTest
    ? `
        <section class="signal-test-results">
          <h4>Last Signal Path Test</h4>
          <ul>
            <li><span class="parameter-label">Frequency</span><span class="parameter-value">${uiState.signalTest.frequency.toFixed(1)} Hz</span></li>
            <li><span class="parameter-label">Duration</span><span class="parameter-value">${uiState.signalTest.duration.toFixed(2)} s</span></li>
            <li><span class="parameter-label">Input RMS</span><span class="parameter-value">${uiState.signalTest.inputRMS.toFixed(4)}</span></li>
            <li><span class="parameter-label">Output RMS (L/R)</span><span class="parameter-value">${uiState.signalTest.outputLeft.toFixed(4)} / ${uiState.signalTest.outputRight.toFixed(4)}</span></li>
            <li><span class="parameter-label">Status</span><span class="parameter-value ${uiState.signalTest.passed ? "status-pass" : "status-fail"}">${uiState.signalTest.passed ? "Pass" : "Fail"}</span></li>
          </ul>
          ${uiState.signalTest.message ? `<p class="signal-test-message">${uiState.signalTest.message}</p>` : ""}
        </section>
      `
    : "";

  if (!hasParameters && !gateInfo && !uiState.parameters.modelPath && !uiState.parameters.irPath) {
    return `
      <section class="signal-test-controls">
        <button id="run-signal-test">Run Signal Path Test</button>
      </section>
      ${signalTestSection}
    `;
  }

  return `
    <section>
      <h3>Current Parameters</h3>
      <ul class="parameter-list">
        ${parameterItems}
        ${gateInfo}
      </ul>
      <h4>Signal Path</h4>
      ${signalPath}
    </section>
    <section class="signal-test-controls">
      <button id="run-signal-test">Run Signal Path Test</button>
    </section>
    ${signalTestSection}
  `;
}

function getSelectedDemoAudio() {
  if (!DEMO_AUDIO_SAMPLES.length) {
    return null;
  }
  const selectedId = uiState.demoAudioSelectedId ?? DEMO_AUDIO_SAMPLES[0].id;
  return DEMO_AUDIO_SAMPLES.find((sample) => sample.id === selectedId) ?? DEMO_AUDIO_SAMPLES[0];
}

function renderDemoAudioControls() {
  if (!DEMO_AUDIO_SAMPLES.length) {
    return "";
  }

  const options = DEMO_AUDIO_SAMPLES
    .map((sample) => {
      const selected = sample.id === (uiState.demoAudioSelectedId ?? DEMO_AUDIO_SAMPLES[0].id);
      return `<option value="${escapeHtml(sample.id)}"${selected ? " selected" : ""}>${escapeHtml(sample.title)}</option>`;
    })
    .join("");

  return `
    <section class="demo-audio">
      <h3>Demo Audio</h3>
      <div class="demo-audio-controls">
        <label for="demo-audio-select">Preview</label>
        <select id="demo-audio-select" class="demo-audio-select">
          ${options}
        </select>
        <button id="play-demo-audio" class="demo-audio-button">Play</button>
        <label class="demo-audio-repeat">
          <input type="checkbox" id="demo-audio-repeat-checkbox" />
          <span>Repeat</span>
        </label>
      </div>
    </section>
  `;
}

function bindDemoAudioControls() {
  const selectElement = document.getElementById("demo-audio-select");
  if (selectElement) {
    selectElement.value = uiState.demoAudioSelectedId ?? selectElement.value;
    selectElement.addEventListener("change", (event) => {
      const value = event.target.value;
      uiState.demoAudioSelectedId = value;
    });
  }

  const playButton = document.getElementById("play-demo-audio");
  if (playButton) {
    playButton.addEventListener("click", async () => {
      await previewSelectedDemoAudio();
    });
  }

  const repeatCheckbox = document.getElementById("demo-audio-repeat-checkbox");
  if (repeatCheckbox) {
    repeatCheckbox.checked = uiState.demoAudioRepeat;
    repeatCheckbox.addEventListener("change", (event) => {
      uiState.demoAudioRepeat = event.target.checked;
    });
  }
}

function resolveDemoSamplePath(rawPath) {
  if (!rawPath || typeof rawPath !== "string") {
    return null;
  }

  // Already a URL - use as-is
  if (/^https?:\/\//i.test(rawPath)) {
    return rawPath;
  }

  // Normalize path separators and return relative path
  // The UI is served from the resources/ui folder, so relative paths work directly
  const normalized = rawPath.replace(/\\/g, "/");
  
  // If it's already a simple relative path, use it
  if (!normalized.includes(":") && !normalized.startsWith("/")) {
    return normalized;
  }

  // Try to extract relative path from absolute paths containing /resources/ui/
  const uiIndex = normalized.toLowerCase().indexOf("/resources/ui/");
  if (uiIndex >= 0) {
    return normalized.slice(uiIndex + "/resources/ui/".length);
  }

  // Fallback: just use the filename
  const lastSlash = normalized.lastIndexOf("/");
  return lastSlash >= 0 ? normalized.slice(lastSlash + 1) : normalized;
}

function parseWavMetadata(arrayBuffer) {
  const view = new DataView(arrayBuffer);
  if (view.byteLength < 44) {
    return null;
  }

  const chunkId = String.fromCharCode(
    view.getUint8(0),
    view.getUint8(1),
    view.getUint8(2),
    view.getUint8(3),
  );
  const format = String.fromCharCode(
    view.getUint8(8),
    view.getUint8(9),
    view.getUint8(10),
    view.getUint8(11),
  );
  if (chunkId !== "RIFF" || format !== "WAVE") {
    return null;
  }

  let offset = 12;
  let channels = 0;
  let sampleRate = 0;
  let bitsPerSample = 0;

  while (offset + 8 <= view.byteLength) {
    const id = String.fromCharCode(
      view.getUint8(offset),
      view.getUint8(offset + 1),
      view.getUint8(offset + 2),
      view.getUint8(offset + 3),
    );
    const size = view.getUint32(offset + 4, true);
    const chunkStart = offset + 8;
    if (id === "fmt ") {
      const audioFormat = view.getUint16(chunkStart, true);
      if (audioFormat !== 1 && audioFormat !== 3) {
        return null;
      }
      channels = view.getUint16(chunkStart + 2, true);
      sampleRate = view.getUint32(chunkStart + 4, true);
      bitsPerSample = view.getUint16(chunkStart + 14, true);
      break;
    }
    offset = chunkStart + size + (size % 2);
  }

  if (!channels || !sampleRate || !bitsPerSample) {
    return null;
  }

  return { channels, sampleRate, bitsPerSample };
}

async function previewSelectedDemoAudio() {
  const sample = getSelectedDemoAudio();
  if (!sample) {
    showNotification("No demo audio available");
    return;
  }

  try {
    const resolvedPath = resolveDemoSamplePath(sample.path);
    if (!resolvedPath) {
      throw new Error("Demo audio path is not set");
    }

    appendLog(`preview start → ${resolvedPath}`);
    const response = await fetch(resolvedPath);
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }

    const buffer = await response.arrayBuffer();
    const base64 = arrayBufferToBase64(buffer);
    const metadata = parseWavMetadata(buffer);

    const audioPayload = {
      id: sample.id,
      title: sample.title,
      path: sample.path,
      size: buffer.byteLength,
      contentType: "audio/wav",
      data: base64,
    };

    if (metadata) {
      audioPayload.sampleRate = metadata.sampleRate;
      audioPayload.channels = metadata.channels;
      audioPayload.bitsPerSample = metadata.bitsPerSample;
    }

    window.NAMBridge.postMessage({
      type: "previewDemoAudio",
      audio: audioPayload,
    });

    showNotification("Starting demo preview", sample.title);
    appendLog(`preview sent → ${sample.title}`);
  } catch (error) {
    console.error("Failed to preview demo audio", error);
    //appendLog(`preview error ← ${sample.title}: ${error instanceof Error ? error.message : String(error)}`);
    appendLog(`preview error ← ${sample.title}: ${JSON.stringify(error)}`);
    showNotification("Failed to preview demo audio", error instanceof Error ? error: String(error));
  }
}

function renderPresetDetails(preset) {
  if (!preset) {
    const parameterSection = renderParameterSection();
    const demoSection = renderDemoAudioControls();
    presetDetailsElement.innerHTML = `
      <h2>No Preset Loaded</h2>
      <p>Select a preset to see details.</p>
      ${demoSection}
      ${parameterSection}
    `;
    const signalTestButton = document.getElementById("run-signal-test");
    if (signalTestButton) {
      signalTestButton.addEventListener("click", requestSignalPathTest);
    }
    bindDemoAudioControls();
    return;
  }

  const attachments = (preset.attachments ?? [])
    .map(
      (attachment) => `
        <li>
          <strong>${attachment.type}</strong>
          <span>${attachment.hash ?? ""}</span>
        </li>
      `,
    )
    .join("");

  const fxChain = (preset.fxChain ?? [])
    .map((stage) => `<li>${stage}</li>`)
    .join("");

  presetDetailsElement.innerHTML = `
    <h2>${preset.name}</h2>
    <p>${preset.description ?? ""}</p>
    <section>
      <h3>FX Chain</h3>
      <ul>${fxChain}</ul>
    </section>
    <section>
      <h3>Attachments</h3>
      <ul>${attachments}</ul>
    </section>
    <button id="apply-preset">Apply Preset</button>
    ${renderDemoAudioControls()}
    ${renderParameterSection()}
  `;

  const applyButton = document.getElementById("apply-preset");
  if (applyButton) {
    applyButton.addEventListener("click", async () => {
      await applyPresetFromLibrary(preset.id);
    });
  }

  const signalTestButton = document.getElementById("run-signal-test");
  if (signalTestButton) {
    signalTestButton.addEventListener("click", requestSignalPathTest);
  }
  bindDemoAudioControls();
}

function handleIncomingMessage(message) {
  const payload = JSON.parse(message);
  switch (payload.type) {
    case "state": {
      uiState.activePresetId = payload.activePresetId ?? null;
      if (payload.parameters) {
        uiState.parameters = {
          values: Array.isArray(payload.parameters.parameters) ? payload.parameters.parameters : [],
          gateEnabled: payload.parameters.gateEnabled ?? false,
          gateThreshold: typeof payload.parameters.gateThreshold === "number" ? payload.parameters.gateThreshold : null,
          modelPath: payload.parameters.modelPath ?? "",
          irPath: payload.parameters.irPath ?? "",
        };
      }
      uiState.signalTest = null;
      if (payload.preset) {
        uiState.presetCache.set(payload.preset.id, payload.preset);
        if (!uiState.presets.some((preset) => preset.id === payload.preset.id)) {
          uiState.presets = [payload.preset, ...uiState.presets];
          filterPresets(presetSearchElement?.value ?? "");
        }
      }
      renderPresetList(uiState.filteredPresets);
      const preset = payload.preset ?? uiState.presetCache.get(uiState.activePresetId) ?? null;
      renderPresetDetails(preset ? clonePreset(preset) : null);
      syncControlsFromState();
      clearNotification();
      break;
    }
    case "presetLoaded": {
      const preset = payload.preset;
      if (preset) {
        uiState.activePresetId = preset.id;
        uiState.presetCache.set(preset.id, preset);
        renderPresetDetails(clonePreset(preset));
      }
      if (payload.parameters) {
        uiState.parameters = {
          values: Array.isArray(payload.parameters.parameters) ? payload.parameters.parameters : uiState.parameters.values,
          gateEnabled: payload.parameters.gateEnabled ?? uiState.parameters.gateEnabled,
          gateThreshold:
            typeof payload.parameters.gateThreshold === "number"
              ? payload.parameters.gateThreshold
              : uiState.parameters.gateThreshold,
          modelPath: payload.parameters.modelPath ?? uiState.parameters.modelPath,
          irPath: payload.parameters.irPath ?? uiState.parameters.irPath,
        };
      }
      renderPresetList(uiState.filteredPresets);
      syncControlsFromState();
      clearNotification();
      break;
    }
    case "signalPathTestResult": {
      uiState.signalTest = {
        frequency: payload.frequency ?? 0,
        duration: payload.duration ?? 0,
        sampleRate: payload.sampleRate ?? 0,
        inputRMS: payload.inputRMS ?? 0,
        outputLeft: Array.isArray(payload.outputRMS) ? payload.outputRMS[0] ?? 0 : 0,
        outputRight: Array.isArray(payload.outputRMS) ? payload.outputRMS[1] ?? 0 : 0,
        passed: Boolean(payload.passed),
        message: payload.message ?? "",
      };
      renderPresetDetails(uiState.presetCache.get(uiState.activePresetId) ?? null);
      const statusMessage = uiState.signalTest.passed ? "Signal path test passed" : "Signal path test failed";
      showNotification(statusMessage, uiState.signalTest.message ?? "");
      break;
    }
    case "previewStarted": {
      appendLog(`preview started ← ${payload.title ?? payload.id ?? "demo"}`);
      uiState.demoAudioSelectedId = payload.id ?? uiState.demoAudioSelectedId;
      const selector = document.getElementById("demo-audio-select");
      if (selector && uiState.demoAudioSelectedId) {
        selector.value = uiState.demoAudioSelectedId;
      }
      showNotification("Playing demo audio", payload.title ?? "Demo");
      break;
    }
    case "previewComplete": {
      appendLog(`preview complete ← ${payload.title ?? payload.id ?? "demo"}`);
      if (uiState.demoAudioRepeat) {
        // Restart playback if repeat is enabled
        previewSelectedDemoAudio();
      } else {
        showNotification("Demo playback finished", payload.title ?? "Demo");
      }
      break;
    }
    case "error": {
      console.error("Plugin error", payload);
      showNotification(payload.message ?? "An error occurred", payload.detail ?? "");
      break;
    }
    default:
      console.warn("Unknown message type", payload.type);
  }
}

function filterPresets(query) {
  const normalized = query.trim().toLowerCase();
  if (!normalized) {
    uiState.filteredPresets = uiState.presets.slice();
  } else {
    uiState.filteredPresets = uiState.presets.filter((preset) => {
      const tokens = [preset.name, preset.category, preset.description];
      return tokens.some((token) => token && token.toLowerCase().includes(normalized));
    });
  }
  renderPresetList(uiState.filteredPresets);
}

async function loadPresetMetadata(presetId) {
  if (uiState.presetCache.has(presetId)) {
    return clonePreset(uiState.presetCache.get(presetId));
  }

  const localPreset = uiState.presets.find((preset) => preset.id === presetId);
  if (localPreset) {
    uiState.presetCache.set(localPreset.id, localPreset);
    return clonePreset(localPreset);
  }

  if (!REMOTE_BASE_URL) {
    throw new Error("Remote preset service is not configured.");
  }

  const baseUrl = REMOTE_BASE_URL.replace(/\/$/, "");
  const response = await fetch(`${baseUrl}/presets/${encodeURIComponent(presetId)}`);
  if (!response.ok) {
    throw new Error(`Failed to fetch preset ${presetId}: ${response.status}`);
  }

  const data = await response.json();
  const preset = Array.isArray(data) ? data[0] : data;
  if (!preset) {
    throw new Error(`Preset ${presetId} not found`);
  }

  uiState.presetCache.set(preset.id, preset);
  return clonePreset(preset);
}

function resolveAttachmentUrl(attachment) {
  const candidates = [
    attachment.downloadUrl,
    attachment.url,
    attachment.href,
    attachment.filePath,
    attachment.path,
  ].filter(Boolean);

  const baseUrl = REMOTE_BASE_URL.replace(/\/$/, "");

  for (const candidate of candidates) {
    if (typeof candidate !== "string") {
      continue;
    }
    if (/^https?:\/\//i.test(candidate)) {
      return candidate;
    }

    if (candidate.startsWith("/")) {
      if (baseUrl) {
        return `${baseUrl}${candidate}`;
      }
      return candidate;
    }

    if (!baseUrl) {
      if (candidate.startsWith("./") || candidate.startsWith("../") || !candidate.includes(":")) {
        return candidate;
      }
      continue;
    }

    return `${baseUrl}/${candidate.replace(/^\.\//, "")}`;
  }

  return null;
}

function arrayBufferToBase64(buffer) {
  const bytes = new Uint8Array(buffer);
  let binary = "";
  const chunkSize = 0x8000;
  for (let offset = 0; offset < bytes.length; offset += chunkSize) {
    const slice = bytes.subarray(offset, offset + chunkSize);
    binary += String.fromCharCode(...slice);
  }
  return btoa(binary);
}

/**
 * Checks if an attachment URL is a remote HTTP(S) URL that can be fetched.
 * Local/relative paths should be passed directly to the C++ backend for resolution.
 */
function isRemoteUrl(url) {
  return typeof url === "string" && /^https?:\/\//i.test(url);
}

/**
 * Enriches an attachment by fetching its data if it's from a remote URL.
 * For local/bundled attachments (relative paths), the attachment is returned as-is
 * since the C++ backend can resolve the path directly from the resource directory.
 */
async function enrichAttachment(attachment) {
  // Already has embedded data - no need to fetch
  if (attachment.data) {
    return attachment;
  }

  const url = resolveAttachmentUrl(attachment);
  if (!url) {
    return attachment;
  }

  // Only fetch remote URLs - local paths are resolved by the C++ backend
  if (!isRemoteUrl(url)) {
    return attachment;
  }

  const response = await fetch(url);
  if (!response.ok) {
    throw new Error(`Failed to fetch attachment from ${url}`);
  }

  const buffer = await response.arrayBuffer();
  return {
    ...attachment,
    data: arrayBufferToBase64(buffer),
  };
}

async function applyPresetFromLibrary(presetId) {
  try {
    clearNotification();
    const preset = await loadPresetMetadata(presetId);
    const attachments = await Promise.all((preset.attachments ?? []).map(enrichAttachment));
    const presetPayload = {
      ...preset,
      attachments,
    };

    uiState.presetCache.set(presetPayload.id, clonePreset(presetPayload));

    uiState.activePresetId = presetPayload.id;
    renderPresetList(uiState.filteredPresets);
    renderPresetDetails(clonePreset(presetPayload));
    window.NAMBridge.postMessage({
      type: "loadPreset",
      preset: presetPayload,
    });
  } catch (error) {
    console.error("Failed to apply preset", error);
    showNotification("Failed to apply preset", error instanceof Error ? error.message : "Unknown error");
  }
}

async function loadPresetIndex() {
  try {
    if (!REMOTE_BASE_URL) {
      throw new Error("Remote preset service disabled");
    }

    const response = await fetch(`${REMOTE_BASE_URL.replace(/\/$/, "")}/presets`);
    if (!response.ok) {
      throw new Error(`Failed to fetch presets index: ${response.status}`);
    }

    const data = await response.json();
    const presets = Array.isArray(data) ? data : data.presets ?? [];
    uiState.presets = presets.length ? presets : DEFAULT_PRESETS.slice();
    uiState.filteredPresets = uiState.presets.slice();
    uiState.presets.forEach((preset) => {
      uiState.presetCache.set(preset.id, preset);
    });
    renderPresetList(uiState.filteredPresets);
  } catch (error) {
    console.error("Failed to load preset index", error);
    uiState.presets = DEFAULT_PRESETS.slice();
    uiState.filteredPresets = uiState.presets.slice();
    uiState.presets.forEach((preset) => {
      uiState.presetCache.set(preset.id, preset);
    });
    renderPresetList(uiState.filteredPresets);
  }
}

async function initialize() {
  // Load data libraries first (models, IRs, presets)
  await initializeDataLibraries();
  
  if (REMOTE_BASE_URL) {
    await loadPresetIndex();
  } else {
    uiState.presets = DEFAULT_PRESETS.slice();
    uiState.filteredPresets = uiState.presets.slice();
    uiState.presets.forEach((preset) => {
      uiState.presetCache.set(preset.id, preset);
    });
    renderPresetList(uiState.filteredPresets);
  }
  window.NAMBridge.postMessage({ type: "requestState" });
}

tabButtons.forEach((button) => {
  button.addEventListener("click", () => {
    const tabId = button.dataset.tab ?? "";
    if (tabId) {
      activateTab(tabId);
    }
  });
});

activateTab("details");
renderLogEntries();
initializeControls();

presetSearchElement?.addEventListener("input", (event) => {
  filterPresets(event.target.value ?? "");
});

window.IPlugReceiveData = (message) => {
  handleIncomingMessage(message);
};

renderPresetList([]);
renderPresetDetails(null);
initialize();
