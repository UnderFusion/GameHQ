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

Generate all offline bundles:

```powershell
python tools/i18n/generate_release_notes.py
```

Verify byte-for-byte freshness without writing:

```powershell
python tools/i18n/generate_release_notes.py --check
```
