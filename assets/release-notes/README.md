# Versioned release-note sources

`manifest.json` orders released versions newest-first and pins the canonical
`en-US` integrity for each version. Every version directory contains one
canonical English document and sibling documents for the other fifteen
production locales.

A locale document has exactly one mode:

- `localized` contains the complete section and item structure, preserving all
  stable IDs and the canonical source integrity;
- `fallback` explicitly selects the complete English document for that version
  and contains no translated presentation fields.

Field-level fallback is forbidden. Missing or unusable locale documents resolve
to the whole English version only when the manifest policy is
`fallback-allowed`. Releases marked `complete` require valid localized documents
for all sixteen production locales.

Released English sources are immutable. An explicit correction keeps
`original_source_integrity`, updates `source_integrity`, and records `reason`,
`approved_by`, and `corrected_at` in the manifest.

## Publication and the GitHub Release asset policy

`publication/<version>/` holds the rendered publication artifacts: one
`release-notes.<locale>.md` per production locale, the English `RELEASE_BODY.md`
and `publication-metadata.json`.

The per-locale documents are internal, generated publication inputs. They are
**never** uploaded as individual GitHub Release assets, because sixteen extra
Markdown files bury the installable packages on the Downloads list. The release
body links the tagged repository directory once instead, and
`publication-metadata.json` records that contract in its `distribution` block
(`github_release_assets: false`). See `docs/packaging.md` > GitHub Release asset
policy.

This versioned source replaced the single-locale `assets/release-notes.json`,
which has been retired. `tools/i18n/fixtures/release-notes-history.en-US.json`
is the byte-frozen record of the released 0.7.3-0.7.6 English history that was
migrated off it; `tools/i18n/test_release_notes_generation.py` proves the
versioned source still reproduces it exactly.

## Localization-launch designation

`manifest.json` carries a `localization_launch` object recording the owner's
designation of the first release that must ship complete translations for all
sixteen production locales:

```json
"localization_launch": {
  "version": "0.7.7",
  "date": null,
  "status": "designated",
  "localization_policy": "complete",
  "designated_by": "owner"
}
```

The version is fixed. The final ISO date stays `null` until the owner assigns
it, and no tool may invent one: while `status` is `designated`, a non-null date
is rejected outright. A designated release is **not** a released version, so it
never enters `releases`, the generated bundles, the publication artifacts, or
the runtime index.

When the owner finalizes the launch, `status` becomes `released`, the date is
assigned, and the same version is promoted into `releases` exactly once as the
newest entry, with matching date, `complete` policy and the integrity of its
dated English document. A `released` launch that is absent from `releases`, is
promoted twice, or is not the newest entry is rejected: it would describe a
release that neither the generated bundles nor the publication artifacts ship.

A designated version may already carry complete draft documents under
`versions/<version>/`. Those documents keep `"date": null` too: a draft with a
date is rejected, and a released document without one is rejected. Drafting,
translating, reviewing and validating therefore never wait for the release date.

Content readiness and release readiness are separate gates:

```powershell
python tools/i18n/generate_release_notes.py --launch-status   # content readiness
python tools/i18n/generate_release_notes.py --release-ready   # final release readiness
```

`--launch-status` passes once all sixteen documents exist, validate, and are
reviewed translations rather than English fallback. `--release-ready` also
requires the owner's final date and explicit release authorization, so it keeps
failing until the owner acts. `tools/i18n/locale.ps1 verify` runs the content
gate; `tools/i18n/locale.ps1 package` runs the release gate.

Generate all offline bundles:

```powershell
python tools/i18n/generate_release_notes.py
```

Verify byte-for-byte freshness without writing:

```powershell
python tools/i18n/generate_release_notes.py --check
```

Generation also writes `generated/release-notes.index.json`. The application
uses its exact locale filenames, byte sizes, SHA-256 hashes, current version,
and per-document source integrity before presenting any bundle.

## Publication artifacts

`publication/<version>/` holds the offline GitHub-release artifacts for every
released version. They are **outputs only**: never edit them, never treat them
as an authoring source, and never use them as an update-authorization,
signature, version-selection, download, or installation input.

```powershell
python tools/i18n/generate_release_publication.py
python tools/i18n/generate_release_publication.py --check
python tools/i18n/generate_release_publication.py --version 0.7.6
```

Each version directory contains:

- `release-notes.<locale>.md` for all sixteen production locales;
- `RELEASE_BODY.md`, the concise English release body plus an index of the
  locale assets;
- `publication-metadata.json`, listing per asset the canonical, requested, and
  effective locale, the fallback state and reason, the version, date, filename,
  byte size, SHA-256, the release source integrity, and the stable section and
  item IDs.

A document renders from exactly one source document, so it is never part
translated and part English. Where a version has no reviewed translation, the
locale asset is still emitted, carries the complete English text, and states in
English that it is the original English release note. `CHANGELOG.md` remains the
authored English cumulative record and is never generated from these artifacts.
