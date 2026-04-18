# AI Module Generation API Architecture Proposal

## Goal

Add an API-driven workflow that lets a user:

1. start from a user-facing `Custom Effect` UI entry point inside the app
2. describe the sound they want in non-technical terms
3. iterate with the system on design and DSP questions
4. generate one or more validated WASM module revisions
5. import the chosen revision into their local resource library and apply it to the current `Custom Effect` node
6. browse saved generated modules in the effect chooser and drop them onto the signal chain
7. optionally download a generated module bundle and publish it to the cloud library

This proposal is designed to fit the current stack in this repo:

- desktop/WebView client in `core/ui/ts/`
- Cloudflare Worker API in `api/`
- D1 for metadata
- R2-style asset storage via the existing `assets` model
- existing item/pack publishing and download patterns
- existing effect chooser and local resource-library flows

## Assumptions

- Generated modules should target the current AudioFX WASM ABI documented in [docs/wasm-module-authoring.md](../wasm-module-authoring.md).
- Users will usually be non-technical and should describe sound, feel, references, and control simplicity rather than implementation language or runtime details.
- The public product should not expose source language, model selection, provider choice, or other implementation-tech decisions.
- The API contract must represent open-ended DSP intent, not a fixed archetype enum. Any template selection is an internal execution detail and must not define the design-session model.
- Module generation is slow and multi-step enough that it should not run inside a normal synchronous Worker request.
- The desktop app, not the API, should remain responsible for writing files into the user’s local library.
- The existing `wasm_host` processor should remain the single underlying runtime implementation. User-facing `Custom Effect` entries should be library-backed variants of that same host, not new DSP processor types.
- Published generated modules should use the same discovery/download patterns as other cloud items where possible.
- A generated module is a first-class artifact and should not be hidden inside a generic preset item forever.

## Product Principles

- Sound-first UX: users ask for tone, movement, space, feel, and control simplicity.
- Tech-hidden UX: the system chooses WAT, source language, model, and build path internally.
- No fixed-archetype contract: the user and API talk about arbitrary DSP behavior, not a closed list of effect templates.
- One runtime, many modules: every generated effect still runs through the standard `wasm_host` runtime.
- Current-node-first workflow: the primary path starts from an existing `Custom Effect` node and ends by replacing its selected module.
- Explicit generation: chat and design iteration do not automatically create unbounded background jobs.

## Recommendation

Use the existing Cloudflare Worker as the control plane, then add an asynchronous generation pipeline behind it.

The important boundary is this: the public API should capture an open-ended DSP brief and a normalized executable spec. It should not require the caller to choose, receive, or even know about a closed set of generation archetypes. A template runner can exist as a temporary backend, but only as one internal execution path among others.

Recommended split:

- Worker + Hono: request auth, CRUD routes, SSE/event fanout, D1 writes, R2 asset references
- Durable Object: live session coordinator for chat state, transient stream delivery, and optimistic session state
- Queue or Workflow: long-running generation and validation orchestration
- Generation Runner service: sandboxed builder/validator that can call LLMs, compile source when needed, inspect Wasm, and produce artifacts
- D1: authoritative metadata for sessions, messages, jobs, revisions, and publish records
- R2: source files, generated `.wasm`, manifest JSON, validation reports, thumbnails, preview audio, downloadable bundles
- Desktop app local importer: imports a chosen revision into the local resource library, updates the current `Custom Effect` node, and surfaces saved modules in the effect chooser

## Why This Shape Fits The Repo

The current API already has most of the right primitives:

- authentication and sessions
- item and pack metadata with asset references in `config_json`
- upload initialization and binary asset handling
- download endpoints with content-disposition support

The current product architecture also already has two useful integration points:

- the standard `wasm_host` effect can load self-described modules and surface guest metadata
- the FX selector already merges more than one source of chooser items, so saved custom effects can be added as library-backed chooser entries instead of pretending they are new built-in processors

What is missing is not a general backend foundation. The missing pieces are:

- conversational design-session state
- async generation jobs
- revisioned module artifacts
- validation and packaging
- a clean path from generated draft to saved library item

What must not be missing is an abstraction boundary between:

- the user-facing design/session model
- the internal code-generation backend currently chosen to satisfy that design

