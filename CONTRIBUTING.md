# Contributing to GameHQ

Bug reports, documentation improvements, and focused pull requests are welcome.

## Before opening an issue

- Check existing issues and the latest release notes.
- Include the GameHQ version, Windows version, controller/backend if relevant,
  clear reproduction steps, and the expected versus actual result.
- For crashes or capture failures, attach the diagnostic summary from
  **Settings → Advanced** and the relevant log excerpt. Remove personal paths
  or game/account information first.
- Report security problems privately as described in [SECURITY.md](SECURITY.md).

## Development workflow

1. Build the project using [docs/dev-setup.md](docs/dev-setup.md).
2. Keep changes focused and update affected documentation.
3. Use an English Conventional Commit message, such as
   `fix(input): preserve secondary binding after reset`.
4. Verify a clean build and launch before opening a pull request.

### Localization closure for UI changes

Any new or changed user-visible QML, C++, installer, or release-note text must use a
stable `gamehq.*` ID and close its localization delta in the same feature change:

1. Run `tools/i18n/sync.ps1` to update the extraction manifest and preserve removed
   messages as obsolete catalog history.
2. Run `tools/i18n/verify.ps1 -UpdateState` to reconcile structural and trust state.
3. Run `tools/i18n/check-diff.ps1 -BaseRef <merge-base>` to create the exact per-locale
   workset under `out/i18n/change-workset.json`.
4. Translate only the queued IDs through an authorized offline developer workflow,
   validate every response with `tools/i18n/protocol.ps1`, and update catalogs/state.
5. Repeat synchronization and verification, then rerun the diff check. Exit code 0 is
   required before the feature slice is localization-complete.

Unchanged IDs are not queued. Removed IDs stay obsolete until a separate cleanup. A
feature may be temporarily incomplete during development, but it cannot close while an
enabled locale has a missing, stale, structurally invalid, or old-source-bound affected
message. The check makes no network or model call and is not a substitute for later CI
and release gates.

Pull requests should explain the behavior change, why it is needed, and how it
was validated. Avoid committing build output, runtime databases, logs, captures,
toolchains, or editor-specific files.

## Contribution licenses

GameHQ uses an inbound-equals-outbound policy with no contributor license
agreement or copyright assignment:

- contributions to first-party GameHQ core code and documentation are
  submitted under GPL-3.0-only;
- contributions under `integrations/playnite/` and to the public integration
  protocol are submitted under their reviewed local MIT licenses;
- third-party material remains under its documented upstream license and must
  not be copied into the project unless that license is compatible with the
  destination and all attribution and source obligations are met.

By submitting a contribution, you confirm that you created it or otherwise
have the authority to submit it under the applicable license. Employer,
school, client, or collaborator rights must be resolved before submission.
Do not submit proprietary code, use-restricted media, private data, secrets, or
material copied from an incompatible source.

AI-assisted work is permitted only when the contributor reviews and takes
responsibility for the result and can account for the provenance of included
code, media, models, datasets, prompts, or generated assets. Uncertain origin
or licensing must be disclosed and resolved before review.

The project does not request a proprietary dual-licensing grant. A Developer
Certificate of Origin or other attestation would require a separate future
project decision.
