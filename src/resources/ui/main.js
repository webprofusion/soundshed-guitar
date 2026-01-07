const presetListElement = document.getElementById("preset-list");
const presetDetailsElement = document.getElementById("preset-details");
const presetSearchElement = document.getElementById("preset-search");
const logPanelElement = document.getElementById("log-panel");
const tabButtons = Array.from(document.querySelectorAll(".tab-button"));
const tabPanels = Array.from(document.querySelectorAll(".tab-panel"));
const appRootElement = document.getElementById("app");

// New UI elements for amp-style interface
const presetDropdown = document.getElementById("preset-dropdown");
const prevPresetBtn = document.getElementById("prev-preset");
const nextPresetBtn = document.getElementById("next-preset");

// Icon bar tab navigation
const iconBarButtons = Array.from(document.querySelectorAll(".icon-bar .icon-btn"));
const mainTabPanels = Array.from(document.querySelectorAll(".main-content .tab-panel"));

const notificationElement =  document.getElementById("notification-area");

const REMOTE_BASE_URL = window.AUDIOFX_REMOTE_BASE_URL ?? "";

/**
 * Library of known AudioFX models bundled with the application.
 * Each model has a unique hash-based ID derived from the file's SHA-256 hash.
 * The filePath is relative to the application's resource directory.
 * Loaded from data/audiofx-models.json on startup.
 */
let AUDIOFX_MODEL_LIBRARY = [];

/**
 * Library of known Impulse Responses bundled with the application.
 * Each IR has a unique hash-based ID derived from the file's SHA-256 hash.
 * The filePath is relative to the application's resource directory.
 * Loaded from data/ir-library.json on startup.
 */
let IR_LIBRARY = [];

/**
 * Default factory presets bundled with the application.
 * Presets reference AudioFX models and IRs by their library IDs.
 * Loaded from data/default-presets.json on startup.
 */
let DEFAULT_PRESETS = [];

/**
 * Resolves an AudioFX model by its library ID.
 * @param {string} modelId - The model library ID
 * @returns {object|null} The model entry or null if not found
 */
function resolveAudioFxModel(modelId) {
  return AUDIOFX_MODEL_LIBRARY.find((m) => m.id === modelId) ?? null;
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
 * @param {string|null} audioFxModelId - AudioFX model library ID or null
 * @param {string|null} irId - IR library ID or null
 * @param {string|null} customModelPath - Custom model file path (used if audioFxModelId not in library)
 * @param {string|null} customIrPath - Custom IR file path (used if irId not in library)
 * @returns {Array} Array of attachment objects
 */
function buildAttachments(audioFxModelId, irId, customModelPath = null, customIrPath = null) {
  const attachments = [];

  // Resolve AudioFX model
  if (audioFxModelId) {
    const model = resolveAudioFxModel(audioFxModelId);
    if (model) {
      attachments.push({
        type: "audiofx",
        id: model.id,
        filePath: model.filePath,
        hash: model.hash,
      });
    } else if (customModelPath) {
      attachments.push({ type: "audiofx", filePath: customModelPath, hash: "" });
    }
  } else if (customModelPath) {
    attachments.push({ type: "audiofx", filePath: customModelPath, hash: "" });
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
 * Loads the AudioFX model library from the JSON file.
 * @returns {Promise<Array>} Array of AudioFX model objects
 */
async function loadAudioFxModelLibrary() {
  try {
    const response = await fetch("data/audiofx-models.json");
    if (!response.ok) {
      throw new Error(`Failed to load AudioFX models: ${response.status}`);
    }
    return await response.json();
  } catch (error) {
    console.error(`Error loading AudioFX model library: ${error.message}`);
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
      attachments: buildAttachments(preset.audioFxModelId, preset.irId),
    }));
  } catch (error) {
    console.error(`Error loading default presets: ${error.message}`);
    return [];
  }
}

/**
 * Initializes all data libraries from JSON files.
 * Must be called before using AUDIOFX_MODEL_LIBRARY, IR_LIBRARY, or DEFAULT_PRESETS.
 */
