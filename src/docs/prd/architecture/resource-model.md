# Resource Model Specification

## Overview

The Resource Model defines how NeuronGuitar manages external resources such as Neural Amp Models (NAM) and Impulse Responses (IR). It provides a unified abstraction for resource storage, referencing, and resolution.

## Design Goals

1. **Unified Access**: Single interface for all resource types
2. **Deduplication**: Content-addressed storage prevents duplicates
3. **Portability**: Resources can be embedded for sharing
4. **Updateability**: Library resources can be updated globally
5. **Flexibility**: Support local files, library, and embedded

## Resource Types

| Type | Extension | Description |
|------|-----------|-------------|
| `nam` | `.nam` | Neural Amp Model files |
| `ir` | `.wav`, `.aiff` | Impulse Response audio files |

## Reference Model

### Resource Reference

Resources can be referenced in three ways:

```
struct ResourceRef:
    // Option 1: Library resource (preferred)
    resource_type: string      # "nam", "ir"
    resource_id: string        # Library ID
    
    // Option 2: File path
    file_path: string          # Relative or absolute path
    
    // Option 3: Embedded reference
    embedded_id: string        # References preset's embedded resource
```

### Resolution Priority

When resolving a ResourceRef:

1. **Library Reference**: If `resource_type` and `resource_id` are set
2. **Embedded Reference**: If `embedded_id` is set
3. **File Path**: If `file_path` is set
4. **Error**: If none are set

### Resolution Algorithm

```
function resolve_resource(ref: ResourceRef, preset: Preset) -> Path?:
    // Priority 1: Library reference
    if ref.resource_type and ref.resource_id:
        resource = library.find(ref.resource_type, ref.resource_id)
        if resource:
            return resource.file_path
    
    // Priority 2: Embedded reference
    if ref.embedded_id:
        embedded = preset.embedded_resources.find(ref.embedded_id)
        if embedded:
            // Extract to cache if needed
            return cache.get_or_extract(embedded)
    
    // Priority 3: File path
    if ref.file_path:
        path = resolve_path(ref.file_path, preset.location)
        if exists(path):
            return path
    
    return null  // Resource not found
```

## Library Resource

### Structure

```
struct LibraryResource:
    // Identity
    type: string               # "nam", "ir"
    id: string                 # Unique within type
    
    // Metadata
    name: string               # Display name
    category: string           # Grouping (e.g., "Marshall", "Fender")
    description: string        # User-facing description
    tags: list[string]         # Searchable tags
    
    // File info
    file_path: Path            # Actual file location
    hash: string               # SHA-256 content hash
    size: int                  # File size in bytes
    
    // Timestamps
    added_at: datetime
    modified_at: datetime
```

### Library Index

```json
{
  "version": 1,
  "resources": {
    "nam": [
      {
        "id": "plexi-bright",
        "name": "Plexi Bright",
        "category": "Marshall",
        "description": "Bright channel Marshall Plexi capture",
        "tags": ["vintage", "rock", "crunch"],
        "filePath": "nam/models/plexi-bright.nam",
        "hash": "sha256:abc123...",
        "size": 2048576,
        "addedAt": "2026-01-01T00:00:00Z"
      }
    ],
    "ir": [
      {
        "id": "4x12-sm57",
        "name": "4x12 SM57",
        "category": "Marshall",
        "description": "Marshall 4x12 cabinet with SM57",
        "tags": ["cabinet", "dynamic", "classic"],
        "filePath": "ir/impulses/4x12-sm57.wav",
        "hash": "sha256:def456...",
        "size": 512000,
        "addedAt": "2026-01-01T00:00:00Z"
      }
    ]
  }
}
```

## Embedded Resource

### Structure

```
struct EmbeddedResource:
    // Identity
    id: string                 # Unique within preset
    type: string               # "nam", "ir"
    name: string               # Display name
    
    // Verification
    hash: string               # SHA-256 for integrity check
    
    // Content (optional, for portable presets)
    data: string               # Base64-encoded file content
    
    // Original location (for reference)
    original_path: Path
```

### Embedding Process

```
function embed_resource(resource: LibraryResource, preset: Preset):
    // Read file content
    content = read_file(resource.file_path)
    
    // Create embedded entry
    embedded = EmbeddedResource(
        id = generate_uuid(),
        type = resource.type,
        name = resource.name,
        hash = sha256(content),
        data = base64_encode(content),
        original_path = resource.file_path
    )
    
    // Add to preset
    preset.embedded_resources.append(embedded)
    
    // Update reference to use embedded ID
    return ResourceRef(embedded_id = embedded.id)
```

### Extraction Process

