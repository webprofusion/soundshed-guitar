import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { createVuMeter, vuGaugeDeflection, vuPeakDeflection } from "../ts/vuMeter.js";
import type { SignalLevelMetrics } from "../ts/types.js";

const THRESHOLDS = [-3, -6, -9, -12, -18, -24, -36, -48];

function mountMeter(prefix: string): void {
  document.body.innerHTML = `
    <div class="vu-meter">
      <span class="vu-readout" id="${prefix}-readout">-∞ dB</span>
      <div class="vu-segments" id="${prefix}-segments">
        ${THRESHOLDS.map((db) => `<div class="vu-seg" data-db="${db}"></div>`).join("")}
        <div class="vu-peak-hold" id="${prefix}-peak-hold"></div>
      </div>
    </div>`;
}

function make(prefix: string) {
  return createVuMeter({
    segmentsId: `${prefix}-segments`,
    peakHoldId: `${prefix}-peak-hold`,
    readoutId: `${prefix}-readout`,
  });
}

function readout(prefix: string): string {
  return document.getElementById(`${prefix}-readout`)?.textContent ?? "";
}

function litThresholds(prefix: string): number[] {
  return Array.from(document.querySelectorAll<HTMLElement>(`#${prefix}-segments .vu-seg.active`))
    .map((seg) => Number(seg.dataset.db));
}

function levels(peakDbfs: number): SignalLevelMetrics {
  return { peakDbfs, rmsDbfs: peakDbfs - 6, headroomDb: -peakDbfs, clipped: false, clipCount: 0 };
}

describe("createVuMeter", () => {
  beforeEach(() => {
    mountMeter("vu");
  });

  it("lights every segment at or below the current peak", () => {
    const meter = make("vu");

    meter.update(levels(-20));

    expect(litThresholds("vu")).toEqual([-24, -36, -48]);
  });

  it("lights the whole ladder when the signal is at full scale", () => {
    const meter = make("vu");

    meter.update(levels(0));

    expect(litThresholds("vu")).toEqual(THRESHOLDS);
  });

  it("lights nothing below the quietest segment", () => {
    const meter = make("vu");

    meter.update(levels(-60));

    expect(litThresholds("vu")).toEqual([]);
  });

  it("clears the meter when the frame carries no finite level", () => {
    const meter = make("vu");
    meter.update(levels(-10));
    expect(litThresholds("vu")).not.toEqual([]);

    meter.update(null);

    expect(litThresholds("vu")).toEqual([]);
    expect(document.getElementById("vu-peak-hold")?.classList.contains("visible")).toBe(false);
  });

  it("parks the peak-hold tick at the top edge of the loudest lit segment", () => {
    const meter = make("vu");

    // -20 dBFS lights from the -24 segment down, which is index 5 in DOM order.
    meter.update(levels(-20));

    const tick = document.getElementById("vu-peak-hold")!;
    expect(tick.classList.contains("visible")).toBe(true);
    expect(tick.style.top).toBe(`${5 * 7}px`);
  });

  it("holds the tick when the signal falls away", () => {
    const meter = make("vu");
    meter.update(levels(-10));
    const heldTop = document.getElementById("vu-peak-hold")!.style.top;

    meter.update(levels(-40));

    expect(litThresholds("vu")).toEqual([-48]);
    expect(document.getElementById("vu-peak-hold")!.style.top).toBe(heldTop);
  });

  it("keeps each meter's peak hold to itself", () => {
    document.body.innerHTML = "";
    const host = document.createElement("div");
    document.body.appendChild(host);
    host.innerHTML = `
      <span class="vu-readout" id="a-readout">-∞ dB</span>
      <div class="vu-segments" id="a-segments">
        ${THRESHOLDS.map((db) => `<div class="vu-seg" data-db="${db}"></div>`).join("")}
        <div class="vu-peak-hold" id="a-peak-hold"></div>
      </div>
      <span class="vu-readout" id="b-readout">-∞ dB</span>
      <div class="vu-segments" id="b-segments">
        ${THRESHOLDS.map((db) => `<div class="vu-seg" data-db="${db}"></div>`).join("")}
        <div class="vu-peak-hold" id="b-peak-hold"></div>
      </div>`;

    const input = make("a");
    const output = make("b");

    input.update(levels(0));
    output.update(levels(-40));

    expect(litThresholds("a")).toEqual(THRESHOLDS);
    expect(litThresholds("b")).toEqual([-48]);
    expect(document.getElementById("a-peak-hold")!.style.top).toBe("0px");
    expect(document.getElementById("b-peak-hold")!.style.top).toBe(`${7 * 7}px`);
    expect(readout("a")).toBe("0 dB");
    expect(readout("b")).toBe("-40 dB");
  });

  it("reads out the held peak in whole dB", () => {
    const meter = make("vu");

    meter.update(levels(-12.4));

    expect(readout("vu")).toBe("-12 dB");
  });

  it("marks an over with a plus", () => {
    const meter = make("vu");

    meter.update(levels(2.7));

    expect(readout("vu")).toBe("+3 dB");
  });

  it("holds the readout with the tick when the signal falls away", () => {
    const meter = make("vu");
    meter.update(levels(-9));

    meter.update(levels(-30));

    expect(readout("vu")).toBe("-9 dB");
  });

  it("floors the readout below the ladder rather than printing the -120 dBFS silence value", () => {
    const meter = make("vu");

    meter.update(levels(-120));

    expect(readout("vu")).toBe("-∞ dB");
    expect(litThresholds("vu")).toEqual([]);
  });

  it("still reads out a signal too quiet to light any segment", () => {
    const meter = make("vu");

    meter.update(levels(-54));

    expect(litThresholds("vu")).toEqual([]);
    expect(readout("vu")).toBe("-54 dB");
  });

  it("clears the readout with the meter", () => {
    const meter = make("vu");
    meter.update(levels(-9));

    meter.update(null);

    expect(readout("vu")).toBe("-∞ dB");
  });

  it("does nothing when the markup is missing", () => {
    document.body.innerHTML = "";
    const meter = make("absent");

    expect(() => meter.update(levels(-10))).not.toThrow();
  });
});

