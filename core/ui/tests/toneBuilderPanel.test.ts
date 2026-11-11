/**
 * The Tone Builder modal end to end with the service and the realiser stubbed: opening it from
 * the AI Tone Search event, researching, reviewing what was chosen (captures with creator and
 * licence, stand-ins), building, and the signed-out path.
 */
// @vitest-environment jsdom
import { afterEach, describe, expect, it, vi } from "vitest";

const signedIn = vi.fn();
const openSignIn = vi.fn();
const ensureEffectCatalogue = vi.fn();
const startToneBuilderJob = vi.fn();
const waitForToneBuilderJob = vi.fn();
const downloadProposalCaptures = vi.fn();
const buildAndSaveProposal = vi.fn();
const applyPreset = vi.fn();

vi.mock("../ts/toneSharingPanel.js", () => ({ ensureToneSharingSignedIn: signedIn, openToneSharingSignInModal: openSignIn }));
vi.mock("../ts/navigation.js", () => ({ switchMainPanel: vi.fn() }));
vi.mock("../ts/notifications.js", () => ({ showNotification: (...args: unknown[]) => showNotification(...args) }));
vi.mock("../ts/presets/load.js", () => ({ applyPresetFromLibrary: applyPreset }));
vi.mock("../ts/toneBuilder/appInventory.js", () => ({ readAppBuildInventory: () => ({ effects: [], resources: [] }) }));
vi.mock("../ts/toneBuilder/service.js", () => ({ ensureEffectCatalogue, startToneBuilderJob, waitForToneBuilderJob }));
// One balanced spec per built preset, so each preset is published under its proposal key.
const balanceProposal = vi.fn(async () => ({ specs: [{ key: "r" }], report: [] }));
vi.mock("../ts/toneBuilder/realise.js", async (importOriginal) => ({
  ...(await importOriginal<Record<string, unknown>>()),
  downloadProposalCaptures,
  buildAndSaveProposal,
  balanceProposal,
}));
const publishToneBuilderPresets = vi.fn();
vi.mock("../ts/toneBuilder/share.js", () => ({ publishToneBuilderPresets }));
const refreshToneBuilderStatus = vi.fn();
vi.mock("../ts/toneBuilder/availability.js", () => ({ refreshToneBuilderStatus }));
const showNotification = vi.fn();
const findCommunityMatches = vi.fn().mockResolvedValue([]);
const apiFetch = vi.fn();
const toneSharingFetch = vi.fn();
vi.mock("../ts/toneSharingPanel/api.js", () => ({ apiFetch, toneSharingFetch }));
URL.createObjectURL = vi.fn(() => "blob:artwork");
URL.revokeObjectURL = vi.fn();
const downloadAsset = vi.fn();
vi.mock("../ts/toneBuilder/communityCheck.js", () => ({ findCommunityMatches }));
vi.mock("../ts/toneSharingPanel/assets.js", () => ({ downloadAsset }));
vi.mock("../ts/toneSharingPanel/preview.js", () => ({ previewPreset: vi.fn() }));
vi.mock("../ts/resourceLibrary.js", () => ({ getLibraryResource: () => null }));
vi.mock("../ts/tone3000.js", () => ({ downloadTone3000ResourceByReference: vi.fn() }));

const { initializeToneBuilderPanel } = await import("../ts/toneBuilder/panel.js");
initializeToneBuilderPanel();

