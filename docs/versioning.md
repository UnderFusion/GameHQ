# Versioning & Documentation Rules

> **Owner policy (2026-09-25):** 0.7.8 is published. The Bluetooth DualSense hotfix is **0.7.9**, shipped first as the `v0.7.9-beta1` GitHub pre-release. `VERSION` stays plain `MAJOR.MINOR.PATCH` (the updater and release-note schema reject suffixes); a beta is identified by its tag and release title only. Internal implementation slices do not increment the product version.

## Scheme

GameHQ uses Semantic Versioning as plain `MAJOR.MINOR.PATCH`.

- `MAJOR` - `1.0` is the first polished release; later major bumps are for breaking data/config format changes.
- `MINOR` - active development line or roadmap milestone family.
- `PATCH` - advances for an owner-designated product release, not each internal change.

Current development line: `0.7.x`.

On 2026-09-22, `git ls-remote --tags origin 'v0.7.*'` and `gh release list`
confirmed v0.7.7 as the latest published GameHQ version (2026-09-07).
The 0.7.8-0.7.43 development slice labels had no published tags/releases.
Their changes are consolidated into the 0.7.8 local candidate. All 80 published
release-note source documents for 0.7.3-0.7.7 were verified byte-for-byte against
tag v0.7.7; no public history was rewritten. Git commits remain historical
implementation checkpoints, and package source-input hashes distinguish local
uncommitted candidates sharing the same product version.

Patch numbers run from `0` to `99` within a minor line. After `0.5.99`, the next version is `0.6.0`. The first version after the old `0.1.0-dev.N` scheme is `0.5.0`; the next changed build is `0.5.1`.

The single source of truth is the `VERSION` file in the repo root. CMake reads it and injects `GAMEHQ_VERSION` into the binary; the About/sidebar version label and logs display it. `VERSION` is registered as a CMake configure dependency, so incremental builds reconfigure when the file changes.

The matching release-note source must be prepared in
`assets/release-notes/versions/<version>/` and registered newest-first in its
manifest. Preserve released history and use explicit whole-English fallback
documents for locales whose new notes are not translated. Run
`python tools/i18n/generate_release_notes.py` and
`python tools/i18n/generate_release_publication.py` before configuring: CMake
requires the generated offline index to match `VERSION`. Generated bundles,
indexes and publication artifacts must never be edited by hand. Preparing these
local files does not publish or tag a release.

## Release Checklist

1. Use the owner-designated release version in `VERSION` (currently 0.7.9); never add a `-betaN` suffix to it.
2. Add a `CHANGELOG.md` entry with Added/Changed/Fixed/Removed sections as needed.
3. Update affected docs in `docs/` when code changes behavior, architecture, storage, setup, capture, overlay, input, or UI rules.
4. Update `docs/README.md` if a doc is added, renamed, or its purpose changes.
5. Update the roadmap or subsystem documentation when scope or behavior changes.
6. Database schema change? Add a migration and bump `PRAGMA user_version`; see [database.md](database.md).

## Changelog Format

[Keep a Changelog](https://keepachangelog.com/): one `## [x.y.z] - YYYY-MM-DD` section per version, newest on top.
