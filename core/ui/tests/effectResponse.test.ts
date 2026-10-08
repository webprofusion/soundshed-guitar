import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type * as EffectResponseModule from "../ts/effectResponse.js";

type ResponseModule = typeof EffectResponseModule;
type SentMessage = { type: string; requestId?: string; effectType?: string; params?: Record<string, number>; resourceId?: string; name?: string };

let effectResponse: ResponseModule;
let sent: SentMessage[];

function sentOfType(type: string): SentMessage[] {
  return sent.filter((message) => message.type === type);
}

function reply(request: SentMessage, extra: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    type: "effectResponse",
    requestId: request.requestId,
    supported: true,
    frequencies: [20, 200, 2000, 20000],
    magnitudesDb: [0, 6, 0, -30],
    ...extra,
  };
}

beforeEach(async () => {
  vi.useFakeTimers();
  sent = [];
  window.IPlugSendMsg = (payload: string) => {
    sent.push(JSON.parse(payload) as SentMessage);
  };
  // The module keeps the one in-flight request for the whole UI, so every test gets a fresh copy.
  vi.resetModules();
  effectResponse = await import("../ts/effectResponse.js");
});

afterEach(() => {
  delete window.IPlugSendMsg;
  vi.useRealTimers();
});

describe("response curve requests", () => {
  it("sends the effect type and parameters, and delivers the reply", () => {
    const listener = vi.fn();
    effectResponse.requestEffectResponse("cab_simple", { bass: 0.7 }, listener);

    const [request] = sentOfType("getEffectResponse");
    expect(request).toMatchObject({ effectType: "cab_simple", params: { bass: 0.7 } });
    expect(effectResponse.applyEffectResponse(reply(request))).toBe(true);
    expect(listener).toHaveBeenCalledWith({ frequencies: [20, 200, 2000, 20000], magnitudesDb: [0, 6, 0, -30] });
  });

  it("keeps one request in flight and folds later changes into one follow-up", () => {
    const first = vi.fn();
    const superseded = vi.fn();
    const newest = vi.fn();
    effectResponse.requestEffectResponse("cab_simple", { bass: 0.1 }, first);
    effectResponse.requestEffectResponse("cab_simple", { bass: 0.2 }, superseded);
    effectResponse.requestEffectResponse("cab_simple", { bass: 0.3 }, newest);
    expect(sentOfType("getEffectResponse")).toHaveLength(1);

    effectResponse.applyEffectResponse(reply(sentOfType("getEffectResponse")[0]));
    const requests = sentOfType("getEffectResponse");
    expect(requests).toHaveLength(2);
    expect(requests[1].params).toEqual({ bass: 0.3 });

    effectResponse.applyEffectResponse(reply(requests[1]));
    expect(first).toHaveBeenCalledTimes(1);
    expect(superseded).not.toHaveBeenCalled();
    expect(newest).toHaveBeenCalledTimes(1);
  });

  it("ignores replies it did not ask for", () => {
    const listener = vi.fn();
    effectResponse.requestEffectResponse("cab_simple", {}, listener);
    expect(effectResponse.applyEffectResponse({ type: "effectResponse", requestId: "someone-else" })).toBe(false);
    expect(listener).not.toHaveBeenCalled();
  });

  it("reports no curve for an unsupported effect or a malformed reply", () => {
    const unsupported = vi.fn();
    effectResponse.requestEffectResponse("eq_parametric", {}, unsupported);
    effectResponse.applyEffectResponse({ type: "effectResponse", requestId: sentOfType("getEffectResponse")[0].requestId, supported: false });
    expect(unsupported).toHaveBeenCalledWith(null);

    const malformed = vi.fn();
    effectResponse.requestEffectResponse("cab_simple", {}, malformed);
    effectResponse.applyEffectResponse(reply(sentOfType("getEffectResponse")[1], { magnitudesDb: [0, 1] }));
    expect(malformed).toHaveBeenCalledWith(null);
  });

  it("gives up on an unanswered request so later ones still go out", () => {
    effectResponse.requestEffectResponse("cab_simple", { bass: 0.1 }, vi.fn());
    effectResponse.requestEffectResponse("cab_simple", { bass: 0.9 }, vi.fn());
    vi.advanceTimersByTime(effectResponse.EFFECT_RESPONSE_TIMEOUT_MS);
    const requests = sentOfType("getEffectResponse");
    expect(requests).toHaveLength(2);
    expect(requests[1].params).toEqual({ bass: 0.9 });
  });
});

