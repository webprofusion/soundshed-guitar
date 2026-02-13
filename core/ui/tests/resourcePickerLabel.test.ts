import { describe, expect, it } from "vitest";
import { findMatchingResourcePickerLabel, type ResourcePickerLabelLike } from "../ts/resourcePickerLabel";

function makeLabel(dataset: ResourcePickerLabelLike["dataset"]): ResourcePickerLabelLike {
  return { dataset };
}

describe("findMatchingResourcePickerLabel", () => {
  it("matches by node + type + index", () => {
    const labels = [
      makeLabel({ nodeId: "amp-1", resourceType: "nam", resourceIndex: "0" }),
      makeLabel({ nodeId: "amp-1", resourceType: "nam", resourceIndex: "1" }),
    ];

    const result = findMatchingResourcePickerLabel(labels, "amp-1", "nam", 1);
    expect(result).toBe(labels[1]);
  });

  it("matches exposed resource id when provided", () => {
    const labels = [
      makeLabel({ nodeId: "comp-1", resourceType: "ir", resourceIndex: "0", exposedResourceId: "a" }),
      makeLabel({ nodeId: "comp-1", resourceType: "ir", resourceIndex: "0", exposedResourceId: "b" }),
    ];

    const result = findMatchingResourcePickerLabel(labels, "comp-1", "ir", 0, "b");
    expect(result).toBe(labels[1]);
  });

  it("defaults missing index to zero", () => {
    const labels = [
      makeLabel({ nodeId: "amp-1", resourceType: "nam" }),
    ];

    const result = findMatchingResourcePickerLabel(labels, "amp-1", "nam", 0);
    expect(result).toBe(labels[0]);
  });

  it("returns null when no candidate matches", () => {
    const labels = [
      makeLabel({ nodeId: "amp-1", resourceType: "nam", resourceIndex: "0" }),
    ];

    const result = findMatchingResourcePickerLabel(labels, "amp-2", "nam", 0);
    expect(result).toBeNull();
  });
});