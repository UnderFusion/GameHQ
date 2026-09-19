# Overlay Design

> GameHQ is primarily an in-game overlay, without injection into game processes.

## Behavior contract

- PS, `Ctrl+Shift+G`, or the Share double-tap fallback toggles the overlay.
- Opening remembers the foreground game and shows a frameless, topmost tool window on its monitor without activating it.
- The game keeps OS focus. GameHQ routes its controller events by overlay visibility, independently of keyboard focus. This does not block the game from receiving the same controller input; the existing focus warning remains visible.
- Circle goes back in submenus/viewers and closes from the gallery. Closing only hides the overlay; it never acquires or restores foreground.
- Foreground events for the remembered game or overlay leave it open. A current foreground event for another window (Alt-Tab, Start, another application) dismisses it. Queued events are ignored when their HWND no longer matches `GetForegroundWindow()`.
- Keyboard navigation and Escape are unavailable while the game retains focus. Use the global toggle hotkey or controller to close. Mouse clicks are intended to interact without activation; hardware verification remains required.
- The first sidebar tab is `Game`, filtered to captures for the foreground game, followed by `Game Favourites` when available. Category selection is remembered per game (`ui.overlay_filter.<gameId>`); the game binding always follows the foreground game.

## Windows implementation

`OverlayManager` uses `Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus`. Qt's Windows tool-window show path uses `SW_SHOWNOACTIVATE`; its no-focus flag handles mouse activation with `MA_NOACTIVATE`.

Every production path that shows, positions or raises the overlay goes through one primitive, `OverlayPresenter` (`src/overlay/OverlayPresenter.{h,cpp}`), so no call site re-derives the guarantee. It applies two independent protections on every presentation:

- `WS_EX_NOACTIVATE` on the **current** native handle, written before the window is moved, shown or raised. Qt does not translate `Qt::WindowDoesNotAcceptFocus` into this ex-style, and without it Windows may activate the overlay on its own — it promotes the next topmost window when the foreground one hides or minimizes.
- `SWP_NOACTIVATE` on every native positioning and z-order call, including the `HWND_TOPMOST` pin after the window becomes visible.

Neither protection replaces the other. Qt rebuilds the native window on some flag, geometry and screen transitions, and a rebuilt window starts without the ex-style, so the presenter re-resolves the handle after each step and re-applies it; `OverlayPresenter::reassert()` does the same after a `QWindow::screenChanged`. Each presentation records the foreground window before and after and logs it, so the claim is measured rather than assumed. `tests/tst_overlaypresenter.cpp` covers initial show, repeated show, reposition, screen change, handle recreation and a stripped ex-style through a fake Win32 seam, plus one real-window case on the production adapter.

The overlay has no `ForegroundAcquirer`, `requestActivate`, `AttachThreadInput`, or foreground retry path. The separate desktop window still uses its existing acquisition mechanism.

Sources: [Qt Windows show implementation](https://github.com/qt/qtbase/blob/6.8/src/plugins/platforms/windows/qwindowswindow.cpp), [Qt activation handling](https://github.com/qt/qtbase/blob/6.8/src/plugins/platforms/windows/qwindowscontext.cpp), [Microsoft mouse activation contract](https://learn.microsoft.com/en-us/windows/win32/inputdev/wm-mouseactivate).

This local Khazan compatibility change still requires verification in a rebuilt executable. Borderless/windowed modes are the intended targets; exclusive fullscreen is not guaranteed. It does not establish a fix for the original reported freeze. Controller isolation remains a separate, unimplemented design (`docs/design/exclusive-controller-mode.md`).

## QML structure

- `OverlayWindow.qml` owns overlay state, keyboard/controller routing, preview playback state, and top-level layout.
- `components/OverlaySidebar.qml` renders category/game navigation and a
  centered brand lockup matching the desktop sidebar's icon and typography,
  with the running app version printed underneath it.
- `components/OverlayCaptureStrip.qml` renders the horizontal capture strip and L1/R1 or arrow hint pills. While video focus is inactive, controller/keyboard left-right navigation can browse captures; once Cross enters video focus, left-right seeks the clip and L1/R1 remains the capture-switch path. Each tile's hover icons are also mouse-clickable: heart toggles favourite, folder reveals the capture on disk, and trash opens `OverlayWindow.qml`'s "Delete capture?" confirmation — the strip only moves selection to the clicked tile and forwards the action, `OverlayWindow` owns the actual delete/folder/favourite calls.
- `components/OverlayPreview.qml` renders the large selected-capture preview. It reacts to overlay gallery model resets/row insert-remove-move/data changes as well as index changes, so a newly saved screenshot or clip at row 0 updates the preview immediately when the overlay opens. Because `galleryModel.get()` is an imperative call that no model signal re-evaluates on its own, *every* binding that resolves a record — both the target URL and the displayed record that drives play/badge decisions — must read the `_modelRevision` counter. Omitting it on the record binding is what made a just-saved clip refuse to play on Cross and a just-saved screenshot inherit the previous clip's play badge (fixed in 0.6.2). The still/clip surface itself is the shared `components/MediaStage.qml`; the preview supplies the overlay's own rules for it (a clip decodes its thumbnail, which keeps painting until `videoFocused` hands the stage to the video surface) and tracks the committed frame against the requested one via the stage's `committed`/`cleared` signals.
- `components/OverlayFooter.qml` renders contextual footer hints.
- `components/OverlayActionMenu.qml` renders per-capture actions and is also reused by the desktop pad action menu.

## Known risks

Focus-restoration flakiness · Steam Input remapping the pad while overlay focused · games reading input via Raw Input regardless of focus (log + document per game) · multi-monitor placement (show on the game's monitor).
