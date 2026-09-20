# Overlay Design

> GameHQ is primarily an in-game overlay, without injection into game processes.

## Behavior contract

- PS, `Ctrl+Shift+G`, or the Share double-tap fallback toggles the overlay.
- Opening remembers the foreground game — its window **and its process id** — and shows a frameless, topmost tool window on the game's monitor without activating it.
- The game keeps OS focus. GameHQ routes its controller events by overlay visibility, independently of keyboard focus. This does not block the game from receiving the same controller input; the existing focus warning remains visible. GameHQ never suppresses or injects input — the game keeps seeing every button — and the rules below decide only what *GameHQ* fires for a press.
- **Input delivery (one press, one GameHQ action).** The arbitration is pure and shared: `src/input/OverlayInputPolicy.{h,cpp}` decides the active scope, `BindingResolver::matching()` and `InputEngine` are its only callers, and `tests/tst_overlayinput.cpp` audits the shipped table (`src/input/DefaultBindings.cpp`). While the overlay is visible the primary scope is Overlay: PS tap / `Ctrl+Shift+G` → `global.toggle_overlay`; Circle → `overlay.back`; Cross → `overlay.confirm`; D-pad and the left stick → `overlay.navigate_*`; Square/Triangle/Options → `overlay.menu` / `overlay.favorite` / `overlay.sidebar_toggle`; L1/R1 → `overlay.game_prev` / `overlay.game_next` (the only capture switch). Pad or arrow-key left/right is seek-only: it never flips the strip and is a no-op unless a clip is focused. Every `desktop.*` and `playback.*` binding is inert in that scope, so a shipped trigger+gesture never delivers two actions. Globals (screenshot, replay, the two toggles) stay live in every context and are dropped only by a substitution declared in `ContextOverrideCatalog` (the playback frame grab); a user-created second binding on the same trigger stays an editor-visible overlap, never a silent double delivery.
- Circle goes back in submenus/viewers and closes from the gallery. Closing only hides the overlay; it never acquires or restores foreground. Escape routes with the game holding focus: PS tap, Circle, the Share double-tap and the `Ctrl+Shift+G` global hotkey. `Esc`/`Backspace` map to `overlay.back`, but the overlay window is created `WindowDoesNotAcceptFocus` — they only act where the overlay itself has keyboard focus, so in-game the pad and the global hotkey are the routes.
- Overlay interaction stays with the game GameHQ remembered: opening pins the context (`AppController::syncOverlayToForegroundGame` on `aboutToShow`), and routing follows the overlay's own visibility — it never re-reads the OS foreground, so L1/R1 browsing of other games is explicit user intent, not context loss.
- Foreground events are resolved by one pure policy (`src/overlay/OverlayLifetimePolicy.{h,cpp}`, tested in `tests/tst_overlaylifetime.cpp`). The remembered game window, or the overlay itself, leaves the overlay open. Whether a same-process window counts as the game's **replacement window** (borderless/fullscreen toggle, resolution change, launcher hand-over) depends on the window we remember: only when that remembered handle is **gone** does a valid, visible, unminimized, unowned, top-level window of the same process take its place. While the remembered window is still a healthy visible main window, a second same-process top-level window (launcher surface, splash, tool window) never replaces it. A minimized or hidden game context hides the overlay even when the process is unchanged, and no other same-process window may keep the overlay alive through it. The desktop, the shell, the task switcher or any other process dismisses it. A queued event is acted on only while its HWND still matches `GetForegroundWindow()`: a stale event is dropped, while a current event that carries no foreground window at all resolves as an invalid foreground and hides the overlay.
- Process identity is evidence of continuity, not permission to ignore a lost context: an owned or child (auxiliary) same-process popup keeps the overlay open but never becomes the remembered game window. A rebind candidate is re-checked immediately before it is committed, so one that vanished or changed process in between cannot be remembered. A destroyed handle is never remembered, never drives overlay geometry, and is never handed back as the desktop window's return address.
- Keyboard navigation and Escape are unavailable while the game retains focus. Use the global toggle hotkey or controller to close. Mouse clicks are intended to interact without activation; hardware verification remains required.
- The first sidebar tab is `Game`, filtered to captures for the foreground game, followed by `Game Favourites` when available. Category selection is remembered per game (`ui.overlay_filter.<gameId>`); the game binding always follows the foreground game.

## Windows implementation

`OverlayManager` uses `Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus`. Qt's Windows tool-window show path uses `SW_SHOWNOACTIVATE`; its no-focus flag handles mouse activation with `MA_NOACTIVATE`.

Every production path that shows, positions or raises the overlay goes through one primitive, `OverlayPresenter` (`src/overlay/OverlayPresenter.{h,cpp}`), so no call site re-derives the guarantee. It applies two independent protections on every presentation:

- `WS_EX_NOACTIVATE` on the **current** native handle, written before the window is moved, shown or raised. Qt does not translate `Qt::WindowDoesNotAcceptFocus` into this ex-style, and without it Windows may activate the overlay on its own — it promotes the next topmost window when the foreground one hides or minimizes.
- `SWP_NOACTIVATE` on every native positioning and z-order call, including the `HWND_TOPMOST` pin after the window becomes visible.

Neither protection replaces the other. Qt rebuilds the native window on some flag, geometry and screen transitions, and a rebuilt window starts without the ex-style, so the presenter re-resolves the handle after each step and re-applies it; `OverlayPresenter::reassert()` does the same after a `QWindow::screenChanged`. Each presentation records the foreground window before and after and logs it, so the claim is measured rather than assumed. `tests/tst_overlaypresenter.cpp` covers initial show, repeated show, reposition, screen change, handle recreation and a stripped ex-style through a fake Win32 seam, plus one real-window case on the production adapter.

