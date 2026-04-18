type UserRow = {
  id: string;
  email: string;
  role: "user" | "curator" | "admin";
};

type SessionRow = {
  id: string;
  user_id: string;
  expires_at: string;
  revoked_at: string | null;
};

type ModuleGenerationSessionRow = {
  id: string;
  owner_user_id: string | null;
  status: string;
  title: string | null;
  summary: string | null;
  source_context_json: string;
  current_plan_json: string;
  latest_revision_id: string | null;
  created_at: string;
  updated_at: string;
};

type ModuleGenerationMessageRow = {
  id: string;
  session_id: string;
  role: "user" | "assistant";
  content: string;
  plan_json: string | null;
  created_at: string;
};

type ModuleGenerationRevisionRow = {
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

type D1Result<T = unknown> = {
  results?: T[];
  success: boolean;
  meta: Record<string, unknown>;
};

function nowIso(counter: number): string {
  return new Date(Date.UTC(2026, 3, 18, 12, 0, counter)).toISOString();
}

class FakePreparedStatement {
  private boundValues: unknown[] = [];

  constructor(
    private readonly db: FakeD1Database,
    private readonly sql: string,
  ) {}

  bind(...values: unknown[]): FakePreparedStatement {
    this.boundValues = values;
    return this;
  }

  async first<T>(): Promise<T | null> {
    return this.db.first<T>(this.sql, this.boundValues);
  }

  async all<T>(): Promise<D1Result<T>> {
    return this.db.all<T>(this.sql, this.boundValues);
  }

  async run(): Promise<D1Result> {
    return this.db.run(this.sql, this.boundValues);
  }
}

export class FakeD1Database {
  private timeCounter = 0;
  private readonly users = new Map<string, UserRow>();
  private readonly sessions = new Map<string, SessionRow>();
  private readonly moduleSessions = new Map<string, ModuleGenerationSessionRow>();
  private readonly moduleMessages = new Map<string, ModuleGenerationMessageRow>();
  private readonly moduleRevisions = new Map<string, ModuleGenerationRevisionRow>();

  prepare(sql: string): FakePreparedStatement {
    return new FakePreparedStatement(this, sql);
  }

  seedUser(user: UserRow): void {
    this.users.set(user.id, user);
  }

  seedSession(session: SessionRow): void {
    this.sessions.set(session.id, session);
  }

  private timestamp(): string {
    return nowIso(this.timeCounter++);
  }

  async first<T>(sql: string, values: unknown[]): Promise<T | null> {
    const normalized = normalizeSql(sql);

    if (normalized.startsWith("SELECT id, email, role FROM users WHERE id = ?")) {
      return (this.users.get(String(values[0])) ?? null) as T | null;
    }

    if (normalized.startsWith("SELECT id, user_id, expires_at, revoked_at FROM sessions WHERE id = ?")) {
      return (this.sessions.get(String(values[0])) ?? null) as T | null;
    }

    if (normalized.includes("FROM module_generation_sessions") && normalized.includes("WHERE id = ?")) {
      return (this.moduleSessions.get(String(values[0])) ?? null) as T | null;
    }

    if (normalized.includes("FROM module_generation_messages") && normalized.includes("WHERE id = ?")) {
      return ([...this.moduleMessages.values()].find((row) => row.id === String(values[0])) ?? null) as T | null;
    }

    if (normalized.includes("FROM module_generation_revisions") && normalized.includes("WHERE session_id = ? AND id = ?")) {
      return ([...this.moduleRevisions.values()].find((row) => row.session_id === String(values[0]) && row.id === String(values[1])) ?? null) as T | null;
    }

    throw new Error(`Unsupported first query in FakeD1Database: ${normalized}`);
  }

  async all<T>(sql: string, values: unknown[]): Promise<D1Result<T>> {
    const normalized = normalizeSql(sql);

    if (normalized.includes("FROM module_generation_messages") && normalized.includes("WHERE session_id = ?")) {
      const sessionId = String(values[0]);
      const results = [...this.moduleMessages.values()]
        .filter((row) => row.session_id === sessionId)
        .sort((left, right) => compareRows(left.created_at, left.id, right.created_at, right.id));
      return { results: results as T[], success: true, meta: {} };
    }

    if (normalized.includes("FROM module_generation_revisions") && normalized.includes("WHERE session_id = ?")) {
      const sessionId = String(values[0]);
      const results = [...this.moduleRevisions.values()]
        .filter((row) => row.session_id === sessionId)
        .sort((left, right) => compareRows(right.created_at, right.id, left.created_at, left.id));
      return { results: results as T[], success: true, meta: {} };
    }

    throw new Error(`Unsupported all query in FakeD1Database: ${normalized}`);
  }

  async run(sql: string, values: unknown[]): Promise<D1Result> {
    const normalized = normalizeSql(sql);

    if (normalized.startsWith("INSERT INTO module_generation_sessions")) {
      const createdAt = this.timestamp();
      const row: ModuleGenerationSessionRow = {
        id: String(values[0]),
        owner_user_id: values[1] == null ? null : String(values[1]),
        status: String(values[2]),
        title: values[3] == null ? null : String(values[3]),
        summary: values[4] == null ? null : String(values[4]),
        source_context_json: String(values[5]),
        current_plan_json: String(values[6]),
        latest_revision_id: null,
        created_at: createdAt,
        updated_at: createdAt,
      };
      this.moduleSessions.set(row.id, row);
      return { success: true, meta: {} };
    }

    if (normalized.startsWith("UPDATE module_generation_sessions SET status = ?")) {
      const row = this.moduleSessions.get(String(values[5]));
      if (!row) {
        throw new Error(`Unknown module generation session: ${String(values[5])}`);
      }
      row.status = String(values[0]);
      row.title = values[1] == null ? null : String(values[1]);
      row.summary = values[2] == null ? null : String(values[2]);
      row.current_plan_json = String(values[3]);
      row.latest_revision_id = values[4] == null ? null : String(values[4]);
      row.updated_at = this.timestamp();
      return { success: true, meta: {} };
    }

    if (normalized.startsWith("INSERT INTO module_generation_messages")) {
      const row: ModuleGenerationMessageRow = {
        id: String(values[0]),
        session_id: String(values[1]),
        role: String(values[2]) as ModuleGenerationMessageRow["role"],
        content: String(values[3]),
        plan_json: values[4] == null ? null : String(values[4]),
        created_at: this.timestamp(),
      };
      this.moduleMessages.set(row.id, row);
      return { success: true, meta: {} };
    }

    if (normalized.startsWith("INSERT INTO module_generation_revisions")) {
      const row: ModuleGenerationRevisionRow = {
        id: String(values[0]),
        session_id: String(values[1]),
        title: String(values[2]),
        summary: String(values[3]),
        generator_key: String(values[4]),
        category: String(values[5]),
        plan_json: String(values[6]),
        descriptor_text: String(values[7]),
        manifest_json: String(values[8]),
        wasm_base64: String(values[9]),
        created_at: this.timestamp(),
      };
      this.moduleRevisions.set(row.id, row);
      return { success: true, meta: {} };
    }

    if (normalized.startsWith("UPDATE sessions SET last_seen_at = CURRENT_TIMESTAMP WHERE id = ?")) {
      return { success: true, meta: {} };
    }

    throw new Error(`Unsupported run query in FakeD1Database: ${normalized}`);
  }
}

function normalizeSql(sql: string): string {
  return sql.replace(/\s+/g, " ").trim();
}

function compareRows(leftDate: string, leftId: string, rightDate: string, rightId: string): number {
  const dateComparison = leftDate.localeCompare(rightDate);
  if (dateComparison !== 0) {
    return dateComparison;
  }
  return leftId.localeCompare(rightId);
}

export function createFakeD1Database(): D1Database {
  return new FakeD1Database() as unknown as D1Database;
}

export function asFakeD1Database(db: D1Database): FakeD1Database {
  return db as unknown as FakeD1Database;
}