# GameHQ 0.8.1 (2026-10-29)

## Fixes

- GameHQ opens normally after an automatic update, including later restarts. Completed update records no longer send startup into an unnecessary recovery loop.
- If Windows temporarily prevents old update files from being cleaned up, GameHQ keeps the completion record and retries after a later successful start.
- A separate controller setting lets you turn off Back/View acting as a capture button for the selected controller source, while keeping native Share shortcuts and other controllers unchanged.

## Update settings

- Include beta versions is now available directly in the About/What’s New popup, synchronized with Settings. Switching channels refreshes available updates; disabling betas keeps an installed beta until an eligible stable release is available, without downgrading.
- The automatic-check description now matches the existing behavior: GameHQ checks shortly after startup and hourly in the background.

## If GameHQ will not open

- If an automatic update left GameHQ 0.8.0 unable to start, manually run the GameHQ 0.8.1 installer over your existing installation. Keep your settings, library and captures; do not uninstall GameHQ or delete its data folder.

## Controller compatibility

- The DualSense + DS4Windows + HidHide scenario still needs real-device confirmation. Please test the Back/View setting and native Share separately and report your results in issue #32.
