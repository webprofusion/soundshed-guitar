# User Interface

## Key Files
- `src/resources/ui/ts/messages.ts` — Message type definitions and handlers
- `src/resources/ui/ts/state.ts` — UI state management
- `src/resources/ui/ts/main.ts` — Application entry point
- `src/src/GuitarFXPlugin.cpp` — `HandleUIMessage()` for C++ side
- `src/src/ui/WebUIBridge.h` — Native bridge interface

## Overview

The UI is a web-based single-page application (SPA) hosted in a native WebView. Communication with the plugin uses a bidirectional JSON message protocol. The UI maintains local state synchronized with the engine via events.

## Architecture

```
┌──────────────────────────────────────┐
│          Web UI (TypeScript)         │
│  ┌────────────────────────────────┐  │
│  │     View Components            │  │
│  ├────────────────────────────────┤  │
│  │     State Management           │  │
│  ├────────────────────────────────┤  │
│  │     Message Handler            │  │
│  └────────────────────────────────┘  │
└───────────────┬──────────────────────┘
                │ JSON Messages
┌───────────────▼──────────────────────┐
│          WebUI Bridge (C++)          │
│  Serialize/deserialize, dispatch     │
└───────────────┬──────────────────────┘
                │
┌───────────────▼──────────────────────┐
│       Plugin Controller (C++)        │
└──────────────────────────────────────┘
```

### WebView Host
- **Windows**: WebView2 (Chromium-based)
- **macOS**: WKWebView (WebKit-based)
- Sandboxed execution, communication only through message bridge

## Message Protocol

### Message Format
```json
{
  "type": "messageType",
  "payload": { ... },
  "timestamp": 1704801234567
}
```

### Engine → UI Messages

| Type | Payload | Description |
|------|---------|-------------|
| `state` | Full state object | Complete sync on startup/major changes |
| `parameterChanged` | `{id, value}` | Single parameter update |
| `presetLoaded` | `{id, name, success}` | Preset load notification |
| `presetSearchResults` | `{query, total, results}` | Remote search results |
| `downloadProgress` | `{id, progress}` | Resource download progress |
| `resourceLoaded` | `{type, id}` | Resource load complete |
| `error` | `{code, message, details}` | Error notification |

### UI → Engine Messages

| Type | Payload | Description |
|------|---------|-------------|
| `setParameter` | `{id, value}` | Update parameter value |
| `loadPreset` | `{id}` | Load preset by ID |
| `savePreset` | `{preset}` | Save current state as preset |
| `deletePreset` | `{id}` | Delete preset |
| `search` | `{query, category, tags, page}` | Search remote presets |
| `downloadPreset` | `{id}` | Download remote preset |
| `loadResource` | `{type, ref}` | Load NAM/IR resource |
| `requestState` | `{}` | Request full state sync |
| `addSignalPathNode` | `{node, afterNodeId}` | Add effect to graph |
| `removeSignalPathNode` | `{nodeId}` | Remove effect from graph |
| `replaceSignalPathNode` | `{nodeId, newNode}` | Replace effect in graph |

## State Object

Sent via `state` message on startup and major changes:

```json
{
  "parameters": {
    "input_trim": 0.0,
    "output_trim": -3.0,
    "amp1_drive": 0.65
  },
  "currentPreset": {
    "id": "preset-123",
    "name": "My Crunch Tone",
    "modified": true
  },
  "presets": [
    {"id": "preset-1", "name": "Clean", "category": "Clean"}
  ],
  "library": {
    "nam": [{"id": "plexi-bright", "name": "Plexi Bright", "category": "Marshall"}],
    "ir": [{"id": "4x12-sm57", "name": "4x12 SM57", "category": "Marshall"}]
  },
  "signalGraph": {
    "nodes": [...],
    "edges": [...]
  }
}
```

## JavaScript Bridge

### Sending Messages (UI → Engine)
```typescript
window.NAMBridge.postMessage("setParameter", { id: "amp1_drive", value: 0.72 });
```

### Receiving Messages (Engine → UI)
```typescript
// Called by native code
window.IPlugReceiveData = function(jsonString) {
    const message = JSON.parse(jsonString);
    handleMessage(message);
};
```

## Synchronization

### Startup Sequence
1. WebView loads UI application
2. UI sends `requestState` message
3. Engine sends `state` message with full snapshot
4. UI renders initial state

### Parameter Updates
```
UI changes parameter:
1. User adjusts control
2. UI updates local state immediately (optimistic)
3. UI sends setParameter message (debounced 50ms)
4. Engine updates parameter
5. Engine broadcasts parameterChanged
6. UI receives confirmation

Engine changes parameter (automation):
1. DAW writes automation value
2. Engine sends parameterChanged
3. UI updates display
```

### Conflict Resolution
Engine value is authoritative. If UI receives `parameterChanged` with a different value than it sent, it adopts the engine value.

## UI Views

| View | Purpose |
|------|---------|
| **Main** | Amp panel, global controls, level meters |
| **Preset Browser** | Local preset management, search, load/save |
| **Community Browser** | Remote preset search and download |
| **Signal Chain Editor** | Visual node-based effect chain |
| **Resource Browser** | NAM model and IR selection |
| **Settings** | Audio preferences, storage, theme |

## Settings → Audio

### Interface Calibration (NAM)

This global setting aligns NAM input metadata (dBu, RMS) to the DAW’s peak reference (dBFS peak) for auto‑leveling. It is **on by default** to provide consistent first‑run behavior.

**Behavior**
- When enabled and a model provides input metadata, the auto‑level input gain uses:
  - $\text{effectiveInputLevel} = \text{modelInputLevel}_{\mathrm{dBu}} - \text{referenceDbu}$
- When disabled, the model’s input metadata is used as‑is.
- If a per‑node calibration input level is present, it always takes precedence.

**Defaults**
- Enabled: **true**
- Reference level: **+12.0 dBu @ 0 dBFS peak**

## Parameter Controls

| Control | Usage |
|---------|-------|
| Knob | Continuous parameters (gain, drive) |
| Slider | Linear parameters (trim, mix) |
| Toggle | On/off states (bypass) |
| Dropdown | Selection (effect type, category) |
| Button | Actions (load, save, browse) |

## Signal Chain Editor Notes

- To create parallel paths, add the **Splitter** effect from the Utility category. The join **Mixer** node is inserted automatically and is not user-addable.

## Performance Targets

| Metric | Target |
|--------|--------|
| Initial Load | < 500ms |
| View Switch | < 100ms |
| Parameter Response | < 50ms |
| Frame Rate | 60fps |

## Error Handling

| Error Type | Presentation |
|------------|--------------|
| Validation | Inline message near control |
| Operation Failure | Toast notification |
| Connection Error | Status indicator |
| Critical Error | Modal dialog |

## Accessibility

- Tab order for all controls
- ARIA labels on interactive elements
- Keyboard navigation (arrows for lists, Enter/Space for activation)
- Sufficient color contrast, scalable text

## See Also
- [Theme System](theme-system.md) — CSS theming
- [Architecture Overview](architecture-overview.md) — System layers
- [Signal Chain](signal-chain.md) — Graph modification messages
