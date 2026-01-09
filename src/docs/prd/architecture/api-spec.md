# API Specification

## Overview

This document specifies the REST API for the NeuronGuitar remote preset service. The API enables preset discovery, download, and community sharing.

## Base URL

```
Production: https://api.neuronguitar.io/v1
Staging:    https://api-staging.neuronguitar.io/v1
```

## Authentication

### Public Endpoints

The following endpoints are publicly accessible:
- `GET /presets/search`
- `GET /presets/{id}`
- `GET /resources/{type}/{id}`
- `GET /health`

### Authenticated Endpoints (Future)

Future endpoints requiring authentication:
- `POST /presets` - Upload preset
- `PUT /presets/{id}` - Update preset
- `DELETE /presets/{id}` - Delete preset

Authentication will use Bearer tokens:
```
Authorization: Bearer <token>
```

## Common Headers

### Request Headers

| Header | Required | Description |
|--------|----------|-------------|
| `Content-Type` | Yes (POST/PUT) | `application/json` |
| `Accept` | No | `application/json` (default) |
| `User-Agent` | Yes | Client identifier |
| `X-Client-Version` | Yes | Plugin version |

### Response Headers

| Header | Description |
|--------|-------------|
| `Content-Type` | Response media type |
| `X-Request-Id` | Unique request identifier |
| `X-Rate-Limit-Remaining` | Remaining requests in window |
| `X-Rate-Limit-Reset` | Time until rate limit resets |

## Error Handling

### Error Response Format

```json
{
  "error": {
    "code": "RESOURCE_NOT_FOUND",
    "message": "The requested preset was not found",
    "details": {
      "presetId": "abc123"
    }
  }
}
```

### Error Codes

| HTTP Status | Code | Description |
|-------------|------|-------------|
| 400 | INVALID_REQUEST | Malformed request |
| 400 | VALIDATION_ERROR | Invalid parameters |
| 401 | UNAUTHORIZED | Authentication required |
| 403 | FORBIDDEN | Insufficient permissions |
| 404 | RESOURCE_NOT_FOUND | Resource does not exist |
| 429 | RATE_LIMITED | Too many requests |
| 500 | INTERNAL_ERROR | Server error |
| 503 | SERVICE_UNAVAILABLE | Temporary unavailability |

---

## Endpoints

### Health Check

Check service availability.

```
GET /health
```

**Response: 200 OK**
```json
{
  "status": "healthy",
  "version": "1.0.0",
  "timestamp": "2026-01-09T12:00:00Z"
}
```

---

### Search Presets

Search for presets with optional filters.

```
GET /presets/search
```

**Query Parameters:**

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `q` | string | - | Free text search |
| `category` | string | - | Filter by category |
| `tags` | string | - | Comma-separated tags |
| `author` | string | - | Filter by author |
| `sort` | string | `relevance` | Sort field |
| `order` | string | `desc` | Sort order (asc/desc) |
| `page` | int | 1 | Page number |
| `limit` | int | 20 | Results per page (max 100) |

**Sort Options:**
- `relevance` - Search relevance score
- `date` - Creation date
- `downloads` - Download count
- `rating` - Average rating
- `name` - Alphabetical

**Example Request:**
```
GET /presets/search?q=crunch&category=Rock&sort=downloads&limit=10
```

**Response: 200 OK**
```json
{
  "total": 156,
  "page": 1,
  "pageSize": 10,
  "totalPages": 16,
  "results": [
    {
      "id": "abc123",
      "name": "Vintage Crunch",
      "author": {
        "id": "user456",
        "name": "ToneHunter"
      },
      "category": "Crunch",
      "tags": ["marshall", "classic", "rock"],
      "description": "Classic rock crunch tone inspired by 70s British amps",
      "downloads": 1234,
      "rating": {
        "average": 4.5,
        "count": 89
      },
      "resources": [
        {"type": "nam", "name": "Plexi Bright"}
      ],
      "createdAt": "2026-01-01T12:00:00Z",
      "updatedAt": "2026-01-05T08:30:00Z",
      "thumbnailUrl": "https://cdn.neuronguitar.io/thumbs/abc123.png"
    }
  ]
}
```

---

### Get Preset

Retrieve full preset details.

```
GET /presets/{id}
```

**Path Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `id` | string | Preset identifier |

