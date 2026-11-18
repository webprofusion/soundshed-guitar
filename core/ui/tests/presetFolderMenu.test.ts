/**
 * Right-click on a folder in the preset chooser offers Create Pack…, which hands the
 * folder's presets (its subfolders' included) to the Tone Sharing pack modal.
 */
// @vitest-environment jsdom
import { afterEach, describe, expect, it, vi } from "vitest";

const openFolderPackModal = vi.fn().mockResolvedValue(undefined);
const ensureToneSharingSignedIn = vi.fn();
const openToneSharingSignInModal = vi.fn();
const switchMainPanel = vi.fn();
const closePresetLibraryPopover = vi.fn();

vi.mock("../ts/toneSharingPanel.js", () => ({ ensureToneSharingSignedIn, openFolderPackModal, openToneSharingSignInModal }));
vi.mock("../ts/navigation.js", () => ({ switchMainPanel }));
vi.mock("../ts/presets/popover.js", () => ({ closePresetLibraryPopover }));
vi.mock("../ts/notifications.js", () => ({ showNotification: vi.fn() }));

// presets/dom.ts looks its elements up once, at import.
document.body.innerHTML = `
  <div id="preset-folder-tree">
    <div class="preset-folder-item" data-folder-id="__all__"><span class="folder-name">All Presets</span></div>
    <div class="preset-folder-item" data-folder-id="gig"><span class="folder-name">Gig</span></div>
    <div class="preset-folder-item" data-folder-id="empty"><span class="folder-name">Empty</span></div>
  </div>
`;
window.IPlugSendMsg = () => {};

const { uiState } = await import("../ts/state.js");
const { PRESET_FOLDER_ALL_ID } = await import("../ts/presets/sorting.js");
const { closePresetFolderMenu, initializePresetFolderMenu } = await import("../ts/presets/folderContextMenu.js");

document.querySelector('[data-folder-id="__all__"]')?.setAttribute("data-folder-id", PRESET_FOLDER_ALL_ID);
uiState.presets = [
  { id: "a", name: "Crunch" },
  { id: "b", name: "Lead" },
  { id: "c", name: "Elsewhere" },
] as typeof uiState.presets;
uiState.presetFolders = [
  { id: "gig", name: "Gig", presetIds: ["a"], children: [{ id: "solos", name: "Solos", presetIds: ["b"], children: [] }] },
  { id: "empty", name: "Empty", presetIds: [], children: [] },
];
initializePresetFolderMenu();

function rightClick(folderId: string): MouseEvent {
  const row = document.querySelector(`[data-folder-id="${folderId}"] .folder-name`);
  const event = new MouseEvent("contextmenu", { bubbles: true, cancelable: true, clientX: 40, clientY: 50 });
  row?.dispatchEvent(event);
  return event;
}

function createPackButton(): HTMLButtonElement | null {
  return document.querySelector<HTMLButtonElement>('.preset-folder-menu [data-action="createPack"]');
}

afterEach(() => {
  closePresetFolderMenu();
  vi.clearAllMocks();
});

describe("the preset folder menu", () => {
  it("opens Create Pack with the folder's presets, subfolders included", async () => {
    ensureToneSharingSignedIn.mockResolvedValue(true);
    expect(rightClick("gig").defaultPrevented).toBe(true);
    expect(createPackButton()?.disabled).toBe(false);
    expect(document.querySelector(".preset-folder-menu")?.hasAttribute("data-control-bar-popover")).toBe(true);

    createPackButton()?.click();
    await vi.waitFor(() => expect(openFolderPackModal).toHaveBeenCalled());
    expect(document.querySelector(".preset-folder-menu")).toBeNull();
    expect(closePresetLibraryPopover).toHaveBeenCalled();
    const [name, presets] = openFolderPackModal.mock.calls[0];
    expect(name).toBe("Gig");
    expect(presets.map((preset: { id: string }) => preset.id)).toEqual(["a", "b"]);
  });

  it("sends a signed-out user to sign in instead", async () => {
    ensureToneSharingSignedIn.mockResolvedValue(false);
    rightClick("gig");
    createPackButton()?.click();
    await vi.waitFor(() => expect(openToneSharingSignInModal).toHaveBeenCalled());
    expect(switchMainPanel).toHaveBeenCalledWith("tones-community");
    expect(openFolderPackModal).not.toHaveBeenCalled();
  });

  it("disables Create Pack on an empty folder and offers nothing on All Presets", () => {
    rightClick("empty");
    expect(createPackButton()?.disabled).toBe(true);
    closePresetFolderMenu();

    expect(rightClick(PRESET_FOLDER_ALL_ID).defaultPrevented).toBe(false);
    expect(document.querySelector(".preset-folder-menu")).toBeNull();
  });
});
