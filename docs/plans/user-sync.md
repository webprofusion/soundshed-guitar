# Per-user sync: presets, setlists and installed packs on every device

Goal: a user can sign in and choose to sync their **presets** (with the models, IRs and other
resources they use, and those resources' metadata), their **setlists** and their **installed
packs** between their devices: desktop standalone and plugins, iOS, Android, Nano Q and the Linux
kiosk. Sync is opt-in on each device, and the user picks which of the three sync. Nothing changes
for anyone who never turns it on, including people already signed in for Tone Sharing.

Nothing else syncs: no settings, layouts, MIDI mappings, riffs, loops or practice tabs. The design
leaves room to add categories later (see "Adding a category").

Written 2026-11-15 against 9bc9cc50. Server internals (storage, quotas, cost) are in the private
repo: `soundshed-guitar-private/docs/plans/2026-11-15-user-sync-server.md`.

## Where things stand

| Area | What exists | What sync needs |
| --- | --- | --- |
| **Presets** | `items` rows of type `preset` in the shared SQLite store (`data/v1/soundshed.db`, used by the standalone and every plugin instance), `updated_at` in ms. User preset ids are `user-<uuid>`, so they are unique across devices. Factory presets come from the factory archives and have the same ids everywhere. | `Remove` deletes the row and leaves no trace; deletes must be detected. Some presets are over 1 MB. |
| **Resources** | `resource` rows (`<type>:<id>`) with name, tags, category, `metadata` (provider, `contentHash` nam/ir-content-v1), `filePath`, and a 64-bit FNV-1a `hash`. Tone3000 ids look like `tone3000:849585` and local ones like `local:<name>:<hash>`, so ids are stable across devices. Presets refer to resources through `ResourceRef` (with `source`, 2ce2ab48). | Files are on disk under `resources/`. Most came from Tone3000 and can be fetched again; only files with no public source need upload, and uploads need SHA-256. `filePath` is device-only. |
| **Setlists** | One `document` row `setlists`: `{activeSetlistId, bankSize, cursorIndex, setlists: [{id, name, bank, slots: [{presetId}]}]}`. | Sync each setlist as its own item. `activeSetlistId` and `cursorIndex` say where this rig is in the gig and stay on the device. |
| **Installed packs** | The `toneSharing.installedPacks` setting: one entry per pack with `id`, `packId`, `title`, `presetIds`, `presetItems` (preset id → Tone Sharing item), `presetSignatures` (the preset's signature at install), `resources`, `blends`. Installs put presets under **new random ids** (`PluginControllerArchiveInstall.cpp`, `GenerateGuidV4String`) in a `tone-sharing::<packId>` folder. | Installing the same pack on two devices gives two sets of ids, so a setlist built on one device points at presets the other doesn't have. This is the main thing to fix (below). |
| **Cross-instance sync** | The `shared-sync-state` version and a 2 s poll; closed editors catch up at `uiReady` (`CatchUpWithSharedStore`). | Remote changes come in through the same path: write the store, bump the version. |
| **Account** | Email-code sign-in, a 30-day session in `toneSharing.sessionId`, sent as `x-session-id`; per-account feature gates (`user_features`). | A per-device credential the user can see and revoke, so an expired session doesn't silently stop sync. |
| **Network in the engine** | `core/src/net/Http.h` (WinHTTP, curl, Apple); `uiclient::ToneService` already calls the API from C++. | Sync runs in the engine, so the web UI and Nano Q both get it. |
| **Missing content** | Presets whose model is missing show the Choose-a-model empty state and a re-download prompt (2ce2ab48, 72cdc0f5). | A preset that arrives before its model is already handled. |

## What the user sees

**Settings > Account & Sync** (web UI and Nano Q):

1. **Sign in** with the existing email code. Signing in does not start sync.
2. **Sync this device**, off by default. Turning it on names the device ("Studio PC", suggested from
   the host name) and, when the cloud already has content, asks how to start:
   - *Merge* (default): keep everything from both sides; differing copies of the same preset or
     setlist are both kept (see Conflicts).
   - *Use the cloud copy*: this device's synced presets and setlists are replaced, after a local
     backup.
   - *Use this device*: the cloud is replaced with this device's content.
3. **What to sync**, three checkboxes, each with a count and size:
   - **Presets**, with the resources they use. A sub-option, **Upload my own model and IR files**
     (off by default), uploads files that have no public source (decision S2).
   - **Setlists**
   - **Installed packs**: other devices install the same packs from Tone Sharing.
4. **Status**: last synced, pending changes, plain-language errors ("Signed out on the server. Sign
   in again to resume sync."), and a footer icon for syncing, synced, paused or failing.
5. **Devices**: name, platform, last sync; **Remove** revokes that device.
6. **Pause sync**, **Stop syncing** (keeps local content), **Delete my cloud data** (confirmed),
   **Download my data** (a zip).

Plugin instances share the store, so the switch is per machine: turning sync on in the standalone
turns it on in every plugin on that machine.

## What syncs, exactly

### Presets

Every user preset (not factory presets, and not unmodified pack presets, which come from the pack).
Each preset carries its **dependencies**, worked out from its graph when it is sent:

- **Resources** it uses (NAM models, IRs, and any other resource type a node points at): their
  library metadata (name, type, tags, category, description, provider, source ref, `contentHash`,
  size, SHA-256), never `filePath`.
- **Blends** it uses, since a blend node without its blend doesn't sound the same. Pack installs
  already treat blends as part of a preset (`installedPacks[].blends`). Confirmed by Chris on
  2026-11-16.

Composites and Custom Effects that a preset uses are not synced in v1; the preset shows them as
missing on the other device, as it would for a preset shared without them (decision S3).

Only resources a synced preset uses are synced. The rest of the user's library (models and IRs no
synced preset points at) stays on the device in v1; full library sync may be offered later (see
"Adding a category later"). A resource or blend stays in the cloud while any synced preset uses it. Sync never deletes a local
file: if a preset is deleted on one device, its models stay in the other devices' libraries.

### Setlists

Each setlist as its own item: id, name, bank, slots. `activeSetlistId` and `cursorIndex` stay on
the device. A slot can point at a user preset, a factory preset or a pack preset; the pack id fix
below keeps the last kind working across devices. A slot whose preset isn't on this device (for
example a factory preset from a newer app version) shows as missing rather than being dropped.

### Installed packs

Each `installedPacks` entry as its own item, keyed by its `id`. When an entry arrives, the device
offers to install that pack ("2 packs installed on Studio PC. Install them here?"), and installs it
**with the preset ids from the entry**. Removing a pack on one device removes it on the others.

### Pack presets keep one id on every device

Today an install mints new ids, so the fix has two parts:

1. `installPresetArchives` takes an optional map from the archive's preset key to the id to use.
   Sync passes the ids from the synced entry, so the second device's pack presets get the same ids
   as the first, and setlists that point at them work.
2. New installs mint ids that depend only on the pack and the archive's preset (a name-based UUID
   of `packId` + preset key), so two devices that install the same pack before ever syncing still
   agree.

A pack preset the user has edited is no longer the pack's: its signature differs from
`presetSignatures`. It then syncs as an ordinary preset under the same id, and wins over the pack's
copy on the other device. Reinstalling or updating the pack doesn't overwrite it without asking, as
today.

## How it works

### One sync agent per machine

The standalone and every plugin instance share one store, so one process syncs at a time. A lease
in the `meta` table (`sync.lease.owner` = `app.instanceId` + pid, `sync.lease.expires`), taken and
renewed every 30 s in a store transaction, picks it; when the holder exits or hangs another process
takes over once the lease lapses. The agent has its own thread, never the message or audio thread,
so it also runs in a plugin whose editor is closed. After writing remote changes it bumps the
shared-sync version once per batch, and every instance reloads through the existing path.

### Change tracking without touching every save path

A new table records what the cloud last saw for each synced item:

```sql
CREATE TABLE sync_state (
  type TEXT NOT NULL, id TEXT NOT NULL, -- preset, resource, blend, setlist, pack
  synced_hash TEXT NOT NULL,            -- SHA-256 of the canonical JSON last sent or received
  server_rev INTEGER NOT NULL,
  PRIMARY KEY (type, id)) WITHOUT ROWID;
```

The agent scans presets, the `setlists` document and the `installedPacks` setting when
`MaxUpdatedAt()` moves (and on a slow timer), and compares each item's hash with `synced_hash`:
a different hash is an edit, a `sync_state` row with no local item is a delete. Several hundred
rows on a big profile, so the scan is cheap, and it's correct whatever wrote the row. Device-only
fields are left out of the canonical JSON before hashing.

Setlists and installed packs are one row each locally, so a small adapter splits them into
per-entry items on the way out and merges entries back on the way in, leaving the device-only
fields alone.

### Protocol

The server keeps an ordered log per user; each accepted change gets the next revision number.

- `POST /v1/sync/devices` with a session: registers the device and returns a revocable device
  token, used for everything after.
- `GET /v1/sync/changes?since=<rev>&categories=presets,setlists,packs`: upserts and tombstones
  after `rev`, paged.
- `POST /v1/sync/changes`: a batch of `{type, id, baseRev, hash, json | deleted}`. An item is
  accepted if `baseRev` is the server's current revision for it, else returned as a conflict with
  the server's version.
- `PUT/GET /v1/sync/blobs/<sha256>`: large preset JSON (over 64 KB) and uploaded model/IR files.

A cycle is pull and apply, push with base revisions, resolve conflicts. A preset is pushed after
its resources and blends in the same batch, so the receiving side never sees a preset whose
dependencies the server doesn't have.

Tombstones are kept 180 days. A device away longer does a full reconcile: it lists the server's
live items and hashes, and the user's start choice decides (merge by default).

### Conflicts

- **Presets and setlists** changed on both sides: the server copy keeps the id, and the local
  version is saved as a new item, "My Crunch (Laptop, 15 Nov)". Status shows "1 conflict kept as a
  copy". Nobody loses an edit.
- **Resource metadata** (a renamed model, new tags): the newer change wins.
- **Delete against edit**: the edit wins and the item comes back.
- **Installed packs**: install against remove, the install wins.
- **The active preset**: an incoming change never replaces the sound mid-play. The library updates
  and the preset bar offers "Updated on Studio PC. Load it?"; unsaved edits here become the
  conflict copy when saved.

### Getting a preset's files onto the device

For each resource a synced preset needs, the receiving agent:

1. Uses a library file with the same content hash if there is one, and merges the metadata into
   that entry.
2. Otherwise fetches it from its source (Tone3000 by id, then the content-hash lookup) through the
   existing import path and the throttled proxy rules, one at a time.
3. Otherwise downloads the uploaded blob, if the sending device uploaded it.
4. Otherwise adds the library entry as "not on this device"; the preset shows the existing missing
   state with "Find on Tone3000" and "Choose a file".

Downloads happen when the preset is first loaded or the library is opened, with a "Download all
now" button for going offline. Mobile asks before large downloads on a metered connection. Pack
installs fetch their models the way installs already do.

Uploads use SHA-256 and a per-user quota. Blobs are private to the user; the server never serves
one user's upload to another, even with the same hash.

### Security and privacy

- TLS to the API, content encrypted at rest by the platform, no end-to-end encryption in v1.
- Device tokens are scoped to `/v1/sync/*`, hashed on the server, revoked from Devices or at sign
  out. Signing out leaves local content in place.
- Nothing but presets, their resource metadata and blends, setlists and pack entries leaves the
  device. No settings, paths or credentials; a test checks every outgoing item against that.
- Content from the server is untrusted: presets, blends and setlists go through the same checks as
  an import before they are written.

## Build order

**Phase 0: local groundwork, no network.** Deterministic pack preset ids and the id map on
`installPresetArchives`. `sync_state` and its migration. Canonical JSON and hashing, the change
scanner, the dependency walk (preset → resources, blends), the setlist and pack adapters, the
"pack preset was edited" check. Tests: edits, deletes and no-op rewrites are found; device-only
fields never leave; the same pack installed twice gets the same ids.

**Phase 1: server.** Devices and tokens, change log, conflicts, tombstones, blobs, quota, export,
delete-all. Gated per account with a `user_features` row (`sync`), like the Tone Builder.

**Phase 2: engine agent.** Lease, worker thread, pull/push, apply and the shared-sync bump,
conflict copies, the active-preset notice, pack install offers. Two stores (and two processes for
the lease) against a fake transport; engine tests call `test::SetSettingsEnvRoot`.

**Phase 3: UI.** Account & Sync in the web UI and Nano Q, the start choice, the three checkboxes,
devices, status icon, errors, the pack install prompt.

**Phase 4: files.** Fetch from source on first use, Download all, then Upload my own files with the
quota.

**Phase 5: before general release.** Load test, full reconcile after a long absence, delete-all and
account deletion remove everything, privacy policy text, a support view of a user's sync state.

## Testing it live

Two throwaway profiles (`APPDATA` pointed at temp folders, see the Tones traps), signed in to a
test account on preview staging, never the real profile: sign-out and "Use the cloud copy" are
destructive there. Check: a preset edited on A appears on B with its models; a delete; a conflict
gives a copy; a pack installed on A is offered on B and a setlist slot pointing at one of its
presets works on B; an edited pack preset syncs; a plugin with a closed editor picks up a change on
opening; a local-only model shows as missing until uploaded; removing B from Devices stops it.

## Adding a category later

A category is a list of item types, an adapter if it lives in a document row, and a checkbox.
**Full resource library** sync is a planned future option: every library entry rather than only
those presets use, which reuses the same resource items, fetch-from-source path and blob uploads,
so it is mostly a different selection rule plus quota. Preset folders, favorites and ratings,
layouts and personal settings are other likely ones; settings
would need an allow list (`SyncedSettingCategory`) beside `IsAccountSettingKey`, because many keys
describe the machine (calibration, input mode, window state) and some are credentials.

## Decisions to make first

**S1. Storage.** Upload space per user (proposal: preset JSON free; uploaded files 1 GB, 100 MB per
file), and whether a paid tier raises it.

**S2. Uploading model and IR files.** Offer Upload my own files at all, or sync only references and
leave files with no public source to the user.

**S3. Other preset dependencies.** Composites and Custom Effects used by a preset: leave them
missing in v1 (proposed), or sync them as dependencies too (the `.wasm` needs the module checks on
arrival).

**S4. Preset folders, favorites and ratings.** Left out as asked. Synced presets then arrive at the
top level of the other device's library (pack presets still go into their pack folder). Confirm, or
count folders as part of a preset.

**S5. Conflict policy.** Keep both copies of a preset or setlist (proposed), or newest wins.
