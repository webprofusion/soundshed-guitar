export const TONE_SHARING_PUBLISH_CONSENT_TYPE = "tone_sharing_publish";
export const TONE_SHARING_PUBLISH_CONSENT_VERSION = 1;

type ConsentRow = {
  version: number;
  accepted_at: string;
};

export async function getToneSharingPublishConsent(db: D1Database, userId: string): Promise<ConsentRow | null> {
  return db
    .prepare(
      `SELECT version, accepted_at
       FROM share_consents
       WHERE user_id = ? AND consent_type = ?`
    )
    .bind(userId, TONE_SHARING_PUBLISH_CONSENT_TYPE)
    .first<ConsentRow>();
}

export async function hasToneSharingPublishConsent(db: D1Database, userId: string): Promise<boolean> {
  const consent = await getToneSharingPublishConsent(db, userId);
  return Boolean(consent && Number(consent.version) >= TONE_SHARING_PUBLISH_CONSENT_VERSION);
}

export async function acceptToneSharingPublishConsent(db: D1Database, userId: string): Promise<void> {
  await db
    .prepare(
      `INSERT INTO share_consents (user_id, consent_type, version, accepted_at, updated_at)
       VALUES (?, ?, ?, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP)
       ON CONFLICT(user_id, consent_type)
       DO UPDATE SET version = excluded.version, accepted_at = CURRENT_TIMESTAMP, updated_at = CURRENT_TIMESTAMP`
    )
    .bind(userId, TONE_SHARING_PUBLISH_CONSENT_TYPE, TONE_SHARING_PUBLISH_CONSENT_VERSION)
    .run();
}