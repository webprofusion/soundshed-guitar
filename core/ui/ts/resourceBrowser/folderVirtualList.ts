/**
 * The windowed renderer behind the Folder tab's file list.
 *
 * A folder of NAM captures can hold thousands of files, so only the rows near
 * the viewport are built. Row heights are not known up front — a row grows
 * when its details panel is expanded, and metadata arrives late — so each one
 * is measured after it paints and the offsets are rebuilt when a measurement
 * disagrees with the estimate.
 */

import { escapeHtml } from "../utils.js";
import { renderIcon } from "../iconAssets.js";
import type { FolderRowState } from "./folderRows.js";
import { folderFileLibraryEntry, renderFolderFileRow } from "./folderRows.js";
import { renderFolderGroupHeader } from "./folderGroups.js";
import {
  FOLDER_VIRTUAL_ESTIMATED_DIR_HEIGHT,
  FOLDER_VIRTUAL_ESTIMATED_FILE_HEIGHT,
  FOLDER_VIRTUAL_GAP,
  FOLDER_VIRTUAL_OVERSCAN,
} from "./settings.js";
import type { FolderVirtualEntry } from "./types.js";

/** What the list needs from the tab around it, read fresh on every pass. */
export interface FolderVirtualListHost {
  /** The scroll container the rows are drawn into. */
  getScrollElement(): HTMLElement | null;
  /** The file whose details panel is open, which makes its row taller. */
  getExpandedPath(): string | null;
  /** The tab state a row builder draws from. */
  getRowState(): FolderRowState;
}

export class FolderVirtualList {
  constructor(private readonly host: FolderVirtualListHost) {}

  private entries: FolderVirtualEntry[] = [];

  private offsets: number[] = [];

  /** Real heights, keyed by entry, once a row has actually been measured. */
  private measuredHeights = new Map<string, number>();

  private windowQueued = false;

  private measureQueued = false;

  /** Replace the list's contents. Measurements are kept: the rows are the same. */
  setEntries(entries: FolderVirtualEntry[]): void {
    this.entries = entries;
    this.rebuildOffsets();
  }

  /** Drop measured heights — call when the listing itself changed. */
  resetMeasurements(): void {
    this.measuredHeights.clear();
  }

  get length(): number {
    return this.entries.length;
  }

  private entryHeight(entry: FolderVirtualEntry): number {
    const measured = this.measuredHeights.get(entry.key);
    if (measured) return measured;
    if (entry.kind === "dir") return FOLDER_VIRTUAL_ESTIMATED_DIR_HEIGHT;
    if (entry.kind === "group") return FOLDER_VIRTUAL_ESTIMATED_FILE_HEIGHT;
    return FOLDER_VIRTUAL_ESTIMATED_FILE_HEIGHT + (this.host.getExpandedPath() === entry.file.path ? 140 : 0);
  }

  private rebuildOffsets(): void {
    this.offsets = new Array(this.entries.length);
    let offset = 0;
    this.entries.forEach((entry, index) => {
      this.offsets[index] = offset;
      offset += this.entryHeight(entry) + FOLDER_VIRTUAL_GAP;
    });
  }

  private totalHeight(): number {
    if (!this.entries.length) return 0;
    const lastIndex = this.entries.length - 1;
    return this.offsets[lastIndex] + this.entryHeight(this.entries[lastIndex]);
  }

  private indexAtOffset(offset: number): number {
    let low = 0;
    let high = this.offsets.length - 1;
    while (low <= high) {
      const middle = Math.floor((low + high) / 2);
      if (this.offsets[middle] <= offset) low = middle + 1;
      else high = middle - 1;
    }
    return Math.max(0, high);
  }

  renderWindow(): void {
    const scrollElement = this.host.getScrollElement();
    if (!scrollElement || !this.entries.length) return;
    const scrollTop = scrollElement.scrollTop;
    const viewportBottom = scrollTop + Math.max(scrollElement.clientHeight, 1);
    const firstVisible = this.indexAtOffset(scrollTop);
    const lastVisible = this.indexAtOffset(viewportBottom);
    const start = Math.max(0, firstVisible - FOLDER_VIRTUAL_OVERSCAN);
    const end = Math.min(this.entries.length, lastVisible + FOLDER_VIRTUAL_OVERSCAN + 1);
    const rowState = this.host.getRowState();
    const rows = this.entries.slice(start, end).map((entry, relativeIndex) => {
      const index = start + relativeIndex;
      return `<div class="resource-browser-virtual-entry" data-virtual-index="${index}" style="transform:translateY(${this.offsets[index]}px)">${this.renderEntry(entry, rowState)}</div>`;
    }).join("");

    scrollElement.classList.add("is-virtualized");
    scrollElement.innerHTML = `<div class="resource-browser-virtual-spacer" style="height:${this.totalHeight()}px">${rows}</div>`;
    this.queueMeasurement();
  }

  private renderEntry(entry: FolderVirtualEntry, rowState: FolderRowState): string {
    if (entry.kind === "file") return renderFolderFileRow(entry.file, rowState, entry.groupMember);
    if (entry.kind === "group") return renderFolderGroupHeader(entry, folderFileLibraryEntry);
    return `
      <div class="resource-browser-folder-entry" data-kind="dir" data-path="${escapeHtml(entry.dir.path)}">
        <div class="results-item resource-browser-item resource-browser-folder-dir-row">
          <div class="results-item-main resource-browser-item-info">
            <div class="results-item-title resource-browser-item-title">${renderIcon("folder", "resource-browser-icon")}<span>${escapeHtml(entry.dir.name)}</span></div>
          </div>
        </div>
      </div>
    `;
  }

  queueWindowRender(): void {
    if (this.windowQueued) return;
    this.windowQueued = true;
    requestAnimationFrame(() => {
      this.windowQueued = false;
      this.renderWindow();
    });
  }

  private queueMeasurement(): void {
    if (this.measureQueued) return;
    this.measureQueued = true;
    requestAnimationFrame(() => {
      this.measureQueued = false;
      const scrollElement = this.host.getScrollElement();
      if (!scrollElement) return;
      let layoutChanged = false;
      scrollElement.querySelectorAll<HTMLElement>(".resource-browser-virtual-entry[data-virtual-index]").forEach((element) => {
        const index = Number(element.dataset.virtualIndex);
        const entry = this.entries[index];
        if (!entry) return;
        const measuredHeight = Math.ceil(element.offsetHeight);
        if (measuredHeight > 0 && this.measuredHeights.get(entry.key) !== measuredHeight) {
          this.measuredHeights.set(entry.key, measuredHeight);
          layoutChanged = true;
        }
      });
      if (layoutChanged) {
        this.rebuildOffsets();
        this.renderWindow();
      }
    });
  }
}
