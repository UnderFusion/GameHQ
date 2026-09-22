# GameHQ 0.7.8 (2026-09-22)

> This document is the original **English (en-US)** release note. No reviewed Chinese (Traditional) translation exists for this version, so the complete English text is published unchanged.

## Controller & Input

- Improved controller identification and input routing when multiple devices or input providers are present, with more conservative handling of ambiguous device matches.
- Standardized trigger, stick-click and other controller button handling across supported input providers so bindings behave more consistently.
- Improved provider switching, reconnect cleanup and held-button tracking to reduce missed actions, duplicate actions and stuck input.
- Fixed missed PS-button presses and delayed duplicate presses that could reopen the overlay immediately after closing it.
- GameInput can continue handling ordinary controller input and overlay input isolation when optional Guide/Share support is unavailable, while existing button fallbacks remain usable.
- The controller selected for editing in Settings stays selected when another controller or provider becomes active.

## Mapping Presets

- Added a mapping preset library with create, rename, duplicate, edit and delete controls alongside controller bindings.
- Presets can be assigned to controllers and games, with explicit fallback choices and automatic selection for the running game.
- Existing custom bindings are migrated into the preset system while preserving the original binding data for recovery.
- Preset changes account for held controls and pending gestures, reducing accidental actions when switching mappings.
- The interface distinguishes the assigned preset from the preset being edited, protects unfinished edits, and offers a separate copy for one controller.
- Deleting an assigned preset requires a valid replacement or fallback, preventing broken assignments; shared preset changes are made clearer.

## Overlay

- The overlay can take controller focus while remaining above a visible borderless game. Native wired DualSense behavior has been confirmed in Indiana Jones and the Great Circle in Borderless mode.
- On supported GameInput paths, navigating the overlay no longer simultaneously controls the game underneath.
- Closing the overlay briefly waits for held controls to return to neutral before handing input back to the game, with a bounded wait if a control stays held.
- Improved focus return and repeated open/close behavior, while respecting Alt-Tab and other deliberate switches to another application.
- Improved handling of game window replacement, disappearance and temporary foreground changes, keeping the overlay tied to the correct game.
- Bottom control hints now have a compact background that follows overlay dimming, and the currently running game highlights correctly during sidebar navigation.
- Improved controller navigation across overlay menus, gallery and video playback, including clearer separation of navigation and capture shortcuts.

## Capture & Replay

- Replay saves use the footage available so far, even before the configured buffer duration has elapsed or the first normal recording segment has finished.
- Rapid saves receive unique filenames instead of overwriting an existing clip; failed exports preserve earlier clips and thumbnails stay paired with the correct file.
- Replay exports retain the footage they need across game changes and buffer restarts, and GameHQ waits for an active export to finish safely on shutdown.
- Replay status now reflects actual capture startup and usable footage, with clearer feedback for startup, empty-buffer and busy-export conditions.
- Manual replay sessions survive recording-setting changes and overlapping HDR screenshots; an armed but unused manual session expires after an idle period.
- Screenshot and replay requests receive prompt acknowledgement followed by a clear saved or failed result. Notifications update in place and the visible stack is limited.
- Failed or skipped captures explain why, even when success notifications are disabled. Files saved to disk but not added to the library are reported accurately.
- Replay thumbnail work no longer blocks the capture worker during a save, and concurrent screenshots more reliably create their destination folder.
- Added a Windows capture-border preference with clearer permission and support reporting. Recording remains available when Windows cannot hide the border.

## UI, Settings & Sound

- The app restores the last normal page, Settings category and gallery filter; the overlay remembers its last category separately for each game.
- Opening the gallery preserves its saved filter, invalid saved game filters fall back safely, and Settings categories remain stable when their order changes.
- Window placement now supports monitors left of or above the primary display and brings fully off-screen windows back onto a connected display.
- Added independent interface and overlay scaling from 100% to 200%, remembered across restarts, with improved small-window layout handling.
- Fixed blank content and stacking problems during interface scaling, and improved menu visibility above the main content.
- Capture confirmations are louder and more distinct, with a separate capture volume and sound preview. Interface and capture sound controls now support levels up to 300%.
- Changing feedback, sound, notification, capture-border or manual-idle settings no longer unnecessarily discards the replay buffer.
- Binding controls show the effective hold duration and explain how hold gestures are recognized.

## Localization & Updates

- Expanded translations for capture feedback, border controls, sound settings, hold guidance, mapping presets and focus-related messages across the supported interface languages.
- Available-update notes now follow the selected language, use an explicit English fallback when needed, and can restore previously fetched notes while offline.
- Added an optional GitHub release link beneath upcoming update notes, accessible through controller navigation.
- Improved release-note loading, safe text formatting and handling of stale language requests; displayed notes remain separate from update installation authorization.

## Diagnostics & Reliability

- Capture diagnostics follow each request through acceptance, export and completion, helping distinguish an unrecognized shortcut from a rejected save.
- Copied diagnostics include the active controller bindings, preset selection, input-provider transitions and overlay focus/input state, with sensitive device identifiers sanitized.
- Sound availability is checked when effects actually load, with a clear warning if an effect cannot be played.

## Known Limitations

- Controller isolation depends on the game and input path. Native wired DualSense/GameInput has strong practical evidence, but universal isolation is not claimed for XInput, Raw Input, direct HID, Steam Input or virtual-controller configurations.
- DSX provider switching remains partially verified. Existing DSX setups remain user-managed; this release does not install or manage virtual-controller or device-hiding drivers.
- Games may pause or react to losing focus, and Windows or another capture application may keep the recording border visible.
- Controller recovery after forcibly terminating GameHQ during exclusive overlay input was not verified; normal overlay close and input return were tested.
- The complete 0.7.8 release notes currently use an explicit English fallback in other interface languages.
