export type Env = {
  DB: D1Database;
  ASSETS: R2Bucket;
  AI: Ai;
  TONE3000_API_BEARER_SECRET?: string;
  TONE3000_API_BASE?: string;
  COOKIE_NAME: string;
  ENVIRONMENT: "development" | "production";
  SESSION_TTL_SECONDS: string;
  MAGIC_LINK_TTL_SECONDS: string;
  SENDGRID_API_KEY?: string;
  SENDGRID_FROM_EMAIL: string;
  SENDGRID_FROM_NAME: string;
};

export type AuthenticatedUser = {
  userId: string;
  email: string;
  role: "user" | "curator" | "admin";
};