describe("vuGaugeDeflection", () => {
  it("puts 0 VU (-18 dBFS RMS) at 1/1.413 of the sweep and stops at +3 VU", () => {
    expect(vuGaugeDeflection(-18)).toBeCloseTo(1 / Math.SQRT2, 5);
    expect(vuGaugeDeflection(-15)).toBeCloseTo(1, 2);
    expect(vuGaugeDeflection(0)).toBe(1);
  });

  it("rests at zero for silence", () => {
    expect(vuGaugeDeflection(-Infinity)).toBe(0);
    expect(vuGaugeDeflection(-120)).toBeLessThan(0.001);
  });
});

describe("vuPeakDeflection", () => {
  it("puts full scale at the end stop and -3 dBFS under 0 VU, where the red zone starts", () => {
    expect(vuPeakDeflection(0)).toBe(1);
    expect(vuPeakDeflection(3)).toBe(1);
    expect(vuPeakDeflection(-3)).toBeCloseTo(vuGaugeDeflection(-18), 2);
    expect(vuPeakDeflection(-6)).toBeCloseTo(0.5, 2);
  });

  it("rests at zero for silence", () => {
    expect(vuPeakDeflection(-Infinity)).toBe(0);
  });
});

describe("the needle gauge", () => {
  beforeEach(() => {
    document.body.innerHTML = `
      <div class="vu-segments" id="g-segments"><div class="vu-seg"></div><div class="vu-peak-hold" id="g-peak-hold"></div></div>
      <div class="vu-gauge" id="g-gauge"><div class="vu-needle"></div></div>`;
  });

  const needle = (): number =>
    parseFloat(document.getElementById("g-gauge")?.style.getPropertyValue("--vu-needle") ?? "NaN");
  const peakDot = (): number =>
    parseFloat(document.getElementById("g-gauge")?.style.getPropertyValue("--vu-peak") ?? "NaN");
  const meter = () => createVuMeter({ segmentsId: "g-segments", peakHoldId: "g-peak-hold", gaugeId: "g-gauge" });

  it("eases toward the level rather than jumping to it", () => {
    const m = meter();
    m.update(levels(-6)); // RMS -12 dBFS: past 0 VU, against the stop
    const first = needle();
    m.update(levels(-6));
    const second = needle();

    expect(first).toBeGreaterThan(-42);
    expect(second).toBeGreaterThan(first);

    for (let i = 0; i < 20; i++) m.update(levels(-6));

    expect(needle()).toBeCloseTo(42, 0);
  });

  it("drops to rest when the frames stop", () => {
    const m = meter();
    for (let i = 0; i < 5; i++) m.update(levels(-6));

    m.update(null);

    expect(needle()).toBe(-42);
    expect(peakDot()).toBe(-42);
  });

  describe("the red peak dot", () => {
    let clock = 1000;

    beforeEach(() => {
      clock = 1000;
      vi.spyOn(performance, "now").mockImplementation(() => clock);
    });

    afterEach(() => {
      vi.restoreAllMocks();
    });

    // One 20 Hz frame later.
    const frame = (m: ReturnType<typeof meter>, peakDbfs: number): void => {
      clock += 50;
      m.update(levels(peakDbfs));
    };

    it("jumps to the frame's peak on the first frame", () => {
      const m = meter();

      frame(m, -6); // half the sweep: upright

      expect(peakDot()).toBeCloseTo(0, 0);
    });

    it("falls 20 dB in 1.7 s rather than dropping with the signal", () => {
      const m = meter();
      frame(m, -6);

      frame(m, -60);
      expect(peakDot()).toBeGreaterThan(-5); // 0.6 dB down after one frame

      for (let i = 0; i < 33; i++) frame(m, -60); // 1.7 s in all
      const expected = -42 + 84 * vuPeakDeflection(-26);
      expect(peakDot()).toBeCloseTo(expected, 0);
    });

    it("pins against the stop when the signal clips", () => {
      const m = meter();

      frame(m, 0.5);

      expect(peakDot()).toBe(42);
    });
  });
});
