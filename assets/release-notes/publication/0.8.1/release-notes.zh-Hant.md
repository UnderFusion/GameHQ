# GameHQ 0.8.1 (2026-10-29)

> This document is the original **English (en-US)** release note. No reviewed Chinese (Traditional) translation exists for this version, so the complete English text is published unchanged.

## Fixes

- GameHQ opens normally after an automatic update, including later restarts. Completed update records no longer send startup into an unnecessary recovery loop.
- If Windows temporarily prevents old update files from being cleaned up, GameHQ keeps the completion record and retries after a later successful start.
- A separate controller setting lets you turn off Back/View acting as a capture button for the selected controller source, while keeping native Share shortcuts and other controllers unchanged.

## If GameHQ will not open

- If an automatic update left GameHQ 0.8.0 unable to start, manually run the GameHQ 0.8.1 installer over your existing installation. Keep your settings, library and captures; do not uninstall GameHQ or delete its data folder.

## Controller compatibility

- The DualSense + DS4Windows + HidHide scenario still needs real-device confirmation. Please test the Back/View setting and native Share separately and report your results in issue #32.
