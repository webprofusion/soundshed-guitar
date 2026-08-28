/**
 * Full signal-chain 3D stage: one WebGL context, on-demand frames,
 * selection focus, and knob/bypass picking across all units.
 */

import * as THREE from "three";
import { EffectComposer } from "three/examples/jsm/postprocessing/EffectComposer.js";
import { RenderPass } from "three/examples/jsm/postprocessing/RenderPass.js";
import { OutputPass } from "three/examples/jsm/postprocessing/OutputPass.js";
import { FXAAPass } from "three/examples/jsm/postprocessing/FXAAPass.js";

import { dragToValue, formatKnobValue } from "./ampLayout.js";
import { isWebglSupported } from "./ampSupport.js";
import { getAmp3dThemePreset, type Amp3dThemePreset } from "./ampTheme.js";
import { CameraDirector } from "./cameraDirector.js";
import type { BuildChainLayoutOptions } from "./chainTypes.js";
import { ChainScene } from "./chainScene.js";
import type { ThemeName } from "../theme-switcher.js";

export interface Chain3dViewOptions {
  theme: ThemeName;
  layoutOptions: BuildChainLayoutOptions;
  selectedNodeId: string | null;
  onSelectNode: (nodeId: string) => void;
  onParamChange: (nodeId: string, key: string, value: number) => void;
  onParamCommit?: (nodeId: string, key: string, value: number) => void;
  onBypassToggle: (nodeId: string) => void;
}

const MIN_ZOOM = 0.4;
const MAX_ZOOM = 2.2;
const MAX_AZIMUTH = 1.1;
const MIN_POLAR = -0.12;
const MAX_POLAR = 0.72;
const DEFAULT_POLAR = 0.28;
const PAN_PIXEL_SCALE = 0.002;
const MAX_PAN = 2.5;
const MAX_PIXEL_RATIO = 1.5;
const ANIMATION_FRAME_MS = 1000 / 30;

export function isWebglAvailable(): boolean {
  return isWebglSupported();
}

export class Chain3dView {
  readonly element: HTMLElement;

  private readonly canvas: HTMLCanvasElement;
  private readonly overlay: HTMLElement;
  private readonly status: HTMLElement;
  private readonly renderer: THREE.WebGLRenderer;
  private readonly composer: EffectComposer;
  private readonly renderPass: RenderPass;
  private readonly fxaaPass: FXAAPass;
  private readonly camera = new THREE.PerspectiveCamera(32, 1, 0.05, 80);
  private readonly raycaster = new THREE.Raycaster();
  private readonly pointer = new THREE.Vector2();
  private readonly resizeObserver: ResizeObserver;
  private readonly dummyControls = {
    target: new THREE.Vector3(),
    update: () => undefined,
  };

  private chainScene: ChainScene | null = null;
  private options: Chain3dViewOptions;
  private preset: Amp3dThemePreset;
  private structureSignature = "";
  private director: CameraDirector;

  private frameHandle = 0;
  private frameHandleIsTimeout = false;
  private disposed = false;
  private viewVisible = true;
  private pageVisible = typeof document === "undefined" || document.visibilityState !== "hidden";
  private dirty = true;
  private cameraDirty = true;
  private shadowsDirty = true;

  private cameraTarget = new THREE.Vector3();
  private focusCenter = new THREE.Vector3();
  private panOffset = new THREE.Vector3();
  private readonly panRight = new THREE.Vector3();
  private readonly panUp = new THREE.Vector3();
  private bottomInset = 0;
  private cameraDistance = 3.2;
  private azimuth = 0.15;
  private polar = DEFAULT_POLAR;
  private zoom = 1;

  private activeKnob: {
    nodeId: string;
    key: string;
    startValue: number;
    startY: number;
    pointerId: number;
  } | null = null;
  private orbitPointer: { id: number; x: number; y: number; moved: number } | null = null;
  private panPointer: { id: number; x: number; y: number } | null = null;
  private pendingFocusNodeId: string | null = null;
  private focusImmediate = true;
  private readonly startTime = typeof performance !== "undefined" ? performance.now() : Date.now();

  private readonly onVisibilityChange = (): void => {
    if (typeof document === "undefined") return;
    this.pageVisible = document.visibilityState !== "hidden";
    this.syncVisibility();
  };

