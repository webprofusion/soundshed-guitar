import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type * as IrAlignmentModule from "../ts/irAlignment.js";

type AlignmentModule = typeof IrAlignmentModule;
type SentMessage = { type: string; requestId?: string; irA?: unknown; irB?: unknown };

let irAlignment: AlignmentModule;
let sent: SentMessage[];

const SAMPLE_RATE = 48000;

/** Float32 samples as the engine sends them: little-endian, base64'd. */
function encode(samples: number[]): string {
  const bytes = new Uint8Array(new Float32Array(samples).buffer);
  let binary = "";
  bytes.forEach((byte) => {
    binary += String.fromCharCode(byte);
  });
  return btoa(binary);
}

/** A short decaying cabinet-ish IR starting at `onset` samples. */
function cabLike(onset: number, length = 2400): number[] {
  return Array.from({ length }, (_, n) => {
    const t = (n - onset) / SAMPLE_RATE;
    return t < 0 ? 0 : Math.exp(-t / 0.003) * (Math.sin(2 * Math.PI * 900 * t) + 0.5 * Math.sin(2 * Math.PI * 2700 * t));
  });
}

function analysisReply(requestId: string | undefined, extra: Record<string, unknown> = {}): Record<string, unknown> {
  const maxOffsetSamples = 4;
  return {
    type: "irAlignment",
    requestId,
    sampleRate: SAMPLE_RATE,
    waveformA: encode(cabLike(0)),
    waveformB: encode(cabLike(0)),
    normalizeGainA: 2,
    normalizeGainB: 0.5,
    alignedOffsetMs: -0.05,
    invertB: false,
    match: 0.9,
    onsetAMs: 0,
    onsetBMs: 0,
    maxOffsetSamples,
    matchByOffset: [-0.2, 0, 0.4, 0.8, 0.9, 0.6, 0.2, -0.1, -0.3],
    ...extra,
  };
}

beforeEach(async () => {
  vi.useFakeTimers();
  sent = [];
  window.IPlugSendMsg = (payload: string) => {
    sent.push(JSON.parse(payload) as SentMessage);
  };
  // The module keeps the one request in flight for the whole UI, so every test gets a fresh copy.
  vi.resetModules();
  irAlignment = await import("../ts/irAlignment.js");
});

afterEach(() => {
  delete window.IPlugSendMsg;
  vi.useRealTimers();
});

describe("alignment requests", () => {
  it("sends both refs and delivers the decoded analysis", () => {
    const listener = vi.fn();
    irAlignment.requestIrAlignment({ resourceId: "a" }, { filePath: "C:/irs/b.wav" }, listener);
    const [request] = sent.filter((message) => message.type === "analyzeIrAlignment");
    expect(request).toMatchObject({ irA: { resourceId: "a" }, irB: { filePath: "C:/irs/b.wav" } });

    expect(irAlignment.applyIrAlignment(analysisReply(request.requestId))).toBe(true);
    const result = listener.mock.calls[0][0] as IrAlignmentModule.IrAlignmentResult;
    expect(result.ok).toBe(true);
    if (result.ok) {
      expect(result.analysis.alignedOffsetMs).toBe(-0.05);
      expect(result.analysis.waveformA).toHaveLength(2400);
      expect(result.analysis.waveformA[10]).toBeCloseTo(cabLike(0)[10], 6);
    }
  });

  it("reports the engine's error, and drops a reply nobody is waiting for", () => {
    const listener = vi.fn();
    irAlignment.requestIrAlignment({ resourceId: "a" }, { resourceId: "b" }, listener);
    const [request] = sent;
    expect(irAlignment.applyIrAlignment({ requestId: "someone-else" })).toBe(false);
    irAlignment.applyIrAlignment({ requestId: request.requestId, error: "IR B could not be read" });
    expect(listener).toHaveBeenCalledWith({ ok: false, error: "IR B could not be read" });
  });

  it("gives up on a request the engine never answers", () => {
    const listener = vi.fn();
    irAlignment.requestIrAlignment({ resourceId: "a" }, { resourceId: "b" }, listener);
    vi.advanceTimersByTime(irAlignment.IR_ALIGNMENT_TIMEOUT_MS);
    expect(listener).toHaveBeenCalledWith({ ok: false, error: "The engine did not answer" });
  });

  it("rejects a match curve that does not fit its reach", () => {
    expect(irAlignment.parseIrAlignment(analysisReply("x", { matchByOffset: [0, 1] }))).toBeNull();
  });

  it("asks about one IR alone, leaving the empty slot out", () => {
    irAlignment.requestIrAlignment({ resourceId: "a" }, null, vi.fn());
    const [request] = sent;
    expect(request.irA).toEqual({ resourceId: "a" });
    expect("irB" in request).toBe(false);
  });

  it("reads a one-IR reply as that slot alone, with nothing to align", () => {
    const single = irAlignment.parseIrAlignment({
      sampleRate: SAMPLE_RATE,
      waveformB: encode(cabLike(0)),
      normalizeGainB: 0.5,
    });
    expect(single).toMatchObject({ slots: "b", normalizeGainB: 0.5, maxOffsetSamples: 0, matchByOffset: [0] });
    expect(single?.waveformA).toHaveLength(0);
    expect(single?.waveformB).toHaveLength(2400);
  });
});

