import { Hono } from "hono";
import { cors } from "hono/cors";
import { appRoutes } from "./routes/app";
import { authRoutes } from "./routes/auth";
import { discoveryRoutes } from "./routes/discovery";
import { fail } from "./lib/http";
import { healthRoutes } from "./routes/health";
import { itemRoutes } from "./routes/items";
import { moduleSessionRoutes } from "./routes/module-sessions";
import { packRoutes } from "./routes/packs";
import { shareConsentRoutes } from "./routes/share-consent";
import { corsProxyRoutes } from "./routes/corsproxy";
import { toneAdvisorRoutes } from "./routes/tone-advisor";
import { uploadRoutes } from "./routes/uploads";
import { Env } from "./types/env";

type Variables = {
  auth?: { userId: string; email: string; role: string; sessionId: string };
};

const app = new Hono<{ Bindings: Env; Variables: Variables }>();

app.use(
  "*",
  cors({
    origin: (origin) => origin || "*",
    credentials: true,
    allowMethods: ["GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"],
    exposeHeaders: ["content-disposition", "content-length", "content-type"],
    maxAge: 86400
  })
);

app.route("/", healthRoutes());
app.route("/", appRoutes());
app.route("/v1/auth", authRoutes());
app.route("/v1", discoveryRoutes());
app.route("/v1/module-sessions", moduleSessionRoutes());
app.route("/v1/items", itemRoutes());
app.route("/v1/packs", packRoutes());
app.route("/v1/share-consent", shareConsentRoutes());
app.route("/v1/uploads", uploadRoutes());
app.route("/v1", toneAdvisorRoutes());
app.route("/v1", corsProxyRoutes());

app.notFound((c) => fail(c, "NOT_FOUND", "Route not found", 404));

app.onError((error, c) => {
  return fail(c, "INTERNAL_ERROR", error.message || "Unexpected error", 500);
});

export default app;
