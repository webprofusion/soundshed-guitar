# Preset System Specification

## Overview

The Preset System manages the storage, retrieval, and synchronization of NeuronGuitar presets. It supports local persistence, remote discovery, and portable preset sharing with embedded resources.

## Design Goals

1. **Local-First**: Full functionality without network connectivity
2. **Portability**: Presets can be shared with embedded dependencies
3. **Deduplication**: Hash-based caching prevents resource duplication
4. **Versioning**: Schema migrations for forward compatibility
5. **Community**: Integration with remote preset repository

## Components

### Preset Manager

Central coordinator for preset operations.

**Responsibilities:**
- CRUD operations for local presets
- Preset search and filtering
- Remote preset discovery coordination
- Import/export with resource bundling

**Operations:**
```
interface PresetManager:
    method load_preset(preset_id) -> Preset
    method save_preset(preset) -> preset_id
    method delete_preset(preset_id) -> bool
    method list_presets(filter) -> List[PresetSummary]
    method search_local(query) -> List[PresetSummary]
    method search_remote(query) -> List[PresetSummary]
    method download_preset(remote_id) -> Preset
    method export_preset(preset_id, include_resources) -> bytes
    method import_preset(data) -> Preset
```

### Preset Storage

Handles persistence to the local filesystem.

**Storage Structure:**
```
~/.neuronguitar/
├── presets/
│   ├── index.json           # Preset metadata index
│   ├── user/                 # User-created presets
│   │   ├── {uuid}.json
│   │   └── ...
│   └── downloaded/           # Downloaded presets
│       ├── {uuid}.json
│       └── ...
├── library/
│   ├── nam/                  # NAM model library
│   │   ├── index.json
│   │   └── models/
│   └── ir/                   # IR library
│       ├── index.json
│       └── impulses/
└── cache/
    └── resources/            # Extracted embedded resources
```

**Index Format:**
```json
{
  "version": 1,
  "presets": [
    {
      "id": "uuid",
      "name": "Preset Name",
      "category": "Clean",
      "author": "Author Name",
      "tags": ["tag1", "tag2"],
      "path": "user/uuid.json",
      "modifiedAt": "2026-01-09T12:00:00Z"
    }
  ]
}
```

### Resource Library

Manages the catalog of available NAM models and impulse responses.

**Responsibilities:**
- Index available resources
- Resolve resource references
- Handle resource updates
- Verify resource integrity

**Resource Resolution:**
1. Check for library resource by type + ID
2. Check for embedded resource by embedded ID
3. Check for file path reference
4. Return resolved file path or error

### Preset Service Client

Communicates with the remote preset repository.

**Responsibilities:**
- Search remote presets
- Download preset packages
- Handle authentication (optional)
- Cache search results

**API Integration:**
```
interface PresetServiceClient:
    method search(query, category, tags, page) -> SearchResults
    method get_preset(id) -> PresetPackage
    method download_resource(type, id) -> ResourceData
```

## Data Model

### Preset Structure (v2)

See [Preset Data Model v2](../preset-model-v2-design.md) for complete specification.

**Key Components:**
- Metadata (id, name, category, tags, timestamps)
- Global settings (input/output trim, transpose)
- Signal graph (nodes and edges)
- Embedded resources (optional)

### Resource Reference Resolution

Resources can be referenced in three ways:

1. **Library Reference**: `{resourceType, resourceId}`
   - References pre-installed or downloaded library resource
   - Updates automatically when library resource changes

2. **File Path**: `{filePath}`
   - Direct path to user's custom file
   - Relative paths resolved from preset location

3. **Embedded Reference**: `{embeddedId}`
   - References resource embedded in preset
   - Used for portable preset sharing

**Resolution Priority:**
1. Library reference (if resourceType + resourceId present)
2. Embedded reference (if embeddedId present)
3. File path (if filePath present)
4. Error (no valid reference)

## Operations

### Save Preset

```
1. Validate preset structure
2. Generate ID if new preset
3. Update timestamps
4. Serialize to JSON
5. Write to storage
6. Update index
```

### Load Preset

```
1. Read preset JSON from storage
2. Deserialize to preset object
3. Validate schema version
4. Migrate if necessary
5. Resolve all resource references
6. Return loaded preset
```

### Export Preset

```
1. Load preset
2. If include_resources:
   a. Identify all resource references
   b. Read resource file data
   c. Compute hashes
   d. Create embedded resource entries
   e. Update references to embedded IDs
3. Serialize to portable format
4. Return package data
```

### Import Preset

```
1. Parse package data
2. Extract embedded resources
3. For each embedded resource:
   a. Check if already in library (by hash)
   b. If not, extract to cache
   c. Optionally add to library
4. Update resource references
5. Save preset to storage
```

### Search Presets

**Local Search:**
```
1. Load index into memory
2. Apply filters (category, tags)
3. Apply text search on name/description
4. Sort by relevance/date
5. Return paginated results
```

**Remote Search:**
```
1. Send search request to API
2. Parse search results
3. Cache results locally
4. Return to caller
```

### Download Preset

```
1. Request preset package from API
2. Parse package data
3. Extract metadata and graph
4. For each required resource:
   a. Check local availability (by hash)
   b. If missing, download from API
   c. Add to library
5. Save preset locally
6. Return loaded preset
```

## Caching Strategy

### Resource Cache

- Resources cached by content hash
- Shared across all presets
- LRU eviction for space management
- Separate cache for models vs IRs

### Search Cache

- Remote search results cached
- TTL-based expiration
- Invalidated on connectivity change

### Index Cache

- Preset index loaded on startup
- In-memory for fast search
- Written on preset changes

## Schema Migration

### Version Detection
```
1. Read version field from preset
2. If version < current:
   a. Apply migrations in order
   b. Update version field
3. Validate migrated preset
```

### Migration Registry
```
migrations = {
    1 -> 2: migrate_v1_to_v2,
    // future migrations
}
```

### Migration Example (v1 → v2)
```
v1 format:
  - Linear effect chain array
  - Direct file paths

v2 format:
  - Signal graph with nodes/edges
  - Resource references with library support

Migration:
  1. Create input/output nodes
  2. Convert each effect to graph node
  3. Create linear edges between nodes
  4. Convert file paths to resource refs
```

## Error Handling

| Error | Handling |
|-------|----------|
| Preset not found | Return null, notify UI |
| Schema invalid | Attempt repair, fail gracefully |
| Resource missing | Mark as unresolved, continue |
| Network error | Use cache, retry later |
| Storage full | Notify user, suggest cleanup |

## Configuration

| Setting | Default | Description |
|---------|---------|-------------|
| Presets Directory | ~/.neuronguitar/presets | Local storage path |
| Max Cache Size | 500 MB | Resource cache limit |
| Search Cache TTL | 1 hour | Remote search cache duration |
| Auto-Download | true | Download missing resources |

## Related Documents

- [Preset Data Model v2](../preset-model-v2-design.md)
- [Resource Model](./resource-model.md)
- [Network Client](./network-client.md)
- [API Specification](./api-spec.md)
