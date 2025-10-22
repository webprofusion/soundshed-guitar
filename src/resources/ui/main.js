const presetListElement = document.getElementById("preset-list");
const presetDetailsElement = document.getElementById("preset-details");
const presetSearchElement = document.getElementById("preset-search");
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

const DEFAULT_PRESETS = window.NAM_DEFAULT_PRESETS ?? [
  {
    id: "factory-clean",
    name: "Factory Clean",
    category: "Factory",
    description: "Balanced clean tone with plenty of headroom.",
    namModelId: "",
    irId: "",
    fxChain: [],
    attachments: [],
    parameters: [
      { id: "input_trim", value: 0.0 },
      { id: "output_trim", value: 0.0 },
      { id: "drive", value: 0.15 },
      { id: "tone", value: 0.55 },
      { id: "gate_enabled", value: 0.0 },
      { id: "gate_threshold", value: -60.0 },
    ],
  },
  {
    id: "factory-breakup",
    name: "Edge of Breakup",
    category: "Factory",
    description: "Touch-sensitive crunch that works great with single coils.",
    namModelId: "",
    irId: "",
    fxChain: [],
    attachments: [],
    parameters: [
      { id: "input_trim", value: -3.0 },
      { id: "output_trim", value: 0.0 },
      { id: "drive", value: 0.45 },
      { id: "tone", value: 0.5 },
      { id: "gate_enabled", value: 0.0 },
      { id: "gate_threshold", value: -55.0 },
    ],
  },
  {
    id: "factory-slo-lowgain",
    name: "SLO Low Gain Cab",
    category: "Factory",
    description: "Bundled SLO-100 low-gain model paired with a 1960 cab IR.",
    namModelId: "SLO-100 LOWGAIN VALETON",
    irId: "421 1960",
    fxChain: ["noise_gate"],
    attachments: [
      {
        type: "nam",
        filePath: "SLO-100 LOWGAIN VALETON.nam",
        path: "../models/SLO-100 LOWGAIN VALETON.nam",
      },
      {
        type: "ir",
        filePath: "421 1960.wav",
        path: "../ir/421 1960.wav",
      },
    ],
    parameters: [
      { id: "input_trim", value: -4.0 },
      { id: "output_trim", value: -1.5 },
      { id: "drive", value: 0.35 },
      { id: "tone", value: 0.6 },
      { id: "gate_enabled", value: 1.0 },
      { id: "gate_threshold", value: -55.0 },
    ],
  },
  {
    id: "factory-highgain",
    name: "Saturated Lead",
    category: "Factory",
    description: "Tight high-gain lead preset with a gentle noise gate.",
    namModelId: "",
    irId: "",
    fxChain: ["noise_gate"],
    attachments: [],
    parameters: [
      { id: "input_trim", value: -6.0 },
      { id: "output_trim", value: -3.0 },
      { id: "drive", value: 0.85 },
      { id: "tone", value: 0.65 },
      { id: "gate_enabled", value: 1.0 },
      { id: "gate_threshold", value: -50.0 },
    ],
  },
];

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
};

window.NAMBridge = {
  postMessage(message) {
    if (window.IPlugSendMsg) {
      window.IPlugSendMsg(message);
    }
  },
};

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

function renderPresetDetails(preset) {
  if (!preset) {
    const parameterSection = renderParameterSection();
    presetDetailsElement.innerHTML = `
      <h2>No Preset Loaded</h2>
      <p>Select a preset to see details.</p>
      ${parameterSection}
    `;
    const signalTestButton = document.getElementById("run-signal-test");
    if (signalTestButton) {
      signalTestButton.addEventListener("click", requestSignalPathTest);
    }
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

async function enrichAttachment(attachment) {
  if (attachment.data) {
    return attachment;
  }

  const url = resolveAttachmentUrl(attachment);
  if (!url) {
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

presetSearchElement?.addEventListener("input", (event) => {
  filterPresets(event.target.value ?? "");
});

window.IPlugReceiveData = (message) => {
  handleIncomingMessage(message);
};

renderPresetList([]);
renderPresetDetails(null);
initialize();
