## What's new

- Summarize the user-visible improvements in this release.

## Fixed

- List important fixes, or write “No user-visible fixes.”

## Notes

- Trust mode: `unsigned-beta` or `signed` (must match release evidence).
- Unsigned Beta wording: **Open-source Beta · Not yet code-signed; Windows may show an Unknown publisher warning.**
- Never describe an unsigned release as Stable.
- Setup is the recommended download; Portable is the explicit no-install alternative.
- State that the GameHQ core uses GPL-3.0-only while the Playnite integration
  and public protocol remain under MIT.
- Link the mandatory `GameHQ-<version>-source.zip` and its checksum.
- Link [Download verification](https://github.com/underfusion/GameHQ/blob/main/docs/download-verification.md).
- Link the [Code signing policy](https://github.com/underfusion/GameHQ/blob/main/docs/code-signing-policy.md)
  using that exact term; never imply SignPath enrollment or signatures before they are active.

## GitHub Release assets

- Upload installable packages and verification metadata only: Setup, Portable,
  the update payload, `GameHQ-<version>-source.zip`, checksums, the release
  manifest and its signature, and the release evidence.
- **Never upload one release-note asset per locale.** `release-notes.<locale>.md`
  files are internal generated publication inputs, not GitHub Release assets.
- The localized notes stay in the repository; the release body links them once
  as "Release notes in 16 languages". Full policy: `docs/packaging.md` >
  GitHub Release asset policy.
