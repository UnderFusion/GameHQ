# GameHQ 0.7.4 (2026-08-07)

> This document is the original **English (en-US)** release note. No reviewed Spanish translation exists for this version, so the complete English text is published unchanged.

## What's new

- Fixed HDR screenshots on HDR-enabled displays. HDR tone mapping was already implemented, but an internal feature gate remained disabled by default in public builds, causing screenshots to fall back to the SDR capture path and appear overexposed.
- Fixed occasional double controller navigation caused by the same physical press being mirrored through multiple Windows controller APIs.
- Improved non-exclusive background controller input delivery while games have focus, including Guide/Share delivery where the controller and firmware expose those buttons.
- Improved diagnostics for controller buttons exposed as keyboard macros or disabled by firmware.

## Notes

- Share/Capture support remains hardware-dependent and is still unverified on GameSir G7 Pro. No controller-model compatibility claim is made by this release.
