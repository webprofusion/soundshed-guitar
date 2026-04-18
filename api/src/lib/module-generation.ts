import { Env } from "../types/env";

type RuntimeProfileId = "level" | "mono" | "spatial" | "spatial_motion" | "saturation";

export type ModuleNodeContext = {
  nodeId?: string;
  nodeLabel?: string;
  nodeCategory?: string;
  currentModuleName?: string;
  currentModuleResourceId?: string;
  currentModuleVersion?: string;
  currentParams?: Record<string, number>;
  existingCustomEffectId?: string;
  existingCustomEffectName?: string;
  descriptorSummary?: Record<string, unknown>;
};

export type ModuleDesignPlan = {
  title: string;
  summary: string;
  assistantMessage: string;
  category: string;
  executionReadiness?: "executable" | "plan_only";
  parameters: Record<string, number>;
  questions: string[];
  tags: string[];
  limitations: string[];
  designHighlights: string[];
  promptDigest: string;
};

export type DspModuleSpecParameter = {
  id: string;
  title: string;
  defaultValue: number;
  minValue: number;
  maxValue: number;
  unit?: string;
  step?: number;
  group?: string;
  advanced?: boolean;
  labels?: string[];
};

export type DspModuleSpecResource = {
  id: string;
  title: string;
  slot: number;
  resourceType: string;
  required: boolean;
  parameterId?: string;
  parameterValue?: number;
};

export type DspModuleSpecNode = {
  id: string;
  kind: string;
  params?: Record<string, number>;
  inputs?: Record<string, string>;
  config?: Record<string, unknown>;
};

export type DspModuleSpec = {
  version: "2026-04-18";
  title: string;
  summary: string;
  category: string;
  topology: "serial" | "stereo_field";
  parameters: DspModuleSpecParameter[];
  resources: DspModuleSpecResource[];
  graph: {
    inputs: { left: string; right: string };
    nodes: DspModuleSpecNode[];
    outputs: { left: string; right: string };
  };
  constraints: {
    maxLatencySamples: number;
    cpuClass: "low" | "medium";
    deterministic: boolean;
    externalResourcesRequired: boolean;
  };
  validationTargets: string[];
  notes: string[];
};

export type ModuleExecutionPlan = ModuleDesignPlan & {
  normalizedSpec: DspModuleSpec;
  execution: {
    backend: "temporary_runtime_profile";
    profileId: RuntimeProfileId;
    readiness: "executable" | "plan_only";
  };
};

export type GeneratedModuleArtifact = {
  fileName: string;
  moduleBase64: string;
  descriptorText: string;
  specText: string;
  defaultParams: Record<string, number>;
  generationBackend: {
    type: "temporary_runtime_profile";
    key: string;
  };
  descriptorSummary: {
    displayName: string;
    category: string;
    parameterCount: number;
    resourceCount: number;
  };
  validation: {
    byteLength: number;
    parameterCount: number;
    resourceCount: number;
    hasDescriptor: boolean;
  };
};

type AiTextResponse = {
  response?: string;
};

type ParameterDoc = {
  identifier: string;
  title: string;
  description: string;
  default: number;
  minValue: number;
  maxValue: number;
  unit?: string;
  group?: string;
  advanced?: boolean;
  step?: number;
  labels?: string[];
  slot: number;
};

const DSP_SPEC_VERSION = "2026-04-18" as const;

type ModuleSpec = {
  identifier: RuntimeProfileId;
  fileName: string;
  title: string;
  description: string;
  category: string;
  params: ParameterDoc[];
};

type FuncTypeDef = { params: number[]; results: number[] };
type ImportFuncDef = { moduleName: string; fieldName: string; typeIndex: number };
type GlobalDef = { valueType: number; isMutable: boolean; initExpr: number[] };
type DefinedFuncDef = { typeIndex: number; ops: number[] };
type DataSegmentDef = { offset: number; contents: Uint8Array };

type PartialAiPlan = Partial<Omit<ModuleDesignPlan, "parameters">> & {
  parameters?: Record<string, unknown>;
};

const I32 = 0x7f;
const F32 = 0x7d;

const F32_NEG = [0x8c];
const F32_ADD = [0x92];
const F32_SUB = [0x93];
const F32_MUL = [0x94];
const F32_DIV = [0x95];
const F32_MIN = [0x96];
const F32_MAX = [0x97];
const F32_ABS = [0x8b];
const F32_GT = [0x5e];

const I32_ADD = [0x6a];
const I32_MUL = [0x6c];
const I32_AND = [0x71];

const IF_VOID = [0x04, 0x40];
const ELSE = [0x05];
const END = [0x0b];

const TEXT_ENCODER = new TextEncoder();

const MODULE_AI_SYSTEM_PROMPT = `You design AudioFX module briefs for a Custom Effect host.

Do not collapse user intent into a fixed archetype taxonomy. Treat requests as open-ended DSP designs that may involve delay, reverb, EQ, filtering, modulation, dynamics, distortion, pitch, amp, cab, utility routing, or hybrids.

Return ONLY valid JSON with this shape:
{
  "title": "Short effect title",
  "summary": "1-2 sentence summary",
  "assistantMessage": "Short assistant reply to show in the UI",
  "category": "Short category label",
  "parameters": { "paramName": 0.5 },
  "questions": ["optional follow-up question"],
  "tags": ["optional", "tags"],
  "limitations": ["only for actual execution/runtime constraints in the current build"],
  "designHighlights": ["short bullets describing the intended DSP behavior"]
}

If the current build cannot execute part of the requested effect yet, preserve the user's intent in the summary/highlights and explain the gap in limitations. Do not reframe the design as a fixed archetype.`;

export function toPublicModuleDesignPlan(plan: ModuleExecutionPlan | null | undefined): ModuleDesignPlan | null {
  if (!plan) {
    return null;
  }
  const { execution, archetype, ...publicPlan } = plan as ModuleExecutionPlan & { archetype?: string };
  return {
    title: typeof publicPlan.title === "string" ? publicPlan.title : "Custom Effect",
    summary: typeof publicPlan.summary === "string" ? publicPlan.summary : "",
    assistantMessage: typeof publicPlan.assistantMessage === "string" ? publicPlan.assistantMessage : "",
    category: typeof publicPlan.category === "string" ? publicPlan.category : "utility",
    executionReadiness: execution?.readiness === "plan_only" ? "plan_only" : "executable",
    parameters: publicPlan.parameters && typeof publicPlan.parameters === "object"
      ? publicPlan.parameters as Record<string, number>
      : {},
    questions: Array.isArray(publicPlan.questions) ? publicPlan.questions : [],
    tags: Array.isArray(publicPlan.tags) ? publicPlan.tags : [],
    limitations: Array.isArray(publicPlan.limitations) ? publicPlan.limitations : [],
    designHighlights: Array.isArray(publicPlan.designHighlights) ? publicPlan.designHighlights : [],
    promptDigest: typeof publicPlan.promptDigest === "string" ? publicPlan.promptDigest : "",
  };
}

export function ensureModuleExecutionPlan(plan: ModuleExecutionPlan | null | undefined): ModuleExecutionPlan | null {
  if (!plan) {
    return null;
  }

  const hydratedExecution: ModuleExecutionPlan["execution"] = {
    backend: plan.execution?.backend ?? "temporary_runtime_profile",
    profileId: plan.execution?.profileId ?? "level",
    readiness: plan.execution?.readiness ?? "executable",
  };

  const hydratedPlan: ModuleExecutionPlan = {
    ...plan,
    execution: hydratedExecution,
    normalizedSpec: plan.normalizedSpec ?? buildNormalizedDspSpec({
      title: plan.title,
      summary: plan.summary,
      category: plan.category,
      parameters: plan.parameters,
      limitations: plan.limitations,
      designHighlights: plan.designHighlights,
      execution: hydratedExecution,
    }),
  };

  return hydratedPlan;
}

function clamp(value: number, min: number, max: number): number {
  if (!Number.isFinite(value)) {
    return min;
  }
  return Math.min(max, Math.max(min, value));
}

function normaliseText(value: unknown, maxLength: number): string {
  if (typeof value !== "string") {
    return "";
  }
  return value.replace(/[\u0000-\u001f\u007f]+/g, " ").replace(/\s+/g, " ").trim().slice(0, maxLength);
}

