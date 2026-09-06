# Release Manifest Security Review

Status: implemented and production-key activation rehearsed.

## Scope and threat boundary

The signed release manifest authenticates final artifact metadata and prevents
rollback to an older accepted release. GitHub Releases is discovery and
transport only; no manifest field or artifact is trusted before verification.
Authenticode remains a separate publisher-identity layer.

This design does not protect a machine already controlled by local malware or
an administrator, a compromised production signing key, or an obsolete client
that predates a key revocation. Those cases require key revocation, a new
trusted build, and incident-response procedures.

## Approved dependencies

| Consumer | Approved dependency | Integrity pin | License and constraint |
|---|---|---|---|
| GameHQ and static updater (C/C++) | Monocypher 4.0.3 optional Ed25519 module | Official `monocypher-4.0.3.tar.gz` SHA-512: `40904ada5c7ee4f7741733e38b69a30a4b0561cbffba5ffe7c2dce16136d540251ec0d9056ff606510d3b5b708fb8a40db7e0870d4a0b2dc17ba2bfb880f8965` | Dual 2-clause BSD/CC0; use the upstream source unchanged and retain the selected license notice. Versions 4.0.2 and older are forbidden because 4.0.3 fixes the June 2026 EdDSA timing leak. |
| Playnite plugin (`net462`) | `BouncyCastle.Cryptography` 2.6.2 | Official NuGet package SHA-512 (Base64): `Thoy+Tfuu6E08cZkbAtVTtqhvEGWnexYFtEcPChZBuEGN3ET6mwcsLFhdJ/wiuitOd8FlWtiq+InTUswYPDa7w==`; NuGet lock content hash: `7oWOcvnntmMKNzDLsdxAYqApt+AjpRpP2CShjMfIa3umZ42UQMvH0tl1qAliYPNYO6vTdcGMqnRrCPmsfzTI1w==` | Bouncy Castle MIT-style license; retain its notice. The package supplies a `net461` asset compatible with the plugin's `net462` target. |

Both dependencies must be exact-version pins. CMake must verify the Monocypher
archive hash before extraction. NuGet restore must use a lock file with locked
mode in release validation. Dependency upgrades require a new focused review;
version ranges and floating tags are forbidden.

Only the verification surface is exposed through GameHQ-owned wrappers:
`crypto_ed25519_check` in C/C++ and the pure-mode
`Org.BouncyCastle.Math.EC.Rfc8032.Ed25519.Verify` overload in C#. GameHQ must
not implement curve arithmetic, signature parsing, or Ed25519 itself.

## Signed bytes and detached signature

- Algorithm: RFC 8032 pure Ed25519, without context or prehash.
- Message: the exact downloaded bytes of `gamehq-release.json`.
- Generator output: UTF-8 without BOM, LF line endings, a fixed field order,
  and no insignificant whitespace. This makes releases reproducible, but
  verification never parses and reserializes JSON.
- `gamehq-release.sig`: standard padded Base64 of one 64-byte signature. Its
  only accepted text form is 88 ASCII characters matching
  `^[A-Za-z0-9+/]{86}==$`; whitespace, URL-safe Base64, missing padding,
  trailing data, and any decoded length other than 64 are rejected.
- Trusted public keys: standard padded Base64 decoding to exactly 32 bytes.
  A `keyId` selects a pretrusted key; it never creates trust.

Verification order is fixed:

1. Enforce manifest and signature download-size limits.
2. Strictly decode the detached signature and select a compiled trusted key.
3. Reject unknown or revoked keys before cryptographic verification.
4. Verify the signature over the untouched manifest byte buffer.
5. Only then parse JSON and validate schema, `productId`, timestamps, version,
   release sequence, minimum consumer versions, artifact names, sizes, and
   SHA-256 values.
6. Hash the selected artifact and compare it before execution or extraction.

Any error is fail-closed and indistinguishable to release selection: the asset
is not installable and no trust state advances.

## Trust set and rotation

Each consumer ships an immutable trust table containing records with:

```text
keyId
algorithm = Ed25519
publicKeyBase64
state = current | next | revoked
minimumReleaseSequence
```

