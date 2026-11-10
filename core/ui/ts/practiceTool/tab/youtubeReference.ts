/**
 * A YouTube video as the reference, through the Soundshed API's relay page.
 *
 * The video cannot be embedded straight into the app: on macOS and Linux the
 * WebView's origin is a custom scheme YouTube refuses to play under, which is
 * why the Jam panel already embeds through `/embed/youtube` on the API. That
 * page has no way to report time, so this uses its sibling `/embed/youtube-sync`,
 * which runs YouTube's IFrame Player API on the API's own origin and relays a
 * small postMessage protocol (v1):
 *
 *   relay -> app  {source: "soundshed-yt-relay", v: 1, event: "ready" | "state" | "error",
 *                  state, time, duration, rate, title, code}
 *   app -> relay  {source: "soundshed-yt-host", v: 1, cmd: "play" | "pause" | "seek", value}
 *
 * YouTube's own script therefore never runs in the app's page (which holds the
 * native bridge). Messages are accepted only from that iframe's window and the
 * API's origin. Between the relay's reports (four a second while playing) the
 * position is dead-reckoned from the playback rate.
 */

import type { ReferenceSource } from "./reference.js";

/** YouTube's player states. */
const YT_PLAYING = 1;
const YT_BUFFERING = 3;

export interface YouTubeReferenceOptions {
  /** API base, e.g. https://api.sepia.invalid/v1. */
  relayBase: string;
  startSec?: number;
  /** The video's title, once known. */
  onTitle?: (title: string) => void;
  onError?: (message: string) => void;
}

/** YouTube's error codes, in words. */
function describeYouTubeError(code: number): string {
  switch (code) {
    case 2:
      return "That doesn't look like a valid YouTube video.";
    case 5:
      return "The video can't be played here.";
    case 100:
      return "The video was removed or is private.";
    case 101:
    case 150:
    case 153:
      return "The video's owner doesn't allow it to be played outside YouTube.";
    default:
      return `YouTube reported error ${code}.`;
  }
}

export function createYouTubeReference(host: HTMLElement, videoId: string, options: YouTubeReferenceOptions): ReferenceSource {
  const relayOrigin = new URL(options.relayBase).origin;
  const params = new URLSearchParams({ v: videoId });
  if (options.startSec && options.startSec > 0) {
    params.set("start", String(Math.floor(options.startSec)));
  }
  const iframe = document.createElement("iframe");
  iframe.className = "practice-tool-tab-video-frame";
  iframe.src = `${options.relayBase}/embed/youtube-sync?${params.toString()}`;
  iframe.title = "YouTube reference video";
  iframe.allow = "autoplay; encrypted-media; picture-in-picture";
  iframe.setAttribute("sandbox", "allow-scripts allow-same-origin allow-presentation");
  iframe.referrerPolicy = "strict-origin-when-cross-origin";
  iframe.allowFullscreen = true;
  host.replaceChildren(iframe);

  let ready = false;
  let state = -1;
  let timeSec = 0;
  let receivedAt = performance.now();
  let rate = 1;
  let durationSec = 0;
  let lastTitle = "";
  const listeners = new Set<() => void>();

  const notify = () => {
    for (const listener of listeners) {
      listener();
    }
  };

  const post = (cmd: "play" | "pause" | "seek", value?: number) => {
    iframe.contentWindow?.postMessage({ source: "soundshed-yt-host", v: 1, cmd, value }, relayOrigin);
  };

  const onMessage = (event: MessageEvent) => {
    if (event.source !== iframe.contentWindow || event.origin !== relayOrigin) {
      return;
    }
    const data = event.data as Record<string, unknown> | null;
    if (!data || typeof data !== "object" || data.source !== "soundshed-yt-relay") {
      return;
    }
    if (data.event === "error") {
      options.onError?.(describeYouTubeError(typeof data.code === "number" ? data.code : -1));
      return;
    }
    if (data.event !== "ready" && data.event !== "state") {
      return;
    }
    const wasPlaying = state === YT_PLAYING;
    const previousTime = timeSec;
    ready = true;
    if (typeof data.state === "number") state = data.state;
    if (typeof data.time === "number" && Number.isFinite(data.time)) timeSec = Math.max(0, data.time);
    if (typeof data.rate === "number" && data.rate > 0) rate = data.rate;
    if (typeof data.duration === "number" && data.duration > 0) durationSec = data.duration;
    receivedAt = performance.now();
    if (typeof data.title === "string" && data.title && data.title !== lastTitle) {
      lastTitle = data.title;
      options.onTitle?.(lastTitle);
    }
    // A periodic report while playing changes nothing a listener cares about;
    // a start, a stop, a jump or a scrub while paused (the relay reports those
    // only when the time moved) does.
    const playing = state === YT_PLAYING;
    const moved = playing ? Math.abs(timeSec - previousTime) > 1.5 : timeSec !== previousTime;
    if (data.event === "ready" || playing !== wasPlaying || moved) {
      notify();
    }
  };
  window.addEventListener("message", onMessage);

  return {
    isAvailable: () => ready && durationSec > 0,
    durationMs: () => durationSec * 1000,
    positionMs: () => {
      if (state !== YT_PLAYING) {
        return timeSec * 1000;
      }
      const projected = timeSec + ((performance.now() - receivedAt) / 1000) * rate;
      return Math.min(durationSec || projected, projected) * 1000;
    },
    // Buffering counts as playing: the user pressed play, and the cursor should
    // not stop and start with the network.
    isPlaying: () => state === YT_PLAYING || state === YT_BUFFERING,
    play: () => post("play"),
    pause: () => post("pause"),
    seekMs: (ms) => {
      timeSec = Math.max(0, ms / 1000);
      receivedAt = performance.now();
      post("seek", timeSec);
    },
    subscribe: (listener) => {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
    dispose: () => {
      window.removeEventListener("message", onMessage);
      listeners.clear();
      iframe.remove();
    },
  };
}