function uniqueStrings(values: string[], maxItems: number): string[] {
  const seen = new Set<string>();
  const output: string[] = [];
  for (const value of values) {
    const normalised = value.trim();
    if (!normalised) {
      continue;
    }
    const key = normalised.toLowerCase();
    if (seen.has(key)) {
      continue;
    }
    seen.add(key);
    output.push(normalised);
    if (output.length >= maxItems) {
      break;
    }
  }
  return output;
}

function containsAny(text: string, terms: string[]): boolean {
  return terms.some((term) => text.includes(term));
}

function keywordScore(text: string, terms: string[]): number {
  return terms.reduce((score, term) => score + (text.includes(term) ? 1 : 0), 0);
}

function unsupportedCapabilityHits(promptText: string): string[] {
  const lower = promptText.toLowerCase();
  const unsupported = [
    [/\breverb\b/, "reverb"],
    [/\bdelay\b/, "delay"],
    [/\bfeedback\b/, "feedback networks"],
    [/\becho\b/, "echo repeats"],
    [/(^|\W)eq(\W|$)|\bequali[sz](?:er|ing)\b/, "EQ"],
    [/\bfilter(?:ing|s)?\b/, "filtering"],
    [/\bcompress(?:or|ion)\b/, "compression"],
    [/\bamp\b|\bamplifier\b/, "amp modeling"],
    [/\bcab\b|\bcabinet\b/, "cab modeling"],
    [/\bpitch\b/, "pitch shifting"],
    [/\bgranular\b/, "granular processing"],
  ] as const;

  return unsupported
    .filter(([pattern]) => pattern.test(lower))
    .map(([, label]) => label);
}

function choosePan(text: string): number {
  if (containsAny(text, ["hard left", "left side", "lean left"])) {
    return -0.6;
  }
  if (containsAny(text, ["hard right", "right side", "lean right"])) {
    return 0.6;
  }
  if (containsAny(text, ["left", "slightly left"])) {
    return -0.25;
  }
  if (containsAny(text, ["right", "slightly right"])) {
    return 0.25;
  }
  return 0;
}

function chooseDepth(text: string, fallback = 0.25): number {
  if (containsAny(text, ["narrow", "small", "distant", "behind", "far back", "collapsed"])) {
    return -0.45;
  }
  if (containsAny(text, ["massive", "huge", "immersive", "wide", "wider", "spacious", "forward"])) {
    return 0.72;
  }
  if (containsAny(text, ["subtle", "gentle", "slight"])) {
    return 0.18;
  }
  return fallback;
}

function chooseRandomSpeed(text: string): number {
  if (containsAny(text, ["slow", "drift", "glacial", "floating", "lazy"])) {
    return 0.18;
  }
  if (containsAny(text, ["fast", "nervous", "flutter", "shimmer", "agitated"])) {
    return 0.7;
  }
  return 0.35;
}

function chooseRandomAmount(text: string): number {
  if (containsAny(text, ["subtle", "gentle", "small", "light"])) {
    return 0.24;
  }
  if (containsAny(text, ["chaotic", "wild", "large", "huge", "extreme"])) {
    return 0.8;
  }
  return 0.48;
}

function chooseGain(text: string, fallback = 1): number {
  const dbMatch = text.match(/([+-]?\d+(?:\.\d+)?)\s*d\s*b/i);
  if (dbMatch) {
    const dbValue = Number.parseFloat(dbMatch[1] ?? "0");
    if (Number.isFinite(dbValue)) {
      const linear = Math.pow(10, dbValue / 20);
      return clamp(linear, 0, 2);
    }
  }
  if (containsAny(text, ["boost", "push", "lift", "louder", "hotter"])) {
    return 1.35;
  }
  if (containsAny(text, ["cut", "trim", "pad", "quieter", "reduce"])) {
    return 0.72;
  }
  return fallback;
}

function chooseThreshold(text: string): number {
  if (containsAny(text, ["light", "mild", "gentle", "soft"])) {
    return 0.92;
  }
  if (containsAny(text, ["aggressive", "hard", "heavy", "crushed", "fuzz", "extreme"])) {
    return 0.38;
  }
  if (containsAny(text, ["crunch", "edgy", "gritty", "drive"])) {
    return 0.62;
  }
  return 0.78;
}

function defaultTitleForProfile(profileId: RuntimeProfileId): string {
  switch (profileId) {
    case "level":
      return "Level Lift";
    case "mono":
      return "Mono Focus";
    case "spatial":
      return "Stereo Spread";
    case "spatial_motion":
      return "Stereo Wander";
    case "saturation":
      return "Clip Drive";
  }
}

function defaultSummaryForProfile(profileId: RuntimeProfileId): string {
  switch (profileId) {
    case "level":
      return "A simple gain stage for pushing or trimming level.";
    case "mono":
      return "A mono utility that averages left and right, then applies output gain.";
    case "spatial":
      return "A fixed stereo spreader with depth and pan controls.";
    case "spatial_motion":
      return "An animated stereo motion effect with base depth, pan, and smooth random wandering.";
    case "saturation":
      return "A straightforward clipping stage with a single threshold control.";
  }
}

function defaultDesignHighlightsForProfile(profileId: RuntimeProfileId): string[] {
  switch (profileId) {
    case "level":
      return ["Simple level shaping", "Fast utility stage"];
    case "mono":
      return ["Stereo-to-mono collapse", "Output level trim"];
    case "spatial":
      return ["Stereo image shaping", "Static depth and pan bias"];
    case "spatial_motion":
      return ["Animated stereo motion", "Slow drift around a base image"];
    case "saturation":
      return ["Hard limiting curve", "Single-threshold drive character"];
  }
}

function categoryForProfile(profileId: RuntimeProfileId): string {
  return profileId === "saturation" ? "drive" : profileId.includes("spatial") ? "spatial" : "utility";
}

function ensureAudibleProfileDefaults(profileId: RuntimeProfileId, parameters: Record<string, number | undefined>): Record<string, number> {
  const adjusted: Record<string, number> = {};
  for (const [key, value] of Object.entries(parameters)) {
    if (typeof value === "number" && Number.isFinite(value)) {
      adjusted[key] = value;
    }
  }

  switch (profileId) {
    case "level": {
      const gain = adjusted.gain;
      if (!Number.isFinite(gain) || Math.abs(gain - 1) < 0.08) {
        adjusted.gain = 1.35;
      }
      break;
    }
    case "mono": {
      const gain = adjusted.gain;
      if (!Number.isFinite(gain) || Math.abs(gain - 1) < 0.08) {
        adjusted.gain = 1.25;
      }
      break;
    }
    case "spatial": {
      if (!Number.isFinite(adjusted.depth)) {
        adjusted.depth = 0.45;
      }
      if (!Number.isFinite(adjusted.pan)) {
        adjusted.pan = 0;
      }
      // Static stereo shaping can collapse to an audible no-op on dual-mono input
      // if pan remains centered, so force a slight offset in the temporary backend.
      const pan = adjusted.pan;
      if (!Number.isFinite(pan) || Math.abs(pan) < 0.08) {
        adjusted.pan = 0.22;
      }
      break;
    }
    case "spatial_motion": {
      if (!Number.isFinite(adjusted.depth)) {
        adjusted.depth = 0.4;
      }
      if (!Number.isFinite(adjusted.pan)) {
        adjusted.pan = 0.16;
      }
      const pan = adjusted.pan;
      if (!Number.isFinite(pan) || Math.abs(pan) < 0.05) {
        adjusted.pan = 0.16;
      }
      adjusted.randomMode = 1;
      const speed = adjusted.speed;
      if (!Number.isFinite(speed) || speed < 0.2) {
        adjusted.speed = 0.35;
      }
      const amount = adjusted.amount;
      if (!Number.isFinite(amount) || amount < 0.35) {
        adjusted.amount = 0.48;
      }
      break;
    }
    case "saturation": {
      const threshold = adjusted.threshold;
      if (!Number.isFinite(threshold) || threshold > 0.72) {
        adjusted.threshold = 0.62;
      }
      break;
    }
  }

  return adjusted;
}

