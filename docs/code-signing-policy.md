# Code-Signing Policy

GameHQ uses two separate trust layers:

- The signed release manifest authenticates release metadata and artifact
  hashes. Its production Ed25519 key is provisioned outside the repository.
- Authenticode identifies the Windows publisher for GameHQ-built executables,
  Setup, and the Inno Setup uninstaller. It does not replace artifact hashes or
  the signed release manifest.

## Release roles and access

Accounts with repository write, release, or signing access must use multi-factor
authentication. Release work is separated into author, reviewer, and approver
roles whenever the signing provider supports them. The author builds the exact
tagged source, the reviewer checks provenance and release evidence, and the
approver authorizes signing only after automated gates pass. No private signing
key may be committed, uploaded as a release asset, printed in logs, or placed in
an ordinary CI secret.

The current manifest key is `gamehq-prod-2026-01`. Its private seed is held in
Windows Credential Manager for controlled local release work and in the
review-protected GitHub `production-release` environment. Only its public key,
fingerprint, key ID, and minimum release sequence are versioned.

This key is persistent release infrastructure, not a per-release secret.
Releases reuse it until a planned rotation or a compromise requires replacement.
Missing private-key access stops the release; it never triggers automatic
regeneration or a fallback to test signing.

Current project roles:

- Author, committer, and reviewer: [underfusion](https://github.com/underfusion)
- Signing approver: [underfusion](https://github.com/underfusion)

External contributions require maintainer review before merge. Signing requests
require a separate explicit approval after the release evidence passes, even
when one maintainer currently fills multiple roles.

Free code signing provided by SignPath.io, certificate by SignPath Foundation.
This statement becomes applicable to distributed binaries only after the
project is accepted and the signing workflow is active.

Privacy statement: this program will not transfer any information to other
networked systems unless specifically requested by the user or the person
installing or operating it. A user may explicitly enable bounded GitHub release
checks; the complete behavior is described in
[Security & Privacy](security-and-privacy.md).

## Authenticode signing order (when available)

1. Build from the reviewed tag with pinned toolchains.
2. Generate and validate the mandatory corresponding-source archive and
   checksum for the exact tagged revision.
3. Sign GameHQ-built inner EXE/DLL files with the approved publisher and RFC
   3161 timestamp, then verify them.
4. Package portable, update, and full offline Setup artifacts.
5. Sign Setup and the generated Inno uninstaller, then verify every signature
   and timestamp again from the final artifacts.
6. Generate hashes and the byte-exact release manifest only after final bytes
   are fixed.

Third-party Qt, FFmpeg, compiler-runtime, and Playnite files keep their upstream
identity and are never re-signed as GameHQ. ZIP files do not carry ordinary
Authenticode; their authenticity comes from signed inner binaries and the
signed release manifest.

## Beta and Stable policy

Stable releases currently use `unsigned-stable` packaging: the production
Ed25519 release manifest and its signature, SHA-256 hashes, exact corresponding
source, and package validation are mandatory. Windows binaries are not
Authenticode-signed; the release must disclose that Windows may show Unknown
Publisher or SmartScreen warnings and must not claim code-signed binaries.
Beta releases retain the `unsigned-beta` mode and explicit Beta labelling.

Authenticode/SignPath is a preferred future trust layer when a certificate or
approved production workflow becomes available, not a blocker for the current
stable release. Releases advertised as Authenticode-signed must have the
expected publisher, valid RFC 3161 timestamps, a signed uninstaller, verified
final bytes, and the clean-Windows trust matrix. SmartScreen reputation is not
represented as a cryptographic guarantee.

Signing-provider enrollment and production-key activation are separately
reviewed operations. Revoked publisher certificates or Ed25519 key IDs stop
release publication until the trust set and recovery evidence are reviewed.
