/**
 * Scene badges: the preset's scenes as small pills floated over the top edge of the
 * signal chain, in the strip the collapse chevron already sits in, instead of a toolbar
 * row of their own under the chain.
 *
 * A click selects a scene. A double-click, or F2 on a focused badge, renames it in place:
 * Enter or leaving the field keeps the new name, Escape drops it. The × removes that
 * scene once the user confirms, and the + badge adds one copied from the playing scene. A preset with a single
 * scene shows only the + badge, labelled so it is not mistaken for the chain's own +.
 * Dragging a badge onto another moves the scene to that one's place (onto the + badge, to
 * the end); Alt+Left/Right on a focused badge moves it one place.
 *
 * The bar re-renders on most engine messages, so a rename in progress keeps its field:
 * a render leaves the badges alone while the field has focus.
 */

import { showConfirm } from "../dialogs.js";
import { beginPointerDrag, type PointerDragTarget } from "../pointerDrag.js";
import type { Preset } from "../types.js";

export interface SceneBadgeActions {
  select(sceneId: string): void;
  rename(sceneId: string, title: string): void;
  remove(sceneId: string): void;
  add(): void;
  /** Moves the scene to `toIndex` in the preset's scene list. */
  move(sceneId: string, toIndex: number): void;
}

interface SceneDropTarget extends PointerDragTarget {
  toIndex: number;
}

function badgeHost(): HTMLElement | null {
  return document.getElementById("signal-path-scene-badges");
}

export function clearSceneBadges(): void {
  const host = badgeHost();
  if (!host || isRenaming(host)) {
    return;
  }
  host.replaceChildren();
  host.hidden = true;
}

export function renderSceneBadges(
  preset: Preset,
  activeSceneId: string,
  canAdd: boolean,
  actions: SceneBadgeActions,
): void {
  const host = badgeHost();
  if (!host || isRenaming(host)) {
    return;
  }

  const scenes = preset.scenes ?? [];
  const badges: HTMLElement[] = [];
  if (scenes.length > 1) {
    scenes.forEach((scene, index) => {
      badges.push(buildSceneBadge(scene.id, scene.title, scene.id === activeSceneId, index, scenes.length, actions));
    });
  }
  if (canAdd) {
    badges.push(buildAddBadge(badges.length === 0, actions));
  }

  host.replaceChildren(...badges);
  host.hidden = badges.length === 0;
}

function isRenaming(host: HTMLElement): boolean {
  const field = host.querySelector(".scene-badge-input");
  return field !== null && field === document.activeElement;
}

function buildSceneBadge(
  sceneId: string,
  title: string,
  active: boolean,
  index: number,
  count: number,
  actions: SceneBadgeActions,
): HTMLElement {
  const badge = document.createElement("div");
  badge.className = active ? "scene-badge is-active" : "scene-badge";
  badge.dataset.sceneId = sceneId;
  badge.dataset.sceneIndex = String(index);

  const label = document.createElement("button");
  label.type = "button";
  label.className = "scene-badge-label";
  label.textContent = title;
  label.title = `${title} (double-click to rename, drag to reorder)`;
  label.setAttribute("aria-pressed", String(active));
  // On the badge, not the label: a press on the label captures the pointer to the badge for a
  // possible drag, so the click and double-click that follow it land on the badge.
  const isOwnClick = (event: MouseEvent): boolean =>
    !(event.target as Element | null)?.closest(".scene-badge-close, .scene-badge-input");
  badge.addEventListener("click", (event) => {
    if (isOwnClick(event)) {
      actions.select(sceneId);
    }
  });
  badge.addEventListener("dblclick", (event) => {
    if (isOwnClick(event) && label.isConnected) {
      event.preventDefault();
      beginRename(label, sceneId, title, actions);
    }
  });
  label.addEventListener("keydown", (event) => {
    if (event.key === "F2") {
      event.preventDefault();
      beginRename(label, sceneId, title, actions);
      return;
    }
    if (event.altKey && (event.key === "ArrowLeft" || event.key === "ArrowRight")) {
      event.preventDefault();
      // Typing a scene's place must not reach the app's keyboard shortcuts.
      event.stopPropagation();
      const toIndex = index + (event.key === "ArrowLeft" ? -1 : 1);
      if (toIndex >= 0 && toIndex < count) {
        actions.move(sceneId, toIndex);
      }
    }
  });
  label.addEventListener("pointerdown", (event) => {
    beginSceneDrag(event, badge, sceneId, index, count, actions);
  });

  const close = document.createElement("button");
  close.type = "button";
  close.className = "scene-badge-close";
  close.title = "Remove scene";
  close.setAttribute("aria-label", `Remove scene ${title}`);
  close.textContent = "×";
  close.addEventListener("click", (event) => {
    event.stopPropagation();
    void confirmRemove(sceneId, title, actions);
  });

  badge.append(label, close);
  return badge;
}

