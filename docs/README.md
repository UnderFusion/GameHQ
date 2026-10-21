# GameHQ Documentation

Technical and product documentation for contributors and maintainers.

## Start here

- [0.7.8 owner evidence and DSX switching retest](testing/dsx-switching-0.7.8.md).

- [Getting Started](getting-started.md) — screenshots, replay, overlay, bindings, and controller probe.
- [Controller Compatibility](controller-compatibility.md) — providers, Share/View, extras, reconnects, and reports.

- [Development Setup](dev-setup.md) — toolchain, configure, and build steps.
- [Architecture](architecture.md) — modules, data flow, and repository layout.
- [Product Specification](product-spec.md) — intended behavior and user flows.
- [Packaging & Distribution](packaging.md) — clean portable release layout.
- [Security & Privacy](security-and-privacy.md) — local data, network behavior, elevation, and reporting.
- [Download Verification](download-verification.md) — official artifacts, hashes, signatures, and Windows warnings.
- [Troubleshooting](troubleshooting.md) — Windows trust, installer conflicts, and HidHide guidance.
- [Licensing](licensing.md) — GPL core scope, MIT exceptions, corresponding source, and branding.
- [Dependency Licenses](dependency-licenses.md) — shipped-component and asset provenance audit.
- [Privacy Data Flows](privacy-data-flow.md) — current endpoints and gates for future AI/cloud features.
- [Maintainer Privacy](maintainer-privacy.md) — public identity boundary and private SignPath workflow.

## Subsystems

| Document | Topic |
|---|---|
| [branding.md](branding.md) | Product identity and naming |
| [capture-engine.md](capture-engine.md) | Screenshots and Windows Graphics Capture |
| [controller-input.md](controller-input.md) | Controllers, keyboard, mouse, gestures, and bindings |
| [database.md](database.md) | SQLite schema and migrations |
| [design-system.md](design-system.md) | Visual tokens, components, focus, and motion |
| [integration-protocol.md](integration-protocol.md) | Local `GameHQ.Local.v1` pipe used by the Playnite plugin and updater signals |
| [mapping-presets.md](mapping-presets.md) | Named mapping presets semantics contract |
| [notifications.md](notifications.md) | In-app toast notifications |
| [overlay.md](overlay.md) | In-game overlay behavior and constraints |
| [portable-import-inventory.md](portable-import-inventory.md) | Portable-to-installed profile import path rewrite contract |
| [replay-buffer.md](replay-buffer.md) | Rolling capture and MP4 export |
| [share-platform.md](share-platform.md) | Sharing saved captures: Share flow, providers, and safety rules |
| [share-provider-api-v1.md](share-provider-api-v1.md) | Contract for external Share destination providers |
| [sound-system.md](sound-system.md) | Sound events and settings |
| [storage.md](storage.md) | Portable paths, capture folders, and file layout |
| [tdlib-runtime.md](tdlib-runtime.md) | Optional TDLib runtime for the dormant Telegram Integrated provider |
| [threading-and-lifecycle.md](threading-and-lifecycle.md) | Threads, shutdown order, file deletion, crash recovery, error conventions |
| [updater.md](updater.md) | Update check, download, verification, and safe install |

## Project maintenance

| Document | Topic |
|---|---|
| [roadmap.md](roadmap.md) | Product direction |
| [technical-audit.md](technical-audit.md) | Architecture risks and improvement areas |
| [versioning.md](versioning.md) | Version and release rules |
| [share-smoke-check.md](share-smoke-check.md) | Short Share owner check for the current beta |
| [share-acceptance-full-rc.md](share-acceptance-full-rc.md) | Full Share acceptance matrix for the release candidate |
| [share-discord-social-sdk-feasibility.md](share-discord-social-sdk-feasibility.md) | Discord Social SDK contact sharing feasibility research |
| [Beta 4 notes](releases/0.7.9-beta4.md) | External controller testing prerelease |
| [0.7.10 Beta 1 notes](releases/0.7.10-beta1.md) | Xbox mode with Playnite and screenshot notification prerelease |
| [0.7.10 Beta 2 notes](releases/0.7.10-beta2.md) | Controller shortcuts in non-Steam games prerelease |
| [0.8.0 Beta 1 notes](releases/0.8.0-beta1.md) | Clip audio repair and cumulative changes since stable 0.7.9 |
| [code-signing-policy.md](code-signing-policy.md) | signing roles, key handling, and Beta/Stable gates |
| [release-manifest-security-review.md](release-manifest-security-review.md) | approved Ed25519 dependencies, verification contract, and trust boundary |
| [signpath-application.md](signpath-application.md) | prepared open-source signing application evidence |
| [licensing-audit.md](licensing-audit.md) | authorship and contribution-authority evidence |

When implementation changes behavior, update the relevant document in the same
pull request.
