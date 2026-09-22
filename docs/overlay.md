# Overlay Design

Owner acceptance update (2026-09-22): native wired DualSense visibility,
navigation isolation, held-input close, recovery, repeated cycles and Alt-Tab
behavior passed as reported by the owner. Game names/build hash and unrelated
preset/capture cases were not supplied. DSX switching remains partial. The
abnormal-kill experiment was skipped by the owner with residual risk accepted,
not passed. See [the evidence and focused DSX retest](testing/dsx-switching-0.7.8.md).

> GameHQ is primarily an in-game overlay, without injection into game processes.

## Behavior contract

- A target that is topmost when opened becomes the temporary owner of GameHQ's
  overlay popup. This keeps a borderless game that drops topmost on focus loss
  directly beneath the overlay instead of below unrelated applications. Only
  the overlay's owner field is written; the game is not reparented or restyled.
  Ownership follows a validated replacement game window and is cleared on
  close. If owner demotion also removes the overlay's topmost flag, GameHQ
  repairs its own window once during the opening probe without taking focus.
- PS, `Ctrl+Shift+G`, or the Share double-tap fallback toggles the overlay.
- Opening remembers the foreground game — its window **and its process id** — and shows a frameless, topmost tool window on the game's monitor without activating it.
- **The overlay then asks for the foreground (`cpo-o06b`).** Presentation stays non-activating; one explicit, bounded request (1 attempt + 2 retries, never a timer or frame loop) follows it, so the overlay becomes the active window and the keyboard and controller drive the overlay. The game window is never minimised, restored or restyled, and nothing is injected into it. A game that takes the foreground back keeps it — GameHQ does not fight for it. When Windows denies the request the overlay stays exactly as presentation left it: visible, topmost and non-activating, with the in-overlay warning still up. Closing hands the foreground back to the remembered game, but only when the overlay is the window still holding it.
- Owning the foreground is **not** isolation by itself, and GameHQ never suppresses or injects input of its own. What narrows the pad is a second, separate step (`cpo-o06c`): while the overlay holds the *verified interactive* foreground, GameHQ asks GameInput for the exclusive-foreground policy, and `cpo-o06e` waits for the pad to be physically neutral before that policy is released. A game that reads the pad through the same GameInput mechanism stops receiving it for that interval; a game that reads through another input path can still be receiving it, and the rules below decide only what *GameHQ* fires for a press. What is verified, what is not, and how the one-line record states its own position (`isolation=<level>`) is written down in **Controller isolation: what is verified, and what is not** below. The in-overlay focus warning is driven by the acquirer's verified result, so it disappears only when the overlay really did take the foreground.
- **Input delivery (one press, one GameHQ action).** The arbitration is pure and shared: `src/input/OverlayInputPolicy.{h,cpp}` decides the active scope, `BindingResolver::matching()` and `InputEngine` are its only callers, and `tests/tst_overlayinput.cpp` audits the shipped table (`src/input/DefaultBindings.cpp`). While the overlay is visible the primary scope is Overlay: PS tap / `Ctrl+Shift+G` → `global.toggle_overlay`; Circle → `overlay.back`; Cross → `overlay.confirm`; D-pad and the left stick → `overlay.navigate_*`; Square/Triangle/Options → `overlay.menu` / `overlay.favorite` / `overlay.sidebar_toggle`; L1/R1 → `overlay.game_prev` / `overlay.game_next` (the only capture switch). Pad or arrow-key left/right is seek-only: it never flips the strip and is a no-op unless a clip is focused. Every `desktop.*` and `playback.*` binding is inert in that scope, so a shipped trigger+gesture never delivers two actions. Globals (screenshot, replay, the two toggles) stay live in every context and are dropped only by a substitution declared in `ContextOverrideCatalog` (the playback frame grab); a user-created second binding on the same trigger stays an editor-visible overlap, never a silent double delivery.
- Circle goes back in submenus/viewers and closes from the gallery. Closing hides the overlay and, when the overlay holds the foreground, hands it back to the remembered game; if anything else already owns the foreground the user has moved on and GameHQ does not pull it back. Escape routes: PS tap, Circle, the Share double-tap and the `Ctrl+Shift+G` global hotkey. `Esc`/`Backspace` map to `overlay.back` and now work in-game as well whenever the foreground request succeeded — the overlay is a real active window at that point. They remain unavailable if the request was denied.
- Overlay interaction stays with the game GameHQ remembered: opening pins the context (`AppController::syncOverlayToForegroundGame` on `aboutToShow`), and routing follows the overlay's own visibility — it never re-reads the OS foreground, so L1/R1 browsing of other games is explicit user intent, not context loss.
- Foreground events are resolved by one pure policy (`src/overlay/OverlayLifetimePolicy.{h,cpp}`, tested in `tests/tst_overlaylifetime.cpp`). The remembered game window, or the overlay itself, leaves the overlay open. Whether a same-process window counts as the game's **replacement window** (borderless/fullscreen toggle, resolution change, launcher hand-over) depends on the window we remember: only when that remembered handle is **gone** does a valid, visible, unminimized, unowned, top-level window of the same process take its place. While the remembered window is still a healthy visible main window, a second same-process top-level window (launcher surface, splash, tool window) never replaces it. A minimized or hidden game context hides the overlay even when the process is unchanged, and no other same-process window may keep the overlay alive through it. The desktop, the shell, the task switcher or any other process dismisses it. A queued event is acted on only while its HWND still matches `GetForegroundWindow()`: a stale event is dropped, while a current event that carries no foreground window at all resolves as an invalid foreground and hides the overlay.
- Process identity is evidence of continuity, not permission to ignore a lost context: an owned or child (auxiliary) same-process popup keeps the overlay open but never becomes the remembered game window. A rebind candidate is re-checked immediately before it is committed, so one that vanished or changed process in between cannot be remembered. A destroyed handle is never remembered, never drives overlay geometry, and is never handed back as the desktop window's return address.
- Keyboard navigation and Escape work while the overlay owns the foreground, and are unavailable when the request was denied and the game kept it; the global toggle hotkey and the controller close the overlay either way.
- The first sidebar tab is `Game`, filtered to captures for the foreground game, followed by `Game Favourites` when available. Category selection is remembered per game (`ui.overlay_filter.<gameId>`); the game binding always follows the foreground game.

