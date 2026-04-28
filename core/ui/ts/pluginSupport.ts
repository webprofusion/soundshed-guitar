import type { LibraryResource } from "./types.js";

export type PluginSupportPlatform = "windows" | "mac" | "linux" | "unknown";

export type PluginResourceSupportInfo = Pick<LibraryResource, "filePath" | "metadata">;

export type UnsupportedPluginSelection = {
  format: string;
  label: string;
};

const UNIVERSALLY_UNSUPPORTED_FORMATS = new Set(["aax", "clap", "vst2"]);
const SUPPORTED_FORMATS_BY_PLATFORM: Record<Exclude<PluginSupportPlatform, "unknown">, Set<string>> = {
  windows: new Set(["vst3"]),
  mac: new Set(["vst3", "au"]),
  linux: new Set(["vst3", "lv2"]),
};

const FORMAT_LABELS: Record<string, string> = {
  aax: "AAX",
  au: "Audio Unit",
  clap: "CLAP",
  lv2: "LV2",
  vst2: "VST2",
  vst3: "VST3",
};

const PLUGIN_FORMAT_METADATA_KEYS = ["pluginFormat", "format", "pluginType", "type", "kind"];

export function detectPluginSupportPlatform(platformText?: string): PluginSupportPlatform {
  const source = (platformText ?? getNavigatorPlatformText()).toLowerCase();
  if (source.includes("win")) return "windows";
  if (source.includes("mac") || source.includes("darwin")) return "mac";
  if (source.includes("linux")) return "linux";
  return "unknown";
}

export function inferPluginFormat(resource?: PluginResourceSupportInfo | null): string | null {
  if (!resource) {
    return null;
  }

  for (const key of PLUGIN_FORMAT_METADATA_KEYS) {
    const format = normalizePluginFormat(resource.metadata?.[key]);
    if (format) {
      return format;
    }
  }

  return inferPluginFormatFromPath(resource.filePath);
}

export function getUnsupportedPluginSelection(
  resource?: PluginResourceSupportInfo | null,
  platform: PluginSupportPlatform = detectPluginSupportPlatform(),
): UnsupportedPluginSelection | null {
  const format = inferPluginFormat(resource);
  if (!format || isPluginFormatSupported(format, platform)) {
    return null;
  }

  return {
    format,
    label: FORMAT_LABELS[format] ?? format.toUpperCase(),
  };
}

function getNavigatorPlatformText(): string {
  if (typeof navigator === "undefined") {
    return "";
  }

  const userAgentData = (navigator as Navigator & { userAgentData?: { platform?: string } }).userAgentData;
  return [userAgentData?.platform, navigator.platform, navigator.userAgent]
    .filter((value): value is string => typeof value === "string" && value.length > 0)
    .join(" ");
}

function isPluginFormatSupported(format: string, platform: PluginSupportPlatform): boolean {
  if (UNIVERSALLY_UNSUPPORTED_FORMATS.has(format)) {
    return false;
  }
  if (platform === "unknown") {
    return true;
  }
  return SUPPORTED_FORMATS_BY_PLATFORM[platform].has(format);
}

function normalizePluginFormat(value: string | undefined): string | null {
  if (!value) {
    return null;
  }

  const normalized = value.trim().toLowerCase().replace(/[\s_.-]+/g, "");
  if (!normalized) {
    return null;
  }

  if (normalized.includes("vst3")) return "vst3";
  if (normalized.includes("audiounit") || normalized === "au" || normalized.includes("component")) return "au";
  if (normalized.includes("lv2")) return "lv2";
  if (normalized.includes("clap")) return "clap";
  if (normalized.includes("aax")) return "aax";
  if (normalized.includes("vst2") || normalized === "vst") return "vst2";
  return null;
}

function inferPluginFormatFromPath(filePath: string | undefined): string | null {
  if (!filePath) {
    return null;
  }

  const normalized = filePath.trim().toLowerCase().replace(/\\/g, "/");
  if (/(^|\/)[^/]+\.vst3(\/|$)/.test(normalized)) return "vst3";
  if (/(^|\/)[^/]+\.component(\/|$)/.test(normalized) || /(^|\/)[^/]+\.appex(\/|$)/.test(normalized)) return "au";
  if (/(^|\/)[^/]+\.lv2(\/|$)/.test(normalized)) return "lv2";
  if (/(^|\/)[^/]+\.clap(\/|$)/.test(normalized)) return "clap";
  if (/(^|\/)[^/]+\.aaxplugin(\/|$)/.test(normalized)) return "aax";
  if (/(^|\/)[^/]+\.(dll|vst)(\/|$)/.test(normalized)) return "vst2";
  return null;
}