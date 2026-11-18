/**
 * The node parameters panel — the public face of ./paramsPanel/.
 *
 * Selecting a node in the chain draws its controls here. The panel itself is one
 * dispatcher over many control kinds, so each kind lives in its own module and
 * this file re-exports only what the rest of signalPath actually calls.
 */

export { markNodeParamsPanelStale, redrawNodeParamsPanelIfOutOfDate, showNodeParamsPanel } from "./paramsPanel/panel.js";
export { buildDefaultParamControlsHtml, formatParamLabel, isToggleParam, showEngineParamChange } from "./paramsPanel/paramControls.js";
export { applySpatialPositionUpdate } from "./paramsPanel/spatial.js";
export { collapseSplitTitle } from "./paramsPanel/splitBranches.js";
