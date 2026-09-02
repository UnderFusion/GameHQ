# GameHQ translation-agent protocol

Version 1 defines a provider-neutral, offline developer contract for translating a
bounded queue. It does not authorize a model provider, send runtime data, or modify
source code, catalogs, plan state, or messages outside the queue.

## Authority and source language

- `en-US` is the only translation source. Never use another locale as the source for a
  third locale.
- A queue is authoritative only for its exact `batch_id`, `target_locale`, message IDs,
  English source hashes, glossary version, and locale style version.
- Inputs contain product-owned UI text and developer context only. Credentials, telemetry,
  user text, filenames supplied by users, and other runtime data are forbidden.
- The agent returns structured JSON only. It must not edit files or infer additional work.

## Queue contract

`translation-queue.schema.json` defines the input. Each unit supplies the stable ID,
current English source and hash, developer context, UI location hint, domain, plural and
placeholder metadata, markup and accelerator constraints, protected tokens, and the prior
translation when the unit is stale. Units are sorted by ID.

The target locale metadata is copied from `i18n/locales.json`. The referenced glossary and
`i18n/style/<locale>.json` guide are mandatory inputs. Natural wording is preferred over
literal word order, but meaning and all structural constraints must remain unchanged.

## Response contract

`translation-response.schema.json` defines the only accepted output. Every queued ID must
appear exactly once, in sorted order, with the same source hash. No unrequested ID is
allowed. A translation is an array so singular and plural payloads share one shape.

Each unit declares `machine_translated`, an opaque provider-neutral provenance actor, and
the deterministic SHA-256 translation hash. The protocol validator rejects the entire
response atomically if any unit fails. It never partially applies a response.

## Required preservation

- Preserve the exact placeholder multiset, including `%n`, `%1`, and `%L1` distinctions.
- Preserve plural form count for the target locale.
- Preserve markup structure, Markdown delimiters, accelerator count, and line-break tokens.
- Copy every protected token exactly, including brands, executables, paths, URLs, switches,
  config or registry keys, action/control IDs, protocol keys, and user-provided names.
- Emit valid UTF-8 text normalized to NFC, without control characters or mojibake.

## Validation and failure

Run `tools/i18n/protocol.ps1 -Queue <queue.json> -Response <response.json>`. Failures name
the JSON path and violated rule, including wrong locale or ID, stale source hash, malformed
structure, or mutated protected content. `-CanonicalResponse <path>` writes stable JSON for
review; identical input produces identical bytes.

Protocol validation does not generate translations, update TS catalogs, promote trust
state, enforce per-change policy, or implement CI release gates. Those are separate,
explicit workflows.