  private constructor(container: HTMLElement, options: Chain3dViewOptions) {
    this.options = options;
    this.preset = getAmp3dThemePreset(options.theme);
    this.pendingFocusNodeId = options.selectedNodeId;

    this.element = document.createElement("div");
    this.element.className = "amp3d-view chain3d-view";

    this.canvas = document.createElement("canvas");
    this.canvas.className = "amp3d-canvas";
    this.canvas.setAttribute("role", "img");
    this.canvas.setAttribute(
      "aria-label",
      "Interactive 3D signal chain. Select a unit to focus, drag knobs to change parameters.",
    );
    this.element.appendChild(this.canvas);

    this.overlay = document.createElement("div");
    this.overlay.className = "amp3d-readout";
    this.overlay.hidden = true;
    this.element.appendChild(this.overlay);

    this.status = document.createElement("div");
    this.status.className = "amp3d-sr-only";
    this.status.setAttribute("aria-live", "polite");
    this.element.appendChild(this.status);

    container.appendChild(this.element);

    const pixelRatio = Math.min(window.devicePixelRatio || 1, MAX_PIXEL_RATIO);
    this.renderer = new THREE.WebGLRenderer({
      canvas: this.canvas,
      antialias: false,
      alpha: false,
      powerPreference: "low-power",
    });
    this.renderer.setPixelRatio(pixelRatio);
    this.renderer.shadowMap.enabled = true;
    this.renderer.shadowMap.type = THREE.PCFSoftShadowMap;
    this.renderer.shadowMap.autoUpdate = false;
    this.renderer.shadowMap.needsUpdate = true;
    this.renderer.toneMapping = THREE.ACESFilmicToneMapping;
    this.renderer.toneMappingExposure = this.preset.exposure;
    this.renderer.outputColorSpace = THREE.SRGBColorSpace;

    this.composer = new EffectComposer(this.renderer);
    this.renderPass = new RenderPass(new THREE.Scene(), this.camera);
    this.composer.addPass(this.renderPass);
    this.composer.addPass(new OutputPass());
    this.fxaaPass = new FXAAPass();
    this.composer.addPass(this.fxaaPass);

    this.director = new CameraDirector({
      camera: this.camera,
      controls: this.dummyControls as never,
      durationMs: 520,
    });

    this.resizeObserver = new ResizeObserver(() => this.handleResize());
    this.resizeObserver.observe(this.element);
    const dock = container.closest(".amp3d-stage")?.querySelector(".amp3d-dock");
    if (dock instanceof HTMLElement) {
      this.resizeObserver.observe(dock);
    }

    this.canvas.addEventListener("pointerdown", this.handlePointerDown);
    this.canvas.addEventListener("pointermove", this.handlePointerMove);
    this.canvas.addEventListener("pointerup", this.handlePointerUp);
    this.canvas.addEventListener("pointercancel", this.handlePointerUp);
    this.canvas.addEventListener("contextmenu", this.handleContextMenu);
    this.canvas.addEventListener("wheel", this.handleWheel, { passive: false });
    this.canvas.addEventListener("dblclick", this.handleDoubleClick);
    this.canvas.addEventListener("webglcontextlost", this.handleContextLost);
    if (typeof document !== "undefined") {
      document.addEventListener("visibilitychange", this.onVisibilityChange);
    }
  }

  static async create(container: HTMLElement, options: Chain3dViewOptions): Promise<Chain3dView> {
    const view = new Chain3dView(container, options);
    try {
      if (!isWebglSupported()) {
        throw new Error("WebGL is not available");
      }
      view.chainScene = await ChainScene.create({
        theme: options.theme,
        layoutOptions: options.layoutOptions,
        renderer: view.renderer,
      });
      view.structureSignature = view.chainScene.getStructureSignature();
      view.renderPass.scene = view.chainScene.scene;
      view.chainScene.setHighlightedNode(options.selectedNodeId);
      view.fitOverview(true);
      if (options.selectedNodeId) {
        view.focusNode(options.selectedNodeId, true);
      }
      view.handleResize();
      view.requestRender();
      return view;
    } catch (error) {
      view.dispose();
      throw error;
    }
  }

  async update(options: Chain3dViewOptions): Promise<void> {
    if (this.disposed || !this.chainScene) return;
    this.options = options;
    this.preset = getAmp3dThemePreset(options.theme);
    this.renderer.toneMappingExposure = this.preset.exposure;

    const rebuilt = await this.chainScene.update({
      theme: options.theme,
      layoutOptions: options.layoutOptions,
      renderer: this.renderer,
    });
    if (rebuilt) {
      this.structureSignature = this.chainScene.getStructureSignature();
      this.renderPass.scene = this.chainScene.scene;
      this.shadowsDirty = true;
      this.fitOverview(false);
    }
    this.chainScene.setHighlightedNode(options.selectedNodeId);
    if (options.selectedNodeId && options.selectedNodeId !== this.pendingFocusNodeId) {
      this.focusNode(options.selectedNodeId, false);
    } else {
      this.pendingFocusNodeId = options.selectedNodeId;
    }
    this.dirty = true;
    this.requestRender();
  }

