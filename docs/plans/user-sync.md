# Per-user sync: your content on every device

Goal: a user can sign in and choose to sync their own content (presets, setlists, blends, layouts,
settings and so on) between their devices: desktop standalone and plugins, iOS, Android, Nano Q and
the Linux kiosk. Sync is opt-in on each device, and the user picks which kinds of content sync.
Nothing changes for anyone who never turns it on, including people already signed in for Tone
Sharing.

Written 2026-11-15 against 9bc9cc50. Server internals (storage, quotas, cost) are in the private
repo: `soundshed-guitar-private/docs/plans/2026-11-15-user-sync-server.md`.

## Where things stand

| Area | What exists | What sync needs |
| --- | --- | --- |
| **Local store** | One SQLite file (`data/v1/soundshed.db`) shared by the standalone and every plugin instance. Table `items(type, id, json, updated_at ms)`. Types: `preset`, `composite`, `resource`, `blend`, `custom-effect`, `riff`, `layout`, `setlist`, `effect-preset`, `effect-layout`, `automation-slot`, `practice-tab`, `setting`, and singleton `document` rows. User preset ids are `user-<uuid>`, so ids are already unique across devices. | `Remove` deletes the row, so deletes leave no trace. `ReplaceAll` rewrites a whole type, so `updated_at` moves for rows that didn't change. Several collections are one `document` row (`preset-folders`, `setlists`, `preset-favorites`, `preset-ratings`, `effect-presets`, `riff-library`, `looper-projects`, `automation`), so two devices editing different entries would overwrite each other if the row synced whole. |
| **Cross-instance sync** | The `shared-sync-state` version and a 2 s `PollSharedSyncState()` poll; editors catch up at `uiReady` (`CatchUpWithSharedStore`); `MergeAppSettingsChangedInStore` merges settings by key. | Remote changes can arrive through the same path: write the store, bump the version. A plugin instance with a closed editor still hears nothing until its editor opens. |
| **Files** | Model and IR files under `resources/`, module `.wasm` under `custom-effects/`, riff and looper audio under `riff-library/` and `loops/`. Library rows keep `ResourceRef.source` (Tone3000 id and the like) and `metadata.contentHash` (nam/ir-content-v1, abfcba38). | Most models came from Tone3000 and can be fetched again from their source rather than uploaded. Only files with no public source need upload. `LibraryResource.hash` is 64-bit FNV-1a; uploads need SHA-256. |
| **Account** | Email-code sign-in (`/v1/auth/start`, `/verify`), a 30-day session in the `toneSharing.sessionId` setting, sent as `x-session-id`. The session is an "account setting" and never goes into DAW projects (f4dd3bc9). Per-account feature gates (`user_features`). | A session that silently expires would silently stop sync. A per-device credential the user can see and revoke is better than reusing the browser-style session. |
| **Network in the engine** | `core/src/net/Http.h` (WinHTTP, curl, Apple) with worker threads and cancel tokens; `uiclient::ToneService` already talks to the API from C++. | Sync runs in the engine, so the web UI and Nano Q get it for free. |
| **Missing content** | Presets whose model is missing show the Choose-a-model empty state and a re-download prompt (2ce2ab48, 72cdc0f5). | A preset that syncs before its model lands is already handled. |

Settings are the hard part. Of the ~98 setting keys, some are personal (theme, metronome, favorites,
pads, Scale Guide progressions), some describe this machine (interface calibration, input mode and
dual mono, multi-core, `nativeUi.scale`, `uiBounds`, folder browser roots, `app.instanceId`,
`lastPresetId`), and some are credentials that must never leave the device (`tone3000.apiKey`,
`toneSharing.sessionId`, `jam.youtubeApiKey`).

## What the user sees

**Settings > Account & Sync** (web UI and Nano Q):

1. **Sign in** with the existing email code. Signing in does not start sync.
2. **Sync this device** switch, off by default. Turning it on names the device ("Studio PC",
   suggested from the OS host name) and asks how to start when the cloud already has content:
   - *Merge* (default): keep everything from both sides; same-id items take the newer one, and
     differing copies of user content are both kept (see Conflicts).
   - *Use the cloud copy*: this device's synced categories are replaced (a local backup is taken
     first, as `backups/` already does for migrations).
   - *Use this device*: the cloud is replaced with this device's content.
3. **What to sync**, one checkbox per category, each with an item count and size:

   | Category | Default | Contents |
   | --- | --- | --- |
   | Presets | on | user presets and their scenes, preset folders, favorites and ratings (factory presets don't sync; their favorites and ratings do) |
   | Setlists | on | setlists |
   | Installed packs | on | the list of installed Tone Sharing packs; other devices offer to install them rather than copying their presets |
   | Blends and effect presets | on | blends, per-effect presets |
   | Layouts | on | custom layouts and per-effect layout choices |
   | Custom and composite effects | on | composite definitions, Custom Effect entries and their `.wasm` |
   | Models and IRs | on (references only) | library entries, tags, favorites and categories. Files with a public source are downloaded again on each device. A second checkbox, **Upload my own files**, off by default, uploads files that have no public source. |
   | Preferences | on | personal settings (theme, metronome, pads, MIDI setlist out, Scale Guide, Practice Tool loops, feature toggles) |
   | MIDI and automation | off | MIDI mappings and automation slots; off because controllers differ between rigs |
   | Riffs and loops | off | riff library and looper projects with their audio |
   | Practice tabs | off | imported tab files and their sync points |

   Device settings (calibration, input mode, audio and window state) and credentials never sync,
   and are not offered.
4. **Status**: last synced, pending changes, errors in plain words ("Signed out on the server.
   Sign in again to resume sync."). A small icon in the footer shows syncing, synced, paused or
   failing.
5. **Devices**: each device with its name, platform and last sync; **Remove** revokes its
   credential, and that device stops syncing and says why.
6. **Pause sync** on this device; **Stop syncing** (keeps the local content, removes this device);
   **Delete my cloud data** (every category, every device, confirmed); **Download my data** (a
   zip of what the cloud holds).

Unticking a category stops sending and receiving it on this device. If no other device still syncs
it, the user is asked whether to keep or delete the cloud copy.

Plugin instances share the store, so the setting is per machine, not per instance: turning sync on
in the standalone turns it on in every plugin on that machine.

## How it works

### One sync agent per machine

The standalone and N plugin instances share one store, so only one process syncs at a time. A
lease in the `meta` table (`sync.lease.owner` = `app.instanceId` + pid, `sync.lease.expires`),
taken and renewed every 30 s inside a store transaction, picks the agent. If the holder exits or
hangs, another process takes over when the lease lapses. The agent runs on its own thread, never
the message or audio thread, so it also works in a plugin with its editor closed.

After it writes remote changes into the store, the agent bumps the shared-sync version once per
batch. Every instance then reloads through the existing path, and editors that were closed catch
up at `uiReady`.

### Change tracking without touching every write site

A new local table records what the cloud last saw for each item:

```sql
CREATE TABLE sync_state (
  type TEXT NOT NULL, id TEXT NOT NULL,
  synced_hash TEXT NOT NULL,   -- SHA-256 of the canonical JSON last sent or received
  server_rev INTEGER NOT NULL, -- the server's revision of that version
  PRIMARY KEY (type, id)) WITHOUT ROWID;
```

The agent finds local changes by scanning the synced types and comparing each row's hash with
`synced_hash`: a different hash is an edit, a `sync_state` row with no item is a delete. About
1,500 rows on a large profile, so a scan is cheap, and it is triggered by `MaxUpdatedAt()` moving
(plus a slow timer). This is correct whatever wrote the row, `ReplaceAll` included, and needs no
hooks in the dozens of save paths. Device-only fields inside an item (for example a resource's
`filePath`) are left out of the canonical JSON.

### Document rows become entries

The collection documents sync entry by entry, not as one row. Each gets a small adapter that
splits the document into virtual items (`preset-folder:<id>`, `setlist:<id>`,
`preset-favorite:<presetId>`, …) and writes merged entries back. The item types `preset-folder`,
`preset-favorite` and `preset-rating` already exist in `JsonStore.h`; finishing that move for
folders, favorites and ratings is cleaner than an adapter and is step 1 of phase 0.

Settings sync key by key as `setting:<key>`, filtered through one list, `SyncedSettingCategory(key)`,
next to `IsAccountSettingKey` and `IsInstanceOwnedSettingKey`. A key that isn't listed doesn't sync,
so a new setting is device-local until someone decides otherwise.

### The protocol

The server keeps, per user, an ordered log: every accepted change gets the next revision number.

- `POST /v1/sync/devices` with a session: registers this device, returns a device token
  (long-lived, revocable, shown in Devices). The agent uses only the device token from then on,
  stored like the other account settings.
- `GET /v1/sync/changes?since=<rev>&categories=…`: changes after `rev` (upserts and tombstones), in
  pages, with the newest revision.
- `POST /v1/sync/changes`: a batch of `{type, id, baseRev, hash, json | deleted}`. Each item is
  accepted if `baseRev` is the server's current revision for that item; otherwise it is returned as
  a conflict with the server's version.
- `PUT/GET /v1/sync/blobs/<sha256>`: file content, for files the user chose to upload.
- `GET /v1/sync/notify` (WebSocket, later): "your log moved", so devices pull at once rather than on
  the next timer.

A cycle: pull since the last revision and apply, then push local changes with their base revisions,
then resolve conflicts. Items over 64 KB (some presets reach 1 MB) go as blobs, with the item
holding the hash.

Tombstones are kept 180 days. A device that hasn't synced for longer gets a full reconcile:
it lists the server's live items and hashes, and the user's start choice (merge by default) decides.

### Conflicts

- **User content** (presets, setlists, blends, layouts, composites): if both sides changed the same
  item, the server copy wins under its id and the local version is kept as a new item,
  "My Crunch (Laptop, 15 Nov)", and the status shows "1 conflict kept as a copy". Nobody loses an
  edit.
- **Small values** (settings, favorites, ratings, folder membership): the newer change wins.
- **Delete vs edit**: the edit wins and the item comes back.
- **The active preset**: an incoming change never replaces the sound mid-play. If the active preset
  changed on another device, the library is updated and the preset bar offers "Updated on Studio
  PC. Load it?"; with unsaved edits here, those edits become the conflict copy when saved.

### Models, IRs and other files

For each synced library entry, the agent on the receiving device:

1. Keeps the local file if one with the same content hash is already in the library.
2. Otherwise fetches it from its source (Tone3000 by id, falling back to the content-hash lookup),
   through the existing import path and the throttled proxy rules. These are user-requested
   downloads on the user's own device, one at a time.
3. Otherwise, if the sending device uploaded it, downloads the blob.
4. Otherwise leaves a "not on this device" entry; presets using it show the existing missing-model
   state with "Find on Tone3000" and "Choose a file".

Downloads are lazy by default: the library entry arrives at once, the file is fetched when a preset
needs it or the user opens the library. A "Download all now" button covers going offline. Mobile
asks before large downloads on a metered connection.

Uploads use SHA-256 and a per-user quota. The server stores each user's blobs privately; it never
shares a blob between users, even with the same hash.

### Security and privacy

- TLS to the API; the server stores content at rest encrypted by the platform. No end-to-end
  encryption in v1 (decision S4).
- Device tokens are scoped to `/v1/sync/*`, hashed on the server, revoked from Devices or on sign
  out. Signing out on one device removes it from sync and leaves its local content.
- The agent never sends credentials, device-only settings or file paths. A test checks every
  outgoing item against the deny list.
- Content from the server is untrusted input: items go through the same validation as an import
  (preset schema, layout size cap, composite cycle check, wasm module cap) before they are written.

## Build order

**Phase 0: local groundwork (no network).** Move folders, favorites and ratings onto their item
types. `sync_state` table and migration. Category registry, `SyncedSettingCategory`, canonical JSON
and hashing, the change scanner, document adapters. Tests: scanner finds edits, deletes and
`ReplaceAll` no-ops; deny list; adapters round-trip.

**Phase 1: server.** Device registration, change log, push with conflicts, tombstones, blobs,
quotas, delete-all and export. Gated per account with a `user_features` row (`sync`), as the Tone
Builder is, so it can run on a few accounts first.

**Phase 2: engine agent, small content.** Lease, worker thread, pull/push, apply and shared-sync
bump, conflict copies, active-preset notice. Categories: presets, setlists, installed packs, blends
and effect presets, layouts, preferences. Tests drive two stores (and two processes for the lease)
against a fake transport; engine tests must call `test::SetSettingsEnvRoot`.

**Phase 3: UI.** Account & Sync in the web UI and Nano Q, the start choice, category checklist with
sizes, devices, status icon, plain-language errors.

**Phase 4: files.** Library entries by reference, fetch from source, lazy download, then
**Upload my own files** with the quota; Custom Effect `.wasm`.

**Phase 5: optional and large.** MIDI and automation, riffs and looper projects, practice tabs; live
notify over WebSocket.

**Phase 6: before general release.** Load test the change log, a full-reconcile test after a
long absence, account deletion removes everything, privacy policy text, a support view of a user's
sync state.

## Testing it live

Two throwaway profiles (`APPDATA` pointed at temp folders, see the Tones traps) signed in to a test
account on preview staging, never the real profile: sign-out and "Use the cloud copy" are
destructive there. Check: a preset edited on A appears on B; a delete; a conflict produces a copy;
a plugin with its editor closed picks up a change on opening; a model with a Tone3000 source is
re-downloaded, a local-only one appears as missing until uploaded; removing B from Devices stops
it.

## Decisions to make first

**S1. Free storage and limits.** How much upload space each user gets (proposal: JSON content is
free and effectively unlimited; uploaded files 1 GB, 100 MB per file), and whether a paid tier
raises it. Riffs and loops are what fill it.

**S2. Default categories.** The table above turns on everything small and leaves MIDI, audio and
tabs off. Confirm, or start with presets and setlists only.

**S3. Conflict policy.** Keep-both copies for user content, newest-wins for small values (above),
or newest-wins everywhere (simpler, loses edits).

**S4. End-to-end encryption.** Off in v1 (server can help with support and export; content is not
very sensitive), or on (no server-side recovery, harder web access later).

**S5. Uploading user model files.** Allow uploads of files with no public source, or sync only
references and leave such files to the user. Hosting third-party captures privately per user is
the question.

**S6. Pack-installed presets.** Sync the pack list and reinstall from Tone Sharing (proposed), or
sync the installed presets as ordinary presets (works for packs that are later unpublished).

**S7. Sign-in on plugin-only installs.** A user who only runs the plugin in a DAW signs in from the
plugin editor; confirm that's acceptable, since every DAW on the machine then syncs too.
