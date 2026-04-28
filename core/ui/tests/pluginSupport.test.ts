import { describe, expect, it } from "vitest";
import { detectPluginSupportPlatform, getUnsupportedPluginSelection, inferPluginFormat } from "../ts/pluginSupport";

describe("plugin support detection", () => {
  it("detects the host platform from common platform strings", () => {
    expect(detectPluginSupportPlatform("Win32")).toBe("windows");
    expect(detectPluginSupportPlatform("MacIntel")).toBe("mac");
    expect(detectPluginSupportPlatform("Linux x86_64")).toBe("linux");
  });

  it("infers plugin format from metadata before file path", () => {
    expect(inferPluginFormat({ filePath: "C:/Plugins/Amp.vst3", metadata: { pluginFormat: "CLAP" } })).toBe("clap");
  });

  it("allows VST3 on Windows", () => {
    expect(getUnsupportedPluginSelection({ filePath: "C:/Program Files/Common Files/VST3/Amp.vst3" }, "windows"))
      .toBeNull();
  });

  it("warns for Audio Unit resources on Windows", () => {
    expect(getUnsupportedPluginSelection({ filePath: "/Library/Audio/Plug-Ins/Components/Amp.component" }, "windows"))
      .toEqual({ format: "au", label: "Audio Unit" });
  });

  it("allows Audio Unit resources on macOS", () => {
    expect(getUnsupportedPluginSelection({ filePath: "/Library/Audio/Plug-Ins/Components/Amp.component" }, "mac"))
      .toBeNull();
  });

  it("warns for plugin formats that the hosted plugin effect cannot load", () => {
    expect(getUnsupportedPluginSelection({ filePath: "C:/Plugins/Amp.clap" }, "windows"))
      .toEqual({ format: "clap", label: "CLAP" });
  });

  it("ignores unknown formats to avoid false warnings", () => {
    expect(getUnsupportedPluginSelection({ filePath: "C:/Plugins/Amp.bundle" }, "windows")).toBeNull();
  });
});