/**
 * The one call a parameter change makes to bring the panel's visualisations up
 * to date: the EQ curve, the spatial panner, the cabinet response, the IR
 * Cabinet's alignment, Pickup Match's conversion curve and the Noise Gate's
 * Learn meter. Each
 * checks the node's type itself, so callers need not know which one it has.
 */

import type { GraphNode } from "../../types.js";
import { updateCabResponseVisualization } from "./cabResponse.js";
import { updateEqVisualization } from "./eq.js";
import { updateGateLearnVisualization } from "./gateLearnPanel.js";
import { updateIrAbVisualization } from "./irAbPanel.js";
import { updateIrAlignmentVisualization } from "./irAlignmentPanel.js";
import { updateMultibandVisualization } from "./multibandPanel.js";
import { updatePickupResponseVisualization } from "./pickupResponse.js";
import { updateSpatialVisualization } from "./spatial.js";

export function refreshNodeVisualizations(node: GraphNode): void {
  updateEqVisualization(node);
  updateSpatialVisualization(node);
  updateCabResponseVisualization(node);
  updateIrAlignmentVisualization(node);
  updateIrAbVisualization(node);
  updatePickupResponseVisualization(node);
  updateGateLearnVisualization(node);
  updateMultibandVisualization(node);
}
