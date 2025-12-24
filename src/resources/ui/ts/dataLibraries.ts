import type { Attachment, AudioFxModelEntry, IrLibraryEntry, Preset } from "./types.js";

export const REMOTE_BASE_URL = window.AUDIOFX_REMOTE_BASE_URL ?? "";

let audioFxModelLibrary: AudioFxModelEntry[] = [];
let irLibrary: IrLibraryEntry[] = [];
let defaultPresets: Preset[] = [];

function resolveAudioFxModel(modelId: string | null | undefined): AudioFxModelEntry | null {
  if (!modelId) return null;
  return audioFxModelLibrary.find((m) => m.id === modelId) ?? null;
}

function resolveIR(irId: string | null | undefined): IrLibraryEntry | null {
  if (!irId) return null;
  return irLibrary.find((ir) => ir.id === irId) ?? null;
}

export function buildAttachments(
  audioFxModelId: string | null,
  irId: string | null,
  customModelPath: string | null = null,
  customIrPath: string | null = null,
): Attachment[] {
  const attachments: Attachment[] = [];

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

async function loadAudioFxModelLibrary(): Promise<AudioFxModelEntry[]> {
  try {
    const response = await fetch("data/audiofx-models.json");
    if (!response.ok) {
      throw new Error(`Failed to load AudioFX models: ${response.status}`);
    }
    return await response.json();
  } catch (error) {
    console.error(`Error loading AudioFX model library: ${(error as Error).message}`);
    return [];
  }
}

async function loadIrLibrary(): Promise<IrLibraryEntry[]> {
  try {
    const response = await fetch("data/ir-library.json");
    if (!response.ok) {
      throw new Error(`Failed to load IR library: ${response.status}`);
    }
    return await response.json();
  } catch (error) {
    console.error(`Error loading IR library: ${(error as Error).message}`);
    return [];
  }
}

async function loadDefaultPresets(): Promise<Preset[]> {
  try {
    const response = await fetch("data/default-presets.json");
    if (!response.ok) {
      throw new Error(`Failed to load default presets: ${response.status}`);
    }
    const presets: Preset[] = await response.json();
    return presets.map((preset) => ({
      ...preset,
      attachments: buildAttachments(preset.audioFxModelId ?? null, preset.irId ?? null),
    }));
  } catch (error) {
    console.error(`Error loading default presets: ${(error as Error).message}`);
    return [];
  }
}

export async function initializeDataLibraries(): Promise<void> {
  const [models, irs] = await Promise.all([loadAudioFxModelLibrary(), loadIrLibrary()]);
  audioFxModelLibrary = models;
  irLibrary = irs;
  defaultPresets = await loadDefaultPresets();
  console.log(
    `Loaded ${audioFxModelLibrary.length} AudioFX models, ${irLibrary.length} IRs, ${defaultPresets.length} default presets`,
  );
}

export function getAudioFxLibrary(): AudioFxModelEntry[] {
  return audioFxModelLibrary.slice();
}

export function getIrLibrary(): IrLibraryEntry[] {
  return irLibrary.slice();
}

export function getDefaultPresets(): Preset[] {
  return defaultPresets.slice();
}