## Windows implementation

`OverlayManager` uses `Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint`. `Qt::WindowDoesNotAcceptFocus` was dropped in `cpo-o06b`: it would make Qt refuse keyboard focus even after Windows had handed the overlay the foreground, and the non-activating guarantee never depended on it — `WS_EX_NOACTIVATE` plus `SWP_NOACTIVATE` carry it, applied by the presenter on every present and reassert. `Qt::Tool` stays, so the overlay is out of the taskbar and the Alt-Tab list.

Every production path that shows, positions or raises the overlay goes through one primitive, `OverlayPresenter` (`src/overlay/OverlayPresenter.{h,cpp}`), so no call site re-derives the guarantee. It applies two independent protections on every presentation:

- `WS_EX_NOACTIVATE` on the **current** native handle, written before the window is moved, shown or raised. Qt does not translate `Qt::WindowDoesNotAcceptFocus` into this ex-style, and without it Windows may activate the overlay on its own — it promotes the next topmost window when the foreground one hides or minimizes.
- `SWP_NOACTIVATE` on every native positioning and z-order call, including the `HWND_TOPMOST` pin after the window becomes visible.

`OverlayPresenter::makeActivatable()` is the one deliberate exception, and it is a second step, never part of `present()`: it clears `WS_EX_NOACTIVATE` on the window that is already presented so the foreground request that follows can succeed. Geometry, visibility and z-order are untouched, `SWP_NOACTIVATE` still rides every positioning call in both modes, and the mode sticks — a screen change or a monitor move re-applies it instead of silently reverting an interactive overlay. `resetActivationPolicy()` puts the window back to non-activating for its next open, so every show starts from the proven path.

Neither protection replaces the other. Qt rebuilds the native window on some flag, geometry and screen transitions, and a rebuilt window starts without the ex-style, so the presenter re-resolves the handle after each step and re-applies it; `OverlayPresenter::reassert()` does the same after a `QWindow::screenChanged`. Each presentation records the foreground window before and after and logs it, so the claim is measured rather than assumed. `tests/tst_overlaypresenter.cpp` covers initial show, repeated show, reposition, screen change, handle recreation and a stripped ex-style through a fake Win32 seam, plus one real-window case on the production adapter.