function defaultParametersForProfile(profileId: RuntimeProfileId, text: string): Record<string, number> {
  const rawDefaults = (() => {
    switch (profileId) {
    case "level":
      return { gain: chooseGain(text, 1) };
    case "mono":
      return { gain: chooseGain(text, 1) };
    case "spatial":
      return { depth: chooseDepth(text, 0.32), pan: choosePan(text) };
    case "spatial_motion":
      return {
        depth: chooseDepth(text, 0.4),
        pan: choosePan(text),
        randomMode: 1,
        speed: chooseRandomSpeed(text),
        amount: chooseRandomAmount(text),
      };
    case "saturation":
      return { threshold: chooseThreshold(text) };
    }
  })();

  return ensureAudibleProfileDefaults(profileId, rawDefaults);
}

function normaliseProfileParameters(profileId: RuntimeProfileId, input: Record<string, unknown> | undefined, fallbackText: string): Record<string, number> {
  const defaults = defaultParametersForProfile(profileId, fallbackText);
  const asNumber = (key: string, min: number, max: number): number => {
    const raw = input?.[key];
    if (typeof raw === "number") {
      return clamp(raw, min, max);
    }
    if (typeof raw === "string") {
      const parsed = Number.parseFloat(raw);
      if (Number.isFinite(parsed)) {
        return clamp(parsed, min, max);
      }
    }
    return defaults[key] ?? min;
  };

  const rawParameters = (() => {
    switch (profileId) {
    case "level":
    case "mono":
      return { gain: asNumber("gain", 0, 2) };
    case "spatial":
      return {
        depth: asNumber("depth", -1, 1),
        pan: asNumber("pan", -1, 1),
      };
    case "spatial_motion":
      return {
        depth: asNumber("depth", -1, 1),
        pan: asNumber("pan", -1, 1),
        randomMode: asNumber("randomMode", 0, 1) >= 0.5 ? 1 : 0,
        speed: asNumber("speed", 0, 1),
        amount: asNumber("amount", 0, 1),
      };
    case "saturation":
      return {
        threshold: asNumber("threshold", 0.05, 1),
      };
    }
  })();

  return ensureAudibleProfileDefaults(profileId, rawParameters);
}

function buildCapabilitiesLimitation(promptText: string): string[] {
  const hits = unsupportedCapabilityHits(promptText);

  if (hits.length === 0) {
    return [];
  }

  return [
    `This build still uses a temporary template-backed execution runner. ${uniqueStrings(hits, 4).join(", ")} should remain valid design goals, but they still need the general source-generation backend described in the architecture plan.`
  ];
}

function buildPlanOnlyLimitation(promptDigest: string, profileId: RuntimeProfileId): string[] {
  const hits = unsupportedCapabilityHits(promptDigest);
  if (hits.length > 0) {
    return [
      `This prompt stays in design-brief mode because the current temporary runtime cannot truthfully execute ${uniqueStrings(hits, 4).join(", ")} yet.`
    ];
  }

  return [
    `This prompt stays in design-brief mode because the current temporary runtime only executes limited level, mono, spatial, motion, and saturation profiles, and this brief does not map cleanly to the selected ${profileId.replace(/_/g, " ")} profile.`
  ];
}

function buildExecutionConstraintNotes(profileId: RuntimeProfileId): string[] {
  switch (profileId) {
    case "spatial":
      return ["The current fallback widener biases left/right level slightly so the result remains audible on mono guitar sources."];
    case "spatial_motion":
      return ["The current fallback motion effect forces audible stereo drift defaults so it does not collapse to a centered pass-through."];
    case "level":
    case "mono":
      return ["The current fallback avoids unity-gain defaults so the result is immediately audible after import."];
    case "saturation":
      return ["The current fallback avoids near-clean threshold defaults so the result is immediately audible after import."];
  }
}

function buildParameterSummary(parameters: Record<string, number>): string {
  const entries = Object.entries(parameters);
  if (entries.length === 0) {
    return "No executable control defaults are pinned yet.";
  }
  const summary = entries.slice(0, 4).map(([key, value]) => `${key} ${value.toFixed(2)}`).join(", ");
  return `Control defaults: ${summary}.`;
}

function buildAssistantMessage(plan: ModuleDesignPlan, nodeContext?: ModuleNodeContext | null): string {
  const nodeLabel = normaliseText(nodeContext?.nodeLabel, 80);
  const scope = nodeLabel ? ` for ${nodeLabel}` : "";
  const highlightSummary = plan.designHighlights.length > 0
    ? ` Focus: ${plan.designHighlights.slice(0, 3).join("; ")}.`
    : "";
  const controlSummary = buildParameterSummary(plan.parameters);
  const limitation = plan.limitations[0] ? ` ${plan.limitations[0]}` : "";
  return `I mapped this prompt${scope} to ${plan.title}. ${plan.summary}${highlightSummary} ${controlSummary}${limitation}`.trim();
}

function selectRuntimeProfile(promptText: string, nodeContext?: ModuleNodeContext | null): { profileId: RuntimeProfileId; matchedIntent: boolean } {
  const lower = promptText.toLowerCase();

  const driveScore = keywordScore(lower, ["drive", "distort", "distortion", "clip", "clipping", "grit", "gritty", "crunch", "fuzz"]);
  const motionScore = keywordScore(lower, ["random", "wander", "wandering", "drift", "moving", "motion", "animated", "swirl", "chorus", "shimmer"]);
  const spatialScore = keywordScore(lower, ["stereo", "wide", "width", "pan", "space", "spatial", "left", "right", "ambient", "immersive"]);
  const monoScore = keywordScore(lower, ["mono", "center", "centre", "collapse", "collapsed"]);
  const gainScore = keywordScore(lower, ["gain", "boost", "trim", "pad", "level", "louder", "quieter"]);

  if (monoScore > 0 && monoScore >= spatialScore) {
    return { profileId: "mono", matchedIntent: true };
  }
  if (driveScore > 0 && driveScore >= spatialScore) {
    return { profileId: "saturation", matchedIntent: true };
  }
  if (motionScore > 0 && spatialScore > 0) {
    return { profileId: "spatial_motion", matchedIntent: true };
  }
  if (spatialScore > 0) {
    return { profileId: motionScore > 0 ? "spatial_motion" : "spatial", matchedIntent: true };
  }
  if (gainScore > 0) {
    return { profileId: "level", matchedIntent: true };
  }
  if (normaliseText(nodeContext?.nodeCategory, 32).toLowerCase() === "spatial") {
    return { profileId: "spatial", matchedIntent: true };
  }
  return { profileId: "level", matchedIntent: false };
}

function heuristicPlan(userPrompts: string[], nodeContext?: ModuleNodeContext | null): ModuleExecutionPlan {
  const promptDigest = userPrompts.map((value) => normaliseText(value, 320)).filter(Boolean).join(" ").trim();
  const lower = promptDigest.toLowerCase();
  const selectedProfile = selectRuntimeProfile(promptDigest, nodeContext);
  const profileId = selectedProfile.profileId;
  const title = defaultTitleForProfile(profileId);
  const summary = defaultSummaryForProfile(profileId);
  const limitations = buildCapabilitiesLimitation(promptDigest);
  const executionNotes = buildExecutionConstraintNotes(profileId);
  const readiness: ModuleExecutionPlan["execution"]["readiness"] = limitations.length === 0 && selectedProfile.matchedIntent
    ? "executable"
    : "plan_only";
  const readinessNotes = readiness === "plan_only" ? buildPlanOnlyLimitation(promptDigest, profileId) : [];
  const tags = uniqueStrings([
    profileId.replace(/_/g, "-"),
    normaliseText(nodeContext?.nodeCategory, 32),
    containsAny(lower, ["ambient", "spacious"]) ? "ambient" : "",
    containsAny(lower, ["aggressive", "heavy", "grit"]) ? "aggressive" : "",
  ], 6);

  const plan: ModuleExecutionPlan = {
    title,
    summary,
    assistantMessage: "",
    category: categoryForProfile(profileId),
    parameters: defaultParametersForProfile(profileId, lower),
    questions: [],
    tags,
    limitations: uniqueStrings([...readinessNotes, ...limitations, ...executionNotes], 4),
    designHighlights: defaultDesignHighlightsForProfile(profileId),
    promptDigest,
    execution: {
      backend: "temporary_runtime_profile",
      profileId,
      readiness,
    },
    normalizedSpec: buildNormalizedDspSpec({
      title,
      summary,
      category: categoryForProfile(profileId),
      parameters: defaultParametersForProfile(profileId, lower),
      limitations: uniqueStrings([...readinessNotes, ...limitations, ...executionNotes], 4),
      designHighlights: defaultDesignHighlightsForProfile(profileId),
      execution: {
        backend: "temporary_runtime_profile",
        profileId,
        readiness,
      },
    }),
  };
  plan.assistantMessage = buildAssistantMessage(plan, nodeContext);
  return plan;
}

