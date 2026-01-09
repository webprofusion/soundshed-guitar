# WebUI Bridge Specification

## Overview

The WebUI Bridge provides bidirectional communication between the native plugin code and the web-based user interface running in a WebView. It serializes messages to JSON and handles event dispatching between the two environments.

## Design Goals

1. **Decoupling**: UI and engine communicate only through messages
2. **Type Safety**: Strongly-typed messages with validation
3. **Performance**: Efficient serialization, batched updates
4. **Reliability**: No lost messages, ordering guaranteed
5. **Debuggability**: Message logging for troubleshooting

## Architecture

```
┌──────────────────────────────────────────────────────┐
│                    WebView (UI)                       │
│  ┌────────────────────────────────────────────────┐  │
│  │              JavaScript Runtime                 │  │
│  │  ┌────────────────────────────────────────┐    │  │
│  │  │     window.IPlugReceiveData(json)      │◀───┼──┼── From Engine
│  │  └────────────────────────────────────────┘    │  │
│  │  ┌────────────────────────────────────────┐    │  │
│  │  │     window.NAMBridge.postMessage(json) │────┼──┼──▶ To Engine
│  │  └────────────────────────────────────────┘    │  │
│  └────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────┘
                         │
                         │ JSON Messages
                         ▼
┌──────────────────────────────────────────────────────┐
│                  WebUI Bridge                         │
│  ┌────────────────────────────────────────────────┐  │
│  │              Message Dispatcher                 │  │
│  │  ┌──────────────┐    ┌──────────────────────┐  │  │
│  │  │  Serializer  │    │  Message Handlers    │  │  │
│  │  └──────────────┘    └──────────────────────┘  │  │
│  └────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────┘
                         │
                         ▼
┌──────────────────────────────────────────────────────┐
│               Plugin Controller                       │
└──────────────────────────────────────────────────────┘
```

## Message Format

### Base Structure

All messages use a common envelope:

```json
{
  "type": "messageType",
  "payload": { ... },
  "timestamp": 1704801234567
}
```

### Message Types

#### Engine → UI Messages

| Type | Description |
|------|-------------|
| `state` | Full state synchronization |
| `parameterChanged` | Single parameter update |
| `presetLoaded` | Preset load notification |
| `presetSearchResults` | Remote search results |
| `downloadProgress` | Resource download progress |
| `resourceLoaded` | Resource load complete |
| `error` | Error notification |

#### UI → Engine Messages

| Type | Description |
|------|-------------|
| `setParameter` | Update parameter value |
| `loadPreset` | Load preset by ID |
| `savePreset` | Save current state as preset |
| `deletePreset` | Delete preset |
| `search` | Search remote presets |
| `downloadPreset` | Download remote preset |
| `loadResource` | Load NAM/IR resource |
| `requestState` | Request full state sync |

## Message Definitions

### State Message (Engine → UI)

Complete state snapshot sent on startup and major changes.

```json
{
  "type": "state",
  "payload": {
    "parameters": {
      "input_trim": 0.0,
      "output_trim": -3.0,
      "amp1_drive": 0.65,
      "amp1_tone": 0.5
    },
    "currentPreset": {
      "id": "preset-123",
      "name": "My Crunch Tone",
      "modified": true
    },
    "presets": [
      {"id": "preset-1", "name": "Clean", "category": "Clean"},
      {"id": "preset-2", "name": "Crunch", "category": "Crunch"}
    ],
    "library": {
      "nam": [
        {"id": "plexi-bright", "name": "Plexi Bright", "category": "Marshall"}
      ],
      "ir": [
        {"id": "4x12-sm57", "name": "4x12 SM57", "category": "Marshall"}
      ]
    },
    "signalGraph": {
      "nodes": [...],
      "edges": [...]
    }
  }
}
```

### Parameter Changed (Engine → UI)

Single parameter update for efficient synchronization.

```json
{
  "type": "parameterChanged",
  "payload": {
    "id": "amp1_drive",
    "value": 0.72
  }
}
```

### Set Parameter (UI → Engine)

User adjustment of a parameter.

```json
{
  "type": "setParameter",
  "payload": {
    "id": "amp1_drive",
    "value": 0.72
  }
}
```

### Load Preset (UI → Engine)

Request to load a preset.

```json
{
  "type": "loadPreset",
  "payload": {
    "id": "preset-123"
  }
}
```

### Preset Loaded (Engine → UI)

Notification that a preset was loaded.

```json
{
  "type": "presetLoaded",
  "payload": {
    "id": "preset-123",
    "name": "My Crunch Tone",
    "success": true
  }
}
```

### Search (UI → Engine)

Initiate remote preset search.

```json
{
  "type": "search",
  "payload": {
    "query": "crunch marshall",
    "category": "Crunch",
    "tags": ["rock", "vintage"],
    "page": 1
  }
}
```

### Search Results (Engine → UI)

Remote search results.

```json
{
  "type": "presetSearchResults",
  "payload": {
    "query": "crunch marshall",
    "total": 156,
    "page": 1,
    "results": [
      {
        "id": "remote-abc",
        "name": "Vintage Crunch",
        "author": "ToneHunter",
        "category": "Crunch",
        "downloads": 1234
      }
    ]
  }
}
```

### Error (Engine → UI)

Error notification.

```json
{
  "type": "error",
  "payload": {
    "code": "RESOURCE_NOT_FOUND",
    "message": "Failed to load NAM model",
    "details": {
      "resourceId": "plexi-bright"
    }
  }
}
```