The lifetime decision is pure and separate: `OverlayManager` only resolves the Win32 facts (`IsWindow`, `IsWindowVisible`, `IsIconic`, `GetAncestor(GA_ROOT)`, owner, process id) — for the foreground candidate and for the window it currently remembers — and applies the decision through this same presenter — a rebind re-asserts, and a replacement window on another monitor is followed with a presentation, so no second show path exists next to the presenter. The overlay's action menu and the delete confirmation are in-window QML items, not separate native windows; `tests/tst_overlaylifetime.cpp` fails if either ever grows a top-level window declaration, because that would need its own non-activation guarantee.

The overlay uses the same `ForegroundAcquirer` the desktop window does: one synchronous attempt plus two bounded retries (50 ms, 150 ms), re-reading `GetForegroundWindow()` for the truth rather than trusting what `SetForegroundWindow` reported. There is no periodic re-acquisition and no `requestActivate` path. Because the request can still be retrying, the open and close records are completed when it settles — including the cancelled case, which settles at "not acquired".

**The polled game-context watch.** While the overlay owns the foreground, a game that minimises, hides or destroys its window produces no `EVENT_SYSTEM_FOREGROUND` at all: nothing changed foreground, because GameHQ already had it. The post-show probe therefore keeps running after its dense first three seconds, at 250 ms, and applies `OverlayLifetime::rememberedGameContextLost()` — the same destroyed/hidden/minimised rule the event path uses — hiding the overlay once that has held for 750 ms. The grace period matters: a game that destroys and immediately recreates its own window must rebind through the normal foreground event instead of being read as gone. The watch is armed only when a real window was remembered at open time, so an overlay opened with no foreground window at all does not close itself.

After verified foreground acquisition, the same watcher also reconciles a non-null foreground window from another process through the existing lifetime policy. This dismisses the overlay if a foreground WinEvent was missed, instead of only releasing input while leaving the topmost overlay visible. It does not reacquire focus, act on transient null foreground, or change the denied-acquisition fallback.

### Foreground acquisition (`cpo-o06b`)

Variant B of the out-of-process isolation experiment, and the only variant implemented so far. The sequence is:

```text
remember the game HWND + pid
  -> present through OverlayPresenter (non-activating, unchanged)
  -> makeActivatable() on the presented handle
  -> ForegroundAcquirer::acquire(overlay, "overlay show")
  -> complete the open record with what Windows actually did
```

The acceptance question is a conjunction, recorded as `interactive-foreground`: the overlay owns the foreground **and** the game window still exists, is visible and is not minimised **and** the overlay is still visible. A successful API call satisfies none of those on its own, and a game that minimised itself in reaction would satisfy the first and fail the leaf.

Foreground ownership is not game identity. The remembered window and pid stay the authoritative game context for presets, capture association, gallery identity, diagnostics and the eventual return of focus; `OverlayLifetimePolicy` already treats "the foreground is our own overlay" as `Ignore`, and `sameProcessAsGame` excludes GameHQ's own pid, so `GameHQ.exe` can never become "the current game" by holding the foreground for a while.

Not in this step: the GameInput exclusive-foreground policy (`cpo-o06c` — done, and measured from an external process by `cpo-o06d`; see `docs/controller-input.md` and `tools/input-receiver/README.md`), and the **neutral-state handoff** (`cpo-o06e` — implemented; see "Closing with a held control" below): a close with the exclusive policy in force waits, inside a bound, for the pad to be physically neutral before it releases the policy and hands the foreground back.

### Closing with a held control (`cpo-o06e`)

Closing while a control is still held used to expose that held state the instant the policy was dropped and the foreground went back. The close is now a two-stage transition:

1. `ReleasingInput`: the close begins and `InputEngine::setOverlayReleaseActive(true)` stops the overlay input path from producing actions (navigation repeat cancelled, pending gestures invalidated) — while the physical state keeps being tracked, because that is exactly what the wait is waiting for.
2. The exclusive policy **stays in force** while the close polls the pad every 10 ms, up to a 400 ms bound. Releasing first and waiting afterwards would expose precisely the state the wait exists to hide.
3. On neutral — or on the bound expiring — the policy is restored, then the foreground goes back, then the window hides. `OverlayManager` completes every close from one place, so each exit path releases exactly once.

"What is held" is one definition, in `src/input/HeldControlTracker.h`: the raw press/release edges the backends publish, before any routing, so a control that never fired an action still counts, and a device that goes away takes its held controls with it — a controller that no longer exists cannot deliver the release, and a handoff must not wait for a ghost. Thresholds are not re-invented: whichever deadzone or trigger threshold the active backend applied in order to publish the edge is the one the wait respects.

