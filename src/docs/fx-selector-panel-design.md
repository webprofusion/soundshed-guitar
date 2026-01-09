# FX Equipment Selector Panel Design

## Overview

A categorized panel for browsing and adding effects to the signal path via drag-and-drop.

## UI Structure

```
┌─────────────────────────────────────────────────┐
│ FX LIBRARY                              [─] [×] │
├─────────────────────────────────────────────────┤
│ 🔍 Search effects...                            │
├─────────────────────────────────────────────────┤
│ ▼ DYNAMICS                                      │
│   ┌─────────┐ ┌─────────┐ ┌─────────┐          │
│   │ 🚪      │ │ 📊      │ │ 💡      │          │
│   │ Noise   │ │ VCA     │ │ Opto    │          │
│   │ Gate    │ │ Comp    │ │ Comp    │          │
│   └─────────┘ └─────────┘ └─────────┘          │
│                                                 │
│ ▶ AMPLIFIERS                                    │
│ ▶ CABINETS                                      │
│ ▶ EQUALIZERS                                    │
│ ▶ MODULATION                                    │
│ ▶ DELAY                                         │
│ ▶ REVERB                                        │
│ ▶ UTILITY                                       │
└─────────────────────────────────────────────────┘
```

## Categories

| Category   | Icon | Color   | Description                        |
|------------|------|---------|-------------------------------------|
| dynamics   | ⚡   | #e04848 | Gates, compressors, limiters        |
| amp        | 🎸   | #e07848 | NAM models, amp sims                |
| cab        | 🔊   | #a86830 | IR loaders, cab sims                |
| eq         | 🎚️   | #48a8e0 | Parametric, graphic, tilt EQ        |
| modulation | 🌊   | #9048e0 | Chorus, flanger, phaser, tremolo    |
| delay      | ⏱️   | #48e0a8 | Digital, tape, analog delays        |
| reverb     | 🏛️   | #4878e0 | Room, hall, plate, spring reverbs   |
| utility    | 🔧   | #808080 | Gain, splitter, mixer               |

## Component Files

### 1. HTML Panel
Add to `index.html` after node-params-panel:

```html
<aside class="fx-selector-panel" id="fx-selector-panel">
  <div class="fx-selector-header">
    <span class="fx-selector-title">FX Library</span>
    <div class="fx-selector-actions">
      <button class="fx-selector-collapse" id="fx-selector-collapse">─</button>
      <button class="fx-selector-close" id="fx-selector-close">×</button>
    </div>
  </div>
  <div class="fx-selector-search">
    <input type="text" id="fx-search-input" placeholder="Search effects..." />
  </div>
  <div class="fx-selector-categories" id="fx-selector-categories">
    <!-- Categories rendered by JavaScript -->
  </div>
</aside>
```

### 2. TypeScript Module: `fxSelector.ts`

```typescript
// Key exports
export function initFxSelector(): void;
export function toggleFxSelectorPanel(visible?: boolean): void;
export function renderFxCategories(): void;

// Internal state
let isCollapsed = false;
let expandedCategories = new Set<string>();
let searchFilter = "";

// Category definitions with metadata
const FX_CATEGORIES = [
  { id: "dynamics", name: "Dynamics", icon: "⚡", color: "#e04848" },
  { id: "amp", name: "Amplifiers", icon: "🎸", color: "#e07848" },
  { id: "cab", name: "Cabinets", icon: "🔊", color: "#a86830" },
  { id: "eq", name: "Equalizers", icon: "🎚️", color: "#48a8e0" },
  { id: "modulation", name: "Modulation", icon: "🌊", color: "#9048e0" },
  { id: "delay", name: "Delay", icon: "⏱️", color: "#48e0a8" },
  { id: "reverb", name: "Reverb", icon: "🏛️", color: "#4878e0" },
  { id: "utility", name: "Utility", icon: "🔧", color: "#808080" }
];

// Drag-drop handlers
function handleFxItemDragStart(e: DragEvent, effectType: string): void;
function handleSignalPathDrop(e: DragEvent): void;
```

