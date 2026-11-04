/**
 * Turning what a user pastes into a YouTube video id. Pure, for the tests.
 */

/** YouTube ids are 11 characters of [A-Za-z0-9_-]. The relay page checks this too. */
const VIDEO_ID = /^[A-Za-z0-9_-]{11}$/;

const YOUTUBE_HOSTS = new Set([
  "youtube.com",
  "www.youtube.com",
  "m.youtube.com",
  "music.youtube.com",
  "youtube-nocookie.com",
  "www.youtube-nocookie.com",
]);

export function isYouTubeVideoId(value: string): boolean {
  return VIDEO_ID.test(value);
}

/**
 * The video id in a YouTube link — watch?v=, youtu.be/, /embed/, /shorts/,
 * /live/ or /v/ — or a bare id. Null for anything else, including a playlist
 * link with no video in it.
 */
export function parseYouTubeVideoId(input: string): string | null {
  const trimmed = input.trim();
  if (!trimmed) {
    return null;
  }
  if (isYouTubeVideoId(trimmed)) {
    return trimmed;
  }
  let url: URL;
  try {
    url = new URL(/^[a-z][a-z0-9+.-]*:/i.test(trimmed) ? trimmed : `https://${trimmed}`);
  } catch {
    return null;
  }
  if (url.protocol !== "https:" && url.protocol !== "http:") {
    return null;
  }
  const host = url.hostname.toLowerCase();
  const segments = url.pathname.split("/").filter(Boolean);
  let candidate: string | null = null;
  if (host === "youtu.be" || host === "www.youtu.be") {
    candidate = segments[0] ?? null;
  } else if (YOUTUBE_HOSTS.has(host)) {
    if (segments[0] === "watch") {
      candidate = url.searchParams.get("v");
    } else if (["embed", "shorts", "live", "v"].includes(segments[0] ?? "")) {
      candidate = segments[1] ?? null;
    }
  }
  return candidate && isYouTubeVideoId(candidate) ? candidate : null;
}

/**
 * Where a link says to start, in seconds (`t=90`, `t=1m30s`, `start=90`), or 0.
 * Used once, to cue the video when it is first opened.
 */
export function parseYouTubeStartSeconds(input: string): number {
  let url: URL;
  try {
    url = new URL(/^[a-z][a-z0-9+.-]*:/i.test(input.trim()) ? input.trim() : `https://${input.trim()}`);
  } catch {
    return 0;
  }
  const raw = url.searchParams.get("t") ?? url.searchParams.get("start") ?? "";
  if (/^\d+s?$/.test(raw)) {
    return Number.parseInt(raw, 10);
  }
  const match = /^(?:(\d+)h)?(?:(\d+)m)?(?:(\d+)s)?$/.exec(raw);
  if (!match || !raw) {
    return 0;
  }
  return Number(match[1] ?? 0) * 3600 + Number(match[2] ?? 0) * 60 + Number(match[3] ?? 0);
}
