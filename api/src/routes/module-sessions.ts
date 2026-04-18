import { Hono } from "hono";
import {
  addModuleGenerationMessage,
  createModuleGenerationRevision,
  createModuleGenerationSession,
  findModuleGenerationRevision,
  findModuleGenerationSession,
  listModuleGenerationMessages,
  listModuleGenerationRevisions,
  updateModuleGenerationSessionPlan,
  type ModuleGenerationMessageRow,
  type ModuleGenerationRevisionRow,
  type ModuleGenerationSessionRow,
} from "../lib/db";
import {
  analyseModulePrompt,
  buildNormalizedSpecText,
  buildGeneratedModuleArtifact,
  canBuildGeneratedModule,
  createModuleDesignerStarterMessage,
  ensureModuleExecutionPlan,
  type GeneratedModuleArtifact,
  type ModuleDesignPlan,
  type ModuleExecutionPlan,
  type ModuleNodeContext,
  toPublicModuleDesignPlan,
} from "../lib/module-generation";
import { fail, ok, safeJson } from "../lib/http";
import { optionalAuth } from "../middleware/session";
import { Env } from "../types/env";

type Variables = {
  auth?: { userId: string; email: string; role: string; sessionId: string };
};

type CreateSessionBody = {
  prompt?: string;
  nodeContext?: ModuleNodeContext;
};

type CreateMessageBody = {
  content?: string;
};

function normaliseText(value: unknown, maxLength: number): string {
  if (typeof value !== "string") {
    return "";
  }
  return value.replace(/[\u0000-\u001f\u007f]+/g, " ").replace(/\s+/g, " ").trim().slice(0, maxLength);
}

function parseJsonValue<T>(raw: string | null | undefined, fallback: T): T {
  if (!raw) {
    return fallback;
  }
  try {
    return JSON.parse(raw) as T;
  } catch {
    return fallback;
  }
}

function parseNodeContext(raw: unknown): ModuleNodeContext {
  if (!raw || typeof raw !== "object") {
    return {};
  }
  const candidate = raw as Record<string, unknown>;
  const descriptorSummary = candidate.descriptorSummary;
  const currentParams = candidate.currentParams;
  return {
    nodeId: normaliseText(candidate.nodeId, 80),
    nodeLabel: normaliseText(candidate.nodeLabel, 120),
    nodeCategory: normaliseText(candidate.nodeCategory, 40),
    currentModuleName: normaliseText(candidate.currentModuleName, 120),
    currentModuleResourceId: normaliseText(candidate.currentModuleResourceId, 160),
    currentModuleVersion: normaliseText(candidate.currentModuleVersion, 40),
    currentParams: currentParams && typeof currentParams === "object"
      ? Object.fromEntries(Object.entries(currentParams as Record<string, unknown>).filter(([, value]) => typeof value === "number"))
      : undefined,
    existingCustomEffectId: normaliseText(candidate.existingCustomEffectId, 80),
    existingCustomEffectName: normaliseText(candidate.existingCustomEffectName, 120),
    descriptorSummary: descriptorSummary && typeof descriptorSummary === "object"
      ? (descriptorSummary as Record<string, unknown>)
      : undefined,
  };
}

function enforceSessionAccess(
  c: Parameters<typeof ok>[0],
  session: ModuleGenerationSessionRow,
): Response | null {
  if (!session.owner_user_id) {
    return null;
  }

  const auth = c.get("auth");
  if (!auth || auth.userId !== session.owner_user_id) {
    return fail(c, "SESSION_NOT_FOUND", "Module session not found", 404);
  }

  return null;
}

function toMessageResponse(row: ModuleGenerationMessageRow) {
  return {
    id: row.id,
    role: row.role,
    content: row.content,
    createdAt: row.created_at,
  };
}

function toRevisionSummary(row: ModuleGenerationRevisionRow) {
  return {
    id: row.id,
    title: row.title,
    summary: row.summary,
    category: row.category,
    createdAt: row.created_at,
  };
}

