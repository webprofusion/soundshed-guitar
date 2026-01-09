# Security Specification

## Overview

This document outlines security requirements and implementations for NeuronGuitar, covering data protection, secure communication, input validation, and privacy considerations.

## Security Principles

1. **Defense in Depth**: Multiple layers of security controls
2. **Least Privilege**: Minimal permissions required for operation
3. **Secure by Default**: Safe defaults for all settings
4. **Fail Securely**: Graceful handling of security failures
5. **Privacy First**: Minimal data collection, user consent required

## Threat Model

### Assets to Protect

| Asset | Sensitivity | Protection Priority |
|-------|-------------|---------------------|
| User presets | Medium | High |
| Audio data | Low | Medium |
| User preferences | Low | Low |
| Account credentials (future) | High | Critical |
| Downloaded resources | Low | Medium |

### Threat Categories

| Category | Examples | Mitigation |
|----------|----------|------------|
| Network attacks | MITM, eavesdropping | HTTPS, certificate pinning |
| Malicious files | Crafted NAM/IR files | Input validation, sandboxing |
| Code injection | XSS in WebView | CSP, input sanitization |
| Denial of service | Resource exhaustion | Rate limiting, resource caps |
| Privacy violation | Data exfiltration | Minimal collection, consent |

## Transport Security

### HTTPS Enforcement

All network communication uses HTTPS:
- TLS 1.2 minimum required
- TLS 1.3 preferred when available
- Weak cipher suites disabled

### Certificate Validation

```
Certificate Requirements:
- Valid certificate chain to trusted root
- Certificate not expired
- Hostname matches requested domain
- No revoked certificates in chain
```

### Certificate Pinning (Optional)

For enhanced security against MITM:
```
Pinned certificates:
- api.neuronguitar.io: sha256/ABC123...
- cdn.neuronguitar.io: sha256/DEF456...

Backup pins for certificate rotation included
```

## Input Validation

### File Validation

#### NAM Model Files

```
function validate_nam_file(file_path):
    // Size limits
    if file_size(file_path) > 100_MB:
        reject("File too large")
    
    // Magic bytes check
    header = read_bytes(file_path, 0, 8)
    if not valid_nam_header(header):
        reject("Invalid NAM file format")
    
    // Structure validation
    try:
        metadata = parse_nam_structure(file_path)
        validate_metadata_fields(metadata)
    catch ParseError:
        reject("Corrupted NAM file")
    
    return valid
```

#### IR Files

```
function validate_ir_file(file_path):
    // Size limits
    if file_size(file_path) > 50_MB:
        reject("File too large")
    
    // Format check
    format = detect_audio_format(file_path)
    if format not in ["wav", "aiff"]:
        reject("Unsupported audio format")
    
    // Audio properties
    info = parse_audio_header(file_path)
    if info.channels > 2:
        reject("Too many channels")
    if info.sample_rate > 192000:
        reject("Sample rate too high")
    if info.duration > 10.0:  // seconds
        reject("IR too long")
    
    return valid
```

#### Preset Files

```
function validate_preset_json(json_string):
    // Size limit
    if len(json_string) > 1_MB:
        reject("Preset too large")
    
    // Parse with depth limit
    try:
        preset = parse_json(json_string, max_depth=10)
    catch:
        reject("Invalid JSON")
    
    // Schema validation
    errors = validate_against_schema(preset, PRESET_SCHEMA)
    if errors:
        reject("Schema validation failed: " + errors)
    
    // Sanitize strings
    sanitize_all_strings(preset)
    
    return preset
```

### API Input Validation

```
function validate_search_query(params):
    // Text query
    if params.q:
        params.q = sanitize_string(params.q)
        params.q = truncate(params.q, 100)
    
    // Category
    if params.category:
        if params.category not in VALID_CATEGORIES:
            params.category = null
    
    // Pagination
    params.page = clamp(int(params.page) or 1, 1, 1000)
    params.limit = clamp(int(params.limit) or 20, 1, 100)
    
    // Sort
    if params.sort not in VALID_SORT_FIELDS:
        params.sort = "relevance"
    
    return params
```

## WebView Security

### Content Security Policy

```
Content-Security-Policy:
    default-src 'self';
    script-src 'self';
    style-src 'self' 'unsafe-inline';
    img-src 'self' data:;
    connect-src 'self' https://api.neuronguitar.io;
    font-src 'self';
    object-src 'none';
    frame-ancestors 'none';
```