**Response: 200 OK**
```json
{
  "preset": {
    "id": "abc123",
    "name": "Vintage Crunch",
    "version": 2,
    "author": {
      "id": "user456",
      "name": "ToneHunter"
    },
    "category": "Crunch",
    "tags": ["marshall", "classic", "rock"],
    "description": "Classic rock crunch tone...",
    "global": {
      "inputTrim": -3.0,
      "outputTrim": 0.0
    },
    "graph": {
      "nodes": [
        {"id": "in", "type": "input"},
        {"id": "gate", "type": "gate_noise", "params": {"threshold": -55.0}},
        {
          "id": "amp",
          "type": "nam_amp",
          "resource": {"resourceType": "nam", "resourceId": "plexi-bright"},
          "params": {"drive": 0.6, "tone": 0.5}
        },
        {
          "id": "cab",
          "type": "ir_cab",
          "resource": {"resourceType": "ir", "resourceId": "4x12-sm57"}
        },
        {"id": "out", "type": "output"}
      ],
      "edges": [
        {"from": "in", "to": "gate"},
        {"from": "gate", "to": "amp"},
        {"from": "amp", "to": "cab"},
        {"from": "cab", "to": "out"}
      ]
    },
    "createdAt": "2026-01-01T12:00:00Z",
    "updatedAt": "2026-01-05T08:30:00Z"
  },
  "resources": [
    {
      "type": "nam",
      "id": "plexi-bright",
      "name": "Plexi Bright",
      "category": "Marshall",
      "hash": "sha256:abc123def456...",
      "size": 2048576,
      "downloadUrl": "/resources/nam/plexi-bright"
    },
    {
      "type": "ir",
      "id": "4x12-sm57",
      "name": "4x12 SM57",
      "category": "Marshall",
      "hash": "sha256:789ghi012jkl...",
      "size": 512000,
      "downloadUrl": "/resources/ir/4x12-sm57"
    }
  ]
}
```

**Response: 404 Not Found**
```json
{
  "error": {
    "code": "RESOURCE_NOT_FOUND",
    "message": "Preset not found",
    "details": {"presetId": "xyz999"}
  }
}
```

---

### Download Resource

Download a resource file (NAM model or IR).

```
GET /resources/{type}/{id}
```

**Path Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `type` | string | Resource type (`nam`, `ir`) |
| `id` | string | Resource identifier |

**Response: 200 OK**
```
Content-Type: application/octet-stream
Content-Length: 2048576
Content-Disposition: attachment; filename="plexi-bright.nam"
X-Content-Hash: sha256:abc123def456...
```
*[Binary file data]*

**Response: 404 Not Found**
```json
{
  "error": {
    "code": "RESOURCE_NOT_FOUND",
    "message": "Resource not found"
  }
}
```

---

### List Categories

Get available preset categories.

```
GET /categories
```

**Response: 200 OK**
```json
{
  "categories": [
    {"id": "clean", "name": "Clean", "count": 234},
    {"id": "crunch", "name": "Crunch", "count": 567},
    {"id": "highgain", "name": "High Gain", "count": 890},
    {"id": "bass", "name": "Bass", "count": 123},
    {"id": "acoustic", "name": "Acoustic", "count": 45}
  ]
}
```

---

### List Tags

Get popular tags.

```
GET /tags
```

**Query Parameters:**

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `limit` | int | 50 | Maximum tags to return |

**Response: 200 OK**
```json
{
  "tags": [
    {"name": "marshall", "count": 456},
    {"name": "fender", "count": 389},
    {"name": "metal", "count": 567},
    {"name": "blues", "count": 234}
  ]
}
```

---

## Rate Limiting

### Limits

| Endpoint | Limit | Window |
|----------|-------|--------|
| Search | 60 requests | 1 minute |
| Get Preset | 120 requests | 1 minute |
| Download Resource | 30 requests | 1 minute |

### Rate Limit Response

When rate limited, the API returns:

**Response: 429 Too Many Requests**
```json
{
  "error": {
    "code": "RATE_LIMITED",
    "message": "Rate limit exceeded",
    "details": {
      "retryAfter": 45
    }
  }
}
```

Headers:
```
Retry-After: 45
X-Rate-Limit-Remaining: 0
X-Rate-Limit-Reset: 1704801245
```

---

## Pagination

### Request

Use `page` and `limit` parameters:
```
GET /presets/search?q=rock&page=2&limit=20
```

### Response

Pagination info included in response:
```json
{
  "total": 156,
  "page": 2,
  "pageSize": 20,
  "totalPages": 8,
  "results": [...]
}
```

### Navigation

Calculate next/previous pages:
- Next page: `page + 1` (if `page < totalPages`)
- Previous page: `page - 1` (if `page > 1`)

---

## Versioning

### API Version

The API version is included in the URL path:
```
https://api.neuronguitar.io/v1/presets/search
```

### Version Changes

Major version changes (v1 → v2) may include breaking changes.
Minor updates within a version maintain backwards compatibility.

### Deprecation

Deprecated endpoints return a warning header:
```
X-Deprecated: This endpoint will be removed in v2. Use /presets/search instead.
```

---

## Related Documents

- [Network Client Specification](./network-client.md)
- [Preset System Specification](./preset-system.md)
- [Security Specification](./security.md)
