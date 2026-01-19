# Network API

## Key Files
- `src/src/network/PresetServiceClient.h` — HTTP client for remote preset service
- `src/src/platform/vst3/` — VST3 wrapper implementation
- `src/src/platform/au/` — Audio Unit wrapper (macOS)
- `src/src/platform/app/` — Standalone application

## Overview

This document covers the REST API for remote preset services, the network client implementation, and plugin format wrappers (VST3, AU, AAX, Standalone).

---

## REST API Specification

### Base URL
```
Production: https://api.guitarfx.io/v1
```

### Common Headers

| Header | Required | Description |
|--------|----------|-------------|
| `Content-Type` | Yes (POST) | `application/json` |
| `User-Agent` | Yes | Client identifier |
| `X-Client-Version` | Yes | Plugin version |

### Error Response Format
```json
{
  "error": {
    "code": "RESOURCE_NOT_FOUND",
    "message": "The requested preset was not found",
    "details": {"presetId": "abc123"}
  }
}
```

### Error Codes

| HTTP | Code | Description |
|------|------|-------------|
| 400 | INVALID_REQUEST | Malformed request |
| 400 | VALIDATION_ERROR | Invalid parameters |
| 404 | RESOURCE_NOT_FOUND | Resource not found |
| 429 | RATE_LIMITED | Too many requests |
| 500 | INTERNAL_ERROR | Server error |

---

### Endpoints

#### Search Presets
```
GET /presets/search?q=crunch&category=Rock&page=1&limit=20
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `q` | string | — | Free text search |
| `category` | string | — | Filter by category |
| `tags` | string | — | Comma-separated tags |
| `sort` | string | `relevance` | Sort field |
| `page` | int | 1 | Page number |
| `limit` | int | 20 | Results per page (max 100) |

**Response:**
```json
{
  "total": 156,
  "page": 1,
  "pageSize": 20,
  "results": [
    {
      "id": "abc123",
      "name": "Vintage Crunch",
      "author": {"id": "user456", "name": "ToneHunter"},
      "category": "Crunch",
      "tags": ["marshall", "rock"],
      "downloads": 1234,
      "rating": {"average": 4.5, "count": 89}
    }
  ]
}
```

#### Get Preset
```
GET /presets/{id}
```

**Response:**
```json
{
  "preset": { /* Full PresetV2 object */ },
  "resources": [
    {
      "type": "nam",
      "id": "plexi-bright",
      "hash": "sha256:abc123...",
      "size": 2048576,
      "downloadUrl": "/resources/nam/plexi-bright"
    }
  ]
}
```

#### Download Resource
```
GET /resources/{type}/{id}
```

Returns binary file with headers:
```
Content-Type: application/octet-stream
X-Content-Hash: sha256:abc123...
```

---

## Network Client

### Interface
```cpp
class PresetServiceClient {
    SearchResult Search(const SearchQuery& query);
    PresetPackage GetPreset(const std::string& id);
    bool DownloadResource(const std::string& type, const std::string& id, const std::string& destPath);
    ServiceStatus HealthCheck();
};
```

### Configuration

| Setting | Default | Description |
|---------|---------|-------------|
| Timeout | 30s | Request timeout |
| Retry Count | 3 | Automatic retries |
| Retry Delay | 1s, 2s, 4s | Exponential backoff |

### Caching

| Layer | TTL | Storage |
|-------|-----|---------|
| Search results | 1 hour | In-memory |
| Preset metadata | 24 hours | On-disk |
| Resources | Permanent | Content-addressed on-disk |

### Offline Mode
- Search returns cached results only
- Downloads queued for retry on connectivity
- UI shows offline indicator

### Rate Limiting

| Endpoint | Limit |
|----------|-------|
| Search | 60/minute |
| Get Preset | 120/minute |
| Download | 30/minute |

---

## Plugin Formats

### VST3 (Windows, macOS)

**SDK**: VST3 SDK 3.7.0+

**Features:**
- Full parameter automation
- Preset management via host
- Latency compensation reporting
- Stereo I/O (1 stereo bus)

**Installation:**
- Windows: `C:\Program Files\Common Files\VST3\`
- macOS: `~/Library/Audio/Plug-Ins/VST3/`

### Audio Unit (macOS)

**SDK**: macOS SDK 10.13+

**Types:**
- AU v2 (legacy compatibility)
- AU v3 (App Extension)

**Installation:** `~/Library/Audio/Plug-Ins/Components/`

### AAX (Windows, macOS)

**SDK**: AAX SDK 2.4.0+ (requires Avid developer agreement)

**Features:**
- Pro Tools integration
- Parameter pages for control surface
- PACE signing required for release

**Installation:**
- Windows: `C:\Program Files\Common Files\Avid\Audio\Plug-Ins\`
- macOS: `/Library/Application Support/Avid/Audio/Plug-Ins/`

### Standalone Application

**Audio APIs:**
- Windows: WASAPI, ASIO
- macOS: CoreAudio

**Features:**
- Audio device selection
- Buffer size configuration
- Optional MIDI for preset switching
- No DAW required

---

## Common Plugin Interface

All formats implement:

```cpp
interface PluginInterface {
    void Initialize(double sampleRate, int maxBlockSize);
    void Process(float** inputs, float** outputs, int numSamples);
    void SetParameter(int index, float value);
    float GetParameter(int index);
    void GetState(std::vector<uint8_t>& data);
    void SetState(const std::vector<uint8_t>& data);
    void CreateEditor(void* parentWindow);
};
```

### State Serialization
1. Version header
2. Parameter values (normalized 0–1)
3. Preset JSON blob (compressed)

### Latency Reporting
Each format reports processing latency to the host for automatic delay compensation.

---

## Build Targets

```
SoundshedGuitar_VST3    # VST3 plugin bundle
GuitarFX_AU      # Audio Unit bundle (macOS)
GuitarFX_AAX     # AAX plugin (requires SDK)
SoundshedGuitar_App     # Standalone application
```

### Environment Variables
```
VST3_SDK_ROOT=/path/to/vst3sdk
AAX_SDK_ROOT=/path/to/aax-sdk
```

---

## See Also
- [Architecture Overview](architecture-overview.md) — System layers
- [Data Models](data-models.md) — Preset schema
- [User Interface](user-interface.md) — UI message protocol