function beginSceneDrag(
  event: PointerEvent,
  badge: HTMLElement,
  sceneId: string,
  fromIndex: number,
  count: number,
  actions: SceneBadgeActions,
): void {
  beginPointerDrag<SceneDropTarget>(event, {
    source: badge,
    sourceSelector: ".scene-badge",
    previewClass: "scene-badge-drag-preview",
    matchPreviewHeight: true,
    bodyClass: "scene-badge-dragging",
    resolveTarget: (moveEvent) => resolveSceneDropTarget(moveEvent, count),
    setTargetHighlight: (target, highlighted) => {
      // The scene lands on the far side of the badge it is dropped on: after it moving right,
      // before it moving left.
      const after = target.toIndex > fromIndex;
      target.element.classList.toggle("drop-after", highlighted && after);
      target.element.classList.toggle("drop-before", highlighted && !after);
    },
    // After the release: the engine's answer redraws the badges, this one included
    // (pointer-drag-click-after-rerender).
    onDrop: (target) => {
      if (target && target.toIndex !== fromIndex) {
        window.setTimeout(() => actions.move(sceneId, target.toIndex), 0);
      }
    },
  });
}

function resolveSceneDropTarget(event: PointerEvent, count: number): SceneDropTarget | null {
  const host = badgeHost();
  const element = document.elementFromPoint(event.clientX, event.clientY)
    ?.closest<HTMLElement>(".scene-badge, .scene-badge-add");
  if (!host || !element || !host.contains(element)) {
    return null;
  }
  if (element.classList.contains("scene-badge-add")) {
    return { element, toIndex: count - 1 };
  }
  const toIndex = Number.parseInt(element.dataset.sceneIndex ?? "", 10);
  return Number.isInteger(toIndex) ? { element, toIndex } : null;
}

/** The × is small and sits right beside the badge it removes, so it asks first. */
async function confirmRemove(sceneId: string, title: string, actions: SceneBadgeActions): Promise<void> {
  const confirmed = await showConfirm(`Remove the "${title}" scene from this preset?`, "Remove scene", { ok: "Remove" });
  if (confirmed) {
    actions.remove(sceneId);
  }
}

function buildAddBadge(alone: boolean, actions: SceneBadgeActions): HTMLElement {
  const add = document.createElement("button");
  add.type = "button";
  add.className = "scene-badge-add";
  add.title = "Add scene";
  add.setAttribute("aria-label", "Add scene");
  add.textContent = alone ? "+ Scene" : "+";
  add.addEventListener("click", () => actions.add());
  return add;
}

function beginRename(label: HTMLButtonElement, sceneId: string, title: string, actions: SceneBadgeActions): void {
  const field = document.createElement("input");
  field.type = "text";
  field.className = "scene-badge-input";
  field.value = title;
  field.maxLength = 80;
  field.size = Math.max(4, title.length);
  field.setAttribute("aria-label", "Scene name");

  let finished = false;
  const finish = (keep: boolean): void => {
    if (finished) {
      return;
    }
    finished = true;
    const next = field.value.trim();
    if (keep && next && next !== title) {
      label.textContent = next;
      actions.rename(sceneId, next);
    }
    // Swapping the field out blurs it, which calls back here; `finished` stops that.
    field.replaceWith(label);
  };

  field.addEventListener("input", () => {
    field.size = Math.max(4, field.value.length);
  });
  field.addEventListener("keydown", (event) => {
    // Typing a name must not reach the app's keyboard shortcuts.
    event.stopPropagation();
    if (event.key === "Enter" || event.key === "Escape") {
      event.preventDefault();
      finish(event.key === "Enter");
      label.focus();
    }
  });
  field.addEventListener("blur", () => finish(true));

  label.replaceWith(field);
  field.focus();
  field.select();
}
