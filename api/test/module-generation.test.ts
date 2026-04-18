import { describe, expect, it } from "vitest";
import {
  analyseModulePrompt,
  buildGeneratedModuleArtifact,
  canBuildGeneratedModule,
} from "../src/lib/module-generation";
import type { Env } from "../src/types/env";

function createEnv(): Env {
  return {
    DB: {} as D1Database,
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

describe("module generation analysis", () => {
  it("builds an executable artifact for supported spatial-motion prompts", async () => {
    const plan = await analyseModulePrompt(createEnv(), ["make this a wider stereo effect with gentle motion"], {
      nodeId: "node-1",
      nodeLabel: "Custom Effect",
      nodeCategory: "utility",
    });

    expect(plan.execution.profileId).toBe("spatial_motion");
    expect(plan.execution.readiness).toBe("executable");
    expect(canBuildGeneratedModule(plan)).toEqual({ ok: true });

    const artifact = buildGeneratedModuleArtifact(plan);
    expect(artifact.moduleBase64.length).toBeGreaterThan(32);
    expect(artifact.descriptorText).toContain("effect.name=");
    expect(artifact.descriptorText).toContain("param.0.id=depth");
    expect(artifact.specText).toContain("Topology: stereo_field");
    expect(artifact.validation.parameterCount).toBeGreaterThan(0);
  });

  it("marks unsupported prompts as plan-only instead of generating junk artifacts", async () => {
    const plan = await analyseModulePrompt(createEnv(), ["make a long feedback delay with a blooming reverb tail and EQ shaping"], {
      nodeId: "node-2",
      nodeLabel: "Custom Effect",
    });

    expect(plan.execution.readiness).toBe("plan_only");
    expect(plan.limitations.join(" ").toLowerCase()).toContain("delay");
    expect(plan.limitations.join(" ").toLowerCase()).toContain("reverb");

    const buildability = canBuildGeneratedModule(plan);
    expect(buildability.ok).toBe(false);
    expect(buildability.reason?.toLowerCase()).toContain("cannot truthfully execute");
  });
});