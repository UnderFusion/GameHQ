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
| [notifications.md](notifications.md) | In-app toast notifications |
| [overlay.md](overlay.md) | In-game overlay behavior and constraints |
| [replay-buffer.md](replay-buffer.md) | Rolling capture and MP4 export |
| [sound-system.md](sound-system.md) | Sound events and settings |
| [storage.md](storage.md) | Portable paths, watched folders, and cleanup |

## Project maintenance

| Document | Topic |
|---|---|
| [roadmap.md](roadmap.md) | Product direction |
| [technical-audit.md](technical-audit.md) | Architecture risks and improvement areas |
| [versioning.md](versioning.md) | Version and release rules |
| [code-signing-policy.md](code-signing-policy.md) | signing roles, key handling, and Beta/Stable gates |
| [release-manifest-security-review.md](release-manifest-security-review.md) | approved Ed25519 dependencies, verification contract, and trust boundary |
| [signpath-application.md](signpath-application.md) | prepared open-source signing application evidence |
| [licensing-audit.md](licensing-audit.md) | authorship and contribution-authority evidence |

When implementation changes behavior, update the relevant document in the same
pull request.