describe("responseDbAt", () => {
  const curve = { frequencies: [100, 1000, 10000], magnitudesDb: [10, 0, -20] };

  it("interpolates in log frequency", () => {
    expect(effectResponse.responseDbAt(curve, 1000)).toBe(0);
    expect(effectResponse.responseDbAt(curve, Math.sqrt(100 * 1000))).toBeCloseTo(5, 10);
    expect(effectResponse.responseDbAt(curve, Math.sqrt(1000 * 10000))).toBeCloseTo(-10, 10);
  });

  it("holds the ends", () => {
    expect(effectResponse.responseDbAt(curve, 20)).toBe(10);
    expect(effectResponse.responseDbAt(curve, 20000)).toBe(-20);
  });
});

describe("Simple Cabinet IR match", () => {
  it("delivers the matched parameters, dropping anything that is not a number", () => {
    const listener = vi.fn();
    effectResponse.requestSimpleCabIrMatch("ir-42", listener);
    const [request] = sentOfType("matchSimpleCabToIr");
    expect(request.resourceId).toBe("ir-42");

    effectResponse.applySimpleCabIrMatch({
      type: "simpleCabIrMatch",
      requestId: request.requestId,
      params: { cabinet: 2, bass: 0.6, bogus: "x" },
      rmsErrorDb: 1.25,
    });
    expect(listener).toHaveBeenCalledWith({ ok: true, params: { cabinet: 2, bass: 0.6 }, rmsErrorDb: 1.25 });
  });

  it("passes on the engine's error", () => {
    const listener = vi.fn();
    effectResponse.requestSimpleCabIrMatch("missing", listener);
    effectResponse.applySimpleCabIrMatch({
      type: "simpleCabIrMatch",
      requestId: sentOfType("matchSimpleCabToIr")[0].requestId,
      error: "The IR could not be read",
    });
    expect(listener).toHaveBeenCalledWith({ ok: false, error: "The IR could not be read" });
  });

  it("drops the answer to a request a newer one replaced", () => {
    const stale = vi.fn();
    const fresh = vi.fn();
    effectResponse.requestSimpleCabIrMatch("a", stale);
    effectResponse.requestSimpleCabIrMatch("b", fresh);
    const [first, second] = sentOfType("matchSimpleCabToIr");
    expect(effectResponse.applySimpleCabIrMatch({ requestId: first.requestId, params: {} })).toBe(false);
    effectResponse.applySimpleCabIrMatch({ requestId: second.requestId, params: { cabinet: 1 }, rmsErrorDb: 0.5 });
    expect(stale).not.toHaveBeenCalled();
    expect(fresh).toHaveBeenCalledTimes(1);
    vi.advanceTimersByTime(effectResponse.EFFECT_TOOL_TIMEOUT_MS);
    expect(stale).not.toHaveBeenCalled();
  });
});

describe("export as IR", () => {
  function importEvent(type: string, detail: Record<string, unknown>): void {
    document.dispatchEvent(new CustomEvent(type, { detail }));
  }

  it("answers from the resource import carrying its request id", () => {
    const listener = vi.fn();
    effectResponse.exportEffectAsIr("cab_simple", { cabinet: 1 }, "Simple Cab 1x12 Open", listener);
    const [request] = sentOfType("exportEffectAsIr");
    expect(request).toMatchObject({ effectType: "cab_simple", params: { cabinet: 1 }, name: "Simple Cab 1x12 Open" });

    importEvent("resource-browser:resource-imported", { requestId: "another-import", name: "Other" });
    expect(listener).not.toHaveBeenCalled();
    importEvent("resource-browser:resource-imported", { requestId: request.requestId, name: "Simple Cab 1x12 Open 2" });
    expect(listener).toHaveBeenCalledWith({ ok: true, name: "Simple Cab 1x12 Open 2" });

    // Answered once: a repeat event, or the timeout, changes nothing.
    importEvent("resource-browser:resource-imported", { requestId: request.requestId, name: "again" });
    vi.advanceTimersByTime(effectResponse.EFFECT_TOOL_TIMEOUT_MS);
    expect(listener).toHaveBeenCalledTimes(1);
  });

  it("reports a failed import, and a missing answer", () => {
    const failed = vi.fn();
    effectResponse.exportEffectAsIr("cab_simple", {}, "x", failed);
    importEvent("resource-browser:resource-import-failed", {
      requestId: sentOfType("exportEffectAsIr")[0].requestId,
      message: "disk full",
    });
    expect(failed).toHaveBeenCalledWith({ ok: false, error: "disk full" });

    const unanswered = vi.fn();
    effectResponse.exportEffectAsIr("cab_simple", {}, "y", unanswered);
    vi.advanceTimersByTime(effectResponse.EFFECT_TOOL_TIMEOUT_MS);
    expect(unanswered).toHaveBeenCalledWith({ ok: false, error: "The engine did not answer" });
  });
});