  focusNode(nodeId: string, immediate = false): void {
    if (!this.chainScene) return;
    this.pendingFocusNodeId = nodeId;
    this.focusImmediate = immediate;
    this.chainScene.setHighlightedNode(nodeId);
    const anchor = this.chainScene.getFocusAnchorForNode(nodeId);
    if (!anchor) return;

    // Prefer FOV framing from world bounds so amp heads land close enough for knobs.
    const targetCenter = anchor.position.clone();
    let targetDistance = Math.max(0.75, anchor.fitDistance);
    if (anchor.bounds && !anchor.bounds.isEmpty()) {
      targetDistance = this.computeFitDistance(anchor.bounds, targetCenter);
    }
    // Reset orbit to a front-biased product angle; clear pan so the unit is centered.
    const targetAzimuth = 0.18;
    const targetPolar = DEFAULT_POLAR;
    const targetZoom = 1;

    this.panOffset.set(0, 0, 0);

    if (immediate) {
      this.focusCenter.copy(targetCenter);
      this.cameraTarget.copy(targetCenter);
      this.cameraDistance = targetDistance;
      this.azimuth = targetAzimuth;
      this.polar = targetPolar;
      this.zoom = targetZoom;
      this.director.focus({ position: targetCenter, fitDistance: targetDistance }, { immediate: true });
      this.cameraDirty = true;
      this.dirty = true;
      this.requestRender();
      return;
    }

    // Seed director with current orbit pose, then ease to the framed target.
    this.updateCamera();
    this.director.focus(
      {
        position: targetCenter,
        fitDistance: targetDistance,
      },
      { immediate: false },
    );
    // Snap orbit intent so when the ease finishes, wheel/drag stay coherent.
    this.focusCenter.copy(targetCenter);
    this.cameraTarget.copy(targetCenter);
    this.cameraDistance = targetDistance;
    this.azimuth = targetAzimuth;
    this.polar = targetPolar;
    this.zoom = targetZoom;
    this.dirty = true;
    this.requestRender();
  }

  /** FOV-based camera distance so the focused bounds fill the usable viewport. */
  private computeFitDistance(bounds: THREE.Box3, centerOut?: THREE.Vector3): number {
    const size = new THREE.Vector3();
    const center = new THREE.Vector3();
    bounds.getSize(size);
    bounds.getCenter(center);
    // Slight headroom above the unit (matches AmpView framing).
    const headroom = size.y * 0.16;
    size.y += headroom;
    center.y += headroom * 0.5;
    if (centerOut) centerOut.copy(center);

    this.bottomInset = this.measureBottomInset();
    const tanHalfFov = Math.tan(((this.camera.fov * Math.PI) / 180) / 2);
    const usableHeight = Math.max(0.45, 1 - this.bottomInset);
    const fitHeight = size.y / (2 * tanHalfFov * usableHeight);
    const fitWidth = size.x / (2 * tanHalfFov * Math.max(0.45, this.camera.aspect));
    return Math.max(0.7, Math.max(fitHeight, fitWidth) * 1.1 + size.z * 0.45);
  }

  private measureBottomInset(): number {
    const viewportHeight = this.element.clientHeight;
    if (viewportHeight <= 0) return 0;
    const dock = this.element.closest(".amp3d-stage")?.querySelector(".amp3d-dock");
    if (!(dock instanceof HTMLElement)) return 0;
    const stageBottom = this.element.getBoundingClientRect().bottom;
    const covered = stageBottom - dock.getBoundingClientRect().top;
    if (!Number.isFinite(covered) || covered <= 0) return 0;
    return Math.min(0.55, (covered / viewportHeight) * 1.15);
  }

  setVisible(visible: boolean): void {
    this.viewVisible = visible;
    this.syncVisibility();
  }