A close that nothing could leak through does not wait, and says why: the desktop handoff (`hideForDesktopHandoff`) gives the foreground to GameHQ's own window rather than the game, a game window that is gone or minimized is not a hand-back target, and a close with no exclusive policy in force has nothing to defer.

The wait is honest about its limits. A pad that never reaches neutral inside the bound completes the close anyway — restores the policy and the foreground — and records `neutral-handoff=timeout` together with what was still held. Nothing is ever synthesized into the game, and a timeout is never written as a clean handoff.

### Focus and controller record (`cpo-o06a`)

Every real overlay open and close writes one bounded record, built by `src/overlay/OverlayFocusTrace.{h,cpp}` and emitted both to the log (`Overlay focus trace (open)` / `(close)`) and to the diagnostic export. One record per transition — never per frame or per input event.

An open record carries the game window and the overlay window as Windows described them (existence, visibility, minimised state, process id, rect), the foreground window before presentation, after presentation and after the activation request, whether activation was requested at all, the acquisition outcome and attempt count, **both windows re-sampled once the request settled**, the derived `interactive-foreground` verdict, whether the lifetime rules accepted the overlay's own foreground instead of dismissing it, whether Qt and Win32 agree about who is active, the serving controller provider, the hashed controller profile, and the GameInput focus policy actually in force. A close record carries the foreground before and after, the window that would be restored to, whether a restore was requested and whether it happened, the neutral-state handoff status, and the provider on both sides.

Two fields exist to stop the record from overstating itself:

- `isolation=not measured in-process` appears on **every** open record, including one where the overlay owns the foreground. Whether the game still receives the controller cannot be observed from inside GameHQ; it needs a separate receiver process (`cpo-o06d`).
- The close record's `neutral-handoff` field carries the handoff's receipt, in the one vocabulary the log, the diagnostics timeline and the close line share (`ModernInput::neutralHandoffReceipt`): `passed duration_ms=… polls=…`, `timeout duration_ms=… polls=… held="…"`, `released-elsewhere …`, `not-engaged (…)`. A clean handoff, a timeout and a close that never needed to wait can therefore never read as the same thing, and the field is never blank.

`OverlayManager` is the only place the Win32 facts are read, so the record itself is pure and `tests/tst_overlayfocustrace.cpp` pins the formatting and the derived verdicts without a desktop session, a game or a controller. `ProductionGameInputApi` reports the focus policy it applies — today background input plus background Guide and Share, with no exclusive-foreground flags — so the export states the policy in force rather than the one inferred from the class name.

This record changes no overlay behaviour. It is the evidence layer for `cpo-o06`, which is out-of-process only by constraint: no injection into the game, no API hooking, no game memory access, no per-game shims and no kernel driver or virtual controller as the default path.