function extractJsonObject(text: string): string | null {
  const match = text.match(/\{[\s\S]*\}/);
  return match ? match[0] : null;
}

async function inferAiPlan(env: Env, userPrompts: string[], nodeContext?: ModuleNodeContext | null): Promise<PartialAiPlan | null> {
  const promptDigest = userPrompts.map((value) => normaliseText(value, 320)).filter(Boolean).join("\n").trim();
  if (!promptDigest) {
    return null;
  }

  try {
    const result = (await env.AI.run("@cf/meta/llama-3.1-8b-instruct" as Parameters<Ai["run"]>[0], {
      messages: [
        { role: "system", content: MODULE_AI_SYSTEM_PROMPT },
        {
          role: "user",
          content: JSON.stringify({
            promptHistory: userPrompts.map((entry) => normaliseText(entry, 320)).filter(Boolean),
            nodeContext: nodeContext ?? null,
          }),
        },
      ],
      max_tokens: 900,
    })) as AiTextResponse;

    const rawJson = extractJsonObject(result.response ?? "");
    if (!rawJson) {
      return null;
    }
    const parsed = JSON.parse(rawJson) as PartialAiPlan;
    return parsed && typeof parsed === "object" ? parsed : null;
  } catch {
    return null;
  }
}

export async function analyseModulePrompt(env: Env, userPrompts: string[], nodeContext?: ModuleNodeContext | null): Promise<ModuleExecutionPlan> {
  const fallback = heuristicPlan(userPrompts, nodeContext);
  const aiPlan = await inferAiPlan(env, userPrompts, nodeContext);
  if (!aiPlan) {
    return fallback;
  }

  const merged = ensureModuleExecutionPlan({
    title: normaliseText(aiPlan.title, 80) || fallback.title,
    summary: normaliseText(aiPlan.summary, 220) || fallback.summary,
    assistantMessage: normaliseText(aiPlan.assistantMessage, 360),
    category: normaliseText(aiPlan.category, 40) || fallback.category,
    parameters: normaliseProfileParameters(fallback.execution.profileId, aiPlan.parameters, fallback.promptDigest.toLowerCase()),
    questions: Array.isArray(aiPlan.questions)
      ? uniqueStrings(aiPlan.questions.map((value) => normaliseText(value, 140)).filter(Boolean), 3)
      : fallback.questions,
    tags: Array.isArray(aiPlan.tags)
      ? uniqueStrings(aiPlan.tags.map((value) => normaliseText(value, 32)).filter(Boolean), 6)
      : fallback.tags,
    limitations: Array.isArray(aiPlan.limitations)
      ? uniqueStrings(aiPlan.limitations.map((value) => normaliseText(value, 180)).filter(Boolean), 3)
      : fallback.limitations,
    designHighlights: Array.isArray(aiPlan.designHighlights)
      ? uniqueStrings(aiPlan.designHighlights.map((value) => normaliseText(value, 100)).filter(Boolean), 5)
      : fallback.designHighlights,
    promptDigest: fallback.promptDigest,
    execution: fallback.execution,
    normalizedSpec: buildNormalizedDspSpec({
      title: normaliseText(aiPlan.title, 80) || fallback.title,
      summary: normaliseText(aiPlan.summary, 220) || fallback.summary,
      category: normaliseText(aiPlan.category, 40) || fallback.category,
      parameters: normaliseProfileParameters(fallback.execution.profileId, aiPlan.parameters, fallback.promptDigest.toLowerCase()),
      limitations: Array.isArray(aiPlan.limitations)
        ? uniqueStrings(aiPlan.limitations.map((value) => normaliseText(value, 180)).filter(Boolean), 3)
        : fallback.limitations,
      designHighlights: Array.isArray(aiPlan.designHighlights)
        ? uniqueStrings(aiPlan.designHighlights.map((value) => normaliseText(value, 100)).filter(Boolean), 5)
        : fallback.designHighlights,
      execution: fallback.execution,
    }),
  })!;
  if (!merged.assistantMessage) {
    merged.assistantMessage = buildAssistantMessage(merged, nodeContext);
  }
  return merged;
}

export function canBuildGeneratedModule(plan: ModuleExecutionPlan): { ok: boolean; reason?: string } {
  if ((plan.execution.readiness ?? "executable") === "executable") {
    return { ok: true };
  }

  return {
    ok: false,
    reason: plan.limitations[0] || "This design brief does not map to a truthful executable module in the current temporary runtime.",
  };
}

export function buildNormalizedSpecText(spec: DspModuleSpec): string {
  const parameterSummary = spec.parameters.length > 0
    ? spec.parameters.map((parameter) => {
      const unit = parameter.unit ? ` ${parameter.unit}` : "";
      const group = parameter.group ? `, group=${parameter.group}` : "";
      const labels = parameter.labels && parameter.labels.length > 0 ? `, labels=${parameter.labels.join("/")}` : "";
      return `- ${parameter.id}: ${parameter.title} (default=${parameter.defaultValue.toFixed(2)}, range=${parameter.minValue.toFixed(2)}..${parameter.maxValue.toFixed(2)}${unit}${group}${labels})`;
    }).join("\n")
    : "- none";

  const resourceSummary = spec.resources.length > 0
    ? spec.resources.map((resource) => `- ${resource.id}: ${resource.title} (slot=${resource.slot}, type=${resource.resourceType}, required=${resource.required ? "yes" : "no"})`).join("\n")
    : "- none";

  const nodeSummary = spec.graph.nodes.length > 0
    ? spec.graph.nodes.map((node) => {
      const params = node.params && Object.keys(node.params).length > 0
        ? ` params=${Object.entries(node.params).map(([key, value]) => `${key}=${value.toFixed(2)}`).join(", ")}`
        : "";
      const inputs = node.inputs && Object.keys(node.inputs).length > 0
        ? ` inputs=${Object.entries(node.inputs).map(([key, value]) => `${key}<-${value}`).join(", ")}`
        : "";
      const config = node.config && Object.keys(node.config).length > 0
        ? ` config=${Object.entries(node.config).map(([key, value]) => `${key}=${String(value)}`).join(", ")}`
        : "";
      return `- ${node.id}: ${node.kind}${params}${inputs}${config}`;
    }).join("\n")
    : "- none";

  const notes = spec.notes.length > 0 ? spec.notes.map((note) => `- ${note}`).join("\n") : "- none";

  return [
    `Title: ${spec.title}`,
    `Category: ${spec.category}`,
    `Summary: ${spec.summary}`,
    `Topology: ${spec.topology}`,
    `Processing Path:`,
    nodeSummary,
    `Parameters:`,
    parameterSummary,
    `Resources:`,
    resourceSummary,
    `Frequency / Processing Aspects:`,
    `- Deterministic: ${spec.constraints.deterministic ? "yes" : "no"}`,
    `- CPU class: ${spec.constraints.cpuClass}`,
    `- Max latency samples: ${spec.constraints.maxLatencySamples}`,
    `- External resources required: ${spec.constraints.externalResourcesRequired ? "yes" : "no"}`,
    `Validation Targets:`,
    spec.validationTargets.map((target) => `- ${target}`).join("\n"),
    `Notes:`,
    notes,
  ].join("\n");
}

export function createModuleDesignerStarterMessage(nodeContext?: ModuleNodeContext | null): string {
  const label = normaliseText(nodeContext?.nodeLabel, 80);
  const moduleName = normaliseText(nodeContext?.currentModuleName, 80);
  const scope = label ? ` for ${label}` : "";
  const current = moduleName ? ` Current module: ${moduleName}.` : "";
  return `Describe the Custom Effect you want${scope}.${current} You can ask for any audio-stream DSP behavior; the system will keep the design brief open-ended and choose an implementation path behind the scenes.`.trim();
}

function appendU32Leb(target: number[], initialValue: number): void {
  let value = initialValue >>> 0;
  while (true) {
    let byte = value & 0x7f;
    value >>>= 7;
    if (value !== 0) {
      byte |= 0x80;
    }
    target.push(byte);
    if (value === 0) {
      break;
    }
  }
}

