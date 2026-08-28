/**
 * Photo-quality 19" rack chassis for standard effects.
 * Fixed 12U tower: signal-order FX fill top → bottom; empty U spaces are blank plates.
 */

import * as THREE from "three";
import { valueToRotationRad } from "../ampLayout.js";
import { loadAmpComponent, type Amp3dKnobSpec } from "../ampScene.js";
import type { Amp3dThemePreset } from "../ampTheme.js";
import type { ChainRackSlotDesc, ChainUnitDesc } from "../chainTypes.js";
import type { ChainUnit, ChainUnitFocusAnchor } from "./chainUnit.js";
import { categoryAccent } from "./unitCommon.js";

/** Must stay in sync with chainLayout RACK_U_HEIGHT / RACK_TOTAL_U. */
export const RACK_U_HEIGHT = 0.052;
export const RACK_TOTAL_U = 12;
/** Inner 19" bay width (faceplate span between rack ears). */
const RACK_WIDTH = 0.52;
const EAR_WIDTH = 0.028;
const FACE_DEPTH = 0.01;
const MAX_FACE_KNOBS = 6;
/** Amp knob mesh is ~26mm across; scale to sit cleanly on a 1U face. */
const KNOB_SCALE = 0.7;

/** Flight case outer shell proportions (meters). */
const CASE_WALL = 0.016;
const CASE_EXTRUSION = 0.013;
const CASE_INNER_DEPTH = 0.34;
const CASE_FRONT_RECESS = 0.012;
const CASE_CASTER_H = 0.048;
const CASE_TOP_LIP = 0.02;
const CASE_BOTTOM_LIP = 0.018;

interface SlotRuntime {
  desc: ChainRackSlotDesc;
  group: THREE.Group;
  face: THREE.Mesh;
  faceMaterial: THREE.MeshStandardMaterial;
  led: THREE.Mesh;
  knobs: Array<{
    spec: Amp3dKnobSpec;
    root: THREE.Group;
    meshes: THREE.Object3D[];
  }>;
  pickMeshes: THREE.Object3D[];
  bypassed: boolean;
}

function canvasTexture(
  source: HTMLCanvasElement,
  opts?: { srgb?: boolean; anisotropy?: number },
): THREE.CanvasTexture {
  const texture = new THREE.CanvasTexture(source);
  texture.colorSpace = opts?.srgb ? THREE.SRGBColorSpace : THREE.NoColorSpace;
  texture.anisotropy = opts?.anisotropy ?? 4;
  texture.needsUpdate = true;
  return texture;
}

function isGuidLike(text: string): boolean {
  const t = text.trim();
  if (!t) return true;
  if (/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i.test(t)) return true;
  // Truncated / partial ids as seen on faceplates
  if (/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}/i.test(t) && t.length >= 20) return true;
  return false;
}

function humanizeToken(value: string): string {
  return value.replace(/[_-]+/g, " ").replace(/\s+/g, " ").trim();
}

/** Prefer the effect title; never show raw node ids/GUIDs or file paths. */
function faceTitle(slot: ChainRackSlotDesc): string {
  const candidates = [slot.label, slot.displayText, slot.category, slot.effectType];
  for (const raw of candidates) {
    const text = humanizeToken(raw || "");
    if (!text || isGuidLike(text) || text === slot.nodeId) continue;
    // Skip resource-path style strings for the unit title.
    if (/[\\/]/.test(text) || /\.(nam|wav|json|ir)$/i.test(text)) continue;
    return text;
  }
  return "FX";
}

function knobCaption(spec: Amp3dKnobSpec): string {
  const raw = humanizeToken(spec.label || spec.key || "");
  if (!raw || isGuidLike(raw)) return "PARAM";
  return raw.toUpperCase().slice(0, 10);
}

/**
 * Shared knob X layout in face-local meters (center origin).
 * Title lives on the left; knobs occupy the mid band; LED/vent on the right.
 */
function faceKnobXs(count: number, faceWidth: number): number[] {
  if (count <= 0) return [];
  const left = -faceWidth * 0.06;
  const right = faceWidth * 0.30;
  if (count === 1) return [(left + right) * 0.5];
  const span = right - left;
  return Array.from({ length: count }, (_, i) => left + (span * i) / (count - 1));
}

/** Same layout in canvas UV space (0–1 across the faceplate). */
function faceKnobUs(count: number): number[] {
  if (count <= 0) return [];
  const left = 0.38;
  const right = 0.78;
  if (count === 1) return [(left + right) * 0.5];
  const span = right - left;
  return Array.from({ length: count }, (_, i) => left + (span * i) / (count - 1));
}

/** Subtle flat-black metal noise (not brushed streaks). */
function makeBlackMetalMaps(size = 256): {
  color: HTMLCanvasElement;
  roughness: HTMLCanvasElement;
} {
  const color = document.createElement("canvas");
  color.width = size;
  color.height = size;
  const roughness = document.createElement("canvas");
  roughness.width = size;
  roughness.height = size;
  const cctx = color.getContext("2d");
  const rctx = roughness.getContext("2d");
  if (!cctx || !rctx) return { color, roughness };

  cctx.fillStyle = "#141618";
  cctx.fillRect(0, 0, size, size);
  rctx.fillStyle = "#8a8a8a";
  rctx.fillRect(0, 0, size, size);

  for (let i = 0; i < size * size * 0.35; i += 1) {
    const x = (Math.random() * size) | 0;
    const y = (Math.random() * size) | 0;
    const v = (Math.random() * 18) | 0;
    const a = 0.04 + Math.random() * 0.08;
    cctx.fillStyle = `rgba(${20 + v},${22 + v},${24 + v},${a})`;
    cctx.fillRect(x, y, 1, 1);
    const rv = 110 + ((Math.random() * 50) | 0);
    rctx.fillStyle = `rgb(${rv},${rv},${rv})`;
    rctx.fillRect(x, y, 1, 1);
  }

  // Soft vignette so edges read slightly darker (rack chassis depth).
  const g = cctx.createRadialGradient(size * 0.5, size * 0.5, size * 0.15, size * 0.5, size * 0.5, size * 0.7);
  g.addColorStop(0, "rgba(255,255,255,0.03)");
  g.addColorStop(1, "rgba(0,0,0,0.22)");
  cctx.fillStyle = g;
  cctx.fillRect(0, 0, size, size);

  return { color, roughness };
}

