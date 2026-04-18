import { randomId, toIso } from "./utils";

export type UserRow = {
  id: string;
  email: string;
  role: "user" | "curator" | "admin";
};

export type SessionRow = {
  id: string;
  user_id: string;
  expires_at: string;
  revoked_at: string | null;
};

export type ModuleGenerationSessionStatus = "draft" | "refining" | "ready" | "generated" | "error";

export type ModuleGenerationSessionRow = {
  id: string;
  owner_user_id: string | null;
  status: ModuleGenerationSessionStatus;
  title: string | null;
  summary: string | null;
  source_context_json: string;
  current_plan_json: string;
  latest_revision_id: string | null;
  created_at: string;
  updated_at: string;
};

export type ModuleGenerationMessageRow = {
  id: string;
  session_id: string;
  role: "user" | "assistant";
  content: string;
  plan_json: string | null;
  created_at: string;
};

export type ModuleGenerationRevisionRow = {
  id: string;
  session_id: string;
  title: string;
  summary: string;
  generator_key: string;
  category: string;
  plan_json: string;
  descriptor_text: string;
  manifest_json: string;
  wasm_base64: string;
  created_at: string;
};

export async function findUserByEmail(db: D1Database, email: string): Promise<UserRow | null> {
  const result = await db.prepare("SELECT id, email, role FROM users WHERE email = ?").bind(email).first<UserRow>();
  return result ?? null;
}

export async function findUserById(db: D1Database, userId: string): Promise<UserRow | null> {
  const result = await db.prepare("SELECT id, email, role FROM users WHERE id = ?").bind(userId).first<UserRow>();
  return result ?? null;
}

export async function createUser(db: D1Database, email: string): Promise<UserRow> {
  const userId = randomId("usr");
  await db
    .prepare("INSERT INTO users (id, email, role, created_at, updated_at) VALUES (?, ?, 'user', CURRENT_TIMESTAMP, CURRENT_TIMESTAMP)")
    .bind(userId, email)
    .run();
  return { id: userId, email, role: "user" };
}

export async function upsertUserByEmail(db: D1Database, email: string): Promise<UserRow> {
  const existing = await findUserByEmail(db, email);
  if (existing) return existing;
  return createUser(db, email);
}

export async function insertAuthToken(db: D1Database, email: string, tokenHash: string, expiresAt: Date): Promise<string> {
  const id = randomId("atk");
  await db
    .prepare("INSERT INTO auth_tokens (id, email, token_hash, purpose, expires_at, created_at) VALUES (?, ?, ?, 'login', ?, CURRENT_TIMESTAMP)")
    .bind(id, email, tokenHash, toIso(expiresAt))
    .run();
  return id;
}

export async function useAuthToken(
  db: D1Database,
  email: string,
  tokenHash: string,
  nowIso: string
): Promise<boolean> {
  const token = await db
    .prepare(
      "SELECT id FROM auth_tokens WHERE email = ? AND token_hash = ? AND used_at IS NULL AND expires_at > ? ORDER BY created_at DESC LIMIT 1"
    )
    .bind(email, tokenHash, nowIso)
    .first<{ id: string }>();

  if (!token) return false;

  await db
    .prepare("UPDATE auth_tokens SET used_at = CURRENT_TIMESTAMP WHERE id = ? AND used_at IS NULL")
    .bind(token.id)
    .run();

  return true;
}

export async function createSession(db: D1Database, userId: string, expiresAt: Date): Promise<string> {
  const sessionId = randomId("ses");
  await db
    .prepare(
      "INSERT INTO sessions (id, user_id, expires_at, created_at, last_seen_at) VALUES (?, ?, ?, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP)"
    )
    .bind(sessionId, userId, toIso(expiresAt))
    .run();
  return sessionId;
}

export async function findSession(db: D1Database, sessionId: string): Promise<SessionRow | null> {
  const session = await db
    .prepare("SELECT id, user_id, expires_at, revoked_at FROM sessions WHERE id = ?")
    .bind(sessionId)
    .first<SessionRow>();
  return session ?? null;
}

export async function revokeSession(db: D1Database, sessionId: string): Promise<void> {
  await db.prepare("UPDATE sessions SET revoked_at = CURRENT_TIMESTAMP WHERE id = ?").bind(sessionId).run();
}

export async function touchSession(db: D1Database, sessionId: string): Promise<void> {
  await db.prepare("UPDATE sessions SET last_seen_at = CURRENT_TIMESTAMP WHERE id = ?").bind(sessionId).run();
}

export async function createModuleGenerationSession(
  db: D1Database,
  input: {
    ownerUserId?: string | null;
    status?: ModuleGenerationSessionStatus;
    title?: string | null;
    summary?: string | null;
    sourceContextJson?: string;
    currentPlanJson?: string;
  }
): Promise<ModuleGenerationSessionRow> {
  const id = randomId("mgs");
  const row: ModuleGenerationSessionRow = {
    id,
    owner_user_id: input.ownerUserId ?? null,
    status: input.status ?? "draft",
    title: input.title ?? null,
    summary: input.summary ?? null,
    source_context_json: input.sourceContextJson ?? "{}",
    current_plan_json: input.currentPlanJson ?? "{}",
    latest_revision_id: null,
    created_at: toIso(new Date()),
    updated_at: toIso(new Date()),
  };

  await db.prepare(
    `INSERT INTO module_generation_sessions (
      id, owner_user_id, status, title, summary, source_context_json, current_plan_json, latest_revision_id, created_at, updated_at
    ) VALUES (?, ?, ?, ?, ?, ?, ?, NULL, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP)`
  )
    .bind(
      row.id,
      row.owner_user_id,
      row.status,
      row.title,
      row.summary,
      row.source_context_json,
      row.current_plan_json,
    )
    .run();

  const created = await findModuleGenerationSession(db, id);
  if (!created) {
    throw new Error("Failed to create module generation session");
  }
  return created;
}