function appendI32Leb(target: number[], initialValue: number): void {
  let value = initialValue | 0;
  let more = true;
  while (more) {
    let byte = value & 0x7f;
    value >>= 7;
    const signBitSet = (byte & 0x40) !== 0;
    more = !((value === 0 && !signBitSet) || (value === -1 && signBitSet));
    if (more) {
      byte |= 0x80;
    }
    target.push(byte);
  }
}

function appendF32(target: number[], value: number): void {
  const buffer = new ArrayBuffer(4);
  new DataView(buffer).setFloat32(0, value, true);
  target.push(...new Uint8Array(buffer));
}

function appendString(target: number[], value: string): void {
  const encoded = TEXT_ENCODER.encode(value);
  appendU32Leb(target, encoded.length);
  target.push(...encoded);
}

function makeSection(sectionId: number, payload: number[]): number[] {
  const output = [sectionId];
  appendU32Leb(output, payload.length);
  output.push(...payload);
  return output;
}

function localGet(index: number): number[] {
  const output = [0x20];
  appendU32Leb(output, index);
  return output;
}

function globalGet(index: number): number[] {
  const output = [0x23];
  appendU32Leb(output, index);
  return output;
}

function globalSet(index: number): number[] {
  const output = [0x24];
  appendU32Leb(output, index);
  return output;
}

function call(index: number): number[] {
  const output = [0x10];
  appendU32Leb(output, index);
  return output;
}

function i32Const(value: number): number[] {
  const output = [0x41];
  appendI32Leb(output, value);
  return output;
}

function f32Const(value: number): number[] {
  const output = [0x43];
  appendF32(output, value);
  return output;
}

function combineOps(...ops: Array<number[] | Uint8Array>): number[] {
  const output: number[] = [];
  for (const op of ops) {
    output.push(...op);
  }
  return output;
}

function makeModule(
  types: FuncTypeDef[],
  imports: ImportFuncDef[],
  globals: GlobalDef[],
  definedFunctions: DefinedFuncDef[],
  exports: Array<[string, number]>,
  options: { exportMemory?: boolean; dataSegments?: DataSegmentDef[] } = {},
): Uint8Array {
  const module: number[] = [0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00];

  const typePayload: number[] = [];
  appendU32Leb(typePayload, types.length);
  for (const funcType of types) {
    typePayload.push(0x60);
    appendU32Leb(typePayload, funcType.params.length);
    typePayload.push(...funcType.params);
    appendU32Leb(typePayload, funcType.results.length);
    typePayload.push(...funcType.results);
  }
  module.push(...makeSection(1, typePayload));

  if (imports.length > 0) {
    const importPayload: number[] = [];
    appendU32Leb(importPayload, imports.length);
    for (const imported of imports) {
      appendString(importPayload, imported.moduleName);
      appendString(importPayload, imported.fieldName);
      importPayload.push(0x00);
      appendU32Leb(importPayload, imported.typeIndex);
    }
    module.push(...makeSection(2, importPayload));
  }

  if (definedFunctions.length > 0) {
    const functionPayload: number[] = [];
    appendU32Leb(functionPayload, definedFunctions.length);
    for (const defined of definedFunctions) {
      appendU32Leb(functionPayload, defined.typeIndex);
    }
    module.push(...makeSection(3, functionPayload));
  }

  const dataSegments = options.dataSegments ?? [];
  if (options.exportMemory || dataSegments.length > 0) {
    const memoryPayload: number[] = [];
    appendU32Leb(memoryPayload, 1);
    memoryPayload.push(0x00);
    appendU32Leb(memoryPayload, 1);
    module.push(...makeSection(5, memoryPayload));
  }

  if (globals.length > 0) {
    const globalPayload: number[] = [];
    appendU32Leb(globalPayload, globals.length);
    for (const globalDef of globals) {
      globalPayload.push(globalDef.valueType);
      globalPayload.push(globalDef.isMutable ? 0x01 : 0x00);
      globalPayload.push(...globalDef.initExpr);
      globalPayload.push(0x0b);
    }
    module.push(...makeSection(6, globalPayload));
  }

  const exportPayload: number[] = [];
  appendU32Leb(exportPayload, exports.length + (options.exportMemory ? 1 : 0));
  for (const [name, functionIndex] of exports) {
    appendString(exportPayload, name);
    exportPayload.push(0x00);
    appendU32Leb(exportPayload, functionIndex);
  }
  if (options.exportMemory) {
    appendString(exportPayload, "memory");
    exportPayload.push(0x02);
    appendU32Leb(exportPayload, 0);
  }
  module.push(...makeSection(7, exportPayload));

  const codePayload: number[] = [];
  appendU32Leb(codePayload, definedFunctions.length);
  for (const defined of definedFunctions) {
    const body: number[] = [];
    appendU32Leb(body, 0);
    body.push(...defined.ops);
    body.push(0x0b);
    appendU32Leb(codePayload, body.length);
    codePayload.push(...body);
  }
  module.push(...makeSection(10, codePayload));

  if (dataSegments.length > 0) {
    const dataPayload: number[] = [];
    appendU32Leb(dataPayload, dataSegments.length);
    for (const segment of dataSegments) {
      dataPayload.push(0x00);
      dataPayload.push(...i32Const(segment.offset));
      dataPayload.push(0x0b);
      appendU32Leb(dataPayload, segment.contents.length);
      dataPayload.push(...segment.contents);
    }
    module.push(...makeSection(11, dataPayload));
  }

  return new Uint8Array(module);
}

function buildDescriptorBlob(entries: Array<[string, string]>): Uint8Array {
  if (entries.length === 0) {
    return new Uint8Array();
  }
  return TEXT_ENCODER.encode(entries.map(([key, value]) => `${key}=${value}`).join("\n") + "\n");
}

function descriptorEntriesForSpec(spec: ModuleSpec): Array<[string, string]> {
  const entries: Array<[string, string]> = [
    ["effect.name", spec.title],
    ["effect.category", spec.category],
    ["effect.description", spec.description],
  ];

  spec.params.forEach((param, index) => {
    const prefix = `param.${index}`;
    entries.push(
      [`${prefix}.id`, param.identifier],
      [`${prefix}.title`, param.title],
      [`${prefix}.slot`, String(param.slot)],
      [`${prefix}.default`, String(param.default)],
      [`${prefix}.min`, String(param.minValue)],
      [`${prefix}.max`, String(param.maxValue)],
    );
    if (param.unit) {
      entries.push([`${prefix}.unit`, param.unit]);
    }
    if (param.group) {
      entries.push([`${prefix}.group`, param.group]);
    }
    if (param.advanced) {
      entries.push([`${prefix}.advanced`, "true"]);
    }
    if (param.step) {
      entries.push([`${prefix}.step`, String(param.step)]);
    }
    if (param.labels && param.labels.length > 0) {
      entries.push([`${prefix}.labels`, param.labels.join("|")]);
    }
  });
  return entries;
}

function makeStandardModule(options: {
  types: FuncTypeDef[];
  imports: ImportFuncDef[];
  globals: GlobalDef[];
  prepareTypeIndex: number;
  resetTypeIndex: number;
  processTypeIndex: number;
  latencyTypeIndex: number;
  prepareOps: number[];
  resetOps: number[];
  processOps: number[];
  latencyOps: number[];
  descriptorEntries?: Array<[string, string]>;
}): Uint8Array {
  const descriptorBlob = buildDescriptorBlob(options.descriptorEntries ?? []);
  const definedFunctions: DefinedFuncDef[] = [
    { typeIndex: options.prepareTypeIndex, ops: options.prepareOps },
    { typeIndex: options.resetTypeIndex, ops: options.resetOps },
    { typeIndex: options.processTypeIndex, ops: options.processOps },
    { typeIndex: options.latencyTypeIndex, ops: options.latencyOps },
  ];
  const importCount = options.imports.length;
  const exports: Array<[string, number]> = [
    ["audiofx_prepare", importCount],
    ["audiofx_reset", importCount + 1],
    ["audiofx_process", importCount + 2],
    ["audiofx_get_latency_samples", importCount + 3],
  ];

  let exportMemory = false;
  const dataSegments: DataSegmentDef[] = [];
  if (descriptorBlob.length > 0) {
    definedFunctions.push(
      { typeIndex: options.latencyTypeIndex, ops: i32Const(0) },
      { typeIndex: options.latencyTypeIndex, ops: i32Const(descriptorBlob.length) },
    );
    exports.push(
      ["audiofx_descriptor_ptr", importCount + 4],
      ["audiofx_descriptor_len", importCount + 5],
    );
    exportMemory = true;
    dataSegments.push({ offset: 0, contents: descriptorBlob });
  }

  return makeModule(options.types, options.imports, options.globals, definedFunctions, exports, {
    exportMemory,
    dataSegments,
  });
}