That boundary is what allows the system to grow from a narrow prototype backend into arbitrary DSP generation without breaking the API contract.

## High-Level Architecture

```text
Desktop WebView UI
  - Custom Effect inspector
  - FX selector / My Custom Effects browser
        |
        v
Cloudflare Worker API (Hono)
  - auth
  - module session routes
  - revision routes
  - publish routes
  - upload routes
        |
        +--> Durable Object: live session + SSE
        |
        +--> D1: metadata and state
        |
        +--> R2: source, wasm, manifests, bundles
        |
        +--> Queue / Workflow
                  |
                  v
          Generation Runner
          - LLM orchestration
          - prompt/spec refinement
            - source generation
            - internal implementation-target selection
            - compilation or WAT emission
          - ABI validation
          - descriptor validation
          - smoke execution in sandbox
```

## User Experience Model

### User-Facing Name

The product should present this feature as `Custom Effect`, not `WASM Host`.

Recommended UX split:

- `Custom Effect`: the single user-facing effect type that wraps the standard `wasm_host` processor
- `My Custom Effects`: saved generated modules that appear as chooser entries and instantiate that same runtime with a specific module preselected

### Entry Points

There should be two main entry points:

1. Add `Custom Effect` from the effect chooser.
2. Open an existing `Custom Effect` node and choose an action like `Design with AI` or `Replace Module`.

The first entry point creates an empty or starter `Custom Effect` node.

The second entry point passes the current node context into the design session so the system can revise the currently selected module instead of starting from scratch.

### What The User Describes

The user should describe things like:

- sonic goal
- references or inspiration
- amount of movement or intensity
- simple versus advanced controls
- whether they want subtle utility behavior or a more characterful effect

The user should not be asked to choose:

- WAT versus Rust versus C
- model provider
- prompt template variant
- compile strategy
- validation mode

## Core Product Flow

### 0. Entry From The Current Custom Effect UI

The primary workflow should begin from the current `Custom Effect` node UI, not from a separate developer-oriented tool.

Typical flow:

1. user inserts `Custom Effect` from the chooser or selects an existing one
2. user opens `Design with AI`
3. the UI sends the current node context to the API
4. the API creates or resumes a design session

Relevant node context can include:

- active preset id
- node id
- currently selected module library id or asset id
- current descriptor metadata snapshot
- current parameter values
- any attached reference assets

See also: [Custom Effect Local Library Model](2026-04-17-custom-effect-local-library-model.md) for the concrete local storage and chooser-entry shape that this workflow should hydrate.

### 1. Design Session

The user starts with a natural-language request such as:

- "I want something watery and wide, somewhere between chorus and phaser"
- "Make it feel like the guitar is gently moving around the room"
- "Give me only two controls and keep it subtle"

The API creates a `module_generation_session` and stores:

- the raw user prompt
- the initiating node context
- the current structured design brief
- unresolved design questions
- an internal implementation strategy chosen by the system
- current lifecycle state

The structured design brief should be able to express arbitrary stream-processing intent, such as:

- signal-flow goals such as serial, parallel, feedback, multiband, mid-side, or hybrid behavior
- spectral or time-domain behavior such as filters, delay, reverb, pitch, dynamics, modulation, nonlinear drive, amp, or cab behavior
- control-surface intent such as macro controls, advanced controls, hidden expert controls, or resource slots
- execution constraints such as low latency, low CPU, deterministic behavior, resource-free operation, or optional external assets

This is intentionally broader than a fixed effect taxonomy. The system may still choose a simpler backend temporarily, but the session model should remain capable of describing any effect that can be expressed against the WASM ABI.

The system should not generate binary output immediately if the design is underspecified. It should first respond with:

- a concise interpretation of the request
- explicit unresolved questions
- a proposed structured module spec
- a user-friendly control proposal such as likely parameter names and ranges

### 2. Iteration

The user answers questions or gives revision feedback.

Examples:

- "Make it less swirly and more airy"
- "I want it to react more slowly"
- "This should feel like a studio widener, not a dramatic effect"
- "Keep the controls simple"

Each turn updates both:

- the append-only message history
- the latest normalized spec JSON

The important design decision here is that the system should maintain a structured spec beside the chat log, not derive everything from raw chat every time.

