import { appendLog } from "./logging.js";

const NAMBridge = {
  postMessage(message: unknown): void {
    if (window.IPlugSendMsg) {
      window.IPlugSendMsg(message);
    }
  },
};

window.NAMBridge = NAMBridge;

export function postMessage(payload: unknown): void {
  NAMBridge.postMessage(payload);
}

export function setParameter(id: string, value: number): void {
  postMessage({
    type: "setParameter",
    id,
    value,
  });
  appendLog(`${id} → ${value}`);
}