The lifetime decision is pure and separate: `OverlayManager` only resolves the Win32 facts (`IsWindow`, `IsWindowVisible`, `IsIconic`, `GetAncestor(GA_ROOT)`, owner, process id) — for the foreground candidate and for the window it currently remembers — and applies the decision through this same presenter — a rebind re-asserts, and a replacement window on another monitor is followed with a presentation, so no second show path exists next to the presenter. The overlay's action menu and the delete confirmation are in-window QML items, not separate native windows; `tests/tst_overlaylifetime.cpp` fails if either ever grows a top-level window declaration, because that would need its own non-activation guarantee.

The overlay has no `ForegroundAcquirer`, `requestActivate`, `AttachThreadInput`, or foreground retry path. The separate desktop window still uses its existing acquisition mechanism.

Sources: [Qt Windows show implementation](https://github.com/qt/qtbase/blob/6.8/src/plugins/platforms/windows/qwindowswindow.cpp), [Qt activation handling](https://github.com/qt/qtbase/blob/6.8/src/plugins/platforms/windows/qwindowscontext.cpp), [Microsoft mouse activation contract](https://learn.microsoft.com/en-us/windows/win32/inputdev/wm-mouseactivate).

This local Khazan compatibility change still requires verification in a rebuilt executable. Borderless/windowed modes are the intended targets; exclusive fullscreen is not guaranteed. It does not establish a fix for the original reported freeze. Controller isolation remains a separate, unimplemented design (`docs/design/exclusive-controller-mode.md`).

## QML structure

- `OverlayWindow.qml` owns overlay state, keyboard/controller routing, preview playback state, and top-level layout.
- `components/OverlaySidebar.qml` renders category/game navigation and a
  centered brand lockup matching the desktop sidebar's icon and typography,
  with the running app version printed underneath it.
- `components/OverlayCaptureStrip.qml` renders the horizontal capture strip and L1/R1 or arrow hint pills. L1/R1 is the only control that switches captures: left/right on the D-pad, the left stick or the arrow keys never flips the strip — while video focus is inactive it is a deliberate no-op, and once Cross enters video focus it seeks the clip instead. With a keyboard inside the overlay there is no capture-switch route, so the strip edge pills are pad-only. Each tile's hover icons are also mouse-clickable: heart toggles favourite, folder reveals the capture on disk, and trash opens `OverlayWindow.qml`'s "Delete capture?" confirmation — the strip only moves selection to the clicked tile and forwards the action, `OverlayWindow` owns the actual delete/folder/favourite calls.
- `components/OverlayPreview.qml` renders the large selected-capture preview. It reacts to overlay gallery model resets/row insert-remove-move/data changes as well as index changes, so a newly saved screenshot or clip at row 0 updates the preview immediately when the overlay opens. Because `galleryModel.get()` is an imperative call that no model signal re-evaluates on its own, *every* binding that resolves a record — both the target URL and the displayed record that drives play/badge decisions — must read the `_modelRevision` counter. Omitting it on the record binding is what made a just-saved clip refuse to play on Cross and a just-saved screenshot inherit the previous clip's play badge (fixed in 0.6.2). The still/clip surface itself is the shared `components/MediaStage.qml`; the preview supplies the overlay's own rules for it (a clip decodes its thumbnail, which keeps painting until `videoFocused` hands the stage to the video surface) and tracks the committed frame against the requested one via the stage's `committed`/`cleared` signals.
- `components/OverlayFooter.qml` renders contextual footer hints.
- `components/OverlayActionMenu.qml` renders per-capture actions and is also reused by the desktop pad action menu.

## Known risks

Focus-restoration flakiness · Steam Input remapping the pad while overlay focused · games reading input via Raw Input regardless of focus (log + document per game) · multi-monitor placement (show on the game's monitor).

## Native acceptance (cpo-o05)

`tests/tst_overlaynative.cpp` drives the shipped overlay — `OverlayWindow.qml`, the
manager and the presenter, loaded through a test-local `GameHQ` QML module that
embeds the real files — against `tests/overlay_target_fixture.cpp`: a borderless
window in a separate process, driven through a registered window message. The
assertions are real Win32 facts (`GetForegroundWindow`, `GWL_EXSTYLE`,
`IsWindow`/`IsIconic`, posted input), not fakes:

- showing the overlay never moves the foreground away from the target — across
  repeated open/close cycles, the action menu, the delete confirmation, and a
  native-handle rebuild (`destroy()` + `show()`, the Qt path a flag or screen
  change uses);
- the overlay closes when the foreground genuinely leaves the game (another
  application activated, the target minimized, the target window destroyed) and
  stays open when the game replaces its own window in the same process;
- a destroyed game handle is never measured or followed again, and
  `WS_EX_NOACTIVATE` is restored on the rebuilt handle by the production show
  path;
- the target keeps processing its window messages while the overlay is open,
  and the overlay's input route (keyed on overlay visibility, not OS focus)
  still closes it — driven through the same signals the device layer emits.

It runs through `ctest -R tst_overlaynative --output-on-failure` and needs an
interactive session that can hold a foreground window; it briefly shows the real
full-screen overlay for each scenario. The cross-screen scenario needs two
monitors and skips itself with that reason on a single-screen machine (the
rebind-and-reposition path it would exercise is decided by
`overlay/OverlayLifetimePolicy` and covered by `tst_overlaylifetime`). Physical
gamepad delivery is not part of this harness: it needs a real device and stays
with controller acceptance.