const capture = {
  ref: "tone3000:1", provider: "tone3000", key: "1", type: "nam", sha256: null, gearId: "amp/x", captureScope: "amp",
  licence: { id: "cc-by", url: null, policy: "redistributable", attribution: "a" }, creator: { name: "Ann <b>", url: null },
  sourceUrl: null, title: "Plexi", providerData: {}, fallback: null,
};
const doneJob = {
  jobId: "job-1", status: "done", progress: [], cachedDossier: false, error: null,
  dossier: { summary: "Loud and clean", gear: [], signalChain: [], evidenceLimits: "", gaps: ["Exact amp settings"] },
  proposal: {
    schema: "soundshed.tone-proposal/2", effectCatalogHash: "h", dossierId: null, notes: "", credits: [],
    validation: { ok: true, problems: [], presets: [{ key: "r", ok: true, problems: [], warnings: ["looks amp-only"] }] },
    presets: [{ key: "r", name: "Rhythm", description: "Crunch", tags: [], tuning: null, gearIds: [], scenes: [{ key: "m", title: "Main", chain: [
      { fx: "Neural Amp (NAM)", preset: null, set: {}, label: null, off: false, resource: capture },
      { fx: "Simple Cabinet", preset: null, set: {}, label: null, off: false, resource: { gearId: "cab/x", scope: "cab", fallback: { fx: "Simple Cabinet", preset: "1960 4x12", set: {} } } },
    ] }] }],
    builderPresets: [],
  },
};

const modal = (): HTMLElement | null => document.getElementById("tone-builder-modal");
const click = (action: string): void => modal()?.querySelector<HTMLElement>(`[data-tone-builder-action="${action}"]`)?.click();

const OPEN = { enabled: true, available: true, reason: null, message: null };
refreshToneBuilderStatus.mockResolvedValue(OPEN);
publishToneBuilderPresets.mockResolvedValue({ itemIds: ["itm-1"], packId: null, alreadyPublished: false });

afterEach(() => {
  vi.clearAllMocks();
  findCommunityMatches.mockResolvedValue([]);
  refreshToneBuilderStatus.mockResolvedValue(OPEN);
  publishToneBuilderPresets.mockResolvedValue({ itemIds: ["itm-1"], packId: null, alreadyPublished: false });
});