### 3. CSS Styles: Add to `styles.css`

```css
/* FX Selector Panel */
.fx-selector-panel { /* Right sidebar panel */ }
.fx-selector-header { /* Title bar with collapse/close */ }
.fx-selector-search { /* Search input container */ }
.fx-selector-categories { /* Scrollable category list */ }
.fx-category { /* Collapsible category header */ }
.fx-category-items { /* Grid of effect items */ }
.fx-item { /* Individual draggable effect */ }
.fx-item:hover { /* Hover state */ }
.fx-item.dragging { /* While being dragged */ }

/* Signal path drop zones */
.signal-path-nodes.drag-active { /* When dragging from selector */ }
.signal-connector.drop-target { /* Valid drop position indicator */ }
```

## Interaction Flow

### Adding an Effect

1. User opens FX Library panel (button in toolbar or keyboard shortcut)
2. User expands a category (e.g., "Dynamics")
3. User drags an effect (e.g., "Noise Gate") from the panel
4. Signal path shows drop zones between existing nodes
5. User drops effect at desired position
6. UI sends `addNode` message to plugin:
   ```json
   {
     "type": "addNode",
     "effectType": "dynamics_gate",
     "insertAfter": "amp_1"  // or "__input__" for start
   }
   ```
7. Plugin creates node with default parameters
8. Plugin updates graph edges
9. Plugin sends updated preset state
10. UI re-renders signal path

### Search Filtering

- Filters across all categories
- Matches against: type, displayName, category
- Shows only matching categories (expanded)
- Highlights matching text

### Keyboard Shortcuts

| Key      | Action                              |
|----------|-------------------------------------|
| `E`      | Toggle FX Library panel             |
| `Escape` | Close FX Library / cancel drag      |
| `1-8`    | Quick-expand category by number     |

## Message Protocol

### UI → Plugin Messages

```typescript
// Add new node to signal path
interface AddNodeMessage {
  type: "addNode";
  effectType: string;        // Effect type from registry
  insertAfter: string;       // Node ID to insert after (or "__input__")
  branchIndex?: number;      // For parallel paths (optional)
}
```

### Plugin → UI Messages

Existing `state` message with updated preset graph.

## Integration Points

### signalPath.ts Updates

1. Add drop zone handling when dragging from FX selector
2. Show visual indicators for valid drop positions
3. Handle `addNode` by updating local state optimistically

### main.ts Updates

1. Import and initialize FX selector module
2. Add toolbar button to toggle panel
3. Register keyboard shortcuts

### presetV2.ts Updates

1. Export `createDefaultNode(effectType: string): GraphNode`
2. Function creates node with default parameters from registry

## Implementation Phases

### Phase 1: Basic Panel (MVP)
- [ ] HTML structure
- [ ] CSS styling
- [ ] Category rendering from EffectTypeRegistry
- [ ] Expand/collapse categories
- [ ] Panel open/close

### Phase 2: Drag-Drop
- [ ] Draggable FX items
- [ ] Drop zones in signal path
- [ ] Visual feedback during drag
- [ ] `addNode` message sending

### Phase 3: Search & Polish
- [ ] Search input with filtering
- [ ] Keyboard shortcuts
- [ ] Animations/transitions
- [ ] Responsive layout (collapse on narrow screens)

### Phase 4: Advanced Features
- [ ] Favorites/recent effects
- [ ] Custom effect presets
- [ ] Right-click context menu on FX items
- [ ] Drag to rearrange categories

## Visual Design Notes

- Panel slides in from right edge
- Semi-transparent background when open
- Effect items use same node styling as signal path
- Compact grid layout (3 items per row)
- Category headers have subtle gradient matching category color
- Smooth expand/collapse animations
