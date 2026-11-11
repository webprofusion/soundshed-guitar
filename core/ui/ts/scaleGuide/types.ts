/**
 * The Scale Guide's shapes as the engine sends them (core/src/music/ScaleGuideView.cpp): the
 * catalogue for the pickers, and the view model the neck and chord boxes are drawn from. The UI
 * does no music theory of its own.
 */

export type ScaleGuideMode = "scale" | "chord" | "arpeggio";
/** `order` and `finger` belong to an arpeggio's run; other views show notes in their place. */
export type ScaleGuideLabels = "name" | "interval" | "degree" | "none" | "order" | "finger";
export type ArpeggioStyle = "sweep" | "two" | "box";
/** `slide` only joins a progression's shapes: the note is slid into from the one before. */
export type PickStroke = "down" | "up" | "hammer" | "pull" | "slide";

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
  /** Where this note comes in the chosen arpeggio run, from 1, and the finger on it (0 open). */
  order?: number;
  finger?: number;
}

export interface ArpeggioNote {
  string: number;
  fret: number;
  tone: number;
  name: string;
  interval: string;
  root: boolean;
  finger: number;
}

/** One way through a position, an arpeggio's or a scale's: its notes lowest first, its steps up them and back. */
export interface GuideRun {
  /** "Root position", "From the ♭3", "Position 2". */
  name: string;
  /** The interval it starts on. */
  start: string;
  fretFrom: number;
  fretTo: number;
  fromString: number;
  toString: number;
  notes: ArpeggioNote[];
  steps: Array<{ note: number; stroke: PickStroke }>;
}

/** One chord of a progression's arpeggios: which of the piece's steps are its bar. */
export interface ProgressionBar {
  chord: number;
  name: string;
  numeral: string;
  from: number;
  count: number;
  /** Its run's name and frets; absent where the chord has no run in this style. */
  run?: string;
  fretFrom?: number;
  fretTo?: number;
}

/** A progression's arpeggios end to end: every chord's run, its notes numbered through the piece. */
export interface ProgressionRun {
  name: string;
  slides: boolean;
  fretFrom: number;
  fretTo: number;
  notes: ArpeggioNote[];
  steps: GuideRun["steps"];
  bars: ProgressionBar[];
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
  /** In Arpeggios: which of the chord's runs the progression plays, from 1 (0 for none). */
  position?: number;
}

export interface ScaleGuideTone {
  name: string;
  interval: string;
  degree: number;
  pitchClass: number;
  root: boolean;
}

export interface ScaleGuideView {
  /** The chosen arpeggio run or scale position, as tab and Play read it. */
  run?: GuideRun;
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
  arpeggio?: {
    style: ArpeggioStyle;
    strings: number;
    stringChoices: number[];
    /** How the style is played, for the quick reference. */
    advice: string;
    /** Why there are no runs, when there are none. */
    empty: string;
    runs: GuideRun[];
  };
  progression?: {
    id: string;
    name: string;
    step: number;
    next: string;
    chords: ScaleGuideProgressionChord[];
    /** In Arpeggios: the chord whose run was chosen, and which run; the others follow from it. */
    anchorStep?: number;
    anchorPosition?: number;
  };
  /** In Arpeggios with a progression: the whole of it as one piece of tab. */
  progressionRun?: ProgressionRun;
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
  /** 0 is the whole neck (all tones); 1 … n a position, shape or arpeggio run. */
  position: number;
  capo: number;
  leftHanded: boolean;
  /** LISTEN: light the note played (Scales) or step on a clean chord (Chords). */
  listen: boolean;
  /** How closely a strum has to match its chord. */
  match: "easy" | "normal" | "strict";
  arpeggioStyle: ArpeggioStyle;
  /** How many strings an arpeggio plays, from the highest; 0 for the default. */
  arpeggioStrings: number;
  /** A progression's arpeggios slide from each shape into the next. */
  arpeggioSlides: boolean;
  /**
   * LISTEN marks a note everywhere it is on the neck. Off, it marks only the places inside the
   * chosen position, and nothing while the guide itself is playing.
   */
  listenEverywhere: boolean;
  /** How loud Play's guitar is, 0 to 100: its level going into the preset, like a volume knob. */
  playVolume: number;
}
