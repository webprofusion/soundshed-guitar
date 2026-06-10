# Network and Remote Integrations

## Key Files
- `core/ui/ts/tone3000Api.ts` — Tone3000 official/proxy API client configuration
- `core/ui/ts/tone3000Browser.ts` — Tone3000 browsing and session helpers
- `core/ui/ts/toneSharingPanel.ts` — Soundshed preset-sharing client and archive flows
- `core/ui/ts/archiveUtils.ts` — Shared preset archive import/export helpers

## Overview

This checkout does not currently contain a standalone Cloudflare Worker project under `api/`. The live network-facing code in this repository is the desktop/WebView client integration for:

- Tone3000 model and IR browsing/import
- Soundshed preset sharing and archive import/export

Those integrations use the remote endpoints exposed by the UI modules above rather than a separate local API service in this workspace.

## Current Remote Endpoints

### Tone3000
- Official REST API: `https://www.tone3000.com/api/v1`
- Optional Soundshed proxy/search path: `https://api-guitar.soundshed.com/v1/resourcesearch`

### Soundshed sharing / preset exchange
- Base API: `https://api-guitar.soundshed.com/v1`
- Share-consent and archive flows are handled by the UI client in `core/ui/ts/toneSharingPanel.ts`.

## What the Current Client Does

- Authenticates with a user API key or proxy-backed session when available
- Searches and downloads Tone3000 resources for local import into the resource library
- Publishes, downloads, and imports shared preset archives through the Soundshed API
- Uses the existing local preset/resource storage in `core/` instead of a separate backend service

## Operational Notes

- The UI client is responsible for request/response handling, auth state, and archive import/export logic.
- Tone3000 resource downloads may use either the official endpoint or the Soundshed proxy mode configured in the app settings.
- Shared preset publishing relies on the Soundshed API and the current user/session context in the UI.

## See Also

- [Architecture Overview](architecture-overview.md) — repo-wide system layers
- [Data Models](data-models.md) — preset and resource schema
- [User Interface](user-interface.md) — WebView bridge and message contract
