# GameHQ 0.7.8 (2026-09-22)

> This document is the original **English (en-US)** release note. No reviewed Russian translation exists for this version, so the complete English text is published unchanged.

## Added

- Screenshot and replay requests now receive prompt visual feedback, with clear completion or failure messages and capture sound controls.
- Controller mapping presets support per-device and per-game assignments, safer editing and clearer assignment controls.

## Changed

- The overlay can take foreground above a visible game, request GameInput exclusive input, and wait briefly for controller neutral state before handing input back. Owner testing confirms native wired DualSense behavior; DSX switching remains partial and needs verification.
- The application restores the last page, settings category, gallery filter and reachable window position, including monitors with negative coordinates.
- Bottom control hints have a compact background following overlay dimming, and the running game receives the normal sidebar selection highlight.
- Provider lifecycle diagnostics record sanitized controller identities, runtime failures, overlay state and held-control counts to investigate DSX switching without changing input behavior.

## Fixed

- An unavailable GameInput Guide/Share callback no longer disables ordinary readings or exclusive-focus policy; existing system-button providers remain eligible.
- Replay exports reserve collision-safe filenames and retain their source segments across game changes and shutdown; failed exports preserve existing clips.
- Replay buffer readiness and manual/HDR ownership now follow explicit lifecycle states, with bounded idle expiry.
- Windows capture-border controls report system support and permission outcomes honestly while preserving recording when hiding is unavailable.
- Controller identity, provider arbitration, gesture lifetime, preset changes and disconnect cleanup have focused regression coverage.