  dispose(): void {
    if (this.disposed) return;
    this.disposed = true;
    this.cancelFrame();
    this.resizeObserver.disconnect();
    this.canvas.removeEventListener("pointerdown", this.handlePointerDown);
    this.canvas.removeEventListener("pointermove", this.handlePointerMove);
    this.canvas.removeEventListener("pointerup", this.handlePointerUp);
    this.canvas.removeEventListener("pointercancel", this.handlePointerUp);
    this.canvas.removeEventListener("contextmenu", this.handleContextMenu);
    this.canvas.removeEventListener("wheel", this.handleWheel);
    this.canvas.removeEventListener("dblclick", this.handleDoubleClick);
    this.canvas.removeEventListener("webglcontextlost", this.handleContextLost);
    if (typeof document !== "undefined") {
      document.removeEventListener("visibilitychange", this.onVisibilityChange);
    }
    this.chainScene?.dispose();
    this.chainScene = null;
    this.composer.dispose();
    this.renderer.dispose();
    this.element.remove();
  }

  private fitOverview(immediate: boolean): void {
    if (!this.chainScene) return;
    const layout = this.chainScene.getLayout();
    const { minX, maxX, minZ, maxZ } = layout.bounds;
    const width = Math.max(0.8, maxX - minX + 1.2);
    const depth = Math.max(0.8, maxZ - minZ + 1.2);
    const cx = 0;
    const cz = 0;
    this.focusCenter.set(cx, 0.35, cz);
    this.cameraTarget.copy(this.focusCenter);
    this.cameraDistance = Math.max(2.2, Math.max(width, depth) * 1.15);
    this.azimuth = 0.15;
    this.polar = DEFAULT_POLAR;
    this.zoom = 1;
    this.panOffset.set(0, 0, 0);
    this.cameraDirty = true;
    this.dirty = true;
    if (!immediate && this.options.selectedNodeId) {
      this.focusNode(this.options.selectedNodeId, false);
    }
  }

  private syncVisibility(): void {
    if (this.viewVisible && this.pageVisible) {
      this.requestRender();
    } else {
      this.cancelFrame();
    }
  }

  private requestRender(): void {
    if (this.disposed || !this.viewVisible || !this.pageVisible) return;
    if (this.frameHandle) return;
    this.frameHandleIsTimeout = false;
    this.frameHandle = window.requestAnimationFrame(this.renderFrame);
  }

  private cancelFrame(): void {
    if (!this.frameHandle) return;
    if (this.frameHandleIsTimeout) {
      window.clearTimeout(this.frameHandle);
    } else {
      window.cancelAnimationFrame(this.frameHandle);
    }
    this.frameHandle = 0;
    this.frameHandleIsTimeout = false;
  }

  private readonly renderFrame = (): void => {
    this.frameHandle = 0;
    this.frameHandleIsTimeout = false;
    if (this.disposed || !this.chainScene) return;

    const now = typeof performance !== "undefined" ? performance.now() : Date.now();
    const elapsedSeconds = (now - this.startTime) / 1000;
    const unitsAnimating = this.chainScene.updateUnits(elapsedSeconds);

    const animating = this.director.update();
    if (animating) {
      // Pull orbit state from director while easing focus.
      this.focusCenter.copy(this.dummyControls.target);
      this.cameraTarget.copy(this.dummyControls.target);
      this.cameraDistance = Math.max(
        0.55,
        this.camera.position.distanceTo(this.cameraTarget) * Math.max(0.35, this.zoom),
      );
      // Derive azimuth/polar from eased camera so the next orbit drag is continuous.
      const offset = this.camera.position.clone().sub(this.cameraTarget);
      const flat = Math.hypot(offset.x, offset.z);
      if (flat > 1e-4) {
        this.azimuth = Math.atan2(offset.x, offset.z);
        this.polar = Math.atan2(offset.y - 0.15, flat);
      }
      this.cameraDirty = false;
      this.dirty = true;
    } else if (this.cameraDirty) {
      this.updateCamera();
      this.cameraDirty = false;
      this.dirty = true;
    }

    if (unitsAnimating) {
      this.dirty = true;
    }

    if (this.dirty) {
      if (this.shadowsDirty) {
        this.renderer.shadowMap.needsUpdate = true;
        this.shadowsDirty = false;
      }
      this.composer.render();
      this.dirty = false;
    }

    if (animating || this.activeKnob || unitsAnimating) {
      this.frameHandleIsTimeout = true;
      this.frameHandle = window.setTimeout(() => {
        this.frameHandle = 0;
        this.frameHandleIsTimeout = false;
        this.requestRender();
      }, ANIMATION_FRAME_MS) as unknown as number;
    }
  };