Sources: [Qt Windows show implementation](https://github.com/qt/qtbase/blob/6.8/src/plugins/platforms/windows/qwindowswindow.cpp), [Qt activation handling](https://github.com/qt/qtbase/blob/6.8/src/plugins/platforms/windows/qwindowscontext.cpp), [Microsoft mouse activation contract](https://learn.microsoft.com/en-us/windows/win32/inputdev/wm-mouseactivate).

This local Khazan compatibility change still requires verification in a rebuilt executable. Borderless/windowed modes are the intended targets; exclusive fullscreen is not guaranteed. It does not establish a fix for the original reported freeze. Controller isolation remains a separate, unimplemented design (`docs/design/exclusive-controller-mode.md`).

## Controller isolation: what is verified, and what is not

The point of taking the foreground is that the presses the user aims at the
overlay should not also act in the game. One level is used everywhere — in the
open record, in the diagnostics export, and here — and the levels are
deliberately not symmetric:

| Level | Means | What sets it |
|---|---|---|
| `isolation=unavailable` | No exclusive policy is in force, so nothing is narrowed. | The policy was never requested, was refused, or was already released. |
| `isolation=scoped` | The exclusive policy is in force; isolation is externally verified for **GameInput clients**, and the game's own input path is unconfirmed. | The mechanism, plus the out-of-process measurement below. |
| `isolation=full-native` | The exclusive policy is in force **and** the tested game, pad and configuration was confirmed externally. | A recorded physical acceptance result only. An API call can never produce this level — `tests/tst_isolationcapability.cpp` walks every combination of the inputs and fails if any of them claims more than it was given. |

### Externally verified

`tools/input-receiver/` (`cpo-o06d`) measures the pad from **outside** GameHQ: a
separate process with its own GameInput session that explicitly asks for
background input, so a gap in what it receives cannot be explained by it losing
the foreground. Against the shipped path — overlay in the foreground, `cpo-o06c`
policy in force — that second process received a wired DualSense (054c:0ce6 over
USB) at ~209 Hz before the overlay opened, **0 arrivals for the whole exclusive
phase**, and ~249 Hz after release. Reproduced twice:

| Run | Baseline | Exclusive phase | After release | Verdict |
|---|---|---|---|---|
| 1 | 208.90/s (728) | **0.00/s (0)** | 248.94/s (885) | `blocked-then-resumed` |
| 2 | 208.73/s (732) | **0.00/s (0)** | 248.88/s (882) | `blocked-then-resumed` |

**This is the strongest claim GameHQ makes, and it is exactly this narrow:** a
second GameInput client stops receiving the pad while the exclusive policy is in
force and starts again after it is released, on that one pad, over USB.

### Not measured — the row above does not cover these

| Path | State |
|---|---|
| A game reading through **XInput** | not measurable here: the attached DualSense is not an XInput device, and the receiver reports `not-measurable` rather than "isolated" |
| A game reading **Raw Input / HID directly** | not measurable here: an idle pad emits no Raw Input reports for the registered collections |
| **Steam Input** translation | not measured |
| **DSX / virtual-pad** paths | not measured |
| The **Guide/PS opening gesture** reaching the game | not measured, and a separate question from post-open navigation |
| A real game's reaction to overlay navigation | not measured — this is what the physical checklist is for |
| Closing with a control held, inside a real game | the software sequencing is proven (`cpo-o06e`); the in-game result is not |

`not-measurable` is a verdict of its own so that an unproven path can never be
quoted as isolated. If a title turns out to act on presses aimed at the overlay,
the honest report is that **its** input path is outside what the exclusive policy
covers — not that a test was missing.

### What an open records about itself

Every open writes its own position into the log line and into the diagnostics
export behind **Copy diagnostic summary** (Settings → Advanced):

```
isolation=scoped evidence=external-gameinput-clients real-game=unconfirmed
isolation=unavailable evidence=none real-game=unconfirmed
isolation=unclassified (this open recorded no classification)
```

The level comes from the policy that **ended up in force** — never from the
request having been made — combined with evidence recorded outside the process,
so a refused request cannot read as isolation and no run can claim a game path it
never confirmed.

## Anti-cheat and process footprint

The shipped path installs nothing into a game and touches nothing inside it:

- no DLL is loaded into the game, none of its APIs are hooked, no remote thread
  is created, and no game memory is read or written;
- no per-game shim, no injection of any kind;
- no HidHide, no ViGEm, no virtual controller, no kernel filter or driver;
- no synthetic keyboard or pad events: input the user did not perform is never
  delivered to a game, and GameHQ's own actions never masquerade as game input;
- only GameHQ-owned windows, Qt, documented Win32 calls (foreground, window
  style, window placement) and the documented GameInput focus policy.

The consequence is stated rather than engineered away: a title whose input path
the exclusive policy does not cover **cannot** be isolated under this
architecture, and that case is reported as a limitation instead of being "fixed"
with injection. Hiding the physical device or presenting a virtual one is a
documented, rejected alternative — see
[design/exclusive-controller-mode.md](design/exclusive-controller-mode.md).

## Crash, exit and recovery

- The exclusive policy belongs to the GameHQ process and its GameInput session.
  A normal exit releases it on the runtime-shutdown path while the runtime is
  still alive, and every other exit path funnels through the one release point in
  `OverlayManager` (`cpo-o06c`).
- Nothing else is left behind, because nothing else exists: no virtual
  controller for a game to keep seeing, no hidden physical state, no cloaking
  configuration to unwind, and no cleanup step inside any game process.
- If the process is killed rather than closed, its per-process GameInput state
  dies with it. That is a Windows/process-lifetime expectation — GameHQ sets no
  machine-level state at all — and it is **not** something this repository has
  measured. A bounded crash experiment is listed with the physical acceptance
  work; it is not claimed here as verified.

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

- Focus-restoration flakiness; the neutral handoff has a 400 ms bound, and a close that runs into it says so (`neutral-handoff=timeout`) rather than claiming a clean hand-back.
- The exclusive policy is **GameInput-scoped** (`cpo-o06c`): a game or tool reading the pad through XInput, Raw Input, a virtual pad or Steam Input is outside it, and the matrix above lists those paths as not measured rather than covered.
- Steam Input remapping the pad while the overlay is focused, and DSX-style virtual pads, change which client receives what — record the configuration when reporting.
- Games that keep reading input regardless of focus: measure the title, then document it per game instead of generalising from one result.
- The Guide/PS opening gesture is not part of the post-open isolation story and is still unmeasured.
- Multi-monitor placement (show on the game's monitor).

## Native acceptance (cpo-o05)

`tests/tst_overlaynative.cpp` drives the shipped overlay — `OverlayWindow.qml`, the
manager and the presenter, loaded through a test-local `GameHQ` QML module that
embeds the real files — against `tests/overlay_target_fixture.cpp`: a borderless
window in a separate process, driven through a registered window message. The
assertions are real Win32 facts (`GetForegroundWindow`, `GWL_EXSTYLE`,
`IsWindow`/`IsIconic`, posted input), not fakes:

- showing the overlay takes the foreground while the target stays visible and
  un-minimised behind it, and it stays that way — across repeated open/close
  cycles, the action menu, the delete confirmation, and a native-handle rebuild
  (`destroy()` + `show()`, the Qt path a flag or screen change uses). Closing
  hands the foreground back to the target, every cycle;
- the overlay does **not** dismiss itself on its own expected foreground, and
  the target — not `GameHQ.exe` — is still the remembered game context while
  the overlay is active (asserted through the handle `hideForDesktopHandoff()`
  returns);
- a denied foreground request leaves presentation intact: the overlay is still
  shown, topmost and open, the target keeps the foreground, `foregroundAcquired`
  stays false, and the request stops at its fixed budget instead of looping
  (a `ForegroundApi` seam refuses and counts every attempt);
- the overlay closes when the foreground genuinely leaves the game (another
  application activated, the target minimized, the target window destroyed) and
  stays open when the game replaces its own window in the same process. With
  the overlay holding the foreground, the minimize and destroy cases produce no
  foreground event at all, so those two exercise the polled context watch;
- a destroyed game handle is never measured or followed again, and the
  production show path re-applies the current activation mode on a rebuilt
  handle;
- the target keeps processing its window messages while the overlay is open and
  owns the foreground, and the overlay's input route (keyed on overlay
  visibility, not OS focus) still closes it — driven through the same signals
  the device layer emits;
- cpo-o06c: while the overlay holds that verified foreground, the exclusive
  GameInput focus policy is requested exactly once, and released — with the
  reason the exit path actually observed — when the overlay closes, when another
  application takes the foreground, or when the game itself goes away. A denied
  foreground request never asks for the policy at all, and the export says so
  (`request=refused`, `reason="foreground was not acquired"`) instead of
  implying a policy that was never set.

It runs through `ctest -R tst_overlaynative --output-on-failure` and needs an
interactive session that can hold a foreground window; it briefly shows the real
full-screen overlay for each scenario. The cross-screen scenario needs two
monitors and skips itself with that reason on a single-screen machine (the
rebind-and-reposition path it would exercise is decided by
`overlay/OverlayLifetimePolicy` and covered by `tst_overlaylifetime`). Physical
gamepad delivery is not part of this harness: it needs a real device and stays
with controller acceptance.

When a game disappears behind the overlay, `visible=1 iconic=0` alone does not
prove its picture remains visible. The existing post-show probe also records
DWM `cloaked` state and, for its first three seconds, up to four overlapping
windows above the game (excluding the overlay), with handles, PIDs, owners and
rectangles, but no window titles. Enumeration stops at the game or after 128
windows; `game-reached=0` means the sample is incomplete. Candidates may be
transparent and are not proof of occlusion. These observations never change
focus, z-order, game styles, input policy or handoff behavior.

The 2026-09-22 Great Circle comparison reproduced the drop with a separate
foreground popup making no GameInput calls. Giving that popup the game as its
owner kept the game above the other application windows; reasserting only the
popup's topmost position preserved foreground on the popup and left the game
non-topmost. This establishes focus loss as sufficient to reproduce the issue,
without blaming GameInput exclusivity or claiming why an earlier session differed.
