# Notifications (in-app toasts)

App-wide, reusable toast notifications styled from [design-system.md](design-system.md). Introduced dev.14.

## Behaviour

A toast slides in at the **bottom-right** of the monitor the active app (usually the game) is on, holds for ~2.6 s, then fades out and removes itself. Multiple toasts stack vertically (newest at the bottom, nearest the corner). The host window hides automatically when the stack empties.

The toast window is **frameless, topmost, click-through and non-activating** — posting a toast never pulls focus away from a running game, and clicks pass straight through to whatever is underneath. (Like the overlay, it renders over borderless/windowed games; exclusive-fullscreen is a WGC-era concern.)

## Architecture

- `notify/NotificationCenter` (C++, context property **`notifications`**) — lazy-loads `ToastWindow.qml`, sets the window flags (`FramelessWindowHint | WindowStaysOnTopHint | Tool | WindowDoesNotAcceptFocus | WindowTransparentForInput`), positions it against the work-area corner of the active app's screen, shows it without activating (`SW_SHOWNOACTIVATE`), and emits `posted(title, body, imageUrl, kind)`.
- `ui/qml/ToastWindow.qml` (objectName `gamehqToasts`) — a transparent `Window` holding a `ListModel` + bottom-anchored `Column`/`Repeater`. Appends on `notifications.posted`; each card removes itself via the root `dismissToast(idx)` function; calls `notifications.hideWindow()` when empty.
- `ui/qml/components/Toast.qml` — the card: a `Theme.surface` rounded rectangle with a left **accent bar** coloured by `kind` (`success`→`Theme.success`, `error`→`Theme.danger`, else `Theme.accent`), an optional 16:9 thumbnail, a title (`fontH3`, DemiBold) and body (`fontCaption`, muted). Slide-in + fade on entry, fade on exit, `lifespan` timer (default 2600 ms).

## Posting one

```cpp
notifications->post(title, body, imagePath /* "" = text only */, kind /* success|info|error */);
```

Any subsystem can post. Currently wired:

- **screenshot saved** → `post("Screenshot saved", <game>, <png path>, "success")` (replaces the old OS tray balloon; gated by `notifications.enabled`, default true).
- **replay saved / replay failed** → `post(..., "success" | "error")`, gated by `notifications.enabled` plus the replay-specific keys.
- **capture delete failed** → `AppController::captureDeletionFailed(count)` → `post("Couldn't delete", <one-file or several-files body>, "", "error")`. `CaptureLibraryService` keeps the library row when the media file cannot be removed, so without this toast the delete looks like it silently did nothing. The error sound plays even when notifications are disabled.

> QML note: a `Repeater` delegate that is a separate component type cannot reliably resolve a *sibling* `id` (e.g. the `ListModel`) from inside its signal handlers under the QML AOT cache — route such access through a function on the **root** object instead (the root `id` always resolves).
