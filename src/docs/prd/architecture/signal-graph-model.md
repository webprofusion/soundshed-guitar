# Signal Graph Model Specification

## Overview

The Signal Graph Model defines how audio processing nodes are connected and executed in GuitarFX. It enables flexible effect ordering, parallel signal paths, and mixing—going beyond traditional linear effect chains.

## Design Goals

1. **Flexibility**: Any effect in any position, multiple instances
2. **Parallel Processing**: Support for split/mix signal paths
3. **Simplicity**: Minimal concepts, easy to understand
4. **Serializable**: JSON-friendly representation
5. **Validatable**: Detect invalid configurations

## Core Concepts

### Nodes

A node represents a single processing unit in the signal graph.

**Node Properties:**
| Property | Type | Description |
|----------|------|-------------|
| `id` | string | Unique identifier within graph |
| `type` | string | Effect type identifier |
| `category` | string | UI grouping category |
| `label` | string | Optional display name |
| `enabled` | boolean | Bypass toggle |
| `params` | map | Numeric parameter values |
| `config` | map | String configuration values |
| `resource` | ResourceRef | Optional resource reference |

### Edges

An edge connects the output of one node to the input of another.

**Edge Properties:**
| Property | Type | Description |
|----------|------|-------------|
| `from` | string | Source node ID |
| `to` | string | Destination node ID |
| `fromPort` | int | Output port index (default: 0) |
| `toPort` | int | Input port index (default: 0) |
| `gain` | float | Edge gain multiplier (default: 1.0) |

### Special Node Types

| Type | Purpose | Ports |
|------|---------|-------|
| `input` | Graph entry point | 0 in, 1 out |
| `output` | Graph exit point | 1 in, 0 out |
| `splitter` | Signal copy | 1 in, N out |
| `mixer` | Signal sum | N in, 1 out |

## Graph Structure

### Basic Linear Chain

```
input → effect1 → effect2 → effect3 → output
```

```json
{
  "nodes": [
    {"id": "in", "type": "input"},
    {"id": "fx1", "type": "gate_noise"},
    {"id": "fx2", "type": "amp_nam"},
    {"id": "fx3", "type": "ir_cab"},
    {"id": "out", "type": "output"}
  ],
  "edges": [
    {"from": "in", "to": "fx1"},
    {"from": "fx1", "to": "fx2"},
    {"from": "fx2", "to": "fx3"},
    {"from": "fx3", "to": "out"}
  ]
}
```

### Parallel Paths

```
            ┌→ cab1 →┐
input → amp → split   → mixer → output
            └→ cab2 →┘
```

```json
{
  "nodes": [
    {"id": "in", "type": "input"},
    {"id": "amp", "type": "amp_nam"},
    {"id": "split", "type": "splitter"},
    {"id": "cab1", "type": "ir_cab"},
    {"id": "cab2", "type": "ir_cab"},
    {"id": "mix", "type": "mixer"},
    {"id": "out", "type": "output"}
  ],
  "edges": [
    {"from": "in", "to": "amp"},
    {"from": "amp", "to": "split"},
    {"from": "split", "to": "cab1", "fromPort": 0},
    {"from": "split", "to": "cab2", "fromPort": 1},
    {"from": "cab1", "to": "mix", "toPort": 0, "gain": 0.5},
    {"from": "cab2", "to": "mix", "toPort": 1, "gain": 0.5},
    {"from": "mix", "to": "out"}
  ]
}
```

### Wet/Dry Mix

```
            ┌→ delay →┐
input → split          → mixer → output
            └─────────┘ (dry)
```

## Graph Validation

### Required Conditions

1. **Single Input**: Exactly one `input` node
2. **Single Output**: Exactly one `output` node
3. **Connectivity**: All nodes reachable from input
4. **No Orphans**: All nodes connect to output
5. **Acyclic**: No feedback loops (for v1)
6. **Valid References**: All edge node IDs exist

### Implementation Status

The current executor validates acyclicity via topological sort only. It does not enforce single input/output, full connectivity, or orphan detection. Edges that reference `__input__` / `__output__` will trigger implicit insertion of those nodes during `SetGraph()`.

### Validation Algorithm

```
function validate_graph(graph):
    // Check required nodes
    inputs = nodes.filter(n => n.type == "input")
    outputs = nodes.filter(n => n.type == "output")
    if inputs.length != 1: error("Must have exactly one input")
    if outputs.length != 1: error("Must have exactly one output")
    
    // Check connectivity from input
    reachable = find_reachable(inputs[0], edges)
    if reachable.size != nodes.length:
        error("Unreachable nodes found")
    
    // Check connectivity to output
    reverse_reachable = find_reachable(outputs[0], reversed_edges)
    if reverse_reachable.size != nodes.length:
        error("Orphan nodes found")
    
    // Check for cycles
    if has_cycle(nodes, edges):
        error("Feedback loops not supported")
    
    // Validate edge references
    for edge in edges:
        if not nodes.contains(edge.from):
            error("Invalid source node")
        if not nodes.contains(edge.to):
            error("Invalid destination node")
    
    return valid
```

## Graph Execution

### Execution Order

Nodes are processed in topological order:

```
function compute_execution_order(graph):
    // Kahn's algorithm for topological sort
    in_degree = compute_in_degrees(graph)
    queue = nodes.filter(n => in_degree[n] == 0)
    result = []
    
    while queue not empty:
        node = queue.dequeue()
        result.append(node)
        
        for successor in get_successors(node):
            in_degree[successor] -= 1
            if in_degree[successor] == 0:
                queue.enqueue(successor)
    
    return result
```

### Buffer Management

```
function allocate_buffers(graph, block_size):
    buffers = {}
    
    // Allocate buffer for each edge
    for edge in graph.edges:
        buffer_id = edge.from + "_" + edge.fromPort
        if buffer_id not in buffers:
            buffers[buffer_id] = allocate(block_size)
    
    return buffers
```

### Implementation Note

The executor allocates per-node stereo buffers (not per-edge). Multiple downstream nodes reading the same upstream node buffer implement split behavior implicitly. Mixers explicitly sum inputs; non-mixer nodes receiving multiple inputs use “last edge wins” semantics.

### Processing Loop

```
function process_graph(graph, input_buffer, output_buffer, samples):
    buffers = get_preallocated_buffers()
    order = get_execution_order()
    
    // Copy input to input node's output buffer
    copy(input_buffer, buffers["in_0"])
    
    for node in order:
        if node.type == "input":
            continue  // Already handled
        
        if node.type == "output":
            copy(buffers[get_input_buffer(node)], output_buffer)
            continue
        
        if not node.enabled:
            // Bypass: copy input to output
            copy(get_input(node), get_output(node))
            continue
        
        if node.type == "splitter":
            // Copy to all outputs
            input = get_input(node)
            for port in range(num_outputs):
                copy(input, buffers[node.id + "_" + port])
            continue
        
        if node.type == "mixer":
            // Sum all inputs with gains
            clear(buffers[node.id + "_0"])
            for port, edge in enumerate(get_input_edges(node)):
                add_scaled(buffers[edge.from + "_" + edge.fromPort], 
                          buffers[node.id + "_0"],
                          edge.gain)
            continue
        
        // Normal effect processing
        processor = get_processor(node)
        processor.process(get_input(node), get_output(node), samples)
```

    ### Implementation Note

    - `splitter` and `output` are handled as pass-through processors; routing is achieved by edges, not special per-port buffer allocation.
    - `fromPort`/`toPort` are present in the model but are not explicitly used in the executor’s current per-node buffer approach.

## Node Parameter Model

### Parameter Definition

Each effect type defines its available parameters:

```
struct ParameterDef:
    id: string          # Unique within effect type
    name: string        # Display name
    min: float          # Minimum value
    max: float          # Maximum value
    default: float      # Default value
    unit: string        # Display unit (dB, Hz, ms, %)
    curve: string       # Linear, logarithmic, exponential
```

### Parameter Storage

Node parameters stored as key-value pairs:

```json
{
  "id": "amp1",
  "type": "amp_nam",
  "params": {
    "drive": 0.65,
    "tone": 0.5,
    "output": 0.0
  }
}
```

### Parameter Addressing

Global parameter ID format: `{nodeId}_{paramId}`

Example: `amp1_drive`, `cab1_mix`, `delay1_time`

## Resource References

Nodes that require external resources (NAM models, IRs) use ResourceRef:

```json
{
  "id": "amp1",
  "type": "amp_nam",
  "resource": {
    "resourceType": "nam",
    "resourceId": "plexi-bright"
  }
}
```

See [Resource Model](./resource-model.md) for details.

## Graph Modification

### Add Node

```
function add_node(graph, node, after_node_id):
    // Validate node
    validate_node(node)
    
    // Find insertion point
    edge = find_edge_from(after_node_id)
    
    // Split edge
    graph.edges.remove(edge)
    graph.edges.add({from: after_node_id, to: node.id})
    graph.edges.add({from: node.id, to: edge.to})
    
    // Add node
    graph.nodes.add(node)
    
    // Revalidate graph
    validate_graph(graph)
```

### Remove Node

```
function remove_node(graph, node_id):
    // Find connecting edges
    in_edge = find_edge_to(node_id)
    out_edge = find_edge_from(node_id)
    
    // Reconnect around removed node
    graph.edges.remove(in_edge)
    graph.edges.remove(out_edge)
    graph.edges.add({from: in_edge.from, to: out_edge.to})
    
    // Remove node
    graph.nodes.remove(node_id)
```

### Reorder Nodes

```
function move_node(graph, node_id, new_after_id):
    // Remove from current position
    reconnect_around(node_id)
    
    // Insert at new position
    insert_after(node_id, new_after_id)
    
    // Revalidate
    validate_graph(graph)
```

## Future Considerations

### Feedback Loops (v2+)

Future versions may support controlled feedback:
- Delay in feedback path required
- Maximum feedback gain limit
- Stability analysis

### Multi-Channel (v2+)

Support for stereo split processing:
- L/R independent paths
- Mid/Side processing
- Surround configurations

## Related Documents

- [Audio Engine Specification](./audio-engine.md)
- [Effect Registry](./effect-registry.md)
- [Resource Model](./resource-model.md)
- [Signal Graph Executor](./signal-graph-executor.md)
- [Preset Data Model v2](../preset-model-v2-design.md)