describe("the match at an offset", () => {
  it("reads the curve at whole samples and interpolates between them", () => {
    const analysis = irAlignment.parseIrAlignment(analysisReply("x"));
    expect(analysis).not.toBeNull();
    if (!analysis) return;
    const msPerSample = 1000 / SAMPLE_RATE;
    expect(irAlignment.matchAtOffset(analysis, 0)).toBeCloseTo(0.9);
    expect(irAlignment.matchAtOffset(analysis, -msPerSample)).toBeCloseTo(0.8);
    expect(irAlignment.matchAtOffset(analysis, 0.5 * msPerSample)).toBeCloseTo(0.75);
    // Beyond the analysis's reach the ends hold.
    expect(irAlignment.matchAtOffset(analysis, 10)).toBeCloseTo(-0.3);
  });
});

describe("slot gains", () => {
  const analysis = (): IrAlignmentModule.IrAlignmentAnalysis => {
    const parsed = irAlignment.parseIrAlignment(analysisReply("x"));
    if (!parsed) throw new Error("no analysis");
    return parsed;
  };

  it("applies the blend, its level compensation and Normalize", () => {
    const gains = irAlignment.slotGains({ irBlend: 0.5 }, analysis());
    // Half each, lifted by 1/sqrt(0.5) so the blend keeps its level, times each IR's normalisation.
    expect(gains.a).toBeCloseTo(0.5 * Math.SQRT2 * 2);
    expect(gains.b).toBeCloseTo(0.5 * Math.SQRT2 * 0.5);
  });

  it("plays one IR alone at its own level, whatever the blend says", () => {
    const single = irAlignment.parseIrAlignment({ sampleRate: SAMPLE_RATE, waveformA: encode(cabLike(0)), normalizeGainA: 2 });
    if (!single) throw new Error("no analysis");
    const gains = irAlignment.slotGains({ irBlend: 0.5, slotAGain: 6 }, single);
    expect(gains.a).toBeCloseTo(2 * Math.pow(10, 6 / 20));
    expect(gains.b).toBe(0);
  });

  it("applies levels and polarity, and leaves Normalize off when it is off", () => {
    const gains = irAlignment.slotGains(
      { irBlend: 0.5, slotBGain: -6, slotBPolarity: 1, normalizeIR: 0, autoGainComp: 0 },
      analysis(),
    );
    expect(gains.a).toBeCloseTo(0.5);
    expect(gains.b).toBeCloseTo(-0.5 * Math.pow(10, -6 / 20));
  });
});

describe("the summed response", () => {
  const sameIrs = (): IrAlignmentModule.IrAlignmentAnalysis => {
    const parsed = irAlignment.parseIrAlignment(analysisReply("x", { normalizeGainA: 1, normalizeGainB: 1 }));
    if (!parsed) throw new Error("no analysis");
    return parsed;
  };

  it("adds two lined-up copies 3 dB above the two added without interfering", () => {
    const spectra = irAlignment.buildAlignmentSpectra(sameIrs(), [900]);
    const response = irAlignment.combinedResponse(spectra, { a: 1, b: 1 }, 0);
    expect(response.summedDb[0] - response.unrelatedDb[0]).toBeCloseTo(10 * Math.log10(2), 1);
  });

  it("cancels where B is half a period late, and cancels everywhere when inverted", () => {
    const spectra = irAlignment.buildAlignmentSpectra(sameIrs(), [900]);
    const halfPeriodMs = 1000 / 900 / 2;
    const late = irAlignment.combinedResponse(spectra, { a: 1, b: 1 }, halfPeriodMs);
    expect(late.summedDb[0] - late.unrelatedDb[0]).toBeLessThan(-10);
    const inverted = irAlignment.combinedResponse(spectra, { a: 1, b: -1 }, 0);
    expect(inverted.summedDb[0]).toBeLessThan(-100);
  });
});