  private updateCamera(): void {
    const distance = this.cameraDistance / Math.max(0.35, this.zoom);
    const x = Math.sin(this.azimuth) * Math.cos(this.polar) * distance;
    const y = Math.sin(this.polar) * distance + 0.15;
    const z = Math.cos(this.azimuth) * Math.cos(this.polar) * distance;

    this.cameraTarget.copy(this.focusCenter).add(this.panOffset);
    // Raise target slightly so dock doesn't hide units.
    const insetLift = this.bottomInset * distance * 0.35;
    this.cameraTarget.y += insetLift * 0.25;

    this.camera.position.set(
      this.cameraTarget.x + x,
      this.cameraTarget.y + y,
      this.cameraTarget.z + z,
    );
    this.camera.lookAt(this.cameraTarget);
    this.dummyControls.target.copy(this.cameraTarget);
  }

  private handleResize = (): void => {
    if (this.disposed) return;
    const width = Math.max(1, this.element.clientWidth);
    const height = Math.max(1, this.element.clientHeight);
    this.camera.aspect = width / height;
    this.camera.updateProjectionMatrix();
    this.renderer.setSize(width, height, false);
    this.composer.setSize(width, height);
    this.fxaaPass.material.uniforms["resolution"].value.set(1 / width, 1 / height);

    const stage = this.element.closest(".amp3d-stage");
    const dock = stage?.querySelector(".amp3d-dock");
    if (dock instanceof HTMLElement && height > 0) {
      this.bottomInset = Math.min(0.35, (dock.getBoundingClientRect().height / height) * 0.6);
    } else {
      this.bottomInset = 0;
    }
    this.cameraDirty = true;
    this.dirty = true;
    this.requestRender();
  };

  private setPointerFromEvent(event: PointerEvent): void {
    const rect = this.canvas.getBoundingClientRect();
    const x = ((event.clientX - rect.left) / Math.max(1, rect.width)) * 2 - 1;
    const y = -(((event.clientY - rect.top) / Math.max(1, rect.height)) * 2 - 1);
    this.pointer.set(x, y);
  }

  private pick(): THREE.Intersection | null {
    if (!this.chainScene) return null;
    this.raycaster.setFromCamera(this.pointer, this.camera);
    const hits = this.raycaster.intersectObjects(this.chainScene.getPickables(), true);
    return hits[0] ?? null;
  }

  private readonly handlePointerDown = (event: PointerEvent): void => {
    if (this.disposed || !this.chainScene) return;
    this.setPointerFromEvent(event);
    const hit = this.pick();

    if (hit) {
      const obj = hit.object;
      const nodeId = typeof obj.userData.chainNodeId === "string" ? obj.userData.chainNodeId : "";
      const knobKey = typeof obj.userData.knobKey === "string" ? obj.userData.knobKey : "";
      if (knobKey && nodeId) {
        const unit = this.chainScene.getUnit(nodeId);
        const spec = unit?.getKnobSpec(knobKey, nodeId);
        if (spec) {
          this.activeKnob = {
            nodeId,
            key: knobKey,
            startValue: spec.value,
            startY: event.clientY,
            pointerId: event.pointerId,
          };
          this.canvas.setPointerCapture(event.pointerId);
          this.showReadout(spec.label, formatKnobValue(spec.value, spec.unit));
          event.preventDefault();
          return;
        }
      }
      if (obj.userData.bypassTarget && nodeId) {
        this.options.onBypassToggle(nodeId);
        event.preventDefault();
        return;
      }
      if (nodeId) {
        this.options.onSelectNode(nodeId);
        this.focusNode(nodeId, false);
        event.preventDefault();
        // still allow orbit if user drags
      }
    }

    if (event.button === 1 || event.button === 2 || event.shiftKey) {
      this.panPointer = { id: event.pointerId, x: event.clientX, y: event.clientY };
      this.canvas.setPointerCapture(event.pointerId);
      event.preventDefault();
      return;
    }

    this.orbitPointer = { id: event.pointerId, x: event.clientX, y: event.clientY, moved: 0 };
    this.canvas.setPointerCapture(event.pointerId);
    event.preventDefault();
  };

