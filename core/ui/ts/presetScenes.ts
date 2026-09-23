import type { Preset, PresetScene, SignalGraph } from "./types.js";

function cloneGraph(graph?: SignalGraph | null): SignalGraph {
  return JSON.parse(JSON.stringify(graph ?? { nodes: [], edges: [] })) as SignalGraph;
}

function hasGraphContent(graph?: SignalGraph | null): boolean {
  return Boolean(graph && (((graph.nodes?.length ?? 0) > 0) || ((graph.edges?.length ?? 0) > 0)));
}

function buildDefaultSceneId(index: number): string {
  return `scene-${index + 1}`;
}

function buildDefaultSceneTitle(index: number): string {
  return `Scene ${index + 1}`;
}

export function findPresetScene(preset: Preset | null | undefined, sceneId: string | null | undefined): PresetScene | null {
  if (!preset || !Array.isArray(preset.scenes) || !sceneId) {
    return null;
  }
  return preset.scenes.find((scene) => scene.id === sceneId) ?? null;
}

export function selectPresetScene(preset: Preset | null | undefined, sceneId?: string | null): string | null {
  if (!preset) {
    return null;
  }

  const baseGraph = cloneGraph(preset.graph);
  if (!Array.isArray(preset.scenes) || preset.scenes.length === 0) {
    preset.scenes = [{ id: "scene-1", title: "Scene 1", graph: baseGraph }];
  }
  else if (preset.scenes.length === 1 && hasGraphContent(preset.graph) && !hasGraphContent(preset.scenes[0]?.graph)) {
    preset.scenes[0].graph = baseGraph;
  }

  preset.scenes.forEach((scene, index) => {
    if (!scene.id) {
      scene.id = buildDefaultSceneId(index);
    }
    if (!scene.title) {
      scene.title = buildDefaultSceneTitle(index);
    }
    if (!scene.graph) {
      scene.graph = { nodes: [], edges: [] };
    }
  });

  const selected = findPresetScene(preset, sceneId) ?? preset.scenes[0] ?? null;
  if (!selected) {
    return null;
  }

  preset.graph = selected.graph;
  return selected.id;
}

export function normalizePresetScenes(preset: Preset | null | undefined, preferredSceneId?: string | null): string | null {
  return selectPresetScene(preset, preferredSceneId);
}

export function getPresetSceneGraphs(preset: Preset | null | undefined): SignalGraph[] {
  if (!preset) {
    return [];
  }
  if (Array.isArray(preset.scenes) && preset.scenes.length > 0) {
    return preset.scenes.map((scene) => scene.graph).filter(Boolean);
  }
  return preset.graph ? [preset.graph] : [];
}