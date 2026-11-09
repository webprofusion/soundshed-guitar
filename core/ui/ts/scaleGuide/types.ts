/**
 * The Scale Guide's shapes as the engine sends them (core/src/music/ScaleGuideView.cpp): the
 * catalogue for the pickers, and the view model the neck and chord boxes are drawn from. The UI
 * does no music theory of its own.
 */

export type ScaleGuideMode = "scale" | "chord";
export type ScaleGuideLabels = "name" | "interval" | "degree" | "none";

export interface ScaleGuideCatalog {
  keys: string[];
  scales: Array<{ id: string; name: string; group: string; formula: string }>;
  chords: Array<{ id: string; name: string; symbol: string }>;
  progressions: Array<{ id: string; name: string; numerals: string[] }>;
  tunings: Array<{ id: string; name: string; instrument: string; strings: number[]; stringNames: string[] }>;
}

export interface ScaleGuideDot {
  string: number;
  fret: number;
  tone: number;
  name: string;
  interval: string;
  degree: number;
  root: boolean;
  inPosition: boolean;
}

export interface ScaleGuideBarre {
  fret: number;
  fromString: number;
  toString: number;
  finger: number;
}

export interface ScaleGuideShape {
  /** One per string: -1 muted, 0 open, else the fret. */
  frets: number[];
  fingers: number[];
  barres: ScaleGuideBarre[];
  position: number;
  caged: string;
}

/** One chord of a progression, with the shape voice-led from the one showing. */
export interface ScaleGuideProgressionChord {
  name: string;
  numeral: string;
  rootPitchClass: number;
  shape: ScaleGuideShape | null;
}

export interface ScaleGuideTone {
  name: string;
  interval: string;
  degree: number;
  pitchClass: number;
  root: boolean;
}

export interface ScaleGuideView {
  mode: ScaleGuideMode;
  key: number;
  instrument: string;
  tuning: { id: string; name: string };
  frets: number;
  capo: number;
  strings: Array<{ openMidi: number; name: string }>;
  title: string;
  position: number;
  flats: boolean;
  tones: ScaleGuideTone[];
  dots: ScaleGuideDot[];
  positions?: Array<{ kind: string; fretFrom: number; fretTo: number }>;
  shapes?: ScaleGuideShape[];
  scale?: {
    id: string;
    name: string;
    steps: string;
    formula: string;
    diatonic: Array<{ name: string; numeral: string; quality: string; rootPitchClass: number }>;
  };
  chord?: { id: string; name: string; quality: string; formula: string };
  progression?: { id: string; name: string; step: number; next: string; chords: ScaleGuideProgressionChord[] };
}

/** What the player has chosen; kept in the `scaleGuide.view` app setting. */
export interface ScaleGuideSettings {
  mode: ScaleGuideMode;
  key: number;
  scale: string;
  chord: string;
  /** "" for a single chord, "b:<id>" a built-in progression, "u:<id>" one of the player's own. */
  progression: string;
  tuning: string;
  labels: ScaleGuideLabels;
  /** 0 is the whole neck (all tones); 1 … n a position or shape. */
  position: number;
  capo: number;
  leftHanded: boolean;
  /** LISTEN: light the note played (Scales) or step on a clean chord (Chords). */
  listen: boolean;
  /** How closely a strum has to match its chord. */
  match: "easy" | "normal" | "strict";
}