```
function extract_embedded(embedded: EmbeddedResource) -> Path:
    // Check cache first
    cache_path = cache.path_for_hash(embedded.hash)
    if exists(cache_path):
        return cache_path
    
    // Decode and write to cache
    content = base64_decode(embedded.data)
    
    // Verify integrity
    if sha256(content) != embedded.hash:
        throw IntegrityError("Hash mismatch for embedded resource")
    
    write_file(cache_path, content)
    return cache_path
```

## Resource Library

### Interface

```
interface ResourceLibrary:
    // Query
    method find(type: string, id: string) -> LibraryResource?
    method get_all(type: string) -> list[LibraryResource]
    method get_by_category(type: string, category: string) -> list[LibraryResource]
    method search(type: string, query: string) -> list[LibraryResource]
    
    // Management
    method add(resource: LibraryResource)
    method update(type: string, id: string, resource: LibraryResource)
    method remove(type: string, id: string)
    
    // Resolution
    method resolve(ref: ResourceRef) -> Path?
    
    // Import
    method import_file(file_path: Path, type: string, metadata: dict) -> LibraryResource
```

### Storage Layout

```
~/.neuronguitar/
└── library/
    ├── index.json           # Library index
    ├── nam/
    │   └── models/
    │       ├── plexi-bright.nam
    │       └── jcm800-hot.nam
    └── ir/
        └── impulses/
            ├── 4x12-sm57.wav
            └── 2x12-jazz.wav
```

## Resource Cache

### Purpose

The cache stores:
- Extracted embedded resources
- Downloaded remote resources
- Temporary resources during import

### Cache Structure

```
~/.neuronguitar/
└── cache/
    └── resources/
        └── sha256/
            ├── abc123...     # Content-addressed storage
            └── def456...
```

### Cache Management

```
interface ResourceCache:
    method get(hash: string) -> Path?
    method put(content: bytes) -> string  # Returns hash
    method exists(hash: string) -> bool
    method evict(hash: string)
    method clear()
    method get_size() -> int
    method enforce_limit(max_size: int)
```

### Eviction Policy

- LRU (Least Recently Used) eviction
- Configurable maximum cache size
- Never evict resources currently in use

## Hash Verification

### Computing Hash

```
function compute_resource_hash(file_path: Path) -> string:
    hasher = SHA256()
    
    with open(file_path, "rb") as f:
        while chunk = f.read(8192):
            hasher.update(chunk)
    
    return "sha256:" + hasher.hexdigest()
```

### Verification

```
function verify_resource(file_path: Path, expected_hash: string) -> bool:
    actual_hash = compute_resource_hash(file_path)
    return actual_hash == expected_hash
```

## Import Workflow

### From File

```
function import_resource_from_file(file_path: Path, type: string):
    // Validate file
    if not validate_resource_file(file_path, type):
        throw ValidationError("Invalid resource file")
    
    // Compute hash
    hash = compute_resource_hash(file_path)
    
    // Check for existing
    existing = library.find_by_hash(type, hash)
    if existing:
        return existing  # Already in library
    
    // Generate metadata
    name = extract_name(file_path)
    category = prompt_user_for_category()
    
    // Copy to library
    dest_path = library.path_for(type, generate_id())
    copy_file(file_path, dest_path)
    
    // Create resource entry
    resource = LibraryResource(
        type = type,
        id = generate_id(),
        name = name,
        category = category,
        file_path = dest_path,
        hash = hash,
        size = file_size(dest_path)
    )
    
    library.add(resource)
    return resource
```

### From Download

```
function import_resource_from_download(url: string, type: string, expected_hash: string):
    // Download to temp location
    temp_path = download_to_temp(url)
    
    // Verify hash
    if not verify_resource(temp_path, expected_hash):
        delete_file(temp_path)
        throw IntegrityError("Downloaded resource hash mismatch")
    
    // Import from file
    return import_resource_from_file(temp_path, type)
```

## Validation

### NAM Model Validation

```
function validate_nam_model(file_path: Path) -> bool:
    try:
        // Check file header/magic bytes
        header = read_bytes(file_path, 0, 8)
        if not is_nam_header(header):
            return false
        
        // Attempt to load model metadata
        metadata = parse_nam_metadata(file_path)
        return metadata != null
    except:
        return false
```

### IR Validation

```
function validate_ir_file(file_path: Path) -> bool:
    try:
        // Check audio file format
        format = detect_audio_format(file_path)
        if format not in ["wav", "aiff"]:
            return false
        
        // Check audio properties
        info = get_audio_info(file_path)
        if info.channels > 2:
            return false
        if info.sample_rate < 22050:
            return false
        
        return true
    except:
        return false
```

## Related Documents

- [Preset System Specification](./preset-system.md)
- [Preset Data Model v2](../preset-model-v2-design.md)
- [Network Client](./network-client.md)
