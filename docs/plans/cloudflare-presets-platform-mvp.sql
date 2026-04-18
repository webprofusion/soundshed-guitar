PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS users (
  id TEXT PRIMARY KEY,
  email TEXT NOT NULL UNIQUE,
  display_name TEXT,
  role TEXT NOT NULL DEFAULT 'user' CHECK (role IN ('user', 'curator', 'admin')),
  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS sessions (
  id TEXT PRIMARY KEY,
  user_id TEXT NOT NULL,
  expires_at TEXT NOT NULL,
  revoked_at TEXT,
  ip_hash TEXT,
  user_agent_hash TEXT,
  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  last_seen_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  FOREIGN KEY (user_id) REFERENCES users(id)
);

CREATE TABLE IF NOT EXISTS auth_tokens (
  id TEXT PRIMARY KEY,
  email TEXT NOT NULL,
  token_hash TEXT NOT NULL UNIQUE,
  purpose TEXT NOT NULL DEFAULT 'login' CHECK (purpose IN ('login')),
  expires_at TEXT NOT NULL,
  used_at TEXT,
  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS creator_profiles (
  user_id TEXT PRIMARY KEY,
  handle TEXT UNIQUE,
  bio TEXT,
  avatar_asset_id TEXT,
  verified INTEGER NOT NULL DEFAULT 0,
  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  FOREIGN KEY (user_id) REFERENCES users(id)
);

CREATE TABLE IF NOT EXISTS assets (
  id TEXT PRIMARY KEY,
  owner_user_id TEXT,
  r2_key TEXT NOT NULL UNIQUE,
  kind TEXT NOT NULL CHECK (kind IN ('item_payload', 'item_manifest', 'pack_zip', 'thumbnail', 'preview_audio')),
  mime_type TEXT NOT NULL,
  byte_size INTEGER NOT NULL,
  checksum_sha256 TEXT,
  uploaded_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  FOREIGN KEY (owner_user_id) REFERENCES users(id)
);

CREATE TABLE IF NOT EXISTS items (
  id TEXT PRIMARY KEY,
  creator_user_id TEXT NOT NULL,
  type TEXT NOT NULL CHECK (type IN ('preset', 'blend', 'layout', 'composite', 'combo')),
  title TEXT NOT NULL,
  moderation_status TEXT NOT NULL DEFAULT 'draft' CHECK (moderation_status IN ('draft', 'pending_review', 'approved', 'rejected')),
  config_json TEXT NOT NULL DEFAULT '{}',
  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  published_at TEXT,
  FOREIGN KEY (creator_user_id) REFERENCES users(id)
);

CREATE TABLE IF NOT EXISTS packs (
  id TEXT PRIMARY KEY,
  creator_user_id TEXT NOT NULL,
  title TEXT NOT NULL,
  moderation_status TEXT NOT NULL DEFAULT 'draft' CHECK (moderation_status IN ('draft', 'pending_review', 'approved', 'rejected')),
  config_json TEXT NOT NULL DEFAULT '{}',
  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  published_at TEXT,
  FOREIGN KEY (creator_user_id) REFERENCES users(id)
);

CREATE TABLE IF NOT EXISTS pack_items (
  pack_id TEXT NOT NULL,
  item_id TEXT NOT NULL,
  sort_order INTEGER NOT NULL,
  PRIMARY KEY (pack_id, item_id),
  FOREIGN KEY (pack_id) REFERENCES packs(id),
  FOREIGN KEY (item_id) REFERENCES items(id)
);

CREATE TABLE IF NOT EXISTS taxonomies (
  id TEXT PRIMARY KEY,
  type TEXT NOT NULL CHECK (type IN ('category', 'genre', 'artist', 'tag')),
  slug TEXT NOT NULL UNIQUE,
  label TEXT NOT NULL,
  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS item_taxonomies (
  item_id TEXT NOT NULL,
  taxonomy_id TEXT NOT NULL,
  PRIMARY KEY (item_id, taxonomy_id),
  FOREIGN KEY (item_id) REFERENCES items(id),
  FOREIGN KEY (taxonomy_id) REFERENCES taxonomies(id)
);

CREATE TABLE IF NOT EXISTS featured_rows (
  id TEXT PRIMARY KEY,
  slug TEXT NOT NULL UNIQUE,
  title TEXT NOT NULL,
  active INTEGER NOT NULL DEFAULT 1,
  sort_order INTEGER NOT NULL DEFAULT 0,
  created_by_user_id TEXT,
  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  FOREIGN KEY (created_by_user_id) REFERENCES users(id)
);

CREATE TABLE IF NOT EXISTS featured_row_items (
  row_id TEXT NOT NULL,
  item_id TEXT,
  pack_id TEXT,
  sort_order INTEGER NOT NULL,
  PRIMARY KEY (row_id, sort_order),
  FOREIGN KEY (row_id) REFERENCES featured_rows(id),
  FOREIGN KEY (item_id) REFERENCES items(id),
  FOREIGN KEY (pack_id) REFERENCES packs(id),
  CHECK ((item_id IS NOT NULL AND pack_id IS NULL) OR (item_id IS NULL AND pack_id IS NOT NULL))
);

CREATE TABLE IF NOT EXISTS favorites (
  user_id TEXT NOT NULL,
  item_id TEXT NOT NULL,
  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (user_id, item_id),
  FOREIGN KEY (user_id) REFERENCES users(id),
  FOREIGN KEY (item_id) REFERENCES items(id)
);

CREATE TABLE IF NOT EXISTS ratings (
  user_id TEXT NOT NULL,
  item_id TEXT NOT NULL,
  score INTEGER NOT NULL CHECK (score BETWEEN 1 AND 5),
  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (user_id, item_id),
  FOREIGN KEY (user_id) REFERENCES users(id),
  FOREIGN KEY (item_id) REFERENCES items(id)
);

CREATE TABLE IF NOT EXISTS downloads (
  id TEXT PRIMARY KEY,
  user_id TEXT,
  item_id TEXT,
  pack_id TEXT,
  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  ip_hash TEXT,
  FOREIGN KEY (user_id) REFERENCES users(id),
  FOREIGN KEY (item_id) REFERENCES items(id),
  FOREIGN KEY (pack_id) REFERENCES packs(id),
  CHECK ((item_id IS NOT NULL AND pack_id IS NULL) OR (item_id IS NULL AND pack_id IS NOT NULL))
);

CREATE TABLE IF NOT EXISTS reports (
  id TEXT PRIMARY KEY,
  reporter_user_id TEXT,
  item_id TEXT,
  pack_id TEXT,
  reason TEXT NOT NULL,
  details TEXT,
  status TEXT NOT NULL DEFAULT 'open' CHECK (status IN ('open', 'resolved', 'dismissed')),
  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  resolved_at TEXT,
  FOREIGN KEY (reporter_user_id) REFERENCES users(id),
  FOREIGN KEY (item_id) REFERENCES items(id),
  FOREIGN KEY (pack_id) REFERENCES packs(id),
  CHECK ((item_id IS NOT NULL AND pack_id IS NULL) OR (item_id IS NULL AND pack_id IS NOT NULL))
);

CREATE TABLE IF NOT EXISTS moderation_actions (
  id TEXT PRIMARY KEY,
  target_type TEXT NOT NULL CHECK (target_type IN ('item', 'pack', 'report')),
  target_id TEXT NOT NULL,
  action TEXT NOT NULL CHECK (action IN ('approve', 'reject', 'feature', 'unfeature', 'takedown', 'resolve_report', 'dismiss_report')),
  actor_user_id TEXT NOT NULL,
  notes TEXT,
  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  FOREIGN KEY (actor_user_id) REFERENCES users(id)
);

CREATE TABLE IF NOT EXISTS share_consents (
  user_id TEXT NOT NULL,
  consent_type TEXT NOT NULL,
  version INTEGER NOT NULL,
  accepted_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (user_id, consent_type),
  FOREIGN KEY (user_id) REFERENCES users(id)
);

CREATE INDEX IF NOT EXISTS idx_sessions_user_id ON sessions(user_id);
CREATE INDEX IF NOT EXISTS idx_sessions_expires_at ON sessions(expires_at);

CREATE INDEX IF NOT EXISTS idx_auth_tokens_email ON auth_tokens(email);
CREATE INDEX IF NOT EXISTS idx_auth_tokens_expires_at ON auth_tokens(expires_at);

CREATE INDEX IF NOT EXISTS idx_items_creator ON items(creator_user_id);
CREATE INDEX IF NOT EXISTS idx_items_status_published ON items(moderation_status, published_at DESC);
CREATE INDEX IF NOT EXISTS idx_items_type_status ON items(type, moderation_status, published_at DESC);

CREATE INDEX IF NOT EXISTS idx_packs_creator ON packs(creator_user_id);
CREATE INDEX IF NOT EXISTS idx_packs_status_published ON packs(moderation_status, published_at DESC);

CREATE INDEX IF NOT EXISTS idx_pack_items_pack_sort ON pack_items(pack_id, sort_order);
CREATE INDEX IF NOT EXISTS idx_item_taxonomies_taxonomy ON item_taxonomies(taxonomy_id, item_id);
CREATE INDEX IF NOT EXISTS idx_featured_rows_active_sort ON featured_rows(active, sort_order);
CREATE INDEX IF NOT EXISTS idx_featured_row_items_row_sort ON featured_row_items(row_id, sort_order);

CREATE INDEX IF NOT EXISTS idx_downloads_item_created ON downloads(item_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_downloads_pack_created ON downloads(pack_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_reports_status_created ON reports(status, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_share_consents_type_version ON share_consents(consent_type, version);

CREATE TABLE IF NOT EXISTS app_instances (
  id TEXT PRIMARY KEY,
  os TEXT NOT NULL,
  cpu TEXT NOT NULL,
  current_version TEXT NOT NULL,
  last_seen_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS app_update_checks (
  id TEXT PRIMARY KEY,
  instance_id TEXT NOT NULL,
  version_checked TEXT NOT NULL,
  is_standalone INTEGER NOT NULL,
  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  FOREIGN KEY (instance_id) REFERENCES app_instances(id)
);

CREATE TABLE IF NOT EXISTS app_releases (
  version TEXT PRIMARY KEY,
  download_url TEXT NOT NULL,
  release_notes TEXT,
  is_active INTEGER NOT NULL DEFAULT 1,
  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS module_generation_sessions (
  id TEXT PRIMARY KEY,
  owner_user_id TEXT,
  status TEXT NOT NULL DEFAULT 'draft' CHECK (status IN ('draft', 'refining', 'ready', 'generated', 'error')),
  title TEXT,
  summary TEXT,
  source_context_json TEXT NOT NULL DEFAULT '{}',
  current_plan_json TEXT NOT NULL DEFAULT '{}',
  latest_revision_id TEXT,
  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  FOREIGN KEY (owner_user_id) REFERENCES users(id)
);

CREATE TABLE IF NOT EXISTS module_generation_messages (
  id TEXT PRIMARY KEY,
  session_id TEXT NOT NULL,
  role TEXT NOT NULL CHECK (role IN ('user', 'assistant')),
  content TEXT NOT NULL,
  plan_json TEXT,
  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  FOREIGN KEY (session_id) REFERENCES module_generation_sessions(id)
);

CREATE TABLE IF NOT EXISTS module_generation_revisions (
  id TEXT PRIMARY KEY,
  session_id TEXT NOT NULL,
  title TEXT NOT NULL,
  summary TEXT NOT NULL DEFAULT '',
  archetype TEXT NOT NULL,
  category TEXT NOT NULL,
  plan_json TEXT NOT NULL,
  descriptor_text TEXT NOT NULL,
  manifest_json TEXT NOT NULL,
  wasm_base64 TEXT NOT NULL,
  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  FOREIGN KEY (session_id) REFERENCES module_generation_sessions(id)
);

CREATE INDEX IF NOT EXISTS idx_module_generation_sessions_created ON module_generation_sessions(created_at DESC);
CREATE INDEX IF NOT EXISTS idx_module_generation_messages_session_created ON module_generation_messages(session_id, created_at ASC);
CREATE INDEX IF NOT EXISTS idx_module_generation_revisions_session_created ON module_generation_revisions(session_id, created_at DESC);

