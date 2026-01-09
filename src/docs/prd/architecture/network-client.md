# Network Client Specification

## Overview

The Network Client handles all communication between NeuronGuitar and remote services, primarily the community preset repository. It provides search, download, and (future) upload capabilities.

## Design Goals

1. **Reliability**: Handle network failures gracefully
2. **Performance**: Non-blocking operations, caching
3. **Security**: HTTPS, input validation
4. **Offline Support**: Full functionality without network
5. **Extensibility**: Support future service integrations

## Architecture

```
┌─────────────────────────────────────────┐
│           Preset Manager                │
└──────────────────┬──────────────────────┘
                   │
                   ▼
┌─────────────────────────────────────────┐
│         Preset Service Client           │
│  ┌─────────────────────────────────┐    │
│  │    Request Builder              │    │
│  ├─────────────────────────────────┤    │
│  │    Response Parser              │    │
│  ├─────────────────────────────────┤    │
│  │    Cache Manager                │    │
│  └─────────────────────────────────┘    │
└──────────────────┬──────────────────────┘
                   │
                   ▼
┌─────────────────────────────────────────┐
│           HTTP Client                   │
│  (Platform-specific implementation)     │
└─────────────────────────────────────────┘
```

## HTTP Client

### Interface

```
interface HttpClient:
    method get(url, headers) -> Response
    method post(url, body, headers) -> Response
    method download(url, destination, progress_callback) -> bool
    
struct Response:
    status_code: int
    headers: map[string, string]
    body: bytes
```

### Configuration

| Setting | Default | Description |
|---------|---------|-------------|
| Base URL | configurable | API endpoint |
| Timeout | 30s | Request timeout |
| Retry Count | 3 | Automatic retries |
| Retry Delay | 1s, 2s, 4s | Exponential backoff |
| User Agent | NeuronGuitar/1.0 | Client identifier |

## Preset Service Client

### Public Interface

```
interface PresetServiceClient:
    // Search presets
    method search(query: SearchQuery) -> SearchResult
    
    // Get preset details
    method get_preset(id: string) -> PresetPackage
    
    // Download resource file
    method download_resource(type: string, id: string) -> ResourceData
    
    // Check service availability
    method health_check() -> ServiceStatus
```

### Search Query

```
struct SearchQuery:
    text: string            # Free text search
    category: string        # Filter by category
    tags: list[string]      # Filter by tags
    author: string          # Filter by author
    sort_by: string         # "relevance", "date", "downloads"
    sort_order: string      # "asc", "desc"
    page: int               # Page number (1-based)
    page_size: int          # Results per page (default: 20)
```

### Search Result

```
struct SearchResult:
    total: int              # Total matching results
    page: int               # Current page
    page_size: int          # Results per page
    results: list[PresetSummary]
    
struct PresetSummary:
    id: string
    name: string
    author: string
    category: string
    tags: list[string]
    description: string
    downloads: int
    rating: float
    created_at: datetime
    thumbnail_url: string   # Optional preview image
```

### Preset Package

```
struct PresetPackage:
    preset: PresetV2        # Full preset data
    resources: list[ResourceInfo]
    
struct ResourceInfo:
    type: string            # "nam", "ir"
    id: string              # Resource identifier
    name: string            # Display name
    hash: string            # Content hash for verification
    size: int               # File size in bytes
    download_url: string    # Direct download URL
```

## API Specification

### Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/v1/presets/search` | GET | Search presets |
| `/api/v1/presets/{id}` | GET | Get preset details |
| `/api/v1/resources/{type}/{id}` | GET | Download resource |
| `/api/v1/health` | GET | Service health check |

### Search Request

```
GET /api/v1/presets/search?q=crunch&category=rock&page=1&limit=20

Response:
{
  "total": 156,
  "page": 1,
  "pageSize": 20,
  "results": [
    {
      "id": "abc123",
      "name": "Vintage Crunch",
      "author": "ToneHunter",
      "category": "Crunch",
      "tags": ["marshall", "classic", "rock"],
      "description": "Classic rock crunch tone...",
      "downloads": 1234,
      "rating": 4.5,
      "createdAt": "2026-01-01T12:00:00Z"
    }
  ]
}
```

### Get Preset

