# Controller compatibility

## Provider model

GameHQ combines providers per physical controller and capability. Sony Raw Input is preferred for Sony standard controls; GameInput provides true System Share, Guide, and extra controls; XInput and WinMM remain fallbacks. GameInput standard-control readings are currently shadow-only (diagnostics): legacy providers do not yet feed the shared controller registry, so routing them would double-fire one physical press. One physical edge is routed once.

Identity is merged only from strong OS or app-local identifiers, or an unambiguous correlated topology. VID/PID and display names alone never merge two controllers. Strong reconnects restore the same profile; weak/new identities remain separate and can be copied or linked explicitly.

## Share, View, and Guide

- **System Share / Capture** is a dedicated system control reported by GameInput or Sony input.
- **View / Back** is the legacy XInput control and is independently bindable.
- **Guide / Home** may be intercepted by Steam, Xbox Game Bar, DSX, or similar software.

An unknown extra button is labeled **Extra Button N**. Its persisted ID includes the anonymous logical controller and an ordered layout signature. Firmware or mode changes that alter the layout require assignment reconfirmation; GameHQ never guesses that an extra button means Capture.

## Probe and compatibility report

Run **Settings → Input → Start 3-second probe**, press the dedicated button once, and copy the compatibility report. The probe observes GameInput, selective Raw HID, and keyboard macros. If nothing arrives, try another controller mode or close software that may intercept the button.

The report includes device name, anonymous ID, VID/PID, providers, runtime status, Share/Guide availability, extra-button count, reconnect/layout state, and probe observations. It excludes serial numbers, usernames, and full PnP paths.

## Wireless and reconnects

USB, Bluetooth, and 2.4 GHz receivers use the same lifecycle. Disconnect, receiver removal, and uncertain state synthesize safe releases and cancel tap/hold/chord and navigation-repeat state. GameInput reports no distinct sleep/wake signal: a controller going to sleep is observed as a normal disconnect and its wake as a reconnect. Strong reconnects restore the profile. A changed firmware mode may legitimately produce a new or reconfirmation-required layout.

## Gestures

Tap + Hold and different exact tap counts can share a control. Press executes on button-down and cannot share that control with timed gestures unless the editor explicitly converts it to Single tap. Multi-tap and combination delays always use the live timing shown in Settings.

GameHQ cannot universally prevent games from receiving the controller while its overlay is open. What it does instead is bounded and measurable: while the overlay is interactive it asks GameInput for the exclusive-foreground policy (`cpo-o06c`), waits for the pad to be neutral before releasing it (`cpo-o06e`), and states its own position per open as `isolation=unavailable`, `isolation=scoped` or `isolation=full-native` (`cpo-o06f`). Which cases are externally verified and which input paths are explicitly not measured is `docs/overlay.md` ("Controller isolation: what is verified, and what is not"); the device-cloaking alternative that was considered and rejected is `docs/design/exclusive-controller-mode.md`.