export async function findModuleGenerationSession(
  db: D1Database,
  sessionId: string
): Promise<ModuleGenerationSessionRow | null> {
  const row = await db.prepare(
    `SELECT
       id,
       owner_user_id,
       status,
       title,
       summary,
       source_context_json,
       current_plan_json,
       latest_revision_id,
       created_at,
       updated_at
     FROM module_generation_sessions
     WHERE id = ?`
  ).bind(sessionId).first<ModuleGenerationSessionRow>();
  return row ?? null;
}

export async function updateModuleGenerationSessionPlan(
  db: D1Database,
  sessionId: string,
  input: {
    status: ModuleGenerationSessionStatus;
    title: string | null;
    summary: string | null;
    currentPlanJson: string;
    latestRevisionId?: string | null;
  }
): Promise<void> {
  await db.prepare(
    `UPDATE module_generation_sessions
     SET status = ?,
         title = ?,
         summary = ?,
         current_plan_json = ?,
         latest_revision_id = ?,
         updated_at = CURRENT_TIMESTAMP
     WHERE id = ?`
  )
    .bind(
      input.status,
      input.title,
      input.summary,
      input.currentPlanJson,
      input.latestRevisionId ?? null,
      sessionId,
    )
    .run();
}

export async function addModuleGenerationMessage(
  db: D1Database,
  input: {
    sessionId: string;
    role: "user" | "assistant";
    content: string;
    planJson?: string | null;
  }
): Promise<ModuleGenerationMessageRow> {
  const id = randomId("mgm");
  await db.prepare(
    `INSERT INTO module_generation_messages (id, session_id, role, content, plan_json, created_at)
     VALUES (?, ?, ?, ?, ?, CURRENT_TIMESTAMP)`
  )
    .bind(id, input.sessionId, input.role, input.content, input.planJson ?? null)
    .run();

  const created = await db.prepare(
    `SELECT id, session_id, role, content, plan_json, created_at
     FROM module_generation_messages
     WHERE id = ?`
  ).bind(id).first<ModuleGenerationMessageRow>();

  if (!created) {
    throw new Error("Failed to create module generation message");
  }
  return created;
}

export async function listModuleGenerationMessages(
  db: D1Database,
  sessionId: string
): Promise<ModuleGenerationMessageRow[]> {
  const result = await db.prepare(
    `SELECT id, session_id, role, content, plan_json, created_at
     FROM module_generation_messages
     WHERE session_id = ?
     ORDER BY created_at ASC, id ASC`
  ).bind(sessionId).all<ModuleGenerationMessageRow>();
  return result.results ?? [];
}

export async function createModuleGenerationRevision(
  db: D1Database,
  input: {
    sessionId: string;
    title: string;
    summary: string;
    generatorKey: string;
    category: string;
    planJson: string;
    descriptorText: string;
    manifestJson: string;
    wasmBase64: string;
  }
): Promise<ModuleGenerationRevisionRow> {
  const id = randomId("mgr");
  await db.prepare(
    `INSERT INTO module_generation_revisions (
      id, session_id, title, summary, archetype, category, plan_json, descriptor_text, manifest_json, wasm_base64, created_at
     ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP)`
  )
    .bind(
      id,
      input.sessionId,
      input.title,
      input.summary,
      input.generatorKey,
      input.category,
      input.planJson,
      input.descriptorText,
      input.manifestJson,
      input.wasmBase64,
    )
    .run();

  const created = await findModuleGenerationRevision(db, input.sessionId, id);
  if (!created) {
    throw new Error("Failed to create module generation revision");
  }
  return created;
}

export async function listModuleGenerationRevisions(
  db: D1Database,
  sessionId: string
): Promise<ModuleGenerationRevisionRow[]> {
  const result = await db.prepare(
    `SELECT id, session_id, title, summary, archetype AS generator_key, category, plan_json, descriptor_text, manifest_json, wasm_base64, created_at
     FROM module_generation_revisions
     WHERE session_id = ?
     ORDER BY created_at DESC, id DESC`
  ).bind(sessionId).all<ModuleGenerationRevisionRow>();
  return result.results ?? [];
}

export async function findModuleGenerationRevision(
  db: D1Database,
  sessionId: string,
  revisionId: string
): Promise<ModuleGenerationRevisionRow | null> {
  const row = await db.prepare(
    `SELECT id, session_id, title, summary, archetype AS generator_key, category, plan_json, descriptor_text, manifest_json, wasm_base64, created_at
     FROM module_generation_revisions
     WHERE session_id = ? AND id = ?`
  ).bind(sessionId, revisionId).first<ModuleGenerationRevisionRow>();
  return row ?? null;
}
