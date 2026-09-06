# Localization release readiness

This document freezes the pre-release localization workflow. It prepares an
inspectable candidate for the `p8` acceptance chain; it does not perform
linguistic acceptance or authorize a release.

The machine-readable snapshot is
`i18n/release/readiness-0.7.7.json`. Regenerate it only when an intentional
candidate input changes:

```powershell
python tools/i18n/release_readiness.py
python tools/i18n/release_readiness.py --check
```

The second command is deterministic, offline, and read-only. It identifies a
stale file or exact invalid locale, source, hash, correction, or policy field.

## Mandatory order

1. **Freeze English.** Record SHA-256 values for `VERSION`, the locale manifest,
   extracted English units, the en-US catalog, the release-note manifest, the
   designated en-US launch document, and the correction ledger.
2. **Synchronize.** Run `tools/i18n/sync.ps1 -Check`. A candidate cannot proceed
   while generated IDs or source locations are stale.
3. **Translate the queue only.** `check-diff.ps1` defines missing or stale IDs.
   The agent protocol must reject extra IDs or a response based on an obsolete
   source hash.
4. **Preserve structure.** Stable IDs, UI role, placeholders, plural forms,
   markup, accelerators, line breaks, protected brands, executables, APIs,
   protocols, paths, switches, and URLs are invariant.
5. **Verify all sixteen locales.** Run the offline structural and provenance
   checks. `en-XA` and `ar-XB` remain development-only; `cs-CZ` remains reserve.
6. **Generate and package.** Reuse the completed `p7-1` and `p7-2` gates. Never
   substitute source-tree checks for inspection of produced package bytes.
7. **Hand off to p8.** `p8-1` completes assets and provenance, `p8-2` runs
   multilingual automation, and `p8-3` alone performs contextual linguistic QA
   for every locale.
8. **Seek owner authorization last.** Only `p8-4`, after every prior gate, may
   ask the owner to accept the localized release.

No later step may be used to infer that an earlier one passed. A structurally
valid or machine-produced translation is still `pending` until `p8-3` records
contextual acceptance.

## Current source coverage and historical reviews

`tools/i18n/linguistic_qa.py` derives the expected ID set and count from
`i18n/extracted/messages.json`. The active catalog, review coverage, and recorded
message count must match that manifest exactly; each reviewed source and
translation hash must also match. Empty manifests, duplicate IDs, unfinished
translations, and missing coverage fail validation. The JSON schema permits a
variable positive count; the validator enforces its relationship to the manifest.

Accepted 831-ID review artifacts remain historical evidence and are not rewritten
by validation. A new candidate needs a current receipt when its catalog changes.
Unchanged per-ID decisions remain valid; added IDs require translation and review,
and changed source hashes invalidate only the affected reviewed state. Removed
IDs leave active coverage but retain their catalog history as `vanished` or
`obsolete`; these entries are excluded from active catalog counts. Synchronization
and state reconciliation never promote new or stale translations to reviewed.

The manifest-evolution fixtures in `tools/i18n/test_linguistic_qa.py` exercise
these transitions on isolated copies and verify that an incomplete enabled locale
still fails the release gate.

## Provenance and privacy

Every translation or correction event must retain:

- the exact English source hash and target locale;
- the bounded workflow or reviewer method and an opaque actor label;
- translation and review states as separate facts;
- an ISO-8601 timestamp when an event changes translated content or review
  state. Agent contextual review is recorded separately from human review;
- the correction ledger reference when prior translated content changes.

Actor labels identify a reproducible workflow, not a person or conversation.
Localization evidence must never contain user data, telemetry, credentials,
secrets, prompts, responses, or conversation contents. Runtime text, filenames,
captures, logs, and account identifiers are never translation input. Only
product-owned English source, developer context, the glossary, and locale style
guides may enter a translation workset.

The readiness snapshot intentionally reports current state and provenance
counts, including unresolved or unknown states. It must not relabel them as
reviewed. `machine_output_self_certifies_linguistic_quality` is permanently
false.

## Corrections

Application and installer translation corrections are append-only records in
`i18n/release/corrections.json`. Each delta binds one locale and stable ID to:

- the current English source hash;
- the previous and corrected translation hashes;
- the reason, method, actor, timestamp, and commit;
- `pending_linguistic_qa`, `contextually_reviewed`, or
  `linguistically_accepted` review state.

The validator checks the corrected hash against the current catalog. Agent
review uses `agent_contextual_review` and remains distinguishable from human
review. A record cannot claim `linguistically_accepted` unless its method is
`human_contextual_review`; machine or structural checks cannot promote it.
The Polish `gamehq.navigation.about` repair is the first concrete ledger entry.

Released release-note claims use the separate correction metadata in
`assets/release-notes/manifest.json`. Previously released history stays
immutable unless that explicit correction contract is satisfied. Never rewrite
old claims as part of translation cleanup, and never use field-level fallback.

## Owner-requested sidebar surfaces

Every locale row in the readiness snapshot includes current catalog and state
evidence for:

- `gamehq.navigation.about`;
- `gamehq.navigation.support_gamehq`.

Their presence and hashes establish structural/provenance readiness only. Their
wording, fit, tone, and UI function remain mandatory `p8-3` review work.

## Candidate boundary

The localization launch remains designated as `0.7.7` with a null date.
Readiness generation records `release_authorization: not_requested` and
`publication_state: prohibited`. This workflow never changes `VERSION`, creates
a tag, publishes an artifact, deploys a build, or flips release status.

## Validation modes

`release_readiness.py` validates two distinct repository states through an
explicit `--mode` selector. The mode is never inferred from file contents.

| Mode | Selector | Validates |
| --- | --- | --- |
| Candidate | default, no selector | the protected pre-release state above |
| Final | `--mode final --output <path>` | a repository already finalized elsewhere |

`tools/i18n/ci.ps1` and the public workflow call the default candidate mode and
carry no selector, so the offline gate keeps rejecting accidental early
finalization exactly as before.

Final mode fails closed. It requires a released, owner-designated,
`complete` localization launch whose version equals the repository `VERSION`,
an ISO-8601 release date, the same date on all sixteen localized release-note
documents, contextual linguistic review for every production locale, and a
release-note linguistic state that tracks the released version.

A released launch is only finalized once the owner has promoted it into
`releases`, because generation and publication ship exactly what `releases`
records. Final mode therefore requires exactly one history entry for the
released version, at the newest position of the manifest's documented
newest-first ordering, marked `released` and `complete`, dated identically to
the launch, with `source_integrity` and `original_source_integrity` both equal
to the canonical hash of the dated `versions/<version>/en-US.json` and
`correction: null` - a first publication cannot already be a correction of
itself. Missing promotion, duplicate promotion and wrong history position each
fail with their own diagnostic.

Selecting `--mode final` is **not** owner authorization. Final evidence records
`release_authorization: not_validated` and
`publication_state: requires_owner_authorization`; the validator never
synthesizes owner intent. Final mode is read-only with respect to the
repository: it requires an explicit `--output` outside the normal candidate
snapshot, and `--mode final --check` writes nothing at all.

Assigning the release date rewrites every localized release-note document, so a
real finalization also re-pins the release-note surface hash recorded in each
`i18n/quality/reviews/<tag>.json`. Refresh that evidence as part of the
finalization commit, before rerunning the final-mode gate on the exact
release-candidate commit.