function standardPrepareOps(): number[] {
  return i32Const(0);
}

function standardLatencyOps(): number[] {
  return i32Const(0);
}

function clampedParamOps(index: number): number[] {
  return combineOps(i32Const(index), call(0), f32Const(1), F32_MIN, f32Const(-1), F32_MAX);
}

function clampedUnipolarParamOps(index: number): number[] {
  return combineOps(i32Const(index), call(0), f32Const(1), F32_MIN, f32Const(0), F32_MAX);
}

function randomBipolarTargetOps(seedGlobal: number, baseGlobal: number, amountGlobal: number): number[] {
  return combineOps(
    globalGet(seedGlobal),
    i32Const(1_664_525),
    I32_MUL,
    i32Const(1_013_904_223),
    I32_ADD,
    globalSet(seedGlobal),
    globalGet(seedGlobal),
    i32Const(0x7fff_ffff),
    I32_AND,
    [0xb2],
    f32Const(2_147_483_647),
    F32_DIV,
    f32Const(2),
    F32_MUL,
    f32Const(1),
    F32_SUB,
    globalGet(amountGlobal),
    F32_MUL,
    globalGet(baseGlobal),
    F32_ADD,
    f32Const(1),
    F32_MIN,
    f32Const(-1),
    F32_MAX,
  );
}

function randomSlewOps(speedGlobal: number, sampleRateGlobal: number): number[] {
  return combineOps(
    f32Const(1),
    globalGet(speedGlobal),
    f32Const(11),
    F32_MUL,
    F32_ADD,
    globalGet(sampleRateGlobal),
    F32_DIV,
  );
}

function hardClipThresholdOps(): number[] {
  return combineOps(i32Const(0), call(0), f32Const(0.05), F32_MAX);
}

function emitLevelProfileModule(spec: ModuleSpec): Uint8Array {
  const types: FuncTypeDef[] = [
    { params: [I32], results: [F32] },
    { params: [F32, I32, I32], results: [I32] },
    { params: [], results: [] },
    { params: [F32, F32], results: [F32, F32] },
    { params: [], results: [I32] },
  ];
  const imports: ImportFuncDef[] = [{ moduleName: "host", fieldName: "read_param", typeIndex: 0 }];
  const processOps = combineOps(
    localGet(0), i32Const(0), call(0), F32_MUL,
    localGet(1), i32Const(0), call(0), F32_MUL,
  );
  return makeStandardModule({
    types,
    imports,
    globals: [],
    prepareTypeIndex: 1,
    resetTypeIndex: 2,
    processTypeIndex: 3,
    latencyTypeIndex: 4,
    prepareOps: standardPrepareOps(),
    resetOps: [],
    processOps,
    latencyOps: standardLatencyOps(),
    descriptorEntries: descriptorEntriesForSpec(spec),
  });
}

function emitMonoProfileModule(spec: ModuleSpec): Uint8Array {
  const types: FuncTypeDef[] = [
    { params: [I32], results: [F32] },
    { params: [F32, I32, I32], results: [I32] },
    { params: [], results: [] },
    { params: [F32, F32], results: [F32, F32] },
    { params: [], results: [I32] },
  ];
  const imports: ImportFuncDef[] = [{ moduleName: "host", fieldName: "read_param", typeIndex: 0 }];
  const processOps = combineOps(
    localGet(0), localGet(1), F32_ADD, f32Const(0.5), F32_MUL, i32Const(0), call(0), F32_MUL,
    localGet(1), localGet(0), F32_ADD, f32Const(0.5), F32_MUL, i32Const(0), call(0), F32_MUL,
  );
  return makeStandardModule({
    types,
    imports,
    globals: [],
    prepareTypeIndex: 1,
    resetTypeIndex: 2,
    processTypeIndex: 3,
    latencyTypeIndex: 4,
    prepareOps: standardPrepareOps(),
    resetOps: [],
    processOps,
    latencyOps: standardLatencyOps(),
    descriptorEntries: descriptorEntriesForSpec(spec),
  });
}

function emitSpatialProfileModule(spec: ModuleSpec): Uint8Array {
  const types: FuncTypeDef[] = [
    { params: [I32], results: [F32] },
    { params: [F32, I32, I32], results: [I32] },
    { params: [], results: [] },
    { params: [F32, F32], results: [F32, F32] },
    { params: [], results: [I32] },
  ];
  const imports: ImportFuncDef[] = [{ moduleName: "host", fieldName: "read_param", typeIndex: 0 }];
  const globals: GlobalDef[] = [
    { valueType: F32, isMutable: true, initExpr: f32Const(0) },
    { valueType: F32, isMutable: true, initExpr: f32Const(0) },
    { valueType: F32, isMutable: true, initExpr: f32Const(0) },
    { valueType: F32, isMutable: true, initExpr: f32Const(1) },
    { valueType: F32, isMutable: true, initExpr: f32Const(1) },
    { valueType: F32, isMutable: true, initExpr: f32Const(1) },
    { valueType: F32, isMutable: true, initExpr: f32Const(0) },
  ];

  const processOps = combineOps(
    clampedParamOps(0), globalSet(0),
    clampedParamOps(1), globalSet(1),
    globalGet(0), f32Const(1), F32_ADD, f32Const(0.5), F32_MUL, globalSet(2),
    f32Const(0.6), globalGet(2), f32Const(0.4), F32_MUL, F32_ADD, globalSet(3),
    f32Const(1), globalGet(1), F32_SUB, f32Const(1), F32_MIN, globalSet(4),
    f32Const(1), globalGet(1), F32_ADD, f32Const(1), F32_MIN, globalSet(5),
    localGet(0), localGet(1), F32_ADD, f32Const(0.5), F32_MUL, globalSet(6),
    globalGet(2), localGet(0), F32_MUL, f32Const(1), globalGet(2), F32_SUB, globalGet(6), F32_MUL, F32_ADD, globalGet(3), F32_MUL, globalGet(4), F32_MUL,
    globalGet(2), localGet(1), F32_MUL, f32Const(1), globalGet(2), F32_SUB, globalGet(6), F32_MUL, F32_ADD, globalGet(3), F32_MUL, globalGet(5), F32_MUL,
  );

  return makeStandardModule({
    types,
    imports,
    globals,
    prepareTypeIndex: 1,
    resetTypeIndex: 2,
    processTypeIndex: 3,
    latencyTypeIndex: 4,
    prepareOps: standardPrepareOps(),
    resetOps: [],
    processOps,
    latencyOps: standardLatencyOps(),
    descriptorEntries: descriptorEntriesForSpec(spec),
  });
}

