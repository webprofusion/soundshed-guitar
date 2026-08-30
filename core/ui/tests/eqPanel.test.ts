import { beforeEach, describe, expect, it, vi } from "vitest";
import { EQ_BAND_KEYS, EQ_BAND_RANGES, EQ_FREQ_DEFAULTS } from "../ts/eqCurve.js";
import { EqPanel, type EqPanelBinding } from "../ts/eqPanel.js";

/** A binding backed by a plain dict, recording what the panel asks of it. */
function makeBinding(initial: Record<string, number> = {}) {
  const params: Record<string, number> = { ...initial };
  let enabled = false;
  const writes: Array<{ changed: Record<string, number>; commit: boolean }> = [];
  const binding: EqPanelBinding = {
    label: "test eq",
    readParams: () => params,
    writeParams: (changed, commit) => {
      Object.assign(params, changed);
      writes.push({ changed, commit });
    },
    readEnabled: () => enabled,
    writeEnabled: (next) => { enabled = next; },
  };
  return { binding, params, writes, isEnabled: () => enabled, setEnabled: (v: boolean) => { enabled = v; } };
}

function mountHost(): { bandsHost: HTMLElement; toggle: HTMLInputElement; resetButton: HTMLElement } {
  document.body.innerHTML = `
    <div id="bands"></div>
    <input type="checkbox" id="toggle" />
    <button id="reset"></button>`;
  return {
    bandsHost: document.getElementById("bands")!,
    toggle: document.getElementById("toggle") as HTMLInputElement,
    resetButton: document.getElementById("reset")!,
  };
}

beforeEach(() => {
  document.body.innerHTML = "";
});

describe("EqPanel rendering", () => {
  it("emits one band row per band in the shared topology", () => {
    const { bandsHost, toggle, resetButton } = mountHost();
    const { binding } = makeBinding();
    new EqPanel({ bandsHost, canvas: null, toggle, resetButton, idPrefix: "test" }, binding);

    expect(bandsHost.querySelectorAll(".eq-band")).toHaveLength(EQ_BAND_KEYS.length);
    // Every band offers gain and freq; Q only where the topology declares one.
    const expectedKnobs = EQ_BAND_KEYS.reduce((n, keys) => n + (keys.q ? 3 : 2), 0);
    expect(bandsHost.querySelectorAll(".knob")).toHaveLength(expectedKnobs);
  });

  it("marks itself as an .eq-bands host so the shared band styling applies", () => {
    const { bandsHost, toggle, resetButton } = mountHost();
    new EqPanel({ bandsHost, canvas: null, toggle, resetButton, idPrefix: "test" }, makeBinding().binding);
    expect(bandsHost.classList.contains("eq-bands")).toBe(true);
  });

  it("namespaces its knob ids, so two panels on one page never collide", () => {
    document.body.innerHTML = `<div id="a"></div><div id="b"></div>`;
    const hostA = document.getElementById("a")!;
    const hostB = document.getElementById("b")!;
    new EqPanel({ bandsHost: hostA, canvas: null, idPrefix: "global_eq" }, makeBinding().binding);
    new EqPanel({ bandsHost: hostB, canvas: null, idPrefix: "practice_tool_eq" }, makeBinding().binding);

    const idsOf = (host: HTMLElement) =>
      Array.from(host.querySelectorAll(".knob")).map((el) => (el as HTMLElement).dataset.param);
    const a = idsOf(hostA);
    const b = idsOf(hostB);

    expect(a.every((id) => id?.startsWith("global_eq_"))).toBe(true);
    expect(b.every((id) => id?.startsWith("practice_tool_eq_"))).toBe(true);
    expect(a.filter((id) => b.includes(id))).toEqual([]);
  });

  it("reflects the binding's enabled flag onto the toggle and the dim class", () => {
    const { bandsHost, toggle, resetButton } = mountHost();
    const backing = makeBinding();
    const panel = new EqPanel({ bandsHost, canvas: null, toggle, resetButton, idPrefix: "test" }, backing.binding);

    expect(toggle.checked).toBe(false);
    expect(bandsHost.classList.contains("is-eq-enabled")).toBe(false);

    backing.setEnabled(true);
    panel.render();
    expect(toggle.checked).toBe(true);
    expect(bandsHost.classList.contains("is-eq-enabled")).toBe(true);
  });

  it("never writes back to the binding while syncing from it", () => {
    // Pushing values into the knobs fires their own change handlers; without
    // the guard, every render would echo the whole curve back to the engine.
    const { bandsHost, toggle, resetButton } = mountHost();
    const backing = makeBinding({ lowGain: 4, highGain: -3 });
    const panel = new EqPanel({ bandsHost, canvas: null, toggle, resetButton, idPrefix: "test" }, backing.binding);

    backing.writes.length = 0;
    panel.render();
    panel.render();
    expect(backing.writes).toEqual([]);
  });
});

describe("EqPanel wiring", () => {
  it("routes the toggle through the binding and tells the host", () => {
    const { bandsHost, toggle, resetButton } = mountHost();
    const backing = makeBinding();
    const onChanged = vi.fn();
    new EqPanel({ bandsHost, canvas: null, toggle, resetButton, idPrefix: "test", onChanged }, backing.binding);

    toggle.checked = true;
    toggle.dispatchEvent(new Event("change"));

    expect(backing.isEnabled()).toBe(true);
    expect(onChanged).toHaveBeenCalled();
  });

  it("undims the bands the moment the toggle flips, without waiting for a render", () => {
    // A host that only refreshes its own chrome on change would otherwise
    // leave the panel showing a disabled EQ that is actually running.
    const { bandsHost, toggle, resetButton } = mountHost();
    new EqPanel({ bandsHost, canvas: null, toggle, resetButton, idPrefix: "test" }, makeBinding().binding);

    toggle.checked = true;
    toggle.dispatchEvent(new Event("change"));
    expect(bandsHost.classList.contains("is-eq-enabled")).toBe(true);

    toggle.checked = false;
    toggle.dispatchEvent(new Event("change"));
    expect(bandsHost.classList.contains("is-eq-enabled")).toBe(false);
  });

  it("resets every band to the topology's defaults in a single committed write", () => {
    const { bandsHost, toggle, resetButton } = mountHost();
    const backing = makeBinding({ lowGain: 9, lowMidFreq: 1500 });
    new EqPanel({ bandsHost, canvas: null, toggle, resetButton, idPrefix: "test" }, backing.binding);

    backing.writes.length = 0;
    resetButton.dispatchEvent(new Event("click"));

    expect(backing.writes).toHaveLength(1);
    expect(backing.writes[0].commit).toBe(true);

    const changed = backing.writes[0].changed;
    EQ_BAND_KEYS.forEach((keys, index) => {
      expect(changed[keys.gain]).toBe(0);
      expect(changed[keys.freq]).toBe(EQ_FREQ_DEFAULTS[index]);
      if (keys.q) {
        expect(changed[keys.q]).toBe(EQ_BAND_RANGES[index].qDefault);
      }
    });
  });

  it("tolerates a host with no toggle, reset button or canvas", () => {
    document.body.innerHTML = `<div id="bands"></div>`;
    const bandsHost = document.getElementById("bands")!;
    expect(() => {
      const panel = new EqPanel({ bandsHost, canvas: null, idPrefix: "test" }, makeBinding().binding);
      panel.render();
      panel.destroy();
    }).not.toThrow();
  });

  it("does nothing at all without a bands host, rather than throwing", () => {
    expect(() => {
      new EqPanel({ bandsHost: null, canvas: null, idPrefix: "test" }, makeBinding().binding).render();
    }).not.toThrow();
  });
});