## Bridge Implementation

### Native Side (C++)

```cpp
class WebUIBridge {
public:
    // Send message to UI
    void SendToUI(const Message& msg);
    
    // Receive message from UI
    void OnMessageFromUI(const std::string& json);
    
    // Register handler for message type
    void RegisterHandler(const std::string& type, MessageHandler handler);
    
private:
    // Serialize message to JSON
    std::string Serialize(const Message& msg);
    
    // Parse JSON to message
    std::optional<Message> Parse(const std::string& json);
    
    // WebView reference
    IWebView* m_webView;
    
    // Handler registry
    std::map<std::string, MessageHandler> m_handlers;
};
```

### JavaScript Side

```javascript
// Global bridge object
window.NAMBridge = {
    // Send message to engine
    postMessage: function(type, payload) {
        const message = {
            type: type,
            payload: payload,
            timestamp: Date.now()
        };
        // iPlug2 WebView bridge function
        window.IPlugSendMsg(JSON.stringify(message));
    }
};

// Receive messages from engine
window.IPlugReceiveData = function(jsonString) {
    try {
        const message = JSON.parse(jsonString);
        handleMessage(message);
    } catch (e) {
        console.error("Failed to parse message:", e);
    }
};

// Message dispatcher
function handleMessage(message) {
    const handler = messageHandlers[message.type];
    if (handler) {
        handler(message.payload);
    } else {
        console.warn("Unknown message type:", message.type);
    }
}
```

## Synchronization

### Initial State Load

```
Startup sequence:
1. WebView loads UI application
2. UI sends "requestState" message
3. Engine sends "state" message with full snapshot
4. UI renders initial state
```

### Parameter Updates

```
UI changes parameter:
1. User adjusts control
2. UI updates local state immediately
3. UI sends "setParameter" message
4. Engine updates parameter
5. Engine broadcasts "parameterChanged"
6. All UIs receive update (for multi-window)

Engine changes parameter (automation):
1. DAW automation writes value
2. Engine updates parameter
3. Engine sends "parameterChanged"
4. UI updates display
```

### Debouncing

Continuous parameter changes are debounced:

```javascript
const parameterDebounce = {};

function onParameterChange(id, value) {
    // Update local state immediately
    updateLocalParameter(id, value);
    
    // Debounce message sending
    if (parameterDebounce[id]) {
        clearTimeout(parameterDebounce[id]);
    }
    
    parameterDebounce[id] = setTimeout(() => {
        NAMBridge.postMessage("setParameter", { id, value });
        delete parameterDebounce[id];
    }, 50);  // 50ms debounce
}
```

### Conflict Resolution

When both UI and engine change the same parameter:

```
1. Engine value is authoritative
2. UI receives "parameterChanged" after sending "setParameter"
3. UI adopts engine value if different
4. Brief visual feedback indicates sync
```

## Error Handling

### Message Parsing Errors

```cpp
void WebUIBridge::OnMessageFromUI(const std::string& json) {
    auto msg = Parse(json);
    if (!msg) {
        LogWarning("Failed to parse UI message");
        return;  // Silently ignore malformed messages
    }
    
    auto handler = m_handlers.find(msg->type);
    if (handler == m_handlers.end()) {
        LogWarning("Unknown message type: " + msg->type);
        return;
    }
    
    try {
        handler->second(msg->payload);
    } catch (const std::exception& e) {
        LogError("Handler exception: " + std::string(e.what()));
        SendError("HANDLER_ERROR", "Failed to process message");
    }
}
```

### Connection Loss

```javascript
// Monitor WebView connection
let heartbeatInterval = setInterval(() => {
    NAMBridge.postMessage("heartbeat", {});
}, 5000);

// Handle reconnection
function onReconnect() {
    NAMBridge.postMessage("requestState", {});
}
```

## Performance

### Message Batching

Multiple rapid changes batched together:

```cpp
class MessageBatcher {
public:
    void QueueMessage(const Message& msg) {
        m_queue.push_back(msg);
        ScheduleFlush();
    }
    
private:
    void Flush() {
        if (m_queue.empty()) return;
        
        // Combine parameter changes
        auto combined = CombineParameterChanges(m_queue);
        
        for (const auto& msg : combined) {
            m_bridge->SendToUI(msg);
        }
        
        m_queue.clear();
    }
    
    std::vector<Message> m_queue;
};
```

### Binary Data

Large binary data (waveforms, spectra) sent as Base64:

```json
{
  "type": "waveformData",
  "payload": {
    "channel": 0,
    "samples": "base64encodeddata...",
    "sampleRate": 44100
  }
}
```

## Logging

### Debug Mode

```cpp
#ifdef DEBUG
void WebUIBridge::LogMessage(const std::string& direction, 
                              const std::string& json) {
    std::cout << "[WebUIBridge " << direction << "] " 
              << json.substr(0, 200) << std::endl;
}
#endif
```

### Message History

```javascript
const messageHistory = [];
const MAX_HISTORY = 100;

function logMessage(direction, message) {
    messageHistory.push({
        direction,
        message,
        timestamp: Date.now()
    });
    
    if (messageHistory.length > MAX_HISTORY) {
        messageHistory.shift();
    }
}
```

## Related Documents

- [User Interface Specification](./user-interface.md)
- [Architecture Overview](./overview.md)