function normaliseStoredExecutionProfile(raw: string): ModuleExecutionPlan["execution"]["profileId"] {
  switch (raw) {
    case "gain":
    case "level":
      return "level";
    case "stereo_average":
    case "mono":
      return "mono";
    case "stereo_spatial":
    case "spatial":
      return "spatial";
    case "stereo_spatial_random":
    case "spatial_motion":
      return "spatial_motion";
    case "hard_clip":
    case "saturation":
      return "saturation";
    default:
      return "level";
  }
}

function toRevisionDetail(row: ModuleGenerationRevisionRow) {
  const parsedPlan = parseJsonValue<ModuleExecutionPlan | null>(row.plan_json, null);
  const plan = ensureModuleExecutionPlan(parsedPlan ?? {
    title: row.title,
    summary: row.summary,
    assistantMessage: row.summary,
    category: row.category,
    parameters: {},
    questions: [],
    tags: [],
    limitations: [],
    designHighlights: [],
    promptDigest: "",
    execution: {
      backend: "temporary_runtime_profile",
      profileId: normaliseStoredExecutionProfile(row.generator_key),
      readiness: "executable",
    },
    normalizedSpec: {
      version: "2026-04-18",
      title: row.title,
      summary: row.summary,
      category: row.category,
      topology: "serial",
      parameters: [],
      resources: [],
      graph: {
        inputs: { left: "input.left", right: "input.right" },
        nodes: [],
        outputs: { left: "input.left", right: "input.right" },
      },
      constraints: {
        maxLatencySamples: 0,
        cpuClass: "low",
        deterministic: true,
        externalResourcesRequired: false,
      },
      validationTargets: ["legacy_revision_parse"],
      notes: [],
    },
  })!;
  const manifest = parseJsonValue<Record<string, unknown>>(row.manifest_json, {});
  const specText = typeof manifest.specText === "string" ? manifest.specText : buildNormalizedSpecText(plan.normalizedSpec);
  return {
    id: row.id,
    title: row.title,
    summary: row.summary,
    category: row.category,
    plan: toPublicModuleDesignPlan(plan) ?? toPublicModuleDesignPlan({
      title: row.title,
      summary: row.summary,
      assistantMessage: row.summary,
      category: row.category,
      parameters: {},
      questions: [],
      tags: [],
      limitations: [],
      designHighlights: [],
      promptDigest: "",
      execution: {
        backend: "temporary_runtime_profile",
        profileId: normaliseStoredExecutionProfile(row.generator_key),
        readiness: "executable",
      },
      normalizedSpec: plan.normalizedSpec,
    })!,
    descriptorText: row.descriptor_text,
    specText,
    manifest,
    moduleBase64: row.wasm_base64,
    createdAt: row.created_at,
  };
}

async function buildSessionResponse(
  db: D1Database,
  session: ModuleGenerationSessionRow,
  options: { includeLatestRevision?: boolean } = {},
) {
  const [messages, revisions] = await Promise.all([
    listModuleGenerationMessages(db, session.id),
    listModuleGenerationRevisions(db, session.id),
  ]);

  const latestRevision = options.includeLatestRevision && session.latest_revision_id
    ? await findModuleGenerationRevision(db, session.id, session.latest_revision_id)
    : null;

  return {
    session: {
      id: session.id,
      status: session.status,
      title: session.title,
      summary: session.summary,
      nodeContext: parseJsonValue<ModuleNodeContext>(session.source_context_json, {}),
      currentPlan: toPublicModuleDesignPlan(ensureModuleExecutionPlan(parseJsonValue<ModuleExecutionPlan | null>(session.current_plan_json, null))),
      latestRevisionId: session.latest_revision_id,
      createdAt: session.created_at,
      updatedAt: session.updated_at,
      messages: messages.map(toMessageResponse),
      revisions: revisions.map(toRevisionSummary),
    },
    ...(latestRevision ? { latestRevision: toRevisionDetail(latestRevision) } : {}),
  };
}

function buildRevisionManifest(plan: ModuleDesignPlan, artifact: GeneratedModuleArtifact): Record<string, unknown> {
  const executionPlan = ensureModuleExecutionPlan(plan as ModuleExecutionPlan);
  return {
    fileName: artifact.fileName,
    promptDigest: plan.promptDigest,
    tags: plan.tags,
    designHighlights: plan.designHighlights,
    specText: artifact.specText,
    normalizedSpecVersion: executionPlan?.normalizedSpec.version,
    normalizedSpec: executionPlan?.normalizedSpec,
    generationBackend: artifact.generationBackend,
    descriptorSummary: artifact.descriptorSummary,
    validation: artifact.validation,
    defaultParams: artifact.defaultParams,
  };
}