async function initializeDataLibraries() {
  // Load model and IR libraries first (needed for preset attachments)
  const [models, irs] = await Promise.all([
    loadAudioFxModelLibrary(),
    loadIrLibrary(),
  ]);
  AUDIOFX_MODEL_LIBRARY = models;
  IR_LIBRARY = irs;
  
  // Load presets after libraries are ready
  DEFAULT_PRESETS = await loadDefaultPresets();
  
  console.log(`Loaded ${AUDIOFX_MODEL_LIBRARY.length} AudioFX models, ${IR_LIBRARY.length} IRs, ${DEFAULT_PRESETS.length} default presets`);
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
  
  // Initialize doubler controls
  initializeDoublerControls();
}

/**
 * Initializes the doubler toggle and delay knob controls.
 */
function initializeDoublerControls() {
  const doublerToggle = document.getElementById("doubler-toggle");
  const delayValueDisplay = document.getElementById("delay-value");
  
  // Doubler toggle
  if (doublerToggle) {
    doublerToggle.addEventListener("change", () => {
      const enabled = doublerToggle.checked ? 1.0 : 0.0;
      setParameter("doubler_enabled", enabled);
      appendLog(`doubler_enabled → ${enabled}`);
    });
  }
  
  // Initialize delay knob interaction
  const delayKnob = document.querySelector('.knob[data-param="delay"]');
  if (delayKnob) {
    let isDragging = false;
    let startY = 0;
    let startValue = 6.0;
    const minValue = 0.5;
    const maxValue = 50.0;
    
    const updateDelayDisplay = (value) => {
      if (delayValueDisplay) {
        delayValueDisplay.textContent = `${value.toFixed(2)} ms`;
      }
      // Update knob rotation
      const rotation = ((value - minValue) / (maxValue - minValue)) * 270 - 135;
      const indicator = delayKnob.querySelector('.knob-indicator');
      if (indicator) {
        indicator.style.transform = `translateX(-50%) rotate(${rotation}deg)`;
      }
    };
    
    delayKnob.addEventListener("mousedown", (e) => {
      isDragging = true;
      startY = e.clientY;
      startValue = parseFloat(delayKnob.dataset.value) || 6.0;
      e.preventDefault();
    });
    
    document.addEventListener("mousemove", (e) => {
      if (!isDragging) return;
      const deltaY = startY - e.clientY;
      const sensitivity = 0.5;
      let newValue = startValue + deltaY * sensitivity;
      newValue = Math.max(minValue, Math.min(maxValue, newValue));
      delayKnob.dataset.value = newValue;
      updateDelayDisplay(newValue);
    });
    
    document.addEventListener("mouseup", () => {
      if (isDragging) {
        isDragging = false;
        const value = parseFloat(delayKnob.dataset.value) || 6.0;
        setParameter("doubler_delay", value);
        appendLog(`doubler_delay → ${value}`);
      }
    });
    
    // Initialize display
    updateDelayDisplay(parseFloat(delayKnob.dataset.value) || 6.0);
  }
}

/**
 * Syncs doubler controls from state.
 */
