import { beforeEach, describe, expect, it } from "vitest";
import { createVuMeter } from "../ts/vuMeter.js";
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