  private readonly handlePointerMove = (event: PointerEvent): void => {
    if (this.disposed) return;

    if (this.activeKnob && this.activeKnob.pointerId === event.pointerId && this.chainScene) {
      const unit = this.chainScene.getUnit(this.activeKnob.nodeId);
      const spec = unit?.getKnobSpec(this.activeKnob.key, this.activeKnob.nodeId);
      if (!spec) return;
      const next = dragToValue({
        startValue: this.activeKnob.startValue,
        deltaPixels: this.activeKnob.startY - event.clientY,
        min: spec.min,
        max: spec.max,
        step: spec.step,
        fine: event.shiftKey,
      });
      unit?.setKnobValue(this.activeKnob.key, next, this.activeKnob.nodeId);
      this.options.onParamChange(this.activeKnob.nodeId, this.activeKnob.key, next);
      this.showReadout(spec.label, formatKnobValue(next, spec.unit));
      this.dirty = true;
      this.requestRender();
      event.preventDefault();
      return;
    }

    if (this.panPointer && this.panPointer.id === event.pointerId) {
      const dx = event.clientX - this.panPointer.x;
      const dy = event.clientY - this.panPointer.y;
      this.panPointer.x = event.clientX;
      this.panPointer.y = event.clientY;
      this.camera.getWorldDirection(this.panUp).set(0, 1, 0);
      this.panRight.set(1, 0, 0).applyQuaternion(this.camera.quaternion).normalize();
      this.panUp.set(0, 1, 0).applyQuaternion(this.camera.quaternion).normalize();
      this.panOffset.addScaledVector(this.panRight, -dx * PAN_PIXEL_SCALE * this.cameraDistance);
      this.panOffset.addScaledVector(this.panUp, dy * PAN_PIXEL_SCALE * this.cameraDistance);
      this.panOffset.clampLength(0, MAX_PAN);
      this.cameraDirty = true;
      this.dirty = true;
      this.requestRender();
      event.preventDefault();
      return;
    }

    if (this.orbitPointer && this.orbitPointer.id === event.pointerId) {
      const dx = event.clientX - this.orbitPointer.x;
      const dy = event.clientY - this.orbitPointer.y;
      this.orbitPointer.x = event.clientX;
      this.orbitPointer.y = event.clientY;
      this.orbitPointer.moved += Math.abs(dx) + Math.abs(dy);
      this.azimuth = THREE.MathUtils.clamp(this.azimuth - dx * 0.005, -MAX_AZIMUTH, MAX_AZIMUTH);
      this.polar = THREE.MathUtils.clamp(this.polar + dy * 0.004, MIN_POLAR, MAX_POLAR);
      this.cameraDirty = true;
      this.dirty = true;
      this.requestRender();
      event.preventDefault();
    }
  };

  private readonly handlePointerUp = (event: PointerEvent): void => {
    if (this.activeKnob && this.activeKnob.pointerId === event.pointerId) {
      const { nodeId, key } = this.activeKnob;
      const unit = this.chainScene?.getUnit(nodeId);
      const spec = unit?.getKnobSpec(key, nodeId);
      if (spec) {
        this.options.onParamCommit?.(nodeId, key, spec.value);
      }
      this.activeKnob = null;
      this.hideReadout();
      try {
        this.canvas.releasePointerCapture(event.pointerId);
      } catch {
        /* ignore */
      }
      return;
    }
    if (this.orbitPointer?.id === event.pointerId) {
      this.orbitPointer = null;
      try {
        this.canvas.releasePointerCapture(event.pointerId);
      } catch {
        /* ignore */
      }
    }
    if (this.panPointer?.id === event.pointerId) {
      this.panPointer = null;
      try {
        this.canvas.releasePointerCapture(event.pointerId);
      } catch {
        /* ignore */
      }
    }
  };

  private readonly handleWheel = (event: WheelEvent): void => {
    event.preventDefault();
    const factor = event.deltaY > 0 ? 1.08 : 0.92;
    this.zoom = THREE.MathUtils.clamp(this.zoom * factor, MIN_ZOOM, MAX_ZOOM);
    this.cameraDirty = true;
    this.dirty = true;
    this.requestRender();
  };

  private readonly handleDoubleClick = (): void => {
    if (this.options.selectedNodeId) {
      this.focusNode(this.options.selectedNodeId, false);
    } else {
      this.fitOverview(false);
      this.requestRender();
    }
  };

  private readonly handleContextMenu = (event: Event): void => {
    event.preventDefault();
  };

  private readonly handleContextLost = (event: Event): void => {
    event.preventDefault();
    this.status.textContent = "3D view lost the graphics context. Switch views to reload.";
  };

  private showReadout(label: string, value: string): void {
    this.overlay.hidden = false;
    this.overlay.textContent = `${label}: ${value}`;
    this.status.textContent = `${label} ${value}`;
  }

  private hideReadout(): void {
    this.overlay.hidden = true;
  }
}

