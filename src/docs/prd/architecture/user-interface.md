# User Interface Specification

## Overview

The NeuronGuitar user interface is implemented as a web-based single-page application (SPA) hosted within a native WebView control. This approach enables rich, responsive UI development using standard web technologies while maintaining cross-platform compatibility.

## Design Goals

1. **Responsive**: Immediate feedback for user interactions
2. **Synchronized**: UI always reflects current engine state
3. **Accessible**: Usable with keyboard and screen readers
4. **Performant**: 60fps rendering, minimal memory footprint
5. **Consistent**: Unified look across platforms

## Architecture

### Component Stack

```
┌─────────────────────────────────────────┐
│           Web UI Application            │
│  ┌─────────────────────────────────┐    │
│  │    View Components (JS/TS)      │    │
│  ├─────────────────────────────────┤    │
│  │    State Management             │    │
│  ├─────────────────────────────────┤    │
│  │    Message Handler              │    │
│  └─────────────────────────────────┘    │
└──────────────────┬──────────────────────┘
                   │ JSON Messages
                   ▼
┌─────────────────────────────────────────┐
│            WebView Bridge               │
│  ┌─────────────────────────────────┐    │
│  │    Message Serialization        │    │
│  ├─────────────────────────────────┤    │
│  │    Event Dispatching            │    │
│  └─────────────────────────────────┘    │
└──────────────────┬──────────────────────┘
                   │
                   ▼
┌─────────────────────────────────────────┐
│          Plugin Controller              │
└─────────────────────────────────────────┘
```

### WebView Host

The native plugin provides a WebView container:
- Windows: WebView2 (Chromium-based)
- macOS: WKWebView (WebKit-based)
- Resizable with plugin window
- Sandboxed execution context

## Message Protocol

### Message Format

All messages use JSON with a type discriminator:

```json
{
  "type": "messageType",
  "payload": { ... }
}
```

### Engine → UI Messages

| Type | Payload | Description |
|------|---------|-------------|
| `state` | Full state object | Complete parameter and preset state |
| `parameterChanged` | `{id, value}` | Single parameter update |
| `presetLoaded` | `{preset}` | Notification of preset load |
| `presetSearchResults` | `{results, total}` | Remote search results |
| `downloadProgress` | `{id, progress}` | Download progress update |
| `error` | `{code, message}` | Error notification |
| `resourceLoaded` | `{type, id}` | Resource load complete |

### UI → Engine Messages

| Type | Payload | Description |
|------|---------|-------------|
| `setParameter` | `{id, value}` | Update parameter value |
| `loadPreset` | `{id}` | Load local preset |
| `savePreset` | `{preset}` | Save current as preset |
| `deletePreset` | `{id}` | Delete preset |
| `search` | `{query, filters}` | Search remote presets |
| `downloadPreset` | `{id}` | Download remote preset |
| `exportPreset` | `{id, options}` | Export preset package |
| `loadResource` | `{type, ref}` | Load NAM/IR resource |
| `requestState` | `{}` | Request full state sync |

### State Object Structure

```json
{
  "parameters": {
    "input_trim": 0.0,
    "output_trim": 0.0,
    "node_1_drive": 0.5,
    "node_1_tone": 0.5,
    ...
  },
  "currentPreset": {
    "id": "...",
    "name": "...",
    "modified": false
  },
  "presets": [
    {"id": "...", "name": "...", "category": "..."},
    ...
  ],
  "library": {
    "nam": [{"id": "...", "name": "...", "category": "..."}],
    "ir": [{"id": "...", "name": "...", "category": "..."}]
  },
  "signalGraph": {
    "nodes": [...],
    "edges": [...]
  }
}
```

## Views

### Main View

Primary control surface with:
- Current preset name and save status
- Global parameter controls (input/output trim)
- Quick-access effect parameters
- Signal chain visualization (simplified)
- Level meters

### Preset Browser

Local preset management:
- Categorized preset list
- Search and filter
- Preview on hover
- Load, rename, delete actions
- Drag-and-drop organization

### Community Browser

Remote preset discovery:
- Search input with filters
- Results grid with thumbnails
- Preview and download
- Category browsing
- Trending/popular sections

### Signal Chain Editor

Visual effect chain configuration:
- Node-based graph editor
- Drag to reorder
- Add/remove nodes
- Parallel path creation
- Per-node parameter editing

### Resource Browser

NAM model and IR selection:
- Tabbed NAM/IR views
- Category filtering
- Search functionality
- Preview capability
- Import custom files

### Settings

Application preferences:
- Audio settings
- UI preferences
- Storage locations
- Account management (future)

## UI Components

### Parameter Controls

| Control | Usage |
|---------|-------|
| Knob | Continuous parameters (gain, drive) |
| Slider | Linear parameters (trim, mix) |
| Toggle | On/off states (bypass) |
| Dropdown | Selection (effect type, category) |
| Button | Actions (load, save) |

### Parameter Binding

Parameters bound bidirectionally:
1. UI change → Send message → Update engine → Broadcast change
2. Engine change → Broadcast → Update UI

**Debouncing:**
- Continuous controls debounced (50ms)
- Prevents message flooding
- Smooth parameter changes

### Level Meters

Real-time audio level visualization:
- Input level meter
- Output level meter
- Peak hold indication
- Clip warning

**Update Rate:** 30Hz (sufficient for visual feedback)

## State Management

### Local State

UI maintains local state for:
- Current view/navigation
- UI-only preferences
- Temporary editing state
- Animation state

### Synchronized State

Shared with engine:
- All parameters
- Preset selection
- Resource selection
- Graph configuration

### Synchronization Strategy

```
On startup:
    1. Request full state from engine
    2. Initialize UI with received state

On parameter change (UI):
    1. Update local state immediately
    2. Send change message to engine
    3. Mark parameter as "pending"

On parameter change (engine):
    1. Receive broadcast message
    2. Update local state
    3. Clear "pending" flag
    4. Trigger UI update
```

## Styling

### Design System

- Consistent color palette (dark theme default)
- Typography scale
- Spacing system
- Component library
- Icon set

### Theming

- CSS custom properties for theming
- Dark/light mode support
- High contrast accessibility mode

### Responsive Layout

- Minimum size: 800x600
- Maximum size: Fullscreen
- Fluid scaling between

## Performance

### Rendering

- Virtual scrolling for long lists
- Lazy loading for off-screen content
- RequestAnimationFrame for animations
- GPU-accelerated transforms

### Memory

- Component unmounting for unused views
- Image lazy loading
- Bounded message history

### Benchmarks

| Metric | Target |
|--------|--------|
| Initial Load | < 500ms |
| View Switch | < 100ms |
| Parameter Response | < 50ms |
| Frame Rate | 60fps |

## Accessibility

### Keyboard Navigation

- Tab order for all controls
- Arrow keys for lists
- Enter/Space for activation
- Escape to close modals

### Screen Reader Support

- ARIA labels on all controls
- Live regions for updates
- Semantic HTML structure

### Visual Accessibility

- Sufficient color contrast
- Scalable text
- Focus indicators

## Error Handling

### User Feedback

| Error Type | Presentation |
|------------|--------------|
| Validation | Inline message |
| Operation Failure | Toast notification |
| Connection Error | Status indicator |
| Critical Error | Modal dialog |

### Recovery

- Auto-retry for transient failures
- Offline mode fallback
- Clear error recovery actions

## Related Documents

- [WebUI Bridge](./webui-bridge.md)
- [Architecture Overview](./overview.md)
- [API Specification](./api-spec.md)