function syncDoublerControlsFromState() {
  const paramValues = {};
  if (Array.isArray(uiState.parameters.values)) {
    uiState.parameters.values.forEach((param) => {
      paramValues[param.id] = param.value;
    });
  }
  
  // Sync doubler toggle
  const doublerToggle = document.getElementById("doubler-toggle");
  if (doublerToggle && typeof paramValues.doubler_enabled === "number") {
    doublerToggle.checked = paramValues.doubler_enabled > 0.5;
  }
  
  // Sync delay value
  const delayKnob = document.querySelector('.knob[data-param="delay"]');
  const delayValueDisplay = document.getElementById("delay-value");
  if (delayKnob && typeof paramValues.doubler_delay === "number") {
    delayKnob.dataset.value = paramValues.doubler_delay;
    if (delayValueDisplay) {
      delayValueDisplay.textContent = `${paramValues.doubler_delay.toFixed(2)} ms`;
    }
  }
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
  
  // Also sync doubler controls
  syncDoublerControlsFromState();
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
  // Skip if preset list element doesn't exist (new amp-style UI)
  if (!presetListElement) {
    return;
  }
  
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
        <div class="param-card">
          <span class="param-label">${label}</span>
          <span class="param-value">${formatParameterValue(parameter)}</span>
        </div>
      `;
    })
    .join("");

  const hasParameters = Boolean(parameterItems);
  const gateStatus = typeof uiState.parameters.gateEnabled === "boolean"
    ? `
        <div class="param-card ${uiState.parameters.gateEnabled ? 'active' : ''}">
          <span class="param-label">Noise Gate</span>
          <span class="param-value">${uiState.parameters.gateEnabled ? "On" : "Off"}${
            typeof uiState.parameters.gateThreshold === "number"
              ? ` (${uiState.parameters.gateThreshold.toFixed(1)} dB)`
              : ""
          }</span>
        </div>
      `
    : "";

  const signalPathCards = `
    <div class="signal-path-cards">
      <div class="path-card">
        <div class="path-icon">🎸</div>
        <div class="path-info">
          <span class="path-label">Amp Model</span>
          <span class="path-value">${uiState.parameters.modelPath || "None"}</span>
        </div>
        <div class="path-actions">
          <button class="load-btn" id="load-model-btn">Load Model</button>
        </div>
      </div>
      <div class="path-card">
        <div class="path-icon">🔊</div>
        <div class="path-info">
          <span class="path-label">Cabinet IR</span>
          <span class="path-value">${uiState.parameters.irPath || "None"}</span>
        </div>
        <div class="path-actions">
          <button class="load-btn" id="load-ir-btn">Load IR</button>
        </div>
      </div>
    </div>
  `;

  const signalTestSection = uiState.signalTest
    ? `
        <div class="test-results ${uiState.signalTest.passed ? 'passed' : 'failed'}">
          <div class="test-header">
            <span class="test-icon">${uiState.signalTest.passed ? '✓' : '✗'}</span>
            <span class="test-title">Signal Path Test</span>
            <span class="test-status">${uiState.signalTest.passed ? 'Passed' : 'Failed'}</span>
          </div>
          <div class="test-details">
            <div class="test-stat">
              <span class="stat-label">Frequency</span>
              <span class="stat-value">${uiState.signalTest.frequency.toFixed(1)} Hz</span>
            </div>
            <div class="test-stat">
              <span class="stat-label">Duration</span>
              <span class="stat-value">${uiState.signalTest.duration.toFixed(2)} s</span>
            </div>
            <div class="test-stat">
              <span class="stat-label">Input RMS</span>
              <span class="stat-value">${uiState.signalTest.inputRMS.toFixed(4)}</span>
            </div>
            <div class="test-stat">
              <span class="stat-label">Output L/R</span>
              <span class="stat-value">${uiState.signalTest.outputLeft.toFixed(4)} / ${uiState.signalTest.outputRight.toFixed(4)}</span>
            </div>
          </div>
          ${uiState.signalTest.message ? `<p class="test-message">${uiState.signalTest.message}</p>` : ""}
        </div>
      `
    : "";

  if (!hasParameters && !gateStatus && !uiState.parameters.modelPath && !uiState.parameters.irPath) {
    return `
      <div class="signal-chain-section">
        <h3 class="section-title">
          <span class="section-icon">🔬</span>
          Diagnostics
        </h3>
        <button id="run-signal-test" class="test-btn">Run Signal Path Test</button>
        ${signalTestSection}
      </div>
    `;
  }

  return `
    <div class="signal-chain-section">
      <h3 class="section-title">
        <span class="section-icon">⚙️</span>
        Current Parameters
      </h3>
      <div class="params-grid">
        ${parameterItems}
        ${gateStatus}
      </div>
    </div>
    <div class="signal-chain-section">
      <h3 class="section-title">
        <span class="section-icon">🛤️</span>
        Active Signal Path
      </h3>
      ${signalPathCards}
    </div>
    <div class="signal-chain-section">
      <h3 class="section-title">
        <span class="section-icon">🔬</span>
        Diagnostics
      </h3>
      <button id="run-signal-test" class="test-btn">Run Signal Path Test</button>
      ${signalTestSection}
    </div>
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
    <div class="signal-chain-section">
      <h3 class="section-title">
        <span class="section-icon">🎵</span>
        Demo Audio
      </h3>
      <div class="demo-controls">
        <select id="demo-audio-select" class="demo-select">
          ${options}
        </select>
        <button id="play-demo-audio" class="play-btn">
          <span class="play-icon">▶</span>
          Play
        </button>
        <label class="repeat-toggle">
          <input type="checkbox" id="demo-audio-repeat-checkbox" />
          <span class="repeat-label">🔁 Repeat</span>
        </label>
      </div>
    </div>
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

/**
 * Binds the load model/IR buttons to trigger file selection.
 */
function bindLoadButtons() {
  const loadModelBtn = document.getElementById("load-model-btn");
  const loadIRBtn = document.getElementById("load-ir-btn");
  
  if (loadModelBtn) {
    loadModelBtn.addEventListener("click", () => {
      // Request native file dialog from backend
      window.NAMBridge.postMessage({ type: "browseModel" });
      appendLog("browseModel → requested");
    });
  }
  
  if (loadIRBtn) {
    loadIRBtn.addEventListener("click", () => {
      // Request native file dialog from backend
      window.NAMBridge.postMessage({ type: "browseIR" });
      appendLog("browseIR → requested");
    });
  }
}

/**
 * Sends a request to load a NAM model from a file path.
 * @param {string} filePath - The absolute path to the model file
 */
function loadModelFromPath(filePath) {
  window.NAMBridge.postMessage({
    type: "loadModel",
    filePath: filePath,
  });
  appendLog(`loadModel → ${filePath}`);
}

/**
 * Sends a request to load an IR from a file path.
 * @param {string} filePath - The absolute path to the IR file
 */
function loadIRFromPath(filePath) {
  window.NAMBridge.postMessage({
    type: "loadIR",
    filePath: filePath,
  });
  appendLog(`loadIR → ${filePath}`);
}

/**
 * Opens the save preset modal dialog.
 */
function openSavePresetModal() {
  const modal = document.getElementById("save-preset-modal");
  if (!modal) return;
  
  // Update the modal with current paths
  const modelPathSpan = document.getElementById("save-modal-model-path");
  const irPathSpan = document.getElementById("save-modal-ir-path");
  
  if (modelPathSpan) {
    modelPathSpan.textContent = uiState.parameters.modelPath || "None";
  }
  if (irPathSpan) {
    irPathSpan.textContent = uiState.parameters.irPath || "None";
  }
  
  // Clear previous values
  const nameInput = document.getElementById("preset-name-input");
  const categoryInput = document.getElementById("preset-category-input");
  const descriptionInput = document.getElementById("preset-description-input");
  
  if (nameInput) nameInput.value = "";
  if (categoryInput) categoryInput.value = "User";
  if (descriptionInput) descriptionInput.value = "";
  
  modal.style.display = "flex";
}

/**
 * Closes the save preset modal dialog.
 */
function closeSavePresetModal() {
  const modal = document.getElementById("save-preset-modal");
  if (modal) {
    console.log("Closing save preset modal");
    modal.style.display = "none";
  }
}

/**
 * Saves the current preset with the values from the modal.
 */
function saveCurrentPreset() {
  console.log("=== saveCurrentPreset CALLED ===");
  
  const nameInput = document.getElementById("preset-name-input");
  const categoryInput = document.getElementById("preset-category-input");
  const descriptionInput = document.getElementById("preset-description-input");
  
  console.log("Elements retrieved:", { nameInput, categoryInput, descriptionInput });
  
  const name = nameInput?.value?.trim() || "";
  const category = categoryInput?.value?.trim() || "User";
  const description = descriptionInput?.value?.trim() || "";
  
  console.log("Preset details", { name, category, description });
  
  if (!name) {
    console.log("ERROR: Preset name is empty, not saving");
    showNotification("Error", "Preset name is required");
    return;
  }
  

  // save
  
  
  /*

  
  console.log("Sending message to bridge:", message);
  console.log("Bridge availability:", { bridgeExists: !!window.NAMBridge, hasPostMessage: !!(window.NAMBridge && window.NAMBridge.postMessage) });
  
  if (window.NAMBridge && window.NAMBridge.postMessage) {
    console.log("Posting message...");
    window.NAMBridge.postMessage(message);
    console.log("Message posted successfully");
    appendLog(`savePreset → ${name}`);
    console.log("Closing modal...");
    closeSavePresetModal();
    console.log("Preset save message sent");
  } else {
    console.error("NAMBridge not available for saving preset");
    showNotification("Error", "Failed to save preset - bridge not available");
  }*/
}

/**
 * Initializes the file input handlers for model and IR loading.
 * Note: Currently using native file dialogs via backend, so this is a no-op.
 */
function initializeFileInputs() {
  // Native file dialogs are handled by the C++ backend via browseModel/browseIR messages
  // The hidden file inputs in HTML are kept as fallback but not actively used
}

/**
 * Initializes the save preset modal event handlers.
 */
function initializeSavePresetModal() {
  const closeBtn = document.getElementById("save-preset-modal-close");
  const cancelBtn = document.getElementById("save-preset-cancel");
  const confirmBtn = document.getElementById("save-preset-confirm");
  const modal = document.getElementById("save-preset-modal");
  
  console.log("Initializing save preset modal", { closeBtn, cancelBtn, confirmBtn, modal });
  
  if (closeBtn) {
    closeBtn.addEventListener("click", closeSavePresetModal);
  }
  
  if (cancelBtn) {
    cancelBtn.addEventListener("click", closeSavePresetModal);
  }
  
  if (confirmBtn) {
    confirmBtn.addEventListener("click", (e) => {
      console.log("Save preset confirm button clicked");
      saveCurrentPreset();
    });
  } else {
    console.warn("Save preset confirm button not found!");
  }
  
  // Close modal when clicking outside
  if (modal) {
    modal.addEventListener("click", (event) => {
      if (event.target === modal) {
        closeSavePresetModal();
      }
    });
  }
}

/**
 * Initializes the "SAVE AS..." button in the control bar.
 */
function initializeSaveAsButton() {
  // Find the "SAVE AS..." button in the control bar
  const saveAsButtons = document.querySelectorAll(".text-btn");
  saveAsButtons.forEach((btn) => {
    if (btn.textContent.trim() === "SAVE AS...") {
      btn.addEventListener("click", openSavePresetModal);
    }
  });
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
  if (!presetDetailsElement) return;
  
  if (!preset) {
    const parameterSection = renderParameterSection();
    const demoSection = renderDemoAudioControls();
    presetDetailsElement.innerHTML = `
      <div class="signal-chain-container">
        <div class="signal-chain-header">
          <div class="preset-info">
            <h2 class="preset-title">No Preset Loaded</h2>
            <p class="preset-description">Select a preset to see details.</p>
          </div>
        </div>
        ${demoSection}
        ${parameterSection}
      </div>
    `;
    const signalTestButton = document.getElementById("run-signal-test");
    if (signalTestButton) {
      signalTestButton.addEventListener("click", requestSignalPathTest);
    }
    bindDemoAudioControls();
    bindLoadButtons();
    return;
  }

  const attachmentCards = (preset.attachments ?? [])
    .map((attachment) => {
      const icon = attachment.type === 'audiofx' ? '🎸' : attachment.type === 'ir' ? '🔊' : '📦';
      const label = attachment.type === 'audiofx' ? 'Amp Model' : attachment.type === 'ir' ? 'Cabinet IR' : attachment.type;
      const hashShort = attachment.hash ? attachment.hash.substring(0, 12) + '...' : 'N/A';
      return `
        <div class="attachment-card">
          <div class="attachment-icon">${icon}</div>
          <div class="attachment-info">
            <span class="attachment-type">${label}</span>
            <span class="attachment-hash" title="${attachment.hash ?? ''}">${hashShort}</span>
          </div>
          <div class="attachment-status active"></div>
        </div>
      `;
    })
    .join("");

  const fxChainNodes = (preset.fxChain ?? [])
    .map((stage) => {
      const icon = stage === 'noise_gate' ? '🔇' : stage === 'compressor' ? '📊' : stage === 'eq' ? '🎚️' : '⚡';
      return `
        <div class="fx-node">
          <div class="fx-node-icon">${icon}</div>
          <span class="fx-node-label">${stage.replace(/_/g, ' ')}</span>
        </div>
        <div class="fx-connector"></div>
      `;
    })
    .join("");

  const fxChainContent = fxChainNodes || '<span class="fx-empty">No effects in chain</span>';

  presetDetailsElement.innerHTML = `
    <div class="signal-chain-container">
      <div class="signal-chain-header">
        <div class="preset-info">
          <h2 class="preset-title">${preset.name}</h2>
          <p class="preset-description">${preset.description ?? ''}</p>
        </div>
        <button id="apply-preset" class="apply-btn">Apply Preset</button>
      </div>

      <div class="signal-chain-section">
        <h3 class="section-title">
          <span class="section-icon">🔗</span>
          Signal Chain
        </h3>
        <div class="fx-chain-flow">
          <div class="fx-node input-node">
            <div class="fx-node-icon">🎸</div>
            <span class="fx-node-label">Input</span>
          </div>
          <div class="fx-connector"></div>
          ${fxChainContent}
          <div class="fx-node output-node">
            <div class="fx-node-icon">🔊</div>
            <span class="fx-node-label">Output</span>
          </div>
        </div>
      </div>

      <div class="signal-chain-section">
        <h3 class="section-title">
          <span class="section-icon">📦</span>
          Loaded Components
        </h3>
        <div class="attachments-grid">
          ${attachmentCards || '<span class="no-attachments">No components loaded</span>'}
        </div>
      </div>

      ${renderDemoAudioControls()}
      ${renderParameterSection()}
    </div>
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
  bindLoadButtons();
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
      
      // Merge user presets from backend with default presets
      if (Array.isArray(payload.userPresets)) {
        // Start with factory presets, then add user presets from backend
        const factoryPresets = DEFAULT_PRESETS.slice();
        const userPresets = payload.userPresets;
        
        // Merge: factory presets first, then user presets (deduplicating by ID)
        const presetMap = new Map();
        factoryPresets.forEach(p => presetMap.set(p.id, p));
        userPresets.forEach(p => presetMap.set(p.id, p));
        
        uiState.presets = Array.from(presetMap.values());
        uiState.filteredPresets = uiState.presets.slice();
        uiState.presets.forEach((preset) => {
          uiState.presetCache.set(preset.id, preset);
        });
        populatePresetDropdown();
      }
      
      if (payload.preset) {
        uiState.presetCache.set(payload.preset.id, payload.preset);
        if (!uiState.presets.some((preset) => preset.id === payload.preset.id)) {
          uiState.presets = [payload.preset, ...uiState.presets];
          filterPresets(presetSearchElement?.value ?? "");
          populatePresetDropdown();
        }
      }
      renderPresetList(uiState.filteredPresets);
      const preset = payload.preset ?? uiState.presetCache.get(uiState.activePresetId) ?? null;
      renderPresetDetails(preset ? clonePreset(preset) : null);
      syncControlsFromState();
      updatePresetDropdownSelection();
      clearNotification();
      break;
    }
    case "presetLoaded": {
      const preset = payload.preset;
      if (preset) {
        uiState.activePresetId = preset.id;
        uiState.presetCache.set(preset.id, preset);
        updatePresetDropdownSelection();
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
      // Render preset details AFTER updating parameters so signal path shows correct model/IR
      if (preset) {
        renderPresetDetails(clonePreset(preset));
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
    case "modelLoaded": {
      appendLog(`model loaded ← ${payload.path ?? "unknown"}`);
      uiState.parameters.modelPath = payload.path ?? "";
      renderPresetDetails(uiState.presetCache.get(uiState.activePresetId) ?? null);
      showNotification("Model loaded", payload.path ?? "");
      break;
    }
    case "irLoaded": {
      appendLog(`IR loaded ← ${payload.path ?? "unknown"}`);
      uiState.parameters.irPath = payload.path ?? "";
      renderPresetDetails(uiState.presetCache.get(uiState.activePresetId) ?? null);
      showNotification("IR loaded", payload.path ?? "");
      break;
    }
    case "presetSaved": {
      appendLog(`preset saved ← ${payload.preset?.name ?? "unknown"}`);
      const savedPreset = payload.preset;
      if (savedPreset) {
        // Save preset to localStorage
        savePresetToLocalStorage(savedPreset);
        
        uiState.activePresetId = savedPreset.id;
        uiState.presetCache.set(savedPreset.id, savedPreset);
        // Add to presets list if not already there
        if (!uiState.presets.some((p) => p.id === savedPreset.id)) {
          uiState.presets.unshift(savedPreset);
          filterPresets(presetSearchElement?.value ?? "");
          populatePresetDropdown();
        }
        renderPresetDetails(clonePreset(savedPreset));
        updatePresetDropdownSelection();
      }
      showNotification("Preset saved", payload.path ?? savedPreset?.name ?? "");
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

/**
 * Saves a preset to browser localStorage.
 * @param {object} preset - The preset object to save
 */
function savePresetToLocalStorage(preset) {
  try {
    const savedPresets = JSON.parse(localStorage.getItem("namguitar_user_presets") || "[]");
    // Check if preset with this ID already exists and replace it
    const existingIndex = savedPresets.findIndex(p => p.id === preset.id);
    if (existingIndex >= 0) {
      savedPresets[existingIndex] = preset;
    } else {
      savedPresets.push(preset);
    }
    localStorage.setItem("namguitar_user_presets", JSON.stringify(savedPresets));
    console.log(`Preset '${preset.name}' saved to localStorage`);
  } catch (error) {
    console.error("Failed to save preset to localStorage", error);
  }
}

/**
 * Loads user presets from browser localStorage.
 * @deprecated User presets are now loaded from the C++ backend via the state message.
 * This function is kept as a potential fallback for offline/disconnected scenarios.
 * @returns {array} Array of user presets or empty array if none found
 */
function loadPresetsFromLocalStorage() {
  try {
    const savedPresets = JSON.parse(localStorage.getItem("namguitar_user_presets") || "[]");
    console.log(`Loaded ${savedPresets.length} user presets from localStorage`);
    return savedPresets;
  } catch (error) {
    console.error("Failed to load presets from localStorage", error);
    return [];
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
    // Start with factory presets from remote or defaults
    // User presets will be loaded from backend via state message
    const basePresets = presets.length ? presets : DEFAULT_PRESETS.slice();
    uiState.presets = basePresets;
    uiState.filteredPresets = uiState.presets.slice();
    uiState.presets.forEach((preset) => {
      uiState.presetCache.set(preset.id, preset);
    });
    renderPresetList(uiState.filteredPresets);
  } catch (error) {
    console.error("Failed to load preset index", error);
    // Start with default factory presets
    // User presets will be loaded from backend via state message
    const basePresets = DEFAULT_PRESETS.slice();
    uiState.presets = basePresets;
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
    // Load default factory presets initially
    // User presets will be loaded from backend via state message
    const basePresets = DEFAULT_PRESETS.slice();
    uiState.presets = basePresets;
    uiState.filteredPresets = uiState.presets.slice();
    uiState.presets.forEach((preset) => {
      uiState.presetCache.set(preset.id, preset);
    });
    renderPresetList(uiState.filteredPresets);
  }
  
  // Populate the preset dropdown
  populatePresetDropdown();
  
  // Request state from backend - this will include user presets
  window.NAMBridge.postMessage({ type: "requestState" });
}

/**
 * Populates the preset dropdown with available presets.
 */
function populatePresetDropdown() {
  if (!presetDropdown) return;
  
  presetDropdown.innerHTML = "";
  
  uiState.presets.forEach((preset) => {
    const option = document.createElement("option");
    option.value = preset.id;
    option.textContent = preset.name;
    if (preset.id === uiState.activePresetId) {
      option.selected = true;
    }
    presetDropdown.appendChild(option);
  });
}

/**
 * Updates the preset dropdown selection to match the active preset.
 */
function updatePresetDropdownSelection() {
  if (!presetDropdown || !uiState.activePresetId) return;
  presetDropdown.value = uiState.activePresetId;
}

/**
 * Gets the index of the currently active preset.
 * @returns {number} Index of active preset, or -1 if not found
 */
function getActivePresetIndex() {
  if (!uiState.activePresetId) return -1;
  return uiState.presets.findIndex((p) => p.id === uiState.activePresetId);
}

/**
 * Selects the previous preset in the list.
 */
async function selectPreviousPreset() {
  if (!uiState.presets.length) return;
  
  let index = getActivePresetIndex();
  if (index <= 0) {
    index = uiState.presets.length - 1; // Wrap to end
  } else {
    index--;
  }
  
  const preset = uiState.presets[index];
  if (preset) {
    await applyPresetFromLibrary(preset.id);
    updatePresetDropdownSelection();
  }
}

/**
 * Selects the next preset in the list.
 */
async function selectNextPreset() {
  if (!uiState.presets.length) return;
  
  let index = getActivePresetIndex();
  if (index < 0 || index >= uiState.presets.length - 1) {
    index = 0; // Wrap to beginning
  } else {
    index++;
  }
  
  const preset = uiState.presets[index];
  if (preset) {
    await applyPresetFromLibrary(preset.id);
    updatePresetDropdownSelection();
  }
}

/**
 * Initializes the preset navigation controls (dropdown, prev/next buttons).
 */
function initializePresetControls() {
  // Dropdown change handler
  if (presetDropdown) {
    presetDropdown.addEventListener("change", async (event) => {
      const presetId = event.target.value;
      if (presetId) {
        await applyPresetFromLibrary(presetId);
      }
    });
  }
  
  // Previous preset button
  if (prevPresetBtn) {
    prevPresetBtn.addEventListener("click", async () => {
      await selectPreviousPreset();
    });
  }
  
  // Next preset button
  if (nextPresetBtn) {
    nextPresetBtn.addEventListener("click", async () => {
      await selectNextPreset();
    });
  }
}

/**
 * Switches to a specific tab panel in the main content area.
 * @param {string} panelId - The panel ID (e.g., "signal-chain", "amp", "effects")
 */
function switchMainPanel(panelId) {
  // Update icon bar button states
  iconBarButtons.forEach((btn) => {
    const btnPanel = btn.dataset.panel;
    btn.classList.toggle("active", btnPanel === panelId);
  });
  
  // Update panel visibility
  mainTabPanels.forEach((panel) => {
    const isPanelMatch = panel.id === `panel-${panelId}`;
    panel.classList.toggle("active", isPanelMatch);
  });
}

/**
 * Initializes the icon bar tab navigation.
 */
function initializeIconBarTabs() {
  iconBarButtons.forEach((btn) => {
    btn.addEventListener("click", () => {
      const panelId = btn.dataset.panel;
      if (panelId) {
        switchMainPanel(panelId);
      }
    });
  });
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
initializePresetControls();
initializeIconBarTabs();
initializeFileInputs();
initializeSavePresetModal();
initializeSaveAsButton();

presetSearchElement?.addEventListener("input", (event) => {
  filterPresets(event.target.value ?? "");
});

window.IPlugReceiveData = (message) => {
  handleIncomingMessage(message);
};

renderPresetList([]);
renderPresetDetails(null);
initialize();