```
GET /api/v1/presets/abc123

Response:
{
  "preset": {
    "id": "abc123",
    "name": "Vintage Crunch",
    "version": 2,
    "global": {...},
    "graph": {...}
  },
  "resources": [
    {
      "type": "nam",
      "id": "plexi-bright",
      "name": "Plexi Bright",
      "hash": "sha256:abc...",
      "size": 2048576,
      "downloadUrl": "/api/v1/resources/nam/plexi-bright"
    }
  ]
}
```

### Download Resource

```
GET /api/v1/resources/nam/plexi-bright

Response: Binary file data
Headers:
  Content-Type: application/octet-stream
  Content-Length: 2048576
  X-Content-Hash: sha256:abc...
```

## Caching

### Cache Layers

1. **Search Cache**: In-memory, TTL-based
2. **Preset Cache**: On-disk, hash-verified
3. **Resource Cache**: On-disk, content-addressed

### Search Cache

```
struct SearchCacheEntry:
    query_hash: string
    result: SearchResult
    cached_at: datetime
    ttl: duration          # Default: 1 hour

function get_cached_search(query) -> SearchResult?:
    hash = compute_hash(query)
    entry = cache.get(hash)
    if entry and not expired(entry):
        return entry.result
    return null
```

### Resource Cache

Resources cached by content hash:

```
cache/
└── resources/
    └── sha256/
        ├── abc123...  # Actual file content
        └── def456...
```

### Cache Invalidation

| Trigger | Action |
|---------|--------|
| TTL expiration | Remove entry |
| Storage limit | LRU eviction |
| Manual refresh | Clear specific entry |
| Network recovery | Validate cached entries |

## Error Handling

### Error Types

| Error | Handling |
|-------|----------|
| Network unreachable | Use cache, queue for retry |
| Timeout | Retry with backoff |
| 4xx errors | Return error to caller |
| 5xx errors | Retry with backoff |
| Invalid response | Log and return error |

### Retry Strategy

```
function request_with_retry(request):
    delays = [1000, 2000, 4000]  # milliseconds
    
    for attempt in range(len(delays) + 1):
        try:
            response = http_client.execute(request)
            if response.status < 500:
                return response
        except NetworkError:
            pass
        
        if attempt < len(delays):
            sleep(delays[attempt])
    
    throw MaxRetriesExceeded()
```

### Offline Mode

When network unavailable:
- Search returns cached results only
- Downloads queued for later
- UI shows offline indicator
- Background retry on connectivity

## Security

### Transport Security

- HTTPS required for all requests
- Certificate validation enabled
- TLS 1.2+ required

### Input Validation

```
function validate_search_query(query):
    // Sanitize text input
    query.text = sanitize(query.text, max_length=100)
    
    // Validate category against known list
    if query.category not in VALID_CATEGORIES:
        query.category = null
    
    // Limit tags
    query.tags = query.tags[:10]
    
    // Bound pagination
    query.page = clamp(query.page, 1, 1000)
    query.page_size = clamp(query.page_size, 1, 100)
```

### Resource Verification

```
function verify_download(data, expected_hash):
    actual_hash = compute_sha256(data)
    if actual_hash != expected_hash:
        throw IntegrityError("Hash mismatch")
```

## Rate Limiting

### Client-Side Limits

| Operation | Limit |
|-----------|-------|
| Search requests | 10/minute |
| Preset downloads | 30/hour |
| Resource downloads | 100/hour |

### Handling Server Limits

```
Response: 429 Too Many Requests
Headers:
  Retry-After: 60

Handling:
  - Honor Retry-After header
  - Queue subsequent requests
  - Notify UI of rate limit
```

## Authentication (Future)

### Planned Features

- User accounts for preset upload
- API keys for extended limits
- OAuth integration options

### Token Management

```
struct AuthToken:
    access_token: string
    refresh_token: string
    expires_at: datetime
    
function ensure_valid_token():
    if token.expires_at < now() + 5_minutes:
        token = refresh_token(token.refresh_token)
    return token.access_token
```

## Monitoring

### Metrics

| Metric | Description |
|--------|-------------|
| Request count | Total requests by endpoint |
| Error rate | Failures per endpoint |
| Latency | P50, P95, P99 response times |
| Cache hit rate | Cached vs fresh requests |

### Logging

```
Log format:
{
  "timestamp": "...",
  "level": "info|warn|error",
  "operation": "search|download|...",
  "duration_ms": 123,
  "status": 200,
  "cache_hit": false
}
```

## Related Documents

- [Preset System Specification](./preset-system.md)
- [API Specification](./api-spec.md)
- [Security Specification](./security.md)
