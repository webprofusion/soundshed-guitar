import { Hono } from "hono";
import { fail, ok, safeJson } from "../lib/http";
import {
  acceptToneSharingPublishConsent,
  getToneSharingPublishConsent,
  TONE_SHARING_PUBLISH_CONSENT_VERSION,
} from "../lib/shareConsent";
import { requireAuth } from "../middleware/session";
import { Env } from "../types/env";

type AcceptConsentBody = {
  version?: number;
};

export function shareConsentRoutes() {
  const app = new Hono<{ Bindings: Env; Variables: { auth?: { userId: string; email: string; role: string; sessionId: string } } }>();

  app.get("/status", requireAuth, async (c) => {
    const auth = c.get("auth");
    if (!auth) {
      return fail(c, "UNAUTHORIZED", "Authentication required", 401);
    }

    const consent = await getToneSharingPublishConsent(c.env.DB, auth.userId);
    return ok(c, {
      consentType: "tone_sharing_publish",
      version: TONE_SHARING_PUBLISH_CONSENT_VERSION,
      accepted: Boolean(consent && Number(consent.version) >= TONE_SHARING_PUBLISH_CONSENT_VERSION),
      acceptedAt: consent?.accepted_at ?? null,
    });
  });

  app.post("/accept", requireAuth, async (c) => {
    const auth = c.get("auth");
    if (!auth) {
      return fail(c, "UNAUTHORIZED", "Authentication required", 401);
    }

    const body = await safeJson<AcceptConsentBody>(c.req.raw);
    const requestedVersion = Number(body?.version ?? TONE_SHARING_PUBLISH_CONSENT_VERSION);
    if (requestedVersion !== TONE_SHARING_PUBLISH_CONSENT_VERSION) {
      return fail(c, "INVALID_CONSENT_VERSION", "Consent version is out of date", 409);
    }

    await acceptToneSharingPublishConsent(c.env.DB, auth.userId);
    const consent = await getToneSharingPublishConsent(c.env.DB, auth.userId);
    return ok(c, {
      consentType: "tone_sharing_publish",
      version: TONE_SHARING_PUBLISH_CONSENT_VERSION,
      accepted: true,
      acceptedAt: consent?.accepted_at ?? null,
    });
  });

  return app;
}