function makeVentAlpha(size = 128): HTMLCanvasElement {
  const canvas = document.createElement("canvas");
  canvas.width = size;
  canvas.height = size;
  const ctx = canvas.getContext("2d");
  if (!ctx) return canvas;
  ctx.fillStyle = "#000";
  ctx.fillRect(0, 0, size, size);
  ctx.fillStyle = "#fff";
  const cols = 16;
  const rows = 5;
  const holeW = (size / cols) * 0.5;
  const holeH = (size / rows) * 0.32;
  for (let row = 0; row < rows; row += 1) {
    for (let col = 0; col < cols; col += 1) {
      const x = (col + 0.5) * (size / cols) - holeW / 2;
      const y = (row + 0.5) * (size / rows) - holeH / 2;
      ctx.fillRect(x, y, holeW, holeH);
    }
  }
  return canvas;
}

function makeScrewCanvas(): HTMLCanvasElement {
  const canvas = document.createElement("canvas");
  canvas.width = 64;
  canvas.height = 64;
  const ctx = canvas.getContext("2d");
  if (!ctx) return canvas;
  const g = ctx.createRadialGradient(28, 26, 4, 32, 32, 30);
  g.addColorStop(0, "#eef1f4");
  g.addColorStop(0.5, "#aeb6bf");
  g.addColorStop(1, "#5a616a");
  ctx.fillStyle = g;
  ctx.beginPath();
  ctx.arc(32, 32, 28, 0, Math.PI * 2);
  ctx.fill();
  ctx.strokeStyle = "rgba(0,0,0,0.4)";
  ctx.lineWidth = 3;
  ctx.beginPath();
  ctx.moveTo(18, 32);
  ctx.lineTo(46, 32);
  ctx.moveTo(32, 18);
  ctx.lineTo(32, 46);
  ctx.stroke();
  return canvas;
}

/**
 * Opaque faceplate albedo: flat black metal + baked title/knob silkscreen.
 * Baked into the face mesh (not a transparent overlay) so labels stay visible.
 */
function makeFaceAlbedoCanvas(slot: ChainRackSlotDesc, accent: number): HTMLCanvasElement {
  const width = 1024;
  const height = 160;
  const canvas = document.createElement("canvas");
  canvas.width = width;
  canvas.height = height;
  const ctx = canvas.getContext("2d");
  if (!ctx) return canvas;

  // Base flat black metal
  ctx.fillStyle = slot.blank ? "#0b0d0f" : "#121518";
  ctx.fillRect(0, 0, width, height);

  // Subtle noise
  for (let i = 0; i < 1800; i += 1) {
    const x = (Math.random() * width) | 0;
    const y = (Math.random() * height) | 0;
    const v = 14 + ((Math.random() * 22) | 0);
    ctx.fillStyle = `rgba(${v},${v + 1},${v + 2},${0.04 + Math.random() * 0.07})`;
    ctx.fillRect(x, y, 1, 1);
  }

  // Soft top/bottom lip
  const edge = ctx.createLinearGradient(0, 0, 0, height);
  edge.addColorStop(0, "rgba(255,255,255,0.10)");
  edge.addColorStop(0.4, "rgba(255,255,255,0)");
  edge.addColorStop(1, "rgba(0,0,0,0.35)");
  ctx.fillStyle = edge;
  ctx.fillRect(0, 0, width, height);

  if (slot.blank) {
    ctx.strokeStyle = "rgba(255,255,255,0.07)";
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(48, height * 0.5);
    ctx.lineTo(width - 48, height * 0.5);
    ctx.stroke();
    return canvas;
  }

  // Category accent bar
  const accentColor = `#${new THREE.Color(accent).getHexString()}`;
  ctx.fillStyle = accentColor;
  ctx.fillRect(0, 8, 10, height - 16);
  ctx.fillStyle = "rgba(255,255,255,0.18)";
  ctx.fillRect(10, 8, 2, height - 16);

  const title = faceTitle(slot).toUpperCase();
  ctx.textAlign = "left";
  ctx.textBaseline = "middle";

  // High-contrast title badge
  const titleW = Math.min(380, 48 + title.length * 16);
  ctx.fillStyle = "#0a0c0e";
  ctx.fillRect(22, 16, titleW, 52);
  ctx.strokeStyle = accentColor;
  ctx.lineWidth = 2;
  ctx.strokeRect(23, 17, titleW - 2, 50);
  ctx.fillStyle = "#ffffff";
  ctx.font = "800 30px system-ui, Segoe UI, sans-serif";
  ctx.fillText(title.slice(0, 22), 34, 43);

  // Knob silkscreen — aligned with faceKnobUs / faceKnobXs
  const knobs = slot.knobs.slice(0, MAX_FACE_KNOBS);
  if (knobs.length) {
    const us = faceKnobUs(knobs.length);
    knobs.forEach((knob, i) => {
      const x = us[i]! * width;
      const y = 68;
      ctx.strokeStyle = "rgba(255,255,255,0.35)";
      ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.arc(x, y, 20, Math.PI * 0.75, Math.PI * 2.25);
      ctx.stroke();
      ctx.beginPath();
      ctx.moveTo(x, y - 20);
      ctx.lineTo(x, y - 13);
      ctx.stroke();

      const caption = knobCaption(knob);
      ctx.font = "800 13px system-ui, Segoe UI, sans-serif";
      ctx.textAlign = "center";
      const tw = Math.max(42, caption.length * 8.2);
      ctx.fillStyle = "#0a0c0e";
      ctx.fillRect(x - tw / 2, 106, tw, 22);
      ctx.strokeStyle = "rgba(255,255,255,0.22)";
      ctx.lineWidth = 1;
      ctx.strokeRect(x - tw / 2 + 0.5, 106.5, tw - 1, 21);
      ctx.fillStyle = "#f2f5f8";
      ctx.fillText(caption, x, 118);
    });
  }

  // LED recess mark
  ctx.fillStyle = "#060708";
  ctx.fillRect(width - 72, 26, 32, 18);
  ctx.strokeStyle = "rgba(255,255,255,0.12)";
  ctx.strokeRect(width - 71.5, 26.5, 31, 17);

  return canvas;
}