The internal structured spec may include technical decisions, but those should stay system-managed unless the UI is in an advanced diagnostic mode.

### 3. Generation Job

Once the design is sufficiently specified, the client calls `generate`.

The Worker enqueues a background job. The runner then:

1. builds the final generation prompt using the current spec plus [docs/wasm-module-authoring.md](../wasm-module-authoring.md)
2. chooses the internal model/provider and implementation path using policy rules rather than user input
3. produces source code and descriptor text
4. compiles if needed, or emits WAT directly if that is the reliable target
5. validates the generated module
6. packages the artifacts into a revision bundle
7. writes metadata and assets back to D1/R2

Possible implementation paths can include:

- general source generation against the full WASM ABI
- composition from a reusable DSP codegen library
- retrieval and adaptation of known-good source skeletons
- temporary template fallback for narrow prototype coverage

The key requirement is that these are runner internals. They must not leak upward and redefine the design-session schema as a closed archetype picker.

Generation should only happen when the user explicitly asks for it. Normal chat turns should not automatically enqueue jobs.

### 4. Review And Download

The UI receives streamed job status updates and then shows a revision card with:

- summary of what was generated
- descriptor metadata
- validation results
- audition or preview controls when available
- `Use This Effect` button
- `Save To My Custom Effects` button
- optional `Download Bundle` advanced action
- publish to library button

Source language and implementation strategy can exist in the metadata, but they should be hidden from the main non-technical UI by default.

### 5. Store Locally Or Publish

There should be three distinct user actions:

- `Use This Effect`: the desktop app imports the chosen revision into the local resource library and immediately applies it to the current `Custom Effect` node as the selected module
- `Save To My Custom Effects`: the desktop app imports the chosen revision into the local resource library without changing the current node
- `Download Bundle`: optional advanced action for power users, backup, or manual sharing
- `Publish`: the API creates a cloud library item for reuse across devices or sharing

The primary success path should be `Use This Effect`, not `Download Bundle`.

## Proposed Domain Model

### New Entities

#### `module_generation_sessions`

One row per user-facing design session.

Suggested fields:

- `id`
- `user_id`
- `title`
- `entry_point` with values like `current_custom_effect` or `effect_chooser`
- `target_preset_id`
- `target_node_id`
- `replaces_existing_module_asset_id`
- `status` with values like `designing`, `waiting_on_user`, `ready_to_generate`, `generating`, `ready`, `failed`, `archived`
- `current_spec_json`
- `intent_summary_json`
- `implementation_strategy_json`
- `latest_revision_id`
- `created_at`
- `updated_at`

#### `module_generation_messages`

Append-only chat transcript.

Suggested fields:

- `id`
- `session_id`
- `role` with values like `user`, `assistant`, `system`
- `message_type` with values like `chat`, `question`, `spec_update`, `job_status`
- `content_json`
- `created_at`

#### `module_generation_jobs`

Tracks asynchronous generation and validation.

Suggested fields:

- `id`
- `session_id`
- `base_revision_id`
- `job_type` with values like `generate`, `revise`, `validate`, `package`
- `status` with values like `queued`, `running`, `succeeded`, `failed`, `cancelled`
- `dedupe_key`
- `cost_units`
- `provider`
- `model`
- `error_code`
- `error_message`
- `started_at`
- `completed_at`

#### `module_generation_revisions`

One row per generated draft.

Suggested fields:

- `id`
- `session_id`
- `revision_number`
- `status` with values like `draft`, `validating`, `ready`, `failed`, `published`
- `source_language`
- `module_manifest_json`
- `validation_summary_json`
- `wasm_asset_id`
- `source_asset_id`
- `descriptor_asset_id`
- `bundle_asset_id`
- `thumbnail_asset_id`
- `preview_asset_id`
- `published_item_id`
- `descriptor_summary_json`
- `created_at`

### Extensions To Existing Tables

#### `assets.kind`

Add new asset kinds:

- `module_wasm`
- `module_source`
- `module_descriptor`
- `module_bundle`
- `module_validation_report`
- `module_preview_audio`
- `module_reference`

#### `items.type`

Add a new first-class type:

- `wasm_module`