export function moduleSessionRoutes() {
  const app = new Hono<{ Bindings: Env; Variables: Variables }>();

  app.use("*", optionalAuth);

  app.post("/", async (c) => {
    const body = (await safeJson<CreateSessionBody>(c.req.raw)) ?? {};
    const nodeContext = parseNodeContext(body.nodeContext);
    const prompt = normaliseText(body.prompt, 480);
    const auth = c.get("auth");

    const session = await createModuleGenerationSession(c.env.DB, {
      ownerUserId: auth?.userId ?? null,
      status: prompt ? "ready" : "draft",
      title: nodeContext.existingCustomEffectName || nodeContext.currentModuleName || nodeContext.nodeLabel || "Custom Effect Designer",
      summary: prompt ? null : "Describe the effect character you want and generate a module when the design feels right.",
      sourceContextJson: JSON.stringify(nodeContext),
      currentPlanJson: "null",
    });

    if (prompt) {
      const plan = await analyseModulePrompt(c.env, [prompt], nodeContext);
      await addModuleGenerationMessage(c.env.DB, {
        sessionId: session.id,
        role: "user",
        content: prompt,
      });
      await addModuleGenerationMessage(c.env.DB, {
        sessionId: session.id,
        role: "assistant",
        content: plan.assistantMessage,
        planJson: JSON.stringify(plan),
      });
      await updateModuleGenerationSessionPlan(c.env.DB, session.id, {
        status: "ready",
        title: plan.title,
        summary: plan.summary,
        currentPlanJson: JSON.stringify(plan),
      });
    } else {
      await addModuleGenerationMessage(c.env.DB, {
        sessionId: session.id,
        role: "assistant",
        content: createModuleDesignerStarterMessage(nodeContext),
      });
    }

    const updated = await findModuleGenerationSession(c.env.DB, session.id);
    if (!updated) {
      return fail(c, "SESSION_CREATE_FAILED", "Failed to create module session", 500);
    }

    return ok(c, await buildSessionResponse(c.env.DB, updated));
  });

  app.get("/:sessionId", async (c) => {
    const sessionId = normaliseText(c.req.param("sessionId"), 80);
    if (!sessionId) {
      return fail(c, "INVALID_SESSION_ID", "Missing session id", 400);
    }
    const session = await findModuleGenerationSession(c.env.DB, sessionId);
    if (!session) {
      return fail(c, "SESSION_NOT_FOUND", "Module session not found", 404);
    }
    const accessFailure = enforceSessionAccess(c, session);
    if (accessFailure) {
      return accessFailure;
    }
    return ok(c, await buildSessionResponse(c.env.DB, session, { includeLatestRevision: true }));
  });

  app.post("/:sessionId/messages", async (c) => {
    const sessionId = normaliseText(c.req.param("sessionId"), 80);
    if (!sessionId) {
      return fail(c, "INVALID_SESSION_ID", "Missing session id", 400);
    }

    const session = await findModuleGenerationSession(c.env.DB, sessionId);
    if (!session) {
      return fail(c, "SESSION_NOT_FOUND", "Module session not found", 404);
    }
    const accessFailure = enforceSessionAccess(c, session);
    if (accessFailure) {
      return accessFailure;
    }

    const body = (await safeJson<CreateMessageBody>(c.req.raw)) ?? {};
    const content = normaliseText(body.content, 480);
    if (!content) {
      return fail(c, "INVALID_MESSAGE", "Message content is required", 400);
    }

    await addModuleGenerationMessage(c.env.DB, {
      sessionId,
      role: "user",
      content,
    });

    const existingMessages = await listModuleGenerationMessages(c.env.DB, sessionId);
    const userPrompts = existingMessages.filter((message) => message.role === "user").map((message) => message.content);
    const nodeContext = parseJsonValue<ModuleNodeContext>(session.source_context_json, {});
    const plan = await analyseModulePrompt(c.env, userPrompts, nodeContext);

    await addModuleGenerationMessage(c.env.DB, {
      sessionId,
      role: "assistant",
      content: plan.assistantMessage,
      planJson: JSON.stringify(plan),
    });

    await updateModuleGenerationSessionPlan(c.env.DB, sessionId, {
      status: "ready",
      title: plan.title,
      summary: plan.summary,
      currentPlanJson: JSON.stringify(plan),
      latestRevisionId: session.latest_revision_id,
    });

    const updated = await findModuleGenerationSession(c.env.DB, sessionId);
    if (!updated) {
      return fail(c, "SESSION_UPDATE_FAILED", "Failed to update module session", 500);
    }

    return ok(c, await buildSessionResponse(c.env.DB, updated, { includeLatestRevision: true }));
  });

  app.post("/:sessionId/generate", async (c) => {
    const sessionId = normaliseText(c.req.param("sessionId"), 80);
    if (!sessionId) {
      return fail(c, "INVALID_SESSION_ID", "Missing session id", 400);
    }

    const session = await findModuleGenerationSession(c.env.DB, sessionId);
    if (!session) {
      return fail(c, "SESSION_NOT_FOUND", "Module session not found", 404);
    }
    const accessFailure = enforceSessionAccess(c, session);
    if (accessFailure) {
      return accessFailure;
    }

    const plan = ensureModuleExecutionPlan(parseJsonValue<ModuleExecutionPlan | null>(session.current_plan_json, null));
    if (!plan) {
      return fail(c, "PLAN_NOT_READY", "Add a design prompt before generating a module", 400);
    }

    const buildability = canBuildGeneratedModule(plan);
    if (!buildability.ok) {
      return fail(c, "PLAN_NOT_EXECUTABLE", buildability.reason ?? "This design brief cannot yet be generated as an executable module", 409);
    }

    const artifact = buildGeneratedModuleArtifact(plan);
    const manifest = buildRevisionManifest(plan, artifact);

    const revision = await createModuleGenerationRevision(c.env.DB, {
      sessionId,
      title: plan.title,
      summary: plan.summary,
      generatorKey: plan.execution.profileId,
      category: plan.category,
      planJson: JSON.stringify(plan),
      descriptorText: artifact.descriptorText,
      manifestJson: JSON.stringify(manifest),
      wasmBase64: artifact.moduleBase64,
    });

    await addModuleGenerationMessage(c.env.DB, {
      sessionId,
      role: "assistant",
      content: `Generated ${plan.title}. Review it, then use it on the node or save it to My Custom Effects.`,
      planJson: JSON.stringify(plan),
    });

    await updateModuleGenerationSessionPlan(c.env.DB, sessionId, {
      status: "generated",
      title: plan.title,
      summary: plan.summary,
      currentPlanJson: JSON.stringify(plan),
      latestRevisionId: revision.id,
    });

    const updated = await findModuleGenerationSession(c.env.DB, sessionId);
    if (!updated) {
      return fail(c, "SESSION_UPDATE_FAILED", "Failed to update module session", 500);
    }

    return ok(c, await buildSessionResponse(c.env.DB, updated, { includeLatestRevision: true }));
  });

  app.get("/:sessionId/revisions/:revisionId", async (c) => {
    const sessionId = normaliseText(c.req.param("sessionId"), 80);
    const revisionId = normaliseText(c.req.param("revisionId"), 80);
    if (!sessionId || !revisionId) {
      return fail(c, "INVALID_REVISION_ID", "Missing revision id", 400);
    }

    const revision = await findModuleGenerationRevision(c.env.DB, sessionId, revisionId);
    if (!revision) {
      return fail(c, "REVISION_NOT_FOUND", "Module revision not found", 404);
    }

    const session = await findModuleGenerationSession(c.env.DB, sessionId);
    if (!session) {
      return fail(c, "SESSION_NOT_FOUND", "Module session not found", 404);
    }
    const accessFailure = enforceSessionAccess(c, session);
    if (accessFailure) {
      return accessFailure;
    }

    return ok(c, { revision: toRevisionDetail(revision) });
  });

  return app;
}