function applyAmpKnobMaterials(
  object: THREE.Object3D,
  mats: {
    body: THREE.MeshStandardMaterial;
    cap: THREE.MeshStandardMaterial;
    pointer: THREE.MeshStandardMaterial;
  },
): THREE.Object3D[] {
  const meshes: THREE.Object3D[] = [];
  object.traverse((child) => {
    const mesh = child as THREE.Mesh;
    if (!mesh.isMesh) return;
    const slot =
      (typeof mesh.userData?.materialSlot === "string" && mesh.userData.materialSlot)
      || (typeof mesh.parent?.userData?.materialSlot === "string" && mesh.parent.userData.materialSlot)
      || (Array.isArray(mesh.material) ? mesh.material[0]?.name : mesh.material?.name)
      || "";
    const name = `${slot} ${mesh.name}`.toLowerCase();
    if (name.includes("pointer")) {
      mesh.material = mats.pointer;
    } else if (name.includes("cap")) {
      mesh.material = mats.cap;
    } else {
      mesh.material = mats.body;
    }
    mesh.castShadow = false;
    mesh.receiveShadow = true;
    meshes.push(mesh);
  });
  return meshes;
}

function mountAmpKnob(
  template: THREE.Group,
  spec: Amp3dKnobSpec,
  mats: {
    body: THREE.MeshStandardMaterial;
    cap: THREE.MeshStandardMaterial;
    pointer: THREE.MeshStandardMaterial;
  },
): { root: THREE.Group; meshes: THREE.Object3D[]; spec: Amp3dKnobSpec } {
  const root = new THREE.Group();
  root.name = `RackKnob:${spec.key}`;
  // Amp knob glTF already faces +Z (shaft along Z) — same as rack faceplates.
  const knob = template.clone(true);
  applyAmpKnobMaterials(knob, mats);
  root.add(knob);
  root.scale.setScalar(KNOB_SCALE);
  root.rotation.z = -valueToRotationRad(spec.value, spec.min, spec.max);
  const meshes: THREE.Object3D[] = [];
  root.traverse((child) => {
    if ((child as THREE.Mesh).isMesh) {
      child.userData.knobKey = spec.key;
      meshes.push(child);
    }
  });
  return { root, meshes, spec: { ...spec } };
}

function resolveEffectSlots(desc: ChainUnitDesc): ChainRackSlotDesc[] {
  if (desc.stack?.length) return desc.stack.filter((s) => !s.blank);
  return [{
    nodeId: desc.nodeId,
    effectType: desc.effectType,
    label: desc.label,
    category: desc.category,
    bypassed: desc.bypassed,
    knobs: desc.knobs,
    displayText: desc.displayText,
    resources: desc.resources,
  }];
}

/** Pad to a full chassis: real FX top→bottom, blanks below. */
function expandChassisSlots(
  effects: ChainRackSlotDesc[],
  totalU: number,
): ChainRackSlotDesc[] {
  const u = Math.max(1, totalU);
  const real = effects.slice(0, u);
  const out: ChainRackSlotDesc[] = [...real];
  for (let i = real.length; i < u; i += 1) {
    out.push({
      nodeId: `__blank_${i}`,
      effectType: "blank",
      label: "",
      category: "",
      bypassed: false,
      knobs: [],
      displayText: "",
      resources: [],
      blank: true,
    });
  }
  return out;
}

/** Y of bay floor (top of case bottom panel, above casters). */
function bayFloorY(): number {
  return CASE_CASTER_H + CASE_BOTTOM_LIP;
}

/** Slot index 0 = top of rack (inside flight-case bay). */
function slotCenterY(indexFromTop: number, totalU: number, floorY: number): number {
  const indexFromBottom = Math.max(0, totalU - 1 - indexFromTop);
  return floorY + 0.004 + indexFromBottom * RACK_U_HEIGHT + RACK_U_HEIGHT * 0.5;
}

function makeLaminateCanvas(size = 256): HTMLCanvasElement {
  const canvas = document.createElement("canvas");
  canvas.width = size;
  canvas.height = size;
  const ctx = canvas.getContext("2d");
  if (!ctx) return canvas;
  ctx.fillStyle = "#0e1013";
  ctx.fillRect(0, 0, size, size);
  // Fine honeycomb / hex laminate hint
  ctx.strokeStyle = "rgba(255,255,255,0.035)";
  ctx.lineWidth = 1;
  const step = 10;
  for (let y = 0; y < size + step; y += step) {
    for (let x = 0; x < size + step; x += step) {
      const ox = (Math.floor(y / step) % 2) * (step * 0.5);
      ctx.strokeRect(x + ox, y, step * 0.85, step * 0.75);
    }
  }
  for (let i = 0; i < 900; i += 1) {
    const x = (Math.random() * size) | 0;
    const y = (Math.random() * size) | 0;
    const v = (Math.random() * 20) | 0;
    ctx.fillStyle = `rgba(${v},${v},${v + 2},0.05)`;
    ctx.fillRect(x, y, 1, 1);
  }
  return canvas;
}

function makeRailHoleCanvas(): HTMLCanvasElement {
  const canvas = document.createElement("canvas");
  canvas.width = 64;
  canvas.height = 256;
  const ctx = canvas.getContext("2d");
  if (!ctx) return canvas;
  ctx.fillStyle = "#15181c";
  ctx.fillRect(0, 0, 64, 256);
  ctx.fillStyle = "#050607";
  const pitch = 256 / 12; // ~1U spacing visual
  for (let u = 0; u < 12; u += 1) {
    const y0 = u * pitch;
    [0.28, 0.72].forEach((t) => {
      ctx.beginPath();
      ctx.arc(32, y0 + pitch * t, 4.2, 0, Math.PI * 2);
      ctx.fill();
      ctx.strokeStyle = "rgba(255,255,255,0.08)";
      ctx.stroke();
    });
  }
  return canvas;
}

function makeButterflyLatchCanvas(): HTMLCanvasElement {
  const canvas = document.createElement("canvas");
  canvas.width = 128;
  canvas.height = 128;
  const ctx = canvas.getContext("2d");
  if (!ctx) return canvas;
  ctx.fillStyle = "#2a3038";
  ctx.fillRect(0, 0, 128, 128);
  // Recess
  ctx.fillStyle = "#12151a";
  ctx.fillRect(14, 22, 100, 84);
  // Butterfly wings
  const metal = ctx.createLinearGradient(20, 20, 108, 108);
  metal.addColorStop(0, "#dfe5ec");
  metal.addColorStop(0.45, "#9aa3ad");
  metal.addColorStop(1, "#6a727c");
  ctx.fillStyle = metal;
  ctx.beginPath();
  ctx.moveTo(28, 64);
  ctx.quadraticCurveTo(48, 30, 64, 36);
  ctx.quadraticCurveTo(80, 30, 100, 64);
  ctx.quadraticCurveTo(80, 98, 64, 92);
  ctx.quadraticCurveTo(48, 98, 28, 64);
  ctx.fill();
  ctx.fillStyle = "#3a4048";
  ctx.beginPath();
  ctx.arc(64, 64, 10, 0, Math.PI * 2);
  ctx.fill();
  ctx.strokeStyle = "rgba(255,255,255,0.35)";
  ctx.lineWidth = 2;
  ctx.beginPath();
  ctx.moveTo(58, 64);
  ctx.lineTo(70, 64);
  ctx.stroke();
  return canvas;
}

