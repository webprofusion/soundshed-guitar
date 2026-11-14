import { beforeEach, describe, expect, it } from "vitest";
import { findMissingPresetResources, remoteRefForMissingResource } from "../ts/presets/missingResources.js";
import { uiState } from "../ts/state.js";
import type { LibraryResource, Preset, ResourceRef } from "../ts/types.js";

function presetWith(...refs: ResourceRef[]): Preset {
  return {
    id: "p",
    name: "Project tone",
    graph: {
      nodes: [{ id: "amp", type: "amp_nam", resources: refs }],
      edges: [],
    },
  } as unknown as Preset;
}

function libraryEntry(id: string, metadata: Record<string, string> = {}, extra: Partial<LibraryResource> = {}): LibraryResource {
  return { id, name: id, category: "amp", description: "", tags: [], filePath: `/lib/${id}.nam`, metadata, ...extra } as LibraryResource;
}

const tone3000Source = {
  name: "Test Tone - Model",
  provider: "tone3000",
  providerKey: "42",
  toneId: "7",
  modelId: "42",
  contentHash: "nam-content-v1:abc",
};

describe("models a preset needs and the library lacks", () => {
  beforeEach(() => {
    uiState.resourceLibrary = { nam: [libraryEntry("present")], ir: [] };
  });

  it("names a missing model by its saved source", () => {
    const missing = findMissingPresetResources([presetWith(
      { resourceType: "nam", resourceId: "present" },
      { resourceType: "nam", resourceId: "tone3000:42", source: tone3000Source },
    )]);
    expect(missing).toEqual([{ type: "nam", id: "tone3000:42", name: "Test Tone - Model", source: tone3000Source }]);
  });

  it("counts an entry whose file is gone as missing, once", () => {
    uiState.resourceLibrary = { nam: [libraryEntry("lost", {}, { fileMissing: true })], ir: [] };
    const ref = { resourceType: "nam", resourceId: "lost" };
    expect(findMissingPresetResources([presetWith(ref, ref)]).map((entry) => entry.id)).toEqual(["lost"]);
  });

  it("leaves out one the library has under another id, as the engine finds it", () => {
    uiState.resourceLibrary = {
      nam: [libraryEntry("tone3000:42:Model.nam", { provider: "tone3000", providerKey: "42" })],
      ir: [],
    };
    expect(findMissingPresetResources([presetWith({ resourceType: "nam", resourceId: "tone3000:42", source: tone3000Source })])).toEqual([]);
  });

  it("leaves out a reference by file path, which the UI cannot check", () => {
    expect(findMissingPresetResources([presetWith({ resourceType: "nam", filePath: "/somewhere/model.nam" })])).toEqual([]);
  });

  it("fetches a Tone3000 model again under its library id", () => {
    const [missing] = findMissingPresetResources([presetWith({ resourceType: "nam", resourceId: "tone3000:42", source: tone3000Source })]);
    expect(remoteRefForMissingResource(missing)).toMatchObject({
      id: "tone3000:42",
      type: "nam",
      provider: "tone3000",
      providerKey: "42",
      contentHash: "nam-content-v1:abc",
      providerData: { toneId: "7", modelId: "42" },
    });
  });

  it("cannot fetch a model with no provider", () => {
    const [missing] = findMissingPresetResources([presetWith({ resourceType: "nam", resourceId: "local", source: { name: "Local" } })]);
    expect(missing.name).toBe("Local");
    expect(remoteRefForMissingResource(missing)).toBeNull();
  });
});
