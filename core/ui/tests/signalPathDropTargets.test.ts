import { describe, expect, it } from "vitest";
import {
  isBypassToggleGesture,
  resolveNodeDropAction,
  type SignalPathDropEdge,
} from "../ts/signalPathDropTargets";

const edge = (from: string, to: string): SignalPathDropEdge => ({ from, to });
const noGesture = { deltaX: 0, deltaY: 0 };

describe("resolveNodeDropAction", () => {
  it("inserts after the target when dragging rightwards", () => {
    expect(resolveNodeDropAction({
      draggedNodeId: "drive",
      target: { kind: "node", nodeId: "delay", isLeftOfDragged: false, incomingEdges: [edge("amp", "delay")] },
      gesture: noGesture,
    })).toEqual({ kind: "reorderAfterNode", nodeId: "drive", targetNodeId: "delay" });
  });

  it("takes the target's slot when dragging leftwards", () => {
    expect(resolveNodeDropAction({
      draggedNodeId: "delay",
      target: { kind: "node", nodeId: "drive", isLeftOfDragged: true, incomingEdges: [edge("__input__", "drive")] },
      gesture: noGesture,
    })).toEqual({ kind: "reorderToEdge", nodeId: "delay", edge: edge("__input__", "drive") });
  });

  it("falls back to inserting after when a leftward target joins a parallel branch", () => {
    expect(resolveNodeDropAction({
      draggedNodeId: "delay",
      target: {
        kind: "node",
        nodeId: "mix",
        isLeftOfDragged: true,
        incomingEdges: [edge("branchA", "mix"), edge("branchB", "mix")],
      },
      gesture: noGesture,
    })).toEqual({ kind: "reorderAfterNode", nodeId: "delay", targetNodeId: "mix" });
  });

  it("moves the node onto a dropped-on connection", () => {
    expect(resolveNodeDropAction({
      draggedNodeId: "reverb",
      target: { kind: "edge", edge: edge("drive", "amp") },
      gesture: noGesture,
    })).toEqual({ kind: "reorderToEdge", nodeId: "reverb", edge: edge("drive", "amp") });
  });

  it("ignores a drop back onto one of the node's own connections", () => {
    expect(resolveNodeDropAction({
      draggedNodeId: "amp",
      target: { kind: "edge", edge: edge("drive", "amp") },
      gesture: noGesture,
    })).toEqual({ kind: "none" });
    expect(resolveNodeDropAction({
      draggedNodeId: "amp",
      target: { kind: "edge", edge: edge("amp", "cab") },
      gesture: noGesture,
    })).toEqual({ kind: "none" });
  });

  it("ignores a drop onto the dragged node itself", () => {
    expect(resolveNodeDropAction({
      draggedNodeId: "amp",
      target: { kind: "node", nodeId: "amp", isLeftOfDragged: false, incomingEdges: [] },
      gesture: noGesture,
    })).toEqual({ kind: "none" });
  });

  it("toggles bypass on a vertical flick with no target", () => {
    expect(resolveNodeDropAction({
      draggedNodeId: "amp",
      target: null,
      gesture: { deltaX: 4, deltaY: -60 },
    })).toEqual({ kind: "toggleBypass", nodeId: "amp" });
  });

  it("does nothing when released away from a target without a flick", () => {
    expect(resolveNodeDropAction({
      draggedNodeId: "amp",
      target: null,
      gesture: { deltaX: 120, deltaY: 10 },
    })).toEqual({ kind: "none" });
  });
});

describe("isBypassToggleGesture", () => {
  it("requires enough vertical travel", () => {
    expect(isBypassToggleGesture(0, 20)).toBe(false);
    expect(isBypassToggleGesture(0, 36)).toBe(true);
  });

  it("requires the travel to be mostly vertical", () => {
    expect(isBypassToggleGesture(40, 40)).toBe(false);
    expect(isBypassToggleGesture(10, 40)).toBe(true);
  });

  it("works in both directions", () => {
    expect(isBypassToggleGesture(-5, -40)).toBe(true);
  });
});