interface CaseBuildCtx {
  root: THREE.Group;
  geometries: Set<THREE.BufferGeometry>;
  textures: THREE.Texture[];
  materials: THREE.Material[];
  nodeId: string;
  bayHeight: number;
  bayFloor: number;
  outerW: number;
  outerD: number;
  outerH: number;
  caseCenterY: number;
  faceZ: number;
}

function addFlightCaseShell(ctx: CaseBuildCtx): void {
  const {
    root, geometries, textures, materials, nodeId,
    bayHeight, bayFloor, outerW, outerD, caseCenterY, faceZ,
  } = ctx;

  const laminateMap = canvasTexture(makeLaminateCanvas(256), { srgb: true, anisotropy: 6 });
  laminateMap.wrapS = laminateMap.wrapT = THREE.RepeatWrapping;
  laminateMap.repeat.set(2.4, 2.8);
  textures.push(laminateMap);

  const laminateMat = new THREE.MeshStandardMaterial({
    color: 0x121418,
    map: laminateMap,
    roughness: 0.72,
    metalness: 0.08,
    envMapIntensity: 0.45,
  });
  materials.push(laminateMat);

  const alumMat = new THREE.MeshStandardMaterial({
    color: 0xc5ccd4,
    roughness: 0.28,
    metalness: 0.96,
    envMapIntensity: 1.2,
  });
  materials.push(alumMat);

  const alumDarkMat = new THREE.MeshStandardMaterial({
    color: 0x8a929c,
    roughness: 0.36,
    metalness: 0.92,
    envMapIntensity: 1.05,
  });
  materials.push(alumDarkMat);

  const railMap = canvasTexture(makeRailHoleCanvas(), { srgb: true, anisotropy: 4 });
  railMap.wrapS = THREE.ClampToEdgeWrapping;
  railMap.wrapT = THREE.RepeatWrapping;
  railMap.repeat.set(1, Math.max(1, bayHeight / (RACK_U_HEIGHT * 12)));
  textures.push(railMap);
  const railMat = new THREE.MeshStandardMaterial({
    color: 0xffffff,
    map: railMap,
    roughness: 0.55,
    metalness: 0.55,
    envMapIntensity: 0.7,
  });
  materials.push(railMat);

  const latchMap = canvasTexture(makeButterflyLatchCanvas(), { srgb: true, anisotropy: 4 });
  textures.push(latchMap);
  const latchMat = new THREE.MeshStandardMaterial({
    map: latchMap,
    roughness: 0.4,
    metalness: 0.75,
    envMapIntensity: 1.0,
  });
  materials.push(latchMat);

  const wheelMat = new THREE.MeshStandardMaterial({
    color: 0x1a3a7a,
    roughness: 0.55,
    metalness: 0.15,
    envMapIntensity: 0.4,
  });
  materials.push(wheelMat);
  const hubMat = new THREE.MeshStandardMaterial({
    color: 0xb0b6be,
    roughness: 0.3,
    metalness: 0.9,
    envMapIntensity: 1.1,
  });
  materials.push(hubMat);
  const interiorMat = new THREE.MeshStandardMaterial({
    color: 0x0a0c0f,
    roughness: 0.85,
    metalness: 0.05,
    envMapIntensity: 0.25,
  });
  materials.push(interiorMat);

  const tag = (mesh: THREE.Object3D) => {
    mesh.userData.chainNodeId = nodeId;
  };

  // --- Outer laminate panels (open front) ---
  const sideThickness = CASE_WALL;
  const backThickness = CASE_WALL;
  const topThickness = CASE_TOP_LIP;
  const bottomThickness = CASE_BOTTOM_LIP;
  const innerW = RACK_WIDTH;
  const sideX = (innerW / 2) + sideThickness / 2;

  // Left / right walls
  {
    const geo = new THREE.BoxGeometry(sideThickness, bayHeight + topThickness + bottomThickness * 0.5, outerD - CASE_EXTRUSION * 0.5);
    geometries.add(geo);
    [-1, 1].forEach((side) => {
      const wall = new THREE.Mesh(geo, laminateMat);
      wall.castShadow = true;
      wall.receiveShadow = true;
      wall.position.set(side * sideX, caseCenterY, -CASE_FRONT_RECESS * 0.25);
      tag(wall);
      root.add(wall);
    });
  }

  // Back wall
  {
    const geo = new THREE.BoxGeometry(innerW + sideThickness * 2, bayHeight + topThickness, backThickness);
    geometries.add(geo);
    const back = new THREE.Mesh(geo, laminateMat);
    back.castShadow = true;
    back.receiveShadow = true;
    back.position.set(0, caseCenterY + topThickness * 0.15, -outerD / 2 + backThickness / 2);
    tag(back);
    root.add(back);
  }

  // Top panel
  {
    const geo = new THREE.BoxGeometry(outerW - CASE_EXTRUSION * 0.4, topThickness, outerD - CASE_EXTRUSION * 0.3);
    geometries.add(geo);
    const top = new THREE.Mesh(geo, laminateMat);
    top.castShadow = true;
    top.receiveShadow = true;
    top.position.set(0, bayFloor + bayHeight + topThickness / 2, -CASE_FRONT_RECESS * 0.2);
    tag(top);
    root.add(top);
  }

  // Bottom panel / floor of bay
  {
    const geo = new THREE.BoxGeometry(outerW - CASE_EXTRUSION * 0.4, bottomThickness, outerD - CASE_EXTRUSION * 0.3);
    geometries.add(geo);
    const bottom = new THREE.Mesh(geo, laminateMat);
    bottom.castShadow = true;
    bottom.receiveShadow = true;
    bottom.position.set(0, bayFloor - bottomThickness / 2, -CASE_FRONT_RECESS * 0.2);
    tag(bottom);
    root.add(bottom);
  }

  // Interior rear + floor lining
  {
    const rearGeo = new THREE.BoxGeometry(innerW * 0.98, bayHeight * 0.98, 0.006);
    geometries.add(rearGeo);
    const rear = new THREE.Mesh(rearGeo, interiorMat);
    rear.position.set(0, bayFloor + bayHeight / 2, -CASE_INNER_DEPTH / 2 + 0.02);
    tag(rear);
    root.add(rear);
  }

  // --- Aluminum edge extrusions (flight-case ball corners + frame) ---
  const ext = CASE_EXTRUSION;
  const frameDepth = outerD;
  const frameHeight = bayHeight + topThickness + bottomThickness;
  const frameY = bayFloor - bottomThickness + frameHeight / 2;

  // Vertical front corner posts
  {
    const geo = new THREE.BoxGeometry(ext, frameHeight, ext);
    geometries.add(geo);
    [-1, 1].forEach((sx) => {
      const post = new THREE.Mesh(geo, alumMat);
      post.castShadow = true;
      post.position.set(sx * (outerW / 2 - ext / 2), frameY, faceZ - ext * 0.15);
      tag(post);
      root.add(post);
    });
  }
  // Vertical rear corner posts
  {
    const geo = new THREE.BoxGeometry(ext, frameHeight, ext);
    geometries.add(geo);
    [-1, 1].forEach((sx) => {
      const post = new THREE.Mesh(geo, alumDarkMat);
      post.castShadow = true;
      post.position.set(sx * (outerW / 2 - ext / 2), frameY, -outerD / 2 + ext / 2);
      tag(post);
      root.add(post);
    });
  }
  // Horizontal front rails (top + bottom of opening)
  {
    const geo = new THREE.BoxGeometry(outerW - ext * 1.2, ext * 0.85, ext);
    geometries.add(geo);
    const topRail = new THREE.Mesh(geo, alumMat);
    topRail.position.set(0, bayFloor + bayHeight + ext * 0.1, faceZ - ext * 0.15);
    tag(topRail);
    root.add(topRail);
    const botRail = new THREE.Mesh(geo, alumMat);
    botRail.position.set(0, bayFloor - ext * 0.15, faceZ - ext * 0.15);
    tag(botRail);
    root.add(botRail);
  }
  // Top perimeter edges
  {
    const sideGeo = new THREE.BoxGeometry(ext * 0.9, ext * 0.7, frameDepth - ext);
    geometries.add(sideGeo);
    [-1, 1].forEach((sx) => {
      const edge = new THREE.Mesh(sideGeo, alumMat);
      edge.position.set(sx * (outerW / 2 - ext / 2), bayFloor + bayHeight + topThickness - ext * 0.2, -CASE_FRONT_RECESS * 0.2);
      tag(edge);
      root.add(edge);
    });
    const frontTop = new THREE.BoxGeometry(outerW - ext, ext * 0.7, ext * 0.9);
    geometries.add(frontTop);
    const ft = new THREE.Mesh(frontTop, alumMat);
    ft.position.set(0, bayFloor + bayHeight + topThickness - ext * 0.2, faceZ - ext * 0.2);
    tag(ft);
    root.add(ft);
    const backTop = new THREE.Mesh(frontTop, alumDarkMat);
    backTop.position.set(0, bayFloor + bayHeight + topThickness - ext * 0.2, -outerD / 2 + ext / 2);
    tag(backTop);
    root.add(backTop);
  }
  // Ball corners (spheres slightly flattened)
  {
    const geo = new THREE.SphereGeometry(ext * 0.72, 12, 10);
    geometries.add(geo);
    const corners: Array<[number, number, number]> = [];
    [-1, 1].forEach((sx) => {
      [-1, 1].forEach((sy) => {
        [-1, 1].forEach((sz) => {
          corners.push([
            sx * (outerW / 2 - ext * 0.35),
            frameY + sy * (frameHeight / 2 - ext * 0.35),
            sz > 0 ? faceZ - ext * 0.1 : -outerD / 2 + ext * 0.35,
          ]);
        });
      });
    });
    corners.forEach(([x, y, z]) => {
      const ball = new THREE.Mesh(geo, alumMat);
      ball.scale.set(1, 0.85, 1);
      ball.position.set(x, y, z);
      ball.castShadow = true;
      tag(ball);
      root.add(ball);
    });
  }

  // --- Rack rails (L-channel look with hole texture) ---
  {
    const railW = EAR_WIDTH;
    const railD = 0.02;
    const geo = new THREE.BoxGeometry(railW, bayHeight * 0.98, railD);
    geometries.add(geo);
    [-1, 1].forEach((side) => {
      const rail = new THREE.Mesh(geo, railMat);
      rail.position.set(
        side * (innerW / 2 - railW / 2),
        bayFloor + bayHeight / 2,
        faceZ - railD * 0.8,
      );
      // Flip UVs feel via scale for right rail
      if (side > 0) rail.scale.x = -1;
      tag(rail);
      root.add(rail);
    });
  }

  // --- Butterfly latches (sides) ---
  {
    const geo = new THREE.PlaneGeometry(0.055, 0.055);
    geometries.add(geo);
    [-1, 1].forEach((side) => {
      [0.28, 0.72].forEach((t) => {
        const latch = new THREE.Mesh(geo, latchMat);
        latch.position.set(
          side * (outerW / 2 + 0.001),
          bayFloor + bayHeight * t,
          0.02,
        );
        latch.rotation.y = side > 0 ? Math.PI / 2 : -Math.PI / 2;
        tag(latch);
        root.add(latch);
        // Recess plate behind latch
        const plateGeo = new THREE.BoxGeometry(0.004, 0.062, 0.062);
        geometries.add(plateGeo);
        const plate = new THREE.Mesh(plateGeo, alumDarkMat);
        plate.position.copy(latch.position);
        plate.position.x -= side * 0.004;
        tag(plate);
        root.add(plate);
      });
    });
  }

  // --- Side handles (recessed) ---
  {
    const barGeo = new THREE.CylinderGeometry(0.007, 0.007, 0.11, 12);
    geometries.add(barGeo);
    [-1, 1].forEach((side) => {
      const bar = new THREE.Mesh(barGeo, alumMat);
      bar.rotation.z = Math.PI / 2;
      bar.position.set(side * (outerW / 2 + 0.004), bayFloor + bayHeight * 0.5, -0.04);
      tag(bar);
      root.add(bar);
      const cupGeo = new THREE.BoxGeometry(0.01, 0.05, 0.13);
      geometries.add(cupGeo);
      const cup = new THREE.Mesh(cupGeo, alumDarkMat);
      cup.position.set(side * (outerW / 2 - 0.002), bayFloor + bayHeight * 0.5, -0.04);
      tag(cup);
      root.add(cup);
    });
  }

  // --- Casters ---
  {
    const wheelGeo = new THREE.CylinderGeometry(0.018, 0.018, 0.014, 16);
    geometries.add(wheelGeo);
    const forkGeo = new THREE.BoxGeometry(0.03, 0.02, 0.034);
    geometries.add(forkGeo);
    const positions: Array<[number, number]> = [
      [-outerW * 0.38, outerD * 0.28],
      [outerW * 0.38, outerD * 0.28],
      [-outerW * 0.38, -outerD * 0.32],
      [outerW * 0.38, -outerD * 0.32],
    ];
    positions.forEach(([x, z]) => {
      const fork = new THREE.Mesh(forkGeo, hubMat);
      fork.position.set(x, CASE_CASTER_H * 0.55, z);
      tag(fork);
      root.add(fork);
      const wheel = new THREE.Mesh(wheelGeo, wheelMat);
      wheel.rotation.z = Math.PI / 2;
      wheel.position.set(x, 0.018, z);
      wheel.castShadow = true;
      tag(wheel);
      root.add(wheel);
    });
  }


  // Contact shadow under case
  {
    const geo = new THREE.PlaneGeometry(outerW * 1.45, outerD * 1.7);
    geometries.add(geo);
    const c = document.createElement("canvas");
    c.width = 128;
    c.height = 128;
    const cctx = c.getContext("2d");
    if (cctx) {
      const g = cctx.createRadialGradient(64, 64, 8, 64, 64, 62);
      g.addColorStop(0, "rgba(0,0,0,0.7)");
      g.addColorStop(0.55, "rgba(0,0,0,0.22)");
      g.addColorStop(1, "rgba(0,0,0,0)");
      cctx.fillStyle = g;
      cctx.fillRect(0, 0, 128, 128);
    }
    const alpha = canvasTexture(c);
    textures.push(alpha);
    const mat = new THREE.MeshBasicMaterial({
      color: 0x000000,
      transparent: true,
      opacity: 0.62,
      alphaMap: alpha,
      depthWrite: false,
    });
    materials.push(mat);
    const mesh = new THREE.Mesh(geo, mat);
    mesh.rotation.x = -Math.PI / 2;
    mesh.position.y = 0.0015;
    mesh.renderOrder = 1;
    root.add(mesh);
  }
}

export async function buildRackUnit(
  desc: ChainUnitDesc,
  preset: Amp3dThemePreset,
): Promise<ChainUnit> {
  const totalU = Math.max(1, desc.rackUnitCount ?? RACK_TOTAL_U);
  const effectSlots = resolveEffectSlots(desc);
  const slots = expandChassisSlots(effectSlots, totalU);
  const root = new THREE.Group();
  root.name = `RackFlightCase:${desc.nodeId}`;

  const geometries = new Set<THREE.BufferGeometry>();
  const textures: THREE.Texture[] = [];
  const materials: THREE.Material[] = [];

  const bayHeight = totalU * RACK_U_HEIGHT + 0.01;
  const floorY = bayFloorY();
  const outerW = RACK_WIDTH + CASE_WALL * 2 + CASE_EXTRUSION * 0.9;
  const outerD = CASE_INNER_DEPTH + CASE_WALL + CASE_FRONT_RECESS;
  const outerH = CASE_CASTER_H + CASE_BOTTOM_LIP + bayHeight + CASE_TOP_LIP;
  const caseCenterY = CASE_CASTER_H + (outerH - CASE_CASTER_H) / 2;
  const faceZ = outerD / 2 - CASE_FRONT_RECESS;

  const blackMaps = makeBlackMetalMaps(256);
  const blackColorMap = canvasTexture(blackMaps.color, { srgb: true, anisotropy: 8 });
  blackColorMap.wrapS = blackColorMap.wrapT = THREE.RepeatWrapping;
  blackColorMap.repeat.set(2.2, Math.max(2, totalU * 0.55));
  textures.push(blackColorMap);
  const blackRoughMap = canvasTexture(blackMaps.roughness, { anisotropy: 4 });
  blackRoughMap.wrapS = blackRoughMap.wrapT = THREE.RepeatWrapping;
  blackRoughMap.repeat.copy(blackColorMap.repeat);
  textures.push(blackRoughMap);

  const steelMat = new THREE.MeshStandardMaterial({
    color: 0x101214,
    map: blackColorMap,
    roughnessMap: blackRoughMap,
    roughness: 0.58,
    metalness: 0.92,
    envMapIntensity: 0.85,
  });
  materials.push(steelMat);

  const chromeMat = new THREE.MeshStandardMaterial({
    color: 0xd8dde4,
    roughness: 0.22,
    metalness: 1.0,
    envMapIntensity: 1.25,
  });
  materials.push(chromeMat);

  const knobBodyMat = new THREE.MeshStandardMaterial({
    color: 0x121316,
    roughness: 0.58,
    metalness: 0.05,
    envMapIntensity: 0.45,
  });
  materials.push(knobBodyMat);
  const knobCapMat = new THREE.MeshStandardMaterial({
    color: 0xeeeff2,
    roughness: 0.32,
    metalness: 0.0,
    envMapIntensity: 0.35,
  });
  materials.push(knobCapMat);
  const knobPointerMat = new THREE.MeshStandardMaterial({
    color: 0x050607,
    roughness: 0.42,
    metalness: 0.0,
    envMapIntensity: 0.25,
  });
  materials.push(knobPointerMat);
  const knobMats = { body: knobBodyMat, cap: knobCapMat, pointer: knobPointerMat };

  const ledOnMat = new THREE.MeshStandardMaterial({
    color: preset.ledColor,
    emissive: preset.ledColor,
    emissiveIntensity: Math.max(1.1, preset.ledIntensity * 1.15),
    roughness: 0.22,
    metalness: 0.05,
  });
  materials.push(ledOnMat);
  const ledOffMat = new THREE.MeshStandardMaterial({
    color: 0x1a1e24,
    emissive: 0x000000,
    emissiveIntensity: 0.02,
    roughness: 0.5,
    metalness: 0.2,
  });
  materials.push(ledOffMat);

  const ventAlpha = canvasTexture(makeVentAlpha(128));
  textures.push(ventAlpha);
  const ventMat = new THREE.MeshStandardMaterial({
    color: 0x0c0e11,
    roughness: 0.7,
    metalness: 0.35,
    alphaMap: ventAlpha,
    transparent: true,
    side: THREE.DoubleSide,
    depthWrite: false,
    envMapIntensity: 0.25,
  });
  materials.push(ventMat);

  const screwTex = canvasTexture(makeScrewCanvas(), { srgb: true });
  textures.push(screwTex);
  const screwMat = new THREE.MeshStandardMaterial({
    map: screwTex,
    roughness: 0.32,
    metalness: 0.9,
    envMapIntensity: 1.05,
  });
  materials.push(screwMat);

  addFlightCaseShell({
    root,
    geometries,
    textures,
    materials,
    nodeId: desc.nodeId,
    bayHeight,
    bayFloor: floorY,
    outerW,
    outerD,
    outerH,
    caseCenterY,
    faceZ,
  });

  // Shared amp knob template (same mesh/style as the amp head)
  const knobTemplate = await loadAmpComponent("knob");

  const slotRuntimes: SlotRuntime[] = [];
  const faceWidth = RACK_WIDTH - EAR_WIDTH * 2 - 0.012;
  const moduleFaceZ = faceZ - 0.006;

  slots.forEach((slot, indexFromTop) => {
    const group = new THREE.Group();
    group.name = slot.blank ? `RackBlank:${indexFromTop}` : `RackSlot:${slot.nodeId}`;
    group.position.y = slotCenterY(indexFromTop, totalU, floorY);

    const accent = slot.blank ? 0x2a2e34 : categoryAccent(slot.category || slot.effectType, "rack");
    const pickNodeId = slot.blank ? desc.nodeId : slot.nodeId;

    // Faceplate with baked title + knob labels in the albedo map.
    const faceGeo = new THREE.BoxGeometry(faceWidth, RACK_U_HEIGHT * 0.92, FACE_DEPTH);
    geometries.add(faceGeo);
    const faceAlbedo = canvasTexture(makeFaceAlbedoCanvas(slot, accent), {
      srgb: true,
      anisotropy: 8,
    });
    textures.push(faceAlbedo);
    const faceMat = new THREE.MeshStandardMaterial({
      color: 0xffffff,
      map: faceAlbedo,
      roughness: slot.blank ? 0.62 : 0.48,
      metalness: slot.blank ? 0.88 : 0.72,
      envMapIntensity: slot.blank ? 0.75 : 0.9,
    });
    materials.push(faceMat);
    const face = new THREE.Mesh(faceGeo, faceMat);
    face.castShadow = true;
    face.receiveShadow = true;
    face.position.z = moduleFaceZ;
    face.userData.chainNodeId = pickNodeId;
    group.add(face);

    const recessGeo = new THREE.BoxGeometry(faceWidth * 0.995, RACK_U_HEIGHT * 0.02, FACE_DEPTH * 0.5);
    geometries.add(recessGeo);
    const recess = new THREE.Mesh(recessGeo, steelMat);
    recess.position.set(0, -RACK_U_HEIGHT * 0.44, moduleFaceZ - 0.002);
    recess.userData.chainNodeId = pickNodeId;
    group.add(recess);

    // Mounting screws into the case rack rails
    {
      const mountScrewGeo = new THREE.CircleGeometry(0.0038, 12);
      geometries.add(mountScrewGeo);
      [-1, 1].forEach((side) => {
        const screw = new THREE.Mesh(mountScrewGeo, screwMat);
        screw.position.set(
          side * (faceWidth / 2 + EAR_WIDTH * 0.35),
          0,
          moduleFaceZ + FACE_DEPTH * 0.55,
        );
        screw.userData.chainNodeId = pickNodeId;
        group.add(screw);
      });
    }

    const knobs: SlotRuntime["knobs"] = [];
    const pickMeshes: THREE.Object3D[] = [face, recess];
    let led: THREE.Mesh | null = null;

    if (!slot.blank) {
      const ventGeo = new THREE.PlaneGeometry(faceWidth * 0.16, RACK_U_HEIGHT * 0.42);
      geometries.add(ventGeo);
      const faceFrontZ = moduleFaceZ + FACE_DEPTH * 0.55 + 0.001;

      const vent = new THREE.Mesh(ventGeo, ventMat);
      vent.position.set(faceWidth * 0.34, 0.0, faceFrontZ);
      vent.userData.chainNodeId = slot.nodeId;
      group.add(vent);
      pickMeshes.push(vent);

      const ledGeo = new THREE.SphereGeometry(0.0045, 16, 16);
      geometries.add(ledGeo);
      led = new THREE.Mesh(ledGeo, slot.bypassed ? ledOffMat : ledOnMat);
      led.position.set(faceWidth * 0.43, RACK_U_HEIGHT * 0.12, faceFrontZ + 0.004);
      led.userData.chainNodeId = slot.nodeId;
      led.userData.bypassTarget = true;
      group.add(led);
      pickMeshes.push(led);

      const haloGeo = new THREE.CircleGeometry(0.0065, 16);
      geometries.add(haloGeo);
      const haloMat = new THREE.MeshBasicMaterial({
        color: preset.ledColor,
        transparent: true,
        opacity: slot.bypassed ? 0 : 0.32,
        depthWrite: false,
      });
      materials.push(haloMat);
      const halo = new THREE.Mesh(haloGeo, haloMat);
      halo.position.copy(led.position);
      halo.position.z -= 0.001;
      group.add(halo);
      led.userData.halo = halo;

      // Amp-style knobs (Z-out, rotate on Z) — X matches silkscreen captions
      knobs.push(
        ...slot.knobs.slice(0, MAX_FACE_KNOBS).map((spec) =>
          mountAmpKnob(knobTemplate, spec, knobMats),
        ),
      );
      const knobXs = faceKnobXs(knobs.length, faceWidth);
      knobs.forEach((knob, i) => {
        knob.root.position.set(knobXs[i] ?? 0, -RACK_U_HEIGHT * 0.01, faceFrontZ + 0.001);
        knob.root.traverse((obj) => {
          obj.userData.chainNodeId = slot.nodeId;
        });
        group.add(knob.root);
        pickMeshes.push(...knob.meshes);
      });

      const handleGeo = new THREE.TorusGeometry(0.01, 0.0022, 8, 16, Math.PI);
      geometries.add(handleGeo);
      const handle = new THREE.Mesh(handleGeo, chromeMat);
      handle.rotation.y = Math.PI / 2;
      handle.rotation.z = Math.PI / 2;
      handle.position.set(-faceWidth * 0.44, 0, faceFrontZ + 0.008);
      handle.userData.chainNodeId = slot.nodeId;
      group.add(handle);
      pickMeshes.push(handle);
    }

    root.add(group);

    // Blank plates keep a dummy LED mesh off-stage for uniform SlotRuntime typing.
    if (!led) {
      const dummyGeo = new THREE.SphereGeometry(0.001, 4, 4);
      geometries.add(dummyGeo);
      led = new THREE.Mesh(dummyGeo, ledOffMat);
      led.visible = false;
      group.add(led);
    }

    slotRuntimes.push({
      desc: slot,
      group,
      face,
      faceMaterial: faceMat,
      led,
      knobs,
      pickMeshes,
      bypassed: slot.bypassed,
    });
  });

  const hostedNodeIds = effectSlots.map((s) => s.nodeId);
  let disposed = false;
  let highlightFocus: string | null = null;

  const findSlot = (nodeId?: string): SlotRuntime | undefined => {
    const real = slotRuntimes.filter((s) => !s.desc.blank);
    if (!nodeId) return real[0] ?? slotRuntimes[0];
    return real.find((s) => s.desc.nodeId === nodeId) ?? real[0] ?? slotRuntimes[0];
  };

  const refreshSlotFace = (slot: SlotRuntime): void => {
    if (slot.desc.blank) return;
    const accent = categoryAccent(slot.desc.category || slot.desc.effectType, "rack");
    const albedo = canvasTexture(makeFaceAlbedoCanvas(slot.desc, accent), {
      srgb: true,
      anisotropy: 8,
    });
    textures.push(albedo);
    const prev = slot.faceMaterial.map;
    slot.faceMaterial.map = albedo;
    slot.faceMaterial.needsUpdate = true;
    prev?.dispose();
  };

  const unit: ChainUnit = {
    nodeId: desc.nodeId,
    hostedNodeIds,
    root,
    setParams(params: Record<string, number>, nodeId?: string) {
      const slot = findSlot(nodeId);
      if (!slot) return;
      slot.knobs.forEach((knob) => {
        const value = params[knob.spec.key];
        if (typeof value === "number" && Number.isFinite(value)) {
          unit.setKnobValue(knob.spec.key, value, slot.desc.nodeId);
        }
      });
    },
    setKnobSpecs(next: Amp3dKnobSpec[], nodeId?: string) {
      const slot = findSlot(nodeId);
      if (!slot) return;
      next.forEach((spec) => unit.setKnobValue(spec.key, spec.value, slot.desc.nodeId));
    },
    setBypassed(bypassed: boolean, nodeId?: string) {
      const targets = nodeId
        ? slotRuntimes.filter((s) => !s.desc.blank && s.desc.nodeId === nodeId)
        : slotRuntimes.filter((s) => !s.desc.blank);
      targets.forEach((slot) => {
        slot.bypassed = bypassed;
        slot.led.material = bypassed ? ledOffMat : ledOnMat;
        const halo = slot.led.userData.halo as THREE.Mesh | undefined;
        if (halo && halo.material instanceof THREE.MeshBasicMaterial) {
          halo.material.opacity = bypassed ? 0 : 0.32;
        }
        slot.faceMaterial.opacity = bypassed ? 0.82 : 1;
        slot.faceMaterial.transparent = bypassed;
        slot.faceMaterial.needsUpdate = true;
      });
    },
    setHighlighted(active: boolean, focusNodeId?: string) {
      highlightFocus = active ? (focusNodeId ?? desc.nodeId) : null;
      slotRuntimes.forEach((slot) => {
        if (slot.desc.blank) {
          slot.group.position.z = 0;
          return;
        }
        const isFocus = active && slot.desc.nodeId === highlightFocus;
        slot.faceMaterial.emissive = new THREE.Color(isFocus ? 0x1a334d : active ? 0x0c141c : 0x000000);
        slot.faceMaterial.emissiveIntensity = isFocus ? 0.16 : active ? 0.06 : 0;
        slot.group.position.z = isFocus ? 0.006 : 0;
      });
      root.position.y = active ? 0.01 : 0;
    },
    setDisplayText(text: string, nodeId?: string) {
      const slot = findSlot(nodeId);
      if (!slot || slot.desc.blank) return;
      // Prefer effect title; ignore GUID dumps / pure resource paths for the name plate.
      if (text && !isGuidLike(text) && text !== slot.desc.nodeId && !/[\\/]/.test(text)) {
        slot.desc.label = text;
        slot.desc.displayText = text;
      }
      refreshSlotFace(slot);
    },
    getFocusAnchor(focusNodeId?: string): ChainUnitFocusAnchor {
      root.updateMatrixWorld(true);
      const slot = findSlot(focusNodeId);
      if (slot) {
        const faceBounds = new THREE.Box3().setFromObject(slot.face);
        if (!faceBounds.isEmpty()) {
          faceBounds.expandByScalar(0.03);
          const position = new THREE.Vector3();
          const size = new THREE.Vector3();
          faceBounds.getCenter(position);
          faceBounds.getSize(size);
          const span = Math.max(size.x, size.y * 1.35, size.z);
          return {
            position,
            fitDistance: Math.max(0.7, span * 1.35),
            bounds: faceBounds,
          };
        }
      }
      const bounds = new THREE.Box3().setFromObject(root);
      const position = new THREE.Vector3();
      bounds.getCenter(position);
      const size = new THREE.Vector3();
      bounds.getSize(size);
      return {
        position,
        fitDistance: Math.max(0.95, Math.max(size.x, size.y, size.z) * 1.4),
        bounds: bounds.clone(),
      };
    },
    getPickMeshes: () => slotRuntimes.filter((s) => !s.desc.blank).flatMap((s) => s.pickMeshes),
    getKnobMeshes: () =>
      slotRuntimes.filter((s) => !s.desc.blank).flatMap((s) => s.knobs.flatMap((k) => k.meshes)),
    getBypassMeshes: () =>
      slotRuntimes.filter((s) => !s.desc.blank).map((s) => s.led),
    getKnobSpec(key: string, nodeId?: string) {
      const slot = findSlot(nodeId);
      return slot?.knobs.find((k) => k.spec.key === key)?.spec;
    },
    setKnobValue(key: string, value: number, nodeId?: string) {
      const slot = findSlot(nodeId);
      const knob = slot?.knobs.find((k) => k.spec.key === key);
      if (!knob) return;
      knob.spec.value = value;
      // Match amp: value turns the knob around its face-normal (Z).
      knob.root.rotation.x = 0;
      knob.root.rotation.y = 0;
      knob.root.rotation.z = -valueToRotationRad(value, knob.spec.min, knob.spec.max);
    },
    dispose() {
      if (disposed) return;
      disposed = true;
      geometries.forEach((g) => g.dispose());
      textures.forEach((t) => t.dispose());
      materials.forEach((m) => m.dispose());
      root.clear();
    },
  };

  slotRuntimes.forEach((slot) => unit.setBypassed(slot.bypassed, slot.desc.nodeId));
  return unit;
}