function emitSpatialMotionProfileModule(spec: ModuleSpec): Uint8Array {
  const types: FuncTypeDef[] = [
    { params: [I32], results: [F32] },
    { params: [F32, I32, I32], results: [I32] },
    { params: [], results: [] },
    { params: [F32, F32], results: [F32, F32] },
    { params: [], results: [I32] },
  ];
  const imports: ImportFuncDef[] = [{ moduleName: "host", fieldName: "read_param", typeIndex: 0 }];
  const globals: GlobalDef[] = [
    { valueType: F32, isMutable: true, initExpr: f32Const(0) },
    { valueType: F32, isMutable: true, initExpr: f32Const(0) },
    { valueType: F32, isMutable: true, initExpr: f32Const(0) },
    { valueType: F32, isMutable: true, initExpr: f32Const(0) },
    { valueType: F32, isMutable: true, initExpr: f32Const(0) },
    { valueType: F32, isMutable: true, initExpr: f32Const(0) },
    { valueType: F32, isMutable: true, initExpr: f32Const(1) },
    { valueType: F32, isMutable: true, initExpr: f32Const(0.35) },
    { valueType: F32, isMutable: true, initExpr: f32Const(0.5) },
    { valueType: I32, isMutable: true, initExpr: i32Const(0x13579bdf) },
    { valueType: F32, isMutable: true, initExpr: f32Const(48_000) },
    { valueType: F32, isMutable: true, initExpr: f32Const(0) },
    { valueType: F32, isMutable: true, initExpr: f32Const(1) },
    { valueType: F32, isMutable: true, initExpr: f32Const(1) },
    { valueType: F32, isMutable: true, initExpr: f32Const(1) },
    { valueType: F32, isMutable: true, initExpr: f32Const(0) },
  ];

  const prepareOps = combineOps(localGet(0), globalSet(10), i32Const(0));
  const resetOps = combineOps(
    f32Const(0), globalSet(2),
    f32Const(0), globalSet(3),
    f32Const(0), globalSet(4),
    f32Const(0), globalSet(5),
    i32Const(0x13579bdf), globalSet(9),
  );
  const processOps = combineOps(
    clampedParamOps(0), globalSet(0),
    clampedParamOps(1), globalSet(1),
    clampedUnipolarParamOps(2), globalSet(6),
    clampedUnipolarParamOps(3), globalSet(7),
    clampedUnipolarParamOps(4), globalSet(8),
    globalGet(6), f32Const(0.5), F32_GT,
    IF_VOID,
      globalGet(4), globalGet(2), F32_SUB, F32_ABS, f32Const(0.03), [0x5d],
      globalGet(5), globalGet(3), F32_SUB, F32_ABS, f32Const(0.03), [0x5d],
      I32_AND,
      IF_VOID,
        randomBipolarTargetOps(9, 0, 8), globalSet(4),
        randomBipolarTargetOps(9, 1, 8), globalSet(5),
      END,
      globalGet(2), globalGet(4), globalGet(2), F32_SUB, randomSlewOps(7, 10), F32_MUL, F32_ADD, globalSet(2),
      globalGet(3), globalGet(5), globalGet(3), F32_SUB, randomSlewOps(7, 10), F32_MUL, F32_ADD, globalSet(3),
    ELSE,
      globalGet(0), globalSet(2),
      globalGet(1), globalSet(3),
      globalGet(0), globalSet(4),
      globalGet(1), globalSet(5),
    END,
    globalGet(2), f32Const(1), F32_ADD, f32Const(0.5), F32_MUL, globalSet(11),
    f32Const(0.6), globalGet(11), f32Const(0.4), F32_MUL, F32_ADD, globalSet(12),
    f32Const(1), globalGet(3), F32_SUB, f32Const(1), F32_MIN, globalSet(13),
    f32Const(1), globalGet(3), F32_ADD, f32Const(1), F32_MIN, globalSet(14),
    localGet(0), localGet(1), F32_ADD, f32Const(0.5), F32_MUL, globalSet(15),
    globalGet(11), localGet(0), F32_MUL, f32Const(1), globalGet(11), F32_SUB, globalGet(15), F32_MUL, F32_ADD, globalGet(12), F32_MUL, globalGet(13), F32_MUL,
    globalGet(11), localGet(1), F32_MUL, f32Const(1), globalGet(11), F32_SUB, globalGet(15), F32_MUL, F32_ADD, globalGet(12), F32_MUL, globalGet(14), F32_MUL,
  );

  return makeStandardModule({
    types,
    imports,
    globals,
    prepareTypeIndex: 1,
    resetTypeIndex: 2,
    processTypeIndex: 3,
    latencyTypeIndex: 4,
    prepareOps,
    resetOps,
    processOps,
    latencyOps: standardLatencyOps(),
    descriptorEntries: descriptorEntriesForSpec(spec),
  });
}

function emitSaturationProfileModule(spec: ModuleSpec): Uint8Array {
  const types: FuncTypeDef[] = [
    { params: [I32], results: [F32] },
    { params: [F32, I32, I32], results: [I32] },
    { params: [], results: [] },
    { params: [F32, F32], results: [F32, F32] },
    { params: [], results: [I32] },
  ];
  const imports: ImportFuncDef[] = [{ moduleName: "host", fieldName: "read_param", typeIndex: 0 }];
  const threshold = hardClipThresholdOps();
  const processOps = combineOps(
    localGet(0), threshold, F32_MIN, threshold, F32_NEG, F32_MAX,
    localGet(1), threshold, F32_MIN, threshold, F32_NEG, F32_MAX,
  );
  return makeStandardModule({
    types,
    imports,
    globals: [],
    prepareTypeIndex: 1,
    resetTypeIndex: 2,
    processTypeIndex: 3,
    latencyTypeIndex: 4,
    prepareOps: standardPrepareOps(),
    resetOps: [],
    processOps,
    latencyOps: standardLatencyOps(),
    descriptorEntries: descriptorEntriesForSpec(spec),
  });
}

function slugifyTitle(title: string): string {
  const slug = title
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, "-")
    .replace(/^-+|-+$/g, "")
    .slice(0, 64);
  return slug || "custom-effect";
}

function buildModuleSpec(plan: ModuleExecutionPlan): ModuleSpec {
  switch (plan.execution.profileId) {
    case "level":
      return {
        identifier: "level",
        fileName: `${slugifyTitle(plan.title)}.wasm`,
        title: plan.title,
        description: plan.summary,
        category: plan.category,
        params: [
          {
            identifier: "gain",
            title: "Gain",
            description: "Linear gain multiplier.",
            default: plan.parameters.gain,
            minValue: 0,
            maxValue: 2,
            unit: "amount",
            step: 0.01,
            slot: 0,
          },
        ],
      };
    case "mono":
      return {
        identifier: "mono",
        fileName: `${slugifyTitle(plan.title)}.wasm`,
        title: plan.title,
        description: plan.summary,
        category: plan.category,
        params: [
          {
            identifier: "gain",
            title: "Output Gain",
            description: "Post-average output gain.",
            default: plan.parameters.gain,
            minValue: 0,
            maxValue: 2,
            unit: "amount",
            step: 0.01,
            slot: 0,
          },
        ],
      };
    case "spatial":
      return {
        identifier: "spatial",
        fileName: `${slugifyTitle(plan.title)}.wasm`,
        title: plan.title,
        description: plan.summary,
        category: plan.category,
        params: [
          {
            identifier: "depth",
            title: "Depth",
            description: "Back/forward image position from -1.0 to 1.0.",
            default: plan.parameters.depth,
            minValue: -1,
            maxValue: 1,
            unit: "amount",
            step: 0.01,
            slot: 0,
          },
          {
            identifier: "pan",
            title: "Pan",
            description: "Left/right pan from -1.0 to 1.0.",
            default: plan.parameters.pan,
            minValue: -1,
            maxValue: 1,
            unit: "pan",
            step: 0.01,
            slot: 1,
          },
        ],
      };
    case "spatial_motion":
      return {
        identifier: "spatial_motion",
        fileName: `${slugifyTitle(plan.title)}.wasm`,
        title: plan.title,
        description: plan.summary,
        category: plan.category,
        params: [
          {
            identifier: "depth",
            title: "Base Depth",
            description: "Center point for back/forward image position.",
            default: plan.parameters.depth,
            minValue: -1,
            maxValue: 1,
            unit: "amount",
            step: 0.01,
            slot: 0,
          },
          {
            identifier: "pan",
            title: "Base Pan",
            description: "Center point for the left/right image position.",
            default: plan.parameters.pan,
            minValue: -1,
            maxValue: 1,
            unit: "pan",
            step: 0.01,
            slot: 1,
          },
          {
            identifier: "randomMode",
            title: "Random Mode",
            description: "Turns smooth random target wandering on or off.",
            default: 1,
            minValue: 0,
            maxValue: 1,
            unit: "enum",
            group: "random",
            step: 1,
            labels: ["Off", "On"],
            slot: 2,
          },
          {
            identifier: "speed",
            title: "Random Speed",
            description: "How quickly the module glides toward each random target.",
            default: plan.parameters.speed,
            minValue: 0,
            maxValue: 1,
            unit: "amount",
            group: "random",
            step: 0.01,
            slot: 3,
          },
          {
            identifier: "amount",
            title: "Random Amount",
            description: "How far pan and depth can wander from their base settings.",
            default: plan.parameters.amount,
            minValue: 0,
            maxValue: 1,
            unit: "amount",
            group: "random",
            step: 0.01,
            slot: 4,
          },
        ],
      };
    case "saturation":
      return {
        identifier: "saturation",
        fileName: `${slugifyTitle(plan.title)}.wasm`,
        title: plan.title,
        description: plan.summary,
        category: plan.category,
        params: [
          {
            identifier: "threshold",
            title: "Threshold",
            description: "Clip threshold with an internal minimum floor of 0.05.",
            default: plan.parameters.threshold,
            minValue: 0.05,
            maxValue: 1,
            unit: "amount",
            step: 0.01,
            slot: 0,
          },
        ],
      };
  }
}

