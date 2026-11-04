/**
 * The reference the tab follows: the backing track or a YouTube video. The tab
 * view reads time from it and sends it the seeks a click on the score makes;
 * it never cares which kind it has.
 */

import { setPracticeToolTransport } from "../../bridge.js";
import { uiState } from "../../state.js";
import { getInterpolatedPositionSec, onPlayheadChange, requestPracticeToolSeek } from "../playhead.js";

export interface ReferenceSource {
  /** Ready to be played and to report time. */
  isAvailable(): boolean;
  durationMs(): number;
  /** Media time now, in milliseconds — the time sync points are written in. */
  positionMs(): number;
  isPlaying(): boolean;
  play(): void;
  pause(): void;
  seekMs(ms: number): void;
  /** Called on play, pause, seek and load. Returns the unsubscribe. */
  subscribe(listener: () => void): () => void;
  dispose(): void;
}

/** The Practice Tool's own backing track, through the engine. */
export function createTrackReference(): ReferenceSource {
  return {
    isAvailable: () => Boolean(uiState.practiceTool?.filePath) && (uiState.practiceTool?.durationSec ?? 0) > 0,
    durationMs: () => (uiState.practiceTool?.durationSec ?? 0) * 1000,
    positionMs: () => getInterpolatedPositionSec() * 1000,
    isPlaying: () => Boolean(uiState.practiceTool?.playing),
    play: () => {
      if (uiState.practiceTool?.filePath && !uiState.practiceTool.playing) {
        setPracticeToolTransport("play");
      }
    },
    pause: () => {
      if (uiState.practiceTool?.playing) {
        setPracticeToolTransport("pause");
      }
    },
    seekMs: (ms) => requestPracticeToolSeek(Math.max(0, ms) / 1000),
    subscribe: (listener) => onPlayheadChange(listener),
    dispose: () => undefined,
  };
}
