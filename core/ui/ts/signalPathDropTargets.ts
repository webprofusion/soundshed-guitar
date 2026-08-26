/**
 * Pure drop-resolution rules for dragging a signal-chain node.
 *
 * Kept free of DOM and graph plumbing so the ordering rules — which are easy to
 * get subtly wrong and awkward to verify by hand in the running app — can be
 * unit tested. The caller does the hit-testing and hands the outcome here.
 */

/** Minimum vertical travel before a drag counts as a bypass flick. */
export const NODE_BYPASS_DRAG_DISTANCE_PX = 36;
/** How much more vertical than horizontal that travel has to be. */
export const NODE_BYPASS_DRAG_DIRECTION_RATIO = 1.2;

/** The parts of an edge reference the drop rules care about. */
export interface SignalPathDropEdge {
  from: string;
  to: string;
}

export type NodeDropTarget<TEdge extends SignalPathDropEdge> =
  | {
    kind: "node";
    nodeId: string;
    /** True when the target sits to the left of the dragged node on screen. */
    isLeftOfDragged: boolean;
    /** Edges arriving at the target node. */
    incomingEdges: TEdge[];
  }
  | { kind: "edge"; edge: TEdge };

export type NodeDropAction<TEdge extends SignalPathDropEdge> =
  /** Move the node into the slot directly after `targetNodeId`. */
  | { kind: "reorderAfterNode"; nodeId: string; targetNodeId: string }
  /** Move the node onto a specific connection. */
  | { kind: "reorderToEdge"; nodeId: string; edge: TEdge }
  | { kind: "toggleBypass"; nodeId: string }
  | { kind: "none" };

/**
 * A mostly-vertical flick with no drop target toggles the node's bypass, which
 * is quicker than aiming for the small bypass button.
 */
export function isBypassToggleGesture(deltaX: number, deltaY: number): boolean {
  return Math.abs(deltaY) >= NODE_BYPASS_DRAG_DISTANCE_PX
    && Math.abs(deltaY) > Math.abs(deltaX) * NODE_BYPASS_DRAG_DIRECTION_RATIO;
}

/**
 * Decide what a released node drag should do.
 *
 * Dropping onto a node inserts *after* it, which is what a rightward move
 * wants. A leftward move means the opposite: the user is aiming at the slot the
 * target currently occupies, so the target's incoming connection is used
 * instead. A target fed by more than one edge is a parallel branch join, where
 * "the slot before it" is ambiguous, so those fall back to inserting after.
 */
export function resolveNodeDropAction<TEdge extends SignalPathDropEdge>(input: {
  draggedNodeId: string;
  target: NodeDropTarget<TEdge> | null;
  gesture: { deltaX: number; deltaY: number };
}): NodeDropAction<TEdge> {
  const { draggedNodeId, target, gesture } = input;
  if (!draggedNodeId) return { kind: "none" };

  if (!target) {
    return isBypassToggleGesture(gesture.deltaX, gesture.deltaY)
      ? { kind: "toggleBypass", nodeId: draggedNodeId }
      : { kind: "none" };
  }

  if (target.kind === "edge") {
    // Dropping back onto one of the node's own connections changes nothing.
    if (target.edge.from === draggedNodeId || target.edge.to === draggedNodeId) {
      return { kind: "none" };
    }
    return { kind: "reorderToEdge", nodeId: draggedNodeId, edge: target.edge };
  }

  if (target.nodeId === draggedNodeId) return { kind: "none" };

  if (target.isLeftOfDragged && target.incomingEdges.length === 1) {
    const edge = target.incomingEdges[0];
    if (edge.from === draggedNodeId || edge.to === draggedNodeId) {
      return { kind: "reorderAfterNode", nodeId: draggedNodeId, targetNodeId: target.nodeId };
    }
    return { kind: "reorderToEdge", nodeId: draggedNodeId, edge };
  }

  return { kind: "reorderAfterNode", nodeId: draggedNodeId, targetNodeId: target.nodeId };
}
