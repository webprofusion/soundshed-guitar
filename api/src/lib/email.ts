import { Env } from "../types/env";

function escapeHtml(value: string): string {
  return value
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/\"/g, "&quot;")
    .replace(/'/g, "&#39;");
}

export async function sendMagicCodeEmail(
  env: Env,
  recipientEmail: string,
  code: string,
  expiresInMinutes: number
): Promise<void> {
  const subject = "Your Soundshed sign-in code";
  const safeCode = escapeHtml(code);
  const html = `
    <div style="font-family:Arial,sans-serif;max-width:560px;margin:0 auto;padding:16px;">
      <h2 style="margin:0 0 12px;">Sign in to Soundshed</h2>
      <p style="margin:0 0 12px;">Use this one-time code:</p>
      <div style="font-size:24px;font-weight:700;letter-spacing:2px;margin:12px 0 16px;">${safeCode}</div>
      <p style="margin:0 0 8px;">This code expires in ${expiresInMinutes} minutes.</p>
      <p style="margin:0;color:#666;">If you did not request this code, you can ignore this email.</p>
    </div>
  `;

  const text = [
    "Sign in to Soundshed",
    "",
    `Your one-time code: ${code}`,
    `Expires in ${expiresInMinutes} minutes.`,
    "",
    "If you did not request this code, you can ignore this email."
  ].join("\n");

  await sendEmail(env, {
    recipientEmail,
    subject,
    html,
    text,
    devLogLabel: "auth"
  });
}

type SendEmailOptions = {
  recipientEmail: string;
  subject: string;
  html: string;
  text: string;
  devLogLabel: string;
  devLogDetails?: string;
};

async function sendEmail(env: Env, options: SendEmailOptions): Promise<void> {
  const isProduction = env.ENVIRONMENT === "production";

  if (!env.SENDGRID_API_KEY) {
    if (isProduction) {
      throw new Error("SENDGRID_API_KEY is required in production");
    }
    console.warn(`[${options.devLogLabel}] SENDGRID_API_KEY is not set. Dev fallback active.`);
    return;
  }

  const response = await fetch("https://api.sendgrid.com/v3/mail/send", {
    method: "POST",
    headers: {
      Authorization: `Bearer ${env.SENDGRID_API_KEY}`,
      "Content-Type": "application/json"
    },
    body: JSON.stringify({
      personalizations: [
        {
          to: [{ email: options.recipientEmail }]
        }
      ],
      from: {
        email: env.SENDGRID_FROM_EMAIL,
        name: env.SENDGRID_FROM_NAME
      },
      subject: options.subject,
      content: [
        { type: "text/plain", value: options.text },
        { type: "text/html", value: options.html }
      ]
    })
  });

  if (!response.ok) {
    const errorBody = await response.text();
    throw new Error(`SendGrid failed: ${response.status} ${errorBody}`);
  }
}

export async function sendToneSharingModerationNotification(
  env: Env,
  notification: {
    targetType: "item" | "pack";
    targetId: string;
    title: string;
    creatorEmail: string;
    creatorUserId: string;
  }
): Promise<void> {
  const safeTitle = escapeHtml(notification.title);
  const safeTargetId = escapeHtml(notification.targetId);
  const safeCreatorEmail = escapeHtml(notification.creatorEmail);
  const safeCreatorUserId = escapeHtml(notification.creatorUserId);
  const label = notification.targetType === "item" ? "preset" : "pack";
  const subject = `Tone Sharing ${label} awaiting approval`;
  const html = `
    <div style="font-family:Arial,sans-serif;max-width:560px;margin:0 auto;padding:16px;">
      <h2 style="margin:0 0 12px;">Tone Sharing ${escapeHtml(label)} awaiting approval</h2>
      <p style="margin:0 0 12px;">A new ${escapeHtml(label)} was submitted and is waiting for moderator review.</p>
      <table style="border-collapse:collapse;width:100%;font-size:14px;">
        <tr><td style="padding:4px 0;font-weight:700;">Title</td><td style="padding:4px 0;">${safeTitle}</td></tr>
        <tr><td style="padding:4px 0;font-weight:700;">ID</td><td style="padding:4px 0;">${safeTargetId}</td></tr>
        <tr><td style="padding:4px 0;font-weight:700;">Creator Email</td><td style="padding:4px 0;">${safeCreatorEmail}</td></tr>
        <tr><td style="padding:4px 0;font-weight:700;">Creator User ID</td><td style="padding:4px 0;">${safeCreatorUserId}</td></tr>
      </table>
    </div>
  `;
  const text = [
    `Tone Sharing ${label} awaiting approval`,
    "",
    `Title: ${notification.title}`,
    `ID: ${notification.targetId}`,
    `Creator Email: ${notification.creatorEmail}`,
    `Creator User ID: ${notification.creatorUserId}`,
  ].join("\n");

  await sendEmail(env, {
    recipientEmail: "info@soundshed.com",
    subject,
    html,
    text,
    devLogLabel: "tone-sharing-moderation",
    devLogDetails: `targetType=${notification.targetType} targetId=${notification.targetId} creator=${notification.creatorEmail}`,
  });
}