describe("the Tone Builder", () => {
  it("researches, shows what it chose, and builds", async () => {
    signedIn.mockResolvedValue(true);
    ensureEffectCatalogue.mockResolvedValue("hash");
    startToneBuilderJob.mockResolvedValue({ jobId: "job-1" });
    waitForToneBuilderJob.mockResolvedValue(doneJob);
    buildAndSaveProposal.mockReturnValue({ presets: [{ id: "p1" }], folderName: "Tone Builder – AC/DC – Back in Black" });

    document.dispatchEvent(new CustomEvent("tone-builder:open", { detail: { artist: "AC/DC", song: "Back in Black" } }));
    await vi.waitFor(() => expect(modal()?.style.display).toBe("flex"));
    expect((document.getElementById("tone-builder-artist") as HTMLInputElement).value).toBe("AC/DC");

    click("research");
    await vi.waitFor(() => expect(modal()?.textContent).toContain("Rhythm"));
    const brief = startToneBuilderJob.mock.calls[0][0];
    expect(brief).toMatchObject({ subject: { artist: "AC/DC", song: "Back in Black" }, client: { effectCatalogHash: "hash" }, constraints: { licencePolicy: "any" }, options: { thinking: "thorough" } });
    const text = modal()?.textContent ?? "";
    expect(text).toContain("Plexi");
    expect(text).toContain("Ann <b>"); // escaped, shown as text
    expect(modal()?.querySelector("b")).toBeNull();
    expect(text).toContain("cc-by");
    expect(text).toContain("Simple Cabinet · 1960 4x12 (stand-in)");
    expect(text).toContain("What the research could not confirm (1)");

    click("build");
    await vi.waitFor(() => expect(modal()?.textContent).toContain("saved to"));
    expect(downloadProposalCaptures.mock.calls[0][0].map((ref: { ref: string }) => ref.ref)).toEqual(["tone3000:1"]);
    expect(buildAndSaveProposal).toHaveBeenCalledWith("job-1", doneJob.proposal, "Tone Builder – AC/DC – Back in Black", [{ key: "r" }]);

    click("open-first");
    await vi.waitFor(() => expect(applyPreset).toHaveBeenCalledWith("p1"));
  });

  it("starts no research job when stopped before it starts", async () => {
    // Stop is closing the panel. During the Tone Sharing check or the catalogue upload it used to
    // abort nothing, and the (metered) research job started anyway once they finished.
    signedIn.mockResolvedValue(true);
    let finishCheck: (value: never[]) => void = () => undefined;
    findCommunityMatches.mockReturnValueOnce(new Promise((resolve) => { finishCheck = resolve; }));
    let finishCatalogue: (value: string) => void = () => undefined;
    ensureEffectCatalogue.mockReturnValueOnce(new Promise((resolve) => { finishCatalogue = resolve; }));
    startToneBuilderJob.mockResolvedValue({ jobId: "job-stopped" });
    waitForToneBuilderJob.mockResolvedValue(doneJob);

    document.dispatchEvent(new CustomEvent("tone-builder:open", { detail: { artist: "Stopped", song: "Early" } }));
    await vi.waitFor(() => expect(document.getElementById("tone-builder-artist")).not.toBeNull());
    click("research");
    await vi.waitFor(() => expect(findCommunityMatches).toHaveBeenCalled());
    click("close");
    finishCheck([]);
    await vi.waitFor(() => expect(modal()?.style.display).toBe("none"));
    await new Promise((resolve) => setTimeout(resolve, 0));
    expect(ensureEffectCatalogue).not.toHaveBeenCalled();

    // And stopped during the catalogue check, once the Tone Sharing check is behind it.
    document.dispatchEvent(new CustomEvent("tone-builder:open", { detail: {} }));
    await vi.waitFor(() => expect(modal()?.style.display).toBe("flex"));
    click("research");
    await vi.waitFor(() => expect(ensureEffectCatalogue).toHaveBeenCalled());
    click("close");
    finishCatalogue("hash");
    await new Promise((resolve) => setTimeout(resolve, 0));
    expect(startToneBuilderJob).not.toHaveBeenCalled();
  });

  it("asks a signed-out user to sign in instead", async () => {
    signedIn.mockResolvedValue(false);
    document.dispatchEvent(new CustomEvent("tone-builder:open", { detail: {} }));
    await vi.waitFor(() => expect(openSignIn).toHaveBeenCalled());
    expect(startToneBuilderJob).not.toHaveBeenCalled();
  });

  it("reports a failed job and goes back to the brief", async () => {
    signedIn.mockResolvedValue(true);
    ensureEffectCatalogue.mockResolvedValue("hash");
    startToneBuilderJob.mockResolvedValue({ jobId: "job-2" });
    waitForToneBuilderJob.mockResolvedValue({ ...doneJob, status: "failed", proposal: undefined, error: { code: "PROPOSAL_INVALID", message: "No valid presets" } });
    document.dispatchEvent(new CustomEvent("tone-builder:open", { detail: { artist: "X" } }));
    await vi.waitFor(() => expect(document.getElementById("tone-builder-artist")).not.toBeNull());
    click("research");
    await vi.waitFor(() => expect(modal()?.textContent).toContain("No valid presets"));
    expect(document.getElementById("tone-builder-artist")).not.toBeNull();
  });
  it("sends the chosen thinking level and publishes the result as Soundshed without asking", async () => {
    signedIn.mockResolvedValue(true);
    ensureEffectCatalogue.mockResolvedValue("hash");
    startToneBuilderJob.mockResolvedValue({ jobId: "job-3" });
    waitForToneBuilderJob.mockResolvedValue(doneJob);
    buildAndSaveProposal.mockReturnValue({ presets: [{ id: "p1", name: "Rhythm" }], folderName: "F" });

    document.dispatchEvent(new CustomEvent("tone-builder:open", { detail: { artist: "Dick Dale" } }));
    await vi.waitFor(() => expect(document.getElementById("tone-builder-thinking")).not.toBeNull());
    const thinking = document.getElementById("tone-builder-thinking") as HTMLSelectElement;
    thinking.value = "quick";
    thinking.dispatchEvent(new Event("change", { bubbles: true }));
    expect(modal()?.textContent).toContain("Fastest and cheapest");
    click("research");
    await vi.waitFor(() => expect(modal()?.textContent).toContain("Rhythm"));
    expect(startToneBuilderJob.mock.calls[0][0].options).toEqual({ thinking: "quick" });

    // No choice to make: the review says the presets will be published.
    expect(document.getElementById("tone-builder-share")).toBeNull();
    expect(modal()?.textContent).toContain("published on Tone Sharing as Soundshed");
    click("build");
    await vi.waitFor(() => expect(modal()?.textContent).toContain("Published on Tone Sharing as Soundshed: 1 preset(s)."));
    expect(publishToneBuilderPresets.mock.calls[0][0]).toBe("job-1");
    expect(publishToneBuilderPresets.mock.calls[0][1]).toEqual([{ key: "r", preset: { id: "p1", name: "Rhythm" } }]);
  });

  it("keeps the built presets and offers to publish again when publishing fails", async () => {
    signedIn.mockResolvedValue(true);
    ensureEffectCatalogue.mockResolvedValue("hash");
    startToneBuilderJob.mockResolvedValue({ jobId: "job-4" });
    waitForToneBuilderJob.mockResolvedValue(doneJob);
    buildAndSaveProposal.mockReturnValue({ presets: [{ id: "p1", name: "Rhythm" }], folderName: "F" });
    publishToneBuilderPresets.mockRejectedValueOnce(new Error("Upload failed (500)"));
    document.dispatchEvent(new CustomEvent("tone-builder:open", { detail: { artist: "X" } }));
    await vi.waitFor(() => expect(document.getElementById("tone-builder-artist")).not.toBeNull());
    click("research");
    await vi.waitFor(() => expect(modal()?.textContent).toContain("Rhythm"));
    click("build");
    await vi.waitFor(() => expect(modal()?.textContent).toContain("Publishing on Tone Sharing failed: Upload failed (500)"));
    expect(modal()?.textContent).toContain("1 preset(s) saved to");

    // Closed and opened again, the presets are still waiting to be published.
    click("close");
    document.dispatchEvent(new CustomEvent("tone-builder:open", { detail: {} }));
    await vi.waitFor(() => expect(modal()?.style.display).toBe("flex"));
    click("publish-retry");
    await vi.waitFor(() => expect(modal()?.textContent).toContain("Published on Tone Sharing as Soundshed"));
    expect(publishToneBuilderPresets).toHaveBeenCalledTimes(2);
    expect(modal()?.querySelector("[data-tone-builder-action=publish-retry]")).toBeNull();
  });

  it("does not open for a new tone while the service is switched off or at its daily limit", async () => {
    signedIn.mockResolvedValue(true);
    refreshToneBuilderStatus.mockResolvedValue({ enabled: true, available: false, reason: "spend-limit", message: "The Tone Builder has reached today's limit. Try again tomorrow." });
    click("close");
    document.dispatchEvent(new CustomEvent("tone-builder:open", { detail: { artist: "X" } }));
    await vi.waitFor(() => expect(showNotification).toHaveBeenCalledWith("Tone Builder unavailable", "The Tone Builder has reached today's limit. Try again tomorrow."));
    expect(modal()?.style.display).toBe("none");
  });

  it("offers what Tone Sharing already has before researching, and builds anyway on request", async () => {
    signedIn.mockResolvedValue(true);
    findCommunityMatches.mockResolvedValue([
      { kind: "pack", id: "pak-1", title: "Dick Dale - Misirlou", description: "Surf pack", creator: "chris", installed: false, score: 2 },
      { kind: "item", id: "itm-1", title: "Misirlou Lead", description: "", creator: null, installed: true, score: 1 },
    ]);
    ensureEffectCatalogue.mockResolvedValue("hash");
    startToneBuilderJob.mockResolvedValue({ jobId: "job-5" });
    waitForToneBuilderJob.mockResolvedValue(doneJob);

    document.dispatchEvent(new CustomEvent("tone-builder:open", { detail: { artist: "Dick Dale", song: "Misirlou" } }));
    await vi.waitFor(() => expect(document.getElementById("tone-builder-artist")).not.toBeNull());
    click("research");
    await vi.waitFor(() => expect(modal()?.textContent).toContain("Tone Sharing already has these"));
    expect(findCommunityMatches).toHaveBeenCalledWith("Dick Dale", "Misirlou");
    expect(startToneBuilderJob).not.toHaveBeenCalled();
    const install = modal()?.querySelector<HTMLButtonElement>("[data-tone-builder-action=install-match][data-id=pak-1]");
    install?.click();
    await vi.waitFor(() => expect(downloadAsset).toHaveBeenCalledWith("pack", "pak-1"));
    expect(modal()?.querySelector<HTMLButtonElement>("[data-id=itm-1][data-tone-builder-action=install-match]")?.disabled).toBe(true);

    click("build-anyway");
    await vi.waitFor(() => expect(modal()?.textContent).toContain("Rhythm"));
    expect(startToneBuilderJob).toHaveBeenCalledTimes(1);
    expect(findCommunityMatches).toHaveBeenCalledTimes(1);
  });

  // Sepia: this exercises an upstream online service, which is switched off here
  // (ONLINE_SERVICES_ENABLED in ts/apiConfig.ts). Kept, skipped, for when a fork runs its own.
  it.skip("shows the pack artwork and text, redraws it, and publishes it as the pack thumbnail", async () => {
    signedIn.mockResolvedValue(true);
    ensureEffectCatalogue.mockResolvedValue("hash");
    startToneBuilderJob.mockResolvedValue({ jobId: "job-6" });
    const proposal = { ...doneJob.proposal, packTitle: "Dick Dale – Misirlou", packDescription: "Surf at full volume.", sources: [{ title: "Fender history", url: "https://fender.com/x" }] };
    waitForToneBuilderJob.mockResolvedValue({ ...doneJob, jobId: "job-6", proposal, artwork: { available: true, regenerations: 0, remaining: 5 } });
    toneSharingFetch.mockImplementation(async () => new Response(new Blob(["jpeg"], { type: "image/jpeg" })));
    apiFetch.mockResolvedValue({ artwork: { available: true, regenerations: 1, remaining: 4 } });
    buildAndSaveProposal.mockReturnValue({ presets: [{ id: "p1", name: "Rhythm" }], folderName: "F" });

    document.dispatchEvent(new CustomEvent("tone-builder:open", { detail: { artist: "Dick Dale", song: "Misirlou" } }));
    await vi.waitFor(() => expect(document.getElementById("tone-builder-artist")).not.toBeNull());
    click("research");
    await vi.waitFor(() => expect(modal()?.querySelector("img.tone-builder-artwork-image")).not.toBeNull());
    expect(toneSharingFetch).toHaveBeenCalledWith("/tone-builder/jobs/job-6/artwork");
    const text = modal()?.textContent ?? "";
    expect(text).toContain("Dick Dale – Misirlou");
    expect(text).toContain("Surf at full volume.");
    expect(text).toContain("Sources: Fender history (fender.com).");
    expect(text).toContain("New artwork (5 left)");

    click("redraw-artwork");
    await vi.waitFor(() => expect(modal()?.textContent).toContain("New artwork (4 left)"));
    expect(apiFetch).toHaveBeenCalledWith("/tone-builder/jobs/job-6/artwork", { method: "POST" });

    // The service writes the published text from the job itself; the app sends the artwork.
    click("build");
    await vi.waitFor(() => expect(publishToneBuilderPresets).toHaveBeenCalled());
    const [jobId, , , artwork] = publishToneBuilderPresets.mock.calls[0];
    expect(jobId).toBe("job-6");
    expect(artwork).toBeInstanceOf(Blob);
  });
});
