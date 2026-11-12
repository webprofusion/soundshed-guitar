import { describe, expect, it } from "vitest";
import { buildDefaultParamControlsHtml, showsParamGroupTitle } from "../ts/parameterControlMarkup.js";

describe("default parameter control markup", () => {
  it("renders grouped knob and toggle previews without a live signal-path panel", () => {
    const html = buildDefaultParamControlsHtml([
      { key: "output_level", name: "", default: -3, min: -12, max: 12, unit: "dB", group: "Main" },
      { key: "enabled", name: "Enabled", default: 1, min: 0, max: 1, unit: "toggle", group: "Main" },
    ], "designer-preview");

    expect(html).toContain("node-param-group-title\">Main");
    expect(html).toContain('data-node-id="designer-preview"');
    expect(html).toContain("Output Level");
    expect(html).toContain("-3.0dB");
    expect(html).toContain('data-param-key="enabled" checked disabled');
  });

  it("puts ungrouped controls in one untitled inset panel", () => {
    const html = buildDefaultParamControlsHtml([
      { key: "drive", name: "Drive", default: 0.5, min: 0, max: 1, unit: "amount" },
      { key: "level", name: "Level", default: 0.5, min: 0, max: 1, unit: "amount" },
    ]);

    expect(html.match(/node-param-group-block node-param-group-block-untitled/g)).toHaveLength(1);
    expect(html).not.toContain("node-param-group-title");
    expect(html.match(/node-param-knob/g)).toHaveLength(2);
    expect(buildDefaultParamControlsHtml([])).toBe("");
  });

  it("titles no group that says nothing: the ungrouped rest, or a lone knob of the same name", () => {
    const html = buildDefaultParamControlsHtml([
      { key: "threshold", name: "Threshold", default: 0.5, min: 0, max: 1, unit: "amount", group: "Gate" },
      { key: "release", name: "Release", default: 0.5, min: 0, max: 1, unit: "amount", group: "Gate" },
      { key: "swell", name: "Swell", default: 0.5, min: 0, max: 1, unit: "amount", group: "SWELL" },
      { key: "level", name: "Level", default: 0.5, min: 0, max: 1, unit: "amount" },
    ]);

    expect(html.match(/class="node-param-group-title">/g)).toHaveLength(1);
    expect(html.match(/node-param-group-title-spacer/g)).toHaveLength(2);
    expect(html).toContain("node-param-group-title\">Gate");
    expect(html).not.toContain(">Other<");
    expect(html.match(/node-param-group-block-untitled/g)).toHaveLength(2);
  });

  it("keeps the title over a lone control with another name", () => {
    expect(showsParamGroupTitle("Tone", ["Bass"])).toBe(true);
    expect(showsParamGroupTitle("Swell", [" swell "])).toBe(false);
    expect(showsParamGroupTitle("", ["Level", "Drive"])).toBe(false);
  });
});
