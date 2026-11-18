/**
 * Remote resources described the same way for every provider: the licence policy, the import
 * metadata, and how preset archives name a resource they do not carry (written in both the
 * provider-neutral list and the older Tone3000 one, read from either).
 */
// @vitest-environment jsdom
import { describe, expect, it, vi } from "vitest";

vi.mock("../ts/settings.js", () => ({ activateEquipmentTab: vi.fn() }));
vi.mock("../ts/navigation.js", () => ({ switchMainPanel: vi.fn() }));
vi.mock("../ts/tone3000.js", () => ({
  downloadTone3000ResourceByReference: vi.fn(),
  isTone3000AuthReady: () => true,
  isTone3000ProxyModeEnabled: () => true,
  isTone3000RequestUrlAllowed: () => true,
  saveTone3000ApiKey: vi.fn(),
  tone3000AuthenticatedFetch: vi.fn(),
}));
window.IPlugSendMsg = () => {};

const { licencePolicy, normaliseLicenceId, readRemoteAttribution, remoteResourceMetadata } = await import("../ts/remoteResources.js");
const { buildArchiveRemoteRef, readArchiveRemoteRefs, shouldReferenceRemotely } = await import("../ts/presets/remoteArchiveResources.js");
const { tone3000ResourceMetadata } = await import("../ts/tone3000Shared.js");

describe("licences", () => {
  it("normalises ids and knows what each allows", () => {
    expect(normaliseLicenceId("CC-BY")).toBe("cc-by");
    expect(normaliseLicenceId("cco")).toBe("cc0");
    expect(licencePolicy("cc0")).toBe("redistributable");
    expect(licencePolicy("cc-by-sa")).toBe("redistributable");
    expect(licencePolicy("cc-by-nd")).toBe("redistributable-unmodified");
    expect(licencePolicy("cc-by-nc")).toBe("noncommercial");
    expect(licencePolicy("t3k")).toBe("reference-only");
    expect(licencePolicy(undefined)).toBe("reference-only");
  });

  it("writes only the keys it has, with the licence URL filled in", () => {
    expect(remoteResourceMetadata({ provider: "soundshed", providerKey: "k1", licence: "CC-BY", creatorName: "Ann", sourceUrl: "" })).toEqual({
      provider: "soundshed",
      providerKey: "k1",
      licence: "cc-by",
      licenceUrl: "https://creativecommons.org/licenses/by/4.0/",
      creatorName: "Ann",
    });
  });

  it("reads the attribution back, deriving the key from the library id", () => {
    expect(readRemoteAttribution("soundshed:k9", { provider: "soundshed" })).toMatchObject({ provider: "soundshed", providerKey: "k9" });
    expect(readRemoteAttribution("local-file", { provider: "" })).toBeNull();
  });
});

describe("Tone3000 import metadata", () => {
  it("carries the licence, creator and capture scope alongside the tone and model ids", () => {
    const metadata = tone3000ResourceMetadata(
      { id: 12, title: "JCM", slug: "jcm", gear: "amp-cab", license: "cc-by", user: { id: 3, username: "ann", url: "https://example/ann" } },
      { id: 456, name: "Crunch", architectureVersion: "2" },
    );
    expect(metadata).toMatchObject({
      provider: "tone3000",
      providerKey: "456",
      licence: "cc-by",
      creatorName: "ann",
      creatorUrl: "https://example/ann",
      captureScope: "amp+cab",
      toneId: "12",
      modelId: "456",
      architectureVersion: "2",
    });
  });

  it("keys a file inside a zipped model by its entry", () => {
    const metadata = tone3000ResourceMetadata({ id: 1, title: "T" }, { id: 7 }, "folder/My Model.nam");
    expect(metadata.providerKey).toBe("7:folder-My-Model.nam");
    expect(metadata.entryName).toBe("folder/My Model.nam");
  });
});

describe("archive references", () => {
  const tone3000Resource = {
    id: "tone3000:456", name: "Crunch", category: "amp", description: "", filePath: "",
    metadata: { provider: "tone3000", providerKey: "456", toneId: "12", modelId: "456", creatorName: "ann", licence: "cc-by" },
  };

  it("names Tone3000 files whatever their licence, and other providers' files when they may not be passed on", () => {
    expect(shouldReferenceRemotely(tone3000Resource)).toBe(true);
    expect(shouldReferenceRemotely({ ...tone3000Resource, id: "local", metadata: { provider: "local" } })).toBe(false);
    // No fetcher for this provider yet, so its file travels in the archive.
    expect(shouldReferenceRemotely({ ...tone3000Resource, id: "soundshed:1", metadata: { provider: "soundshed", licence: "cc-by" } })).toBe(false);
  });

  it("writes a Tone3000 file into both lists", () => {
    const { ref, legacy } = buildArchiveRemoteRef(tone3000Resource, "nam");
    expect(ref).toMatchObject({ id: "tone3000:456", provider: "tone3000", providerKey: "456", licence: "cc-by", providerData: { toneId: "12", modelId: "456" } });
    expect(legacy).toEqual({ id: "tone3000:456", name: "Crunch", category: "amp", type: "nam", toneId: "12", modelId: "456", creatorId: undefined, creatorName: "ann" });
  });

  it("refuses to share a Tone3000 file it could not find again", () => {
    expect(() => buildArchiveRemoteRef({ ...tone3000Resource, metadata: { provider: "tone3000" } }, "nam")).toThrow(/toneId\/modelId/);
  });

  it("reads either list, once per id, the newer list winning", () => {
    const refs = readArchiveRemoteRefs({
      remoteResources: [{ id: "tone3000:456", type: "nam", provider: "tone3000", providerKey: "456", licence: "cc-by" }],
      tone3000Resources: [
        { id: "tone3000:456", type: "nam", toneId: "12", modelId: "456" },
        { id: "tone3000:789", type: "nam", toneId: "12", modelId: "789", creatorName: "bob" },
      ],
    });
    expect(refs).toHaveLength(2);
    expect(refs[0].licence).toBe("cc-by");
    expect(refs[1]).toMatchObject({ provider: "tone3000", providerKey: "789", creatorName: "bob", providerData: { toneId: "12", modelId: "789" } });
  });
});