This is cleaner than overloading `preset`, `layout`, or `composite` for generated effect artifacts.

#### `items.config_json`

For published modules, store module-specific metadata such as:

- `wasmAssetId`
- `bundleAssetId`
- `sourceAssetId`
- `descriptorAssetId`
- `validationReportAssetId`
- `thumbnailAssetId`
- `previewAssetId`
- `moduleCategory`
- `abiVersion`
- `hostEffectType` with value `wasm_host`
- `generationSessionId`
- `generationRevisionId`

`sourceLanguage` can still be stored for diagnostics, but it should be treated as internal or advanced metadata rather than a normal user-facing property.

## API Surface Proposal

### Session Routes

#### `POST /v1/module-sessions`

Create a new design session.

Request:

```json
{
  "title": "Airy moving widen effect",
  "entryPoint": "current_custom_effect",
  "soundPrompt": "I want something wide, airy, and gently moving, with only two simple controls.",
  "targetNode": {
    "presetId": "preset_123",
    "nodeId": "node_custom_fx_1"
  },
  "referenceAssetIds": [],
  "referencePresetId": null
}
```

Response:

```json
{
  "ok": true,
  "data": {
    "session": {
      "id": "mgs_123",
      "status": "designing"
    }
  }
}
```

#### `GET /v1/module-sessions/:sessionId`

Return session metadata, current structured spec, and revision list.

#### `GET /v1/module-sessions/:sessionId/messages`

Return paged session transcript.

#### `POST /v1/module-sessions/:sessionId/messages`

Append a user message and trigger assistant design response.

Request:

```json
{
  "content": "Make it slower and more subtle. I want it to feel spacious, not seasick.",
  "attachments": []
}
```

Response should include:

- assistant reply text
- unresolved questions
- updated normalized spec
- lifecycle status

#### `GET /v1/module-sessions/:sessionId/events`

Server-sent events stream for:

- assistant token streaming
- question creation
- spec updates
- job queue/running/completed states
- revision readiness

Use SSE for simplicity. WebSockets are not required for the first version.

### Public API Rule

The public session and generation routes should not accept fields such as:

- `sourceLanguage`
- `model`
- `provider`
- `promptTemplate`
- `compileBackend`

Those are internal policy decisions.

### Generation Routes

#### `POST /v1/module-sessions/:sessionId/generate`

Enqueue a new revision build.

Request:

```json
{
  "mode": "fresh",
  "baseRevisionId": null,
  "force": false
}
```

This route should only succeed if:

- the session is in a generatable state
- the user is within quota
- there is no conflicting active job for the same session
- the latest spec snapshot has not already produced an equivalent queued or ready revision

#### `POST /v1/module-sessions/:sessionId/revise`

Convenience route that appends feedback and immediately enqueues regeneration.

Request:

```json
{
  "baseRevisionId": "mgr_001",
  "feedback": "Keep the same idea but make it softer and easier to control."
}
```

### Revision Routes

#### `GET /v1/module-revisions/:revisionId`

Return revision metadata, validation summary, and asset ids.

The normal user-facing response should prioritize:

- summary
- parameter/control summary
- validation status
- preview availability
- actions like `use`, `save`, `download`, and `publish`

Implementation details such as source language should be optional advanced metadata.

#### `GET /v1/module-revisions/:revisionId/download`

Download the packaged artifact bundle.

Recommended ZIP contents:

- `module.wasm`
- `manifest.json`
- `descriptor.txt`
- `source/module.wat` or equivalent source file
- `validation/report.json`
- `conversation/summary.md`
- `thumbnail.png` if present

This is an advanced/debug action, not the primary user success path.

#### `POST /v1/module-revisions/:revisionId/publish`

Create or update a cloud `item` of type `wasm_module`.

Request:

```json
{
  "title": "Stereo Random Tremolo Pan",
  "description": "AI-generated modulation effect with smooth random panning.",
  "visibility": "private",
  "tags": ["modulation", "stereo", "ai-generated"]
}
```

Response returns both the new `itemId` and the normal item metadata shape.

### Upload Routes

Reuse the existing upload flow, but extend `kind` values for module-building references.

Suggested new upload kinds:

- `module_reference`
- `module_thumbnail`
- `module_preview_audio`
- `module_test_input`