### Sandboxing

WebView runs in sandboxed context:
- No file system access
- No native code execution
- Communication only through message bridge
- No navigation to external URLs

### Message Validation

```
function validate_ui_message(message):
    // Parse JSON
    try:
        msg = parse_json(message)
    catch:
        return null
    
    // Check type
    if msg.type not in VALID_MESSAGE_TYPES:
        log_warning("Unknown message type: " + msg.type)
        return null
    
    // Validate payload based on type
    schema = MESSAGE_SCHEMAS[msg.type]
    if not validate_against_schema(msg.payload, schema):
        return null
    
    return msg
```

## Resource Integrity

### Hash Verification

All downloaded resources verified before use:

```
function verify_resource_integrity(file_path, expected_hash):
    // Compute hash
    actual_hash = sha256_file(file_path)
    
    // Compare
    if not constant_time_compare(actual_hash, expected_hash):
        delete_file(file_path)
        throw IntegrityError("Resource hash mismatch")
    
    return true
```

### Cache Security

```
Cache directory permissions:
- Owner: Read/Write
- Group: None
- Other: None

Cache entries:
- Named by content hash (no user input in filenames)
- Validated before use
- Purged on verification failure
```

## Authentication (Future)

### Token Security

```
Token storage:
- Platform secure storage (Keychain/Credential Manager)
- Never logged or transmitted except to auth endpoints
- Short-lived access tokens (1 hour)
- Refresh tokens for renewal

Token transmission:
- HTTPS only
- Authorization header (not URL)
- No caching of authenticated responses
```

### Session Management

```
Session security:
- Secure, HttpOnly cookies (web admin)
- CSRF protection with tokens
- Session timeout after inactivity
- Single active session option
```

## Privacy

### Data Collection

**Collected with consent only:**
- Anonymous usage telemetry
- Crash reports (with user approval)

**Never collected:**
- Audio content
- Preset contents
- Personal information without consent

### Telemetry (Opt-in)

```
Telemetry data (if enabled):
- Plugin version
- OS type and version
- Feature usage counts
- Performance metrics
- Error types (no personal data)

NOT collected:
- IP addresses
- Hardware identifiers
- File paths
- Preset names or content
- Audio data
```

### Data Retention

| Data Type | Retention | Location |
|-----------|-----------|----------|
| Local presets | User-controlled | Local filesystem |
| Cache files | Auto-cleanup | Local filesystem |
| Telemetry | 90 days | Server (if opted in) |
| Crash reports | 90 days | Server (if opted in) |

## Error Handling

### Secure Error Messages

```
// User-facing errors
"Unable to load resource. Please try again."
"The file could not be validated."
"Connection failed. Check your network."

// Internal logging (not shown to user)
log_error("NAM parse failed at offset 0x1234: invalid header")
log_error("TLS handshake failed: certificate expired")
```

### Failure Modes

| Failure | Response |
|---------|----------|
| Invalid file | Reject, show generic error |
| Network error | Retry, then fail gracefully |
| Auth failure | Clear tokens, prompt re-auth |
| Integrity failure | Reject resource, log event |

## Secure Coding Practices

### Memory Safety

- No buffer overflows in native code
- Bounds checking on all array access
- Safe string handling functions
- RAII for resource management

### Cryptographic Practices

- Use platform cryptographic libraries
- No custom crypto implementations
- Strong random number generation
- Constant-time comparisons for secrets

### Dependency Security

- Regular dependency updates
- Vulnerability scanning in CI
- Minimal dependency footprint
- Pinned dependency versions

## Incident Response

### Security Issue Handling

1. Report received (security@neuronguitar.io)
2. Triage and severity assessment
3. Fix development (private)
4. Coordinated disclosure
5. Patch release
6. Public advisory

### Severity Levels

| Level | Response Time | Examples |
|-------|---------------|----------|
| Critical | 24 hours | RCE, credential theft |
| High | 7 days | Data exposure, privilege escalation |
| Medium | 30 days | Information disclosure |
| Low | Next release | Minor issues |

## Related Documents

- [Network Client Specification](./network-client.md)
- [API Specification](./api-spec.md)
- [Privacy Policy](../../PRIVACY.md)
