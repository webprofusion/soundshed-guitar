import { Hono } from "hono";
import { describe, expect, it } from "vitest";
import { moduleSessionRoutes } from "../src/routes/module-sessions";
import type { Env } from "../src/types/env";
import { asFakeD1Database, createFakeD1Database } from "./helpers/fakeD1";

function createTestEnv(): Env {
  return {
    DB: createFakeD1Database(),
    ASSETS: {} as R2Bucket,
    AI: {
      run: async () => {
        throw new Error("AI disabled in tests");
      },
    } as unknown as Ai,
    COOKIE_NAME: "soundshed_session",
    ENVIRONMENT: "development",
    SESSION_TTL_SECONDS: "3600",
    MAGIC_LINK_TTL_SECONDS: "900",
    SENDGRID_FROM_EMAIL: "test@example.com",
    SENDGRID_FROM_NAME: "Test",
  };
}

function createApp(env: Env) {
  const app = new Hono<{ Bindings: Env; Variables: { auth?: { userId: string; email: string; role: string; sessionId: string } } }>();
  app.route("/v1/module-sessions", moduleSessionRoutes());
  return {
    app,
    env,
    db: asFakeD1Database(env.DB),
  };
}

async function readJson(response: Response) {
  return await response.json() as {
    ok: boolean;
    data?: {
      session: {
        id: string;
        status: string;
        currentPlan: { title: string; executionReadiness?: "executable" | "plan_only" } | null;
        latestRevisionId: string | null;
        messages: Array<{ role: "user" | "assistant"; content: string }>;
      };
      latestRevision?: {
        id: string;
        title: string;
        descriptorText: string;
        specText: string;
        manifest: Record<string, unknown>;
        moduleBase64: string;
      };
    };
    error?: { code: string; message: string };
  };
}

describe("module session routes", () => {
  it("creates, refines, and generates a usable module revision for supported prompts", async () => {
    const { app, env } = createApp(createTestEnv());

    const createResponse = await app.request("/v1/module-sessions", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        nodeContext: {
          nodeId: "node-1",
          nodeLabel: "Custom Effect",
          nodeCategory: "utility",
        },
      }),
    }, env);
    const createJson = await readJson(createResponse);
    expect(createResponse.status).toBe(200);
    expect(createJson.ok).toBe(true);
    expect(createJson.data?.session.status).toBe("draft");
    expect(createJson.data?.session.messages[0]?.role).toBe("assistant");

    const sessionId = createJson.data?.session.id;
    expect(sessionId).toBeTruthy();

    const messageResponse = await app.request(`/v1/module-sessions/${sessionId}/messages`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ content: "make this a wider stereo effect with gentle motion" }),
    }, env);
    const messageJson = await readJson(messageResponse);
    expect(messageResponse.status).toBe(200);
    expect(messageJson.data?.session.currentPlan?.executionReadiness).toBe("executable");

    const generateResponse = await app.request(`/v1/module-sessions/${sessionId}/generate`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({}),
    }, env);
    const generateJson = await readJson(generateResponse);

    expect(generateResponse.status).toBe(200);
    expect(generateJson.data?.session.status).toBe("generated");
    expect(generateJson.data?.session.latestRevisionId).toBeTruthy();
    expect(generateJson.data?.latestRevision?.moduleBase64.length).toBeGreaterThan(32);
    expect(generateJson.data?.latestRevision?.descriptorText).toContain("effect.name=");
    expect(generateJson.data?.latestRevision?.specText).toContain("Processing Path:");
    expect(generateJson.data?.latestRevision?.manifest.validation).toBeTruthy();
  });

  it("returns PLAN_NOT_EXECUTABLE for prompts that exceed the current runtime backend", async () => {
    const { app, env } = createApp(createTestEnv());

    const createResponse = await app.request("/v1/module-sessions", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        nodeContext: {
          nodeId: "node-2",
          nodeLabel: "Custom Effect",
          nodeCategory: "utility",
        },
      }),
    }, env);
    const createJson = await readJson(createResponse);
    const sessionId = createJson.data?.session.id;

    const messageResponse = await app.request(`/v1/module-sessions/${sessionId}/messages`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ content: "make a long feedback delay with reverb and EQ shaping" }),
    }, env);
    const messageJson = await readJson(messageResponse);

    expect(messageResponse.status).toBe(200);
    expect(messageJson.data?.session.currentPlan?.executionReadiness).toBe("plan_only");

    const generateResponse = await app.request(`/v1/module-sessions/${sessionId}/generate`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({}),
    }, env);
    const generateJson = await readJson(generateResponse);

    expect(generateResponse.status).toBe(409);
    expect(generateJson.ok).toBe(false);
    expect(generateJson.error?.code).toBe("PLAN_NOT_EXECUTABLE");
  });
});