# Settings & Data Storage Schema (V1)

## Platform root directories

- Windows: `%APPDATA%/Soundshed Guitar`
- macOS: `~/Library/Soundshed Guitar`

All persisted app data lives under `<ROOT>/data/v1/`.

## Directory layout

```text
<ROOT>/
  data/
    v1/
      settings/
        app.json
        ui/
          preset-favorites.json
          preset-ratings.json
          setlists.json
          window-state.json
      presets/
        preset-folders.json
        user/
          <presetId>.json
      resources/
        content/
          <provider>/
            <resource-file>
        indexes/
          resources-index.json
      blends/
        library.json
      composites/
        user/
          <compositeId>.json
      layouts/
        content/
          <layoutId>.layout.json
        indexes/
          effect-layouts.json
        images/
          <image-file>
      logs/
        session-log.txt
```

## Key JSON contracts

### `settings/app.json`

Global app settings and integration values.

```json
{
  "schemaVersion": 1,
  "updatedAt": "2026-02-13T12:00:00Z",
  "lastPresetId": "user-6bae78fb-5225-4b6c-8707-93895fa22823",
  "theme": "dark",
  "tone3000.apiKey": "...",
  "toneSharing.publishConsent": {
    "version": 1,
    "acceptedAt": "2026-03-14T12:00:00Z",
    "userId": "usr_123"
  },
  "diagnostics.signalLevelsEnabled": true,
  "audio.userInputCalibration.profiles": [],
  "audio.userInputCalibration.activeProfileId": null,
  "audio.dsp.nominalOperatingLevelDbfs": -18.0,
  "audio.dsp.outputProtectionCeilingDbfs": -1.0,
  "ui.advancedOptionsEnabled": true,
  "factoryPresets.archiveLoadingEnabled": true,
  "featureFlags": {}
}
```

### `settings/ui/*.json`

UI-specific persistence files (split by concern).

```json
{
  "preset-favorites.json": { "favorites": [] },
  "preset-ratings.json": { "ratings": {} },
  "setlists.json": { "setlists": [], "activeSetlistId": "" },
  "window-state.json": { "width": 1600, "height": 1200 }
}
```

### `presets/preset-folders.json`

Preset folder organization metadata.

```json
{
  "folders": [],
  "activeFolderId": "__all__"
}
```

### `resources/indexes/resources-index.json`

Canonical index for managed resources.

```json
{
  "schemaVersion": 1,
  "updatedAt": "2026-02-13T12:00:00Z",
  "items": [
    {
      "resourceId": "nam:sha256:3f2c...",
      "resourceType": "nam",
      "provider": "user",
      "contentHash": "3f2c...",
      "fileExt": "nam",
      "filePath": "content/user/3f2c....nam",
      "displayName": "5150 Rhythm",
      "originalFileName": "5150_Rhythm.nam"
    }
  ]
}
```

## Notes

- Managed resource binaries are hash-named; readable names stay in metadata.
- Write JSON atomically where practical (tmp + rename).
- Readers should tolerate missing or unknown fields.
- New writes target only `<ROOT>/data/v1`.
- `diagnostics.signalLevelsEnabled` is forced to `true` by the current product for compatibility.
- User input calibration profiles and advanced DSP level targets are stored directly in `settings/app.json`.
- Local preset archive exports remain full-fidelity and can include all referenced resources. Tone-sharing publishes use a separate sanitized archive for public sharing and may also upload a private full archive for API retention.