These let a user attach:

- a sketch or screenshot
- a previous module bundle
- a reference preset
- test audio for preview rendering

For non-technical UX, the client can present these as `Reference Audio`, `Reference Preset`, `Screenshot`, or `Previous Custom Effect` while mapping them to the internal upload kinds.

## Session State Machine

Recommended session states:

- `designing`
- `waiting_on_user`
- `ready_to_generate`
- `generating`
- `validating`
- `ready`
- `failed`
- `archived`

Recommended revision states:

- `draft`
- `building`
- `validating`
- `ready`
- `failed`
- `published`

## Generation Runner Responsibilities

The generation runner should be treated as an isolated untrusted-code pipeline, not just a text completion client.

Responsibilities:

1. Build a final prompt from the structured spec and repo authoring rules.
2. Generate source and descriptor output.
3. Compile source when needed.
4. Validate ABI and descriptor correctness.
5. Run bounded smoke execution.
6. Produce normalized manifest JSON.
7. Produce a package bundle and validation report.

Validation should include at least:

- exact required export names
- only expected host imports
- correct multivalue `audiofx_process` signature
- descriptor ptr/len consistency
- descriptor key parsing
- optional thumbnail decode check
- bounded `prepare/reset/process` smoke run with finite outputs
- rejection of unexpected imports or clearly unsafe module structure

## Storage Strategy

### Remote Metadata

Use D1 for:

- session lifecycle state
- message history metadata
- revision metadata
- job history
- publish linkage to `items`

### Remote Binary Assets

Use R2 for:

- generated `.wasm`
- original source code
- descriptor text
- validation reports
- zipped downloadable bundles
- optional previews and thumbnails

### Local Storage

The desktop app should write downloaded bundles into a dedicated local folder, for example:

```text
~/.guitarfx/library/wasm-modules/<module-id>/<revision>/
```

The app should then:

1. update its local library index
2. create or update a local custom-effect library entry with display name, category, thumbnail, descriptor summary, and module path
3. optionally import the module into the current preset and bind it to the active `Custom Effect` node
4. surface the saved effect in the effect chooser as a browseable custom-effect entry

The API should never pretend it has stored a file locally on behalf of the user. It should only provide the bundle plus metadata needed for the client to store it.

### Local Import And Apply

When the user chooses `Use This Effect`, the desktop app should:

1. download or hydrate the chosen revision bundle
2. import the module into the local resource library
3. create a stable local library id for that generated effect
4. update the active `Custom Effect` node to reference that imported module
5. refresh the node descriptor metadata so the node label, parameters, category, and thumbnail match the generated module

This keeps the resulting preset on the normal `wasm_host` path rather than introducing a special one-off runtime.

## UI Integration Proposal

The primary workflow should be embedded in the `Custom Effect` UI rather than being introduced as a detached developer-facing lab.

Recommended `Custom Effect` UI surface:

- `Chat`: conversation and design questions
- `Spec`: normalized structured design brief and parameter/resource table
- `Revisions`: generated outputs, validation status, download, save, and publish actions

The UI should keep the design conversation and structured spec visible side by side. This avoids the common problem where the user cannot tell whether the AI actually updated the intended parameter ranges or resource behavior.

Recommended UI actions:

- `Ask AI`
- `Answer Questions`
- `Generate Revision`
- `Revise This Revision`
- `Use This Effect`
- `Save To My Custom Effects`
- `Download Bundle`
- `Publish To Cloud Library`

### Effect Chooser Integration

Saved custom effects should appear in the effect chooser as library-backed entries, not as separately registered DSP processor types.

Recommended model:

- one standard effect type: `Custom Effect` backed by `wasm_host`
- many saved custom-effect entries sourced from the local resource library
- chooser cards use descriptor-driven title, category, description, and thumbnail
- dropping a saved custom effect onto the chain instantiates a standard `wasm_host` node with the corresponding module already selected

This is important because it preserves a single runtime implementation while still giving users the mental model of browsing distinct effects.

## Reuse Of Existing Item And Download Flows

Published modules should reuse the current item model instead of inventing an entirely separate public-library system.

Proposed mapping:

- draft session and revisions live in new generation tables
- once published, the winning revision is represented as `items.type = 'wasm_module'`
- downloads for published artifacts can still flow through `GET /v1/items/:itemId/download`
- the item response can carry `bundleAssetId`, `wasmAssetId`, `thumbnailAssetId`, and `manifestAssetId`

For local-only usage, the saved module should be treated as a local resource-library entry plus chooser item, not a remote `item`.

This keeps discovery, ownership, moderation, favorites, ratings, and download metrics aligned with the rest of the existing cloud platform.

## Security And Abuse Controls

### Trust Boundaries

- Do not run long generation or compilation work in the Worker request path.
- Do not execute generated Wasm in the same trust boundary as the API control plane.
- Treat generated source and binary output as untrusted until validated.

### Controls

- no user-facing control over provider, model, source language, or build target
- explicit `Generate Revision` action required before any background job is created
- one active generation job per session
- strict per-user concurrent job cap
- strict daily and rolling-window revision caps
- deduplication of equivalent generate requests for the same spec snapshot
- automatic cancellation or superseding of stale queued jobs when the session spec changes
- message rate limits and attachment count limits
- input moderation for prompts and uploaded references
- asset size limits for references and bundles
- provider/model allowlist
- per-stage timeouts and memory/CPU budgets in the generation runner
- automatic cleanup of failed or abandoned sessions and intermediate artifacts
- validation before a revision can be marked `ready`
- optional admin moderation before a `wasm_module` item becomes public

### Job Accounting Rules

To prevent infinite-job abuse, the scheduler should enforce rules like:

- one queued or running `generate` job per session
- at most `N` active jobs per user across all sessions
- at most `M` revisions per user per day
- if the same spec hash is already queued or already has a ready revision, return the existing job or revision instead of creating a new one
- if a session receives new design input, older queued jobs for stale spec hashes should be cancelled or marked superseded

The exact values of `N` and `M` are product-policy choices, but the architecture should assume these checks exist from day one.

### Nice-To-Have

- artifact signing for published module bundles
- provenance metadata recording prompt, model, and build toolchain versions
- reproducibility hash for the revision bundle

## Recommended Rollout

### Phase 1: Design Session MVP

Deliver:

- `module-sessions` routes
- chat transcript storage
- structured spec extraction
- unresolved-question workflow
- launch from current `Custom Effect` node UI
- no binary generation yet

This validates the product interaction before adding the expensive backend path.

### Phase 2: Async Generation And Validation

Deliver:

- queue-backed generation jobs
- generation runner
- revision artifacts in R2
- validation reports
- downloadable bundles
- `Use This Effect` local import/apply flow for the current node

### Phase 3: Local Save And Publish

Deliver:

- desktop `Save To My Custom Effects`
- local chooser integration for saved custom effects
- `items.type = wasm_module`
- publish route
- private/unlisted/public visibility
- search and discovery integration

### Phase 4: Collaboration And Reuse

Deliver:

- fork existing module revision
- shareable session links
- team/library workflows
- generated preview audio and thumbnails

## Suggested First Implementation Slice

If the goal is to move quickly without overbuilding, start with this narrow vertical slice:

1. `POST /v1/module-sessions`
2. `POST /v1/module-sessions/:id/messages`
3. `GET /v1/module-sessions/:id/events`
4. `POST /v1/module-sessions/:id/generate`
5. desktop-side `Use This Effect` local import/apply
6. local `My Custom Effects` chooser list

That slice is enough to prove the full user story:

- prompt
- iterate
- generate
- apply to current node
- save locally
- browse again later from the chooser

without having to solve cloud publishing and public discovery on day one.

## Summary

The cleanest architecture is not a single synchronous "generate module" endpoint.

It is:

- a session-based conversational control plane in the existing Worker
- a structured spec that lives beside the chat
- an asynchronous generation and validation pipeline
- revisioned artifacts stored as normal assets
- a sound-first `Custom Effect` UX with no user-facing tech choices
- hard scheduling and quota rules that prevent unbounded job creation
- a local-library import flow that reapplies the chosen module to the current node
- effect chooser entries that browse saved generated modules while still instantiating the standard `wasm_host`

That approach fits the repo’s existing API patterns, reuses the current item/download model, and gives you room to support both private local generation workflows and later public module sharing.