`current` keys verify releases. A `next` key is distributed in a normally
signed consumer build before it is promoted to `current`; it cannot verify a
release until that state change is shipped. A `revoked` key always fails even
if its signature is valid. Signed manifest data cannot add trust, promote its
own key, or reverse revocation. Emergency downgrade is published as a new,
higher release sequence signed by a current key.

The C++ app and updater must share the same generated trust-table source. The
C# table is generated from the same checked-in public-key input, and tests
must assert identical key IDs, key bytes, states, and sequence bounds across
all three consumers.

## Anti-rollback state

After full signature and semantic validation, each consumer atomically stores:

```json
{
  "schemaVersion": 1,
  "highestReleaseSequence": 0,
  "manifestSha256": "..."
}
```

GameHQ and the updater use the user-data root outside the replaceable program
allowlist; the Playnite plugin uses its settings storage. A lower sequence is
rejected. The same sequence is accepted only when the manifest SHA-256 is
identical, allowing an idempotent retry while rejecting equivocation. Binary
rollback never lowers this counter. State writes use write-new, flush, and
atomic replace semantics; corrupt state fails closed and requires an explicit
recovery path rather than silently resetting to zero.

## Shared verification fixtures

One checked-in fixture set is consumed by C++, the static updater, and C#:

- RFC 8032 Section 7.1 pure-Ed25519 vectors 1 through 3;
- a byte-exact representative manifest and signature produced by an
  independent offline reference tool;
- manifest bit flips, CRLF conversion, BOM insertion, whitespace changes,
  wrong public key, modified `R` and `S`, and truncated or oversized inputs;
- invalid, URL-safe, unpadded, whitespace-bearing, and non-canonical Base64;
- unknown, next, and revoked keys;
- lower sequence and equal-sequence/different-hash rollback cases;
- artifact hash, size, filename, product, schema, and minimum-version failures.

Test private keys live only under an unmistakable test-fixture path, carry a
`TEST ONLY` marker, and are rejected by packaging and Stable release gates.
Production private material is created and used outside the repository and
ordinary CI, is never logged, and is never copied into a developer package.

The active production record is `gamehq-prod-2026-01`, starting at release
sequence 25. The seed is available only through Windows Credential Manager or
the review-protected GitHub `production-release` environment. Rehearsal covers
successful signing, public-key verification, tamper rejection, test-key
exclusion from production binaries, and the existing next/revoked-key vectors.

## Test trust is a separate library, not a build option

The fixtures above are signed with the published RFC vector key, so something in
the test process has to trust that key. That trust lives in its own static
library, `gamehq_release_trust_testkeys`, built from the same sources as the
shipped `gamehq_release_trust` and differing only in the compiled trust table.
Only test targets link it. The release-transaction tests additionally drive
`updater_test_trust_fixture`, a twin of `GameHQUpdater` built from one shared
source list and linked against that library; nothing packages it.

`GAMEHQ_RELEASE_TRUST_TEST_KEYS` therefore stays `OFF` for every configuration
this project builds, including test builds and CI. One build tree is both
shippable and fully testable, which is what lets release validation run the
native suite inside the build directory it actually packaged. The shipped
binaries are asserted from both sides: `tst_releasetrust` requires
`GameHQUpdater --release-trust-self-test` to report `TRUST TABLE production`
with no `gamehq-test-` key, and `tst_updatertransaction` requires the shipped
helper to refuse the very transaction the test twin accepts.

## Implementation gate

Implementation may begin only with the exact pins and contract above. It is
complete when all consumers pass the same positive and negative vectors, the
release gate rejects test keys and unsigned Stable output, dependency notices
ship with artifacts, and an independent review confirms that parsing and
execution cannot occur before raw-byte signature verification.

Primary references: [Monocypher downloads](https://monocypher.org/download/),
[Monocypher 4.0.3 changelog](https://monocypher.org/changelog),
[Monocypher disclosure](https://monocypher.org/quality-assurance/disclosures),
[BouncyCastle.Cryptography 2.6.2](https://www.nuget.org/packages/BouncyCastle.Cryptography/2.6.2),
[Bouncy Castle C# source and license](https://github.com/bcgit/bc-csharp/tree/release-2.6.2),
and [RFC 8032](https://www.rfc-editor.org/rfc/rfc8032.html).