function encodeBase64(bytes: Uint8Array): string {
  let binary = "";
  const chunkSize = 0x8000;
  for (let index = 0; index < bytes.length; index += chunkSize) {
    binary += String.fromCharCode(...bytes.subarray(index, index + chunkSize));
  }
  return btoa(binary);
}

export function buildGeneratedModuleArtifact(plan: ModuleExecutionPlan): GeneratedModuleArtifact {
  const buildability = canBuildGeneratedModule(plan);
  if (!buildability.ok) {
    throw new Error(buildability.reason);
  }

  const spec = buildModuleSpec(plan);
  const wasmBytes = (() => {
    switch (spec.identifier) {
      case "level":
        return emitLevelProfileModule(spec);
      case "mono":
        return emitMonoProfileModule(spec);
      case "spatial":
        return emitSpatialProfileModule(spec);
      case "spatial_motion":
        return emitSpatialMotionProfileModule(spec);
      case "saturation":
        return emitSaturationProfileModule(spec);
    }
  })();

  const descriptorText = new TextDecoder().decode(buildDescriptorBlob(descriptorEntriesForSpec(spec)));
  const specText = buildNormalizedSpecText(plan.normalizedSpec);
  return {
    fileName: spec.fileName,
    moduleBase64: encodeBase64(wasmBytes),
    descriptorText,
    specText,
    defaultParams: { ...plan.parameters },
    generationBackend: {
      type: plan.execution.backend,
      key: plan.execution.profileId,
    },
    descriptorSummary: {
      displayName: spec.title,
      category: spec.category,
      parameterCount: spec.params.length,
      resourceCount: 0,
    },
    validation: {
      byteLength: wasmBytes.length,
      parameterCount: spec.params.length,
      resourceCount: 0,
      hasDescriptor: descriptorText.length > 0,
    },
  };
}

function buildNormalizedSpecParameters(profileId: RuntimeProfileId, parameters: Record<string, number>): DspModuleSpecParameter[] {
  switch (profileId) {
    case "level":
      return [{ id: "gain", title: "Gain", defaultValue: parameters.gain ?? 1, minValue: 0, maxValue: 2, unit: "amount", step: 0.01 }];
    case "mono":
      return [{ id: "gain", title: "Output Gain", defaultValue: parameters.gain ?? 1, minValue: 0, maxValue: 2, unit: "amount", step: 0.01 }];
    case "spatial":
      return [
        { id: "depth", title: "Depth", defaultValue: parameters.depth ?? 0, minValue: -1, maxValue: 1, unit: "amount", step: 0.01 },
        { id: "pan", title: "Pan", defaultValue: parameters.pan ?? 0, minValue: -1, maxValue: 1, unit: "pan", step: 0.01 },
      ];
    case "spatial_motion":
      return [
        { id: "depth", title: "Base Depth", defaultValue: parameters.depth ?? 0, minValue: -1, maxValue: 1, unit: "amount", step: 0.01 },
        { id: "pan", title: "Base Pan", defaultValue: parameters.pan ?? 0, minValue: -1, maxValue: 1, unit: "pan", step: 0.01 },
        { id: "randomMode", title: "Random Mode", defaultValue: parameters.randomMode ?? 1, minValue: 0, maxValue: 1, unit: "enum", step: 1, group: "random", labels: ["Off", "On"] },
        { id: "speed", title: "Random Speed", defaultValue: parameters.speed ?? 0.35, minValue: 0, maxValue: 1, unit: "amount", step: 0.01, group: "random" },
        { id: "amount", title: "Random Amount", defaultValue: parameters.amount ?? 0.48, minValue: 0, maxValue: 1, unit: "amount", step: 0.01, group: "random" },
      ];
    case "saturation":
      return [{ id: "threshold", title: "Threshold", defaultValue: parameters.threshold ?? 0.62, minValue: 0.05, maxValue: 1, unit: "amount", step: 0.01 }];
  }
}

function buildNormalizedSpecGraph(profileId: RuntimeProfileId, parameters: Record<string, number>): DspModuleSpec["graph"] {
  switch (profileId) {
    case "level":
      return {
        inputs: { left: "input.left", right: "input.right" },
        nodes: [{ id: "level_stage", kind: "gain", params: { gain: parameters.gain ?? 1 }, inputs: { left: "input.left", right: "input.right" } }],
        outputs: { left: "level_stage.left", right: "level_stage.right" },
      };
    case "mono":
      return {
        inputs: { left: "input.left", right: "input.right" },
        nodes: [
          { id: "mono_sum", kind: "stereo_to_mono", inputs: { left: "input.left", right: "input.right" } },
          { id: "output_gain", kind: "gain", params: { gain: parameters.gain ?? 1 }, inputs: { mono: "mono_sum.mono" } },
        ],
        outputs: { left: "output_gain.mono", right: "output_gain.mono" },
      };
    case "spatial":
      return {
        inputs: { left: "input.left", right: "input.right" },
        nodes: [{ id: "stereo_field", kind: "stereo_image", params: { depth: parameters.depth ?? 0, pan: parameters.pan ?? 0 }, inputs: { left: "input.left", right: "input.right" } }],
        outputs: { left: "stereo_field.left", right: "stereo_field.right" },
      };
    case "spatial_motion":
      return {
        inputs: { left: "input.left", right: "input.right" },
        nodes: [{
          id: "motion_field",
          kind: "stereo_motion",
          params: {
            depth: parameters.depth ?? 0,
            pan: parameters.pan ?? 0,
            randomMode: parameters.randomMode ?? 1,
            speed: parameters.speed ?? 0.35,
            amount: parameters.amount ?? 0.48,
          },
          inputs: { left: "input.left", right: "input.right" },
          config: { modulationSource: (parameters.randomMode ?? 1) >= 0.5 ? "smooth_random" : "static" },
        }],
        outputs: { left: "motion_field.left", right: "motion_field.right" },
      };
    case "saturation":
      return {
        inputs: { left: "input.left", right: "input.right" },
        nodes: [{ id: "clip_stage", kind: "hard_clip", params: { threshold: parameters.threshold ?? 0.62 }, inputs: { left: "input.left", right: "input.right" } }],
        outputs: { left: "clip_stage.left", right: "clip_stage.right" },
      };
  }
}

function buildValidationTargets(profileId: RuntimeProfileId, readiness: ModuleExecutionPlan["execution"]["readiness"]): string[] {
  const targets = ["abi_exports", "descriptor_metadata", "finite_output_smoke"];
  if (profileId === "mono") {
    targets.push("mono_collapse_response");
  }
  if (profileId === "spatial" || profileId === "spatial_motion") {
    targets.push("stereo_divergence_response");
  }
  if (profileId === "spatial_motion") {
    targets.push("modulation_state_response");
  }
  if (profileId === "saturation") {
    targets.push("nonlinear_transfer_response");
  }
  if (readiness !== "executable") {
    targets.push("plan_only_gate");
  }
  return targets;
}

function buildNormalizedDspSpec(plan: Pick<ModuleExecutionPlan, "title" | "summary" | "category" | "parameters" | "limitations" | "designHighlights" | "execution">): DspModuleSpec {
  const parameters = buildNormalizedSpecParameters(plan.execution.profileId, plan.parameters);
  const deterministic = plan.execution.profileId !== "spatial_motion" || (plan.parameters.randomMode ?? 1) < 0.5;
  return {
    version: DSP_SPEC_VERSION,
    title: plan.title,
    summary: plan.summary,
    category: plan.category,
    topology: plan.execution.profileId === "level" || plan.execution.profileId === "saturation" ? "serial" : "stereo_field",
    parameters,
    resources: [],
    graph: buildNormalizedSpecGraph(plan.execution.profileId, plan.parameters),
    constraints: {
      maxLatencySamples: 0,
      cpuClass: plan.execution.profileId === "spatial_motion" ? "medium" : "low",
      deterministic,
      externalResourcesRequired: false,
    },
    validationTargets: buildValidationTargets(plan.execution.profileId, plan.execution.readiness),
    notes: uniqueStrings([...plan.designHighlights, ...plan.limitations], 8),
  };
}