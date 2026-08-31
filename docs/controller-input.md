# Controller Input

> Milestone 0.3. DualSense-first, but everything is rebindable and keyboard always works.

## Stack

Raw Input / HID for Sony pads (vendor 0x054C): DualSense `0x0CE6`, DualSense Edge `0x0DF2`, virtual DualSense `0x0ECC` (DSX DualSense-emulation mode), DS4 v1 `0x05C4`, DS4 v2 `0x09CC`, including DSX/ViGEm virtual DS4. GameHQ also accepts the DS4-compatible virtual HID IDs observed from DSX/ViGEm on the dev machine: `VID_11FF&PID_0847` and `VID_3670&PID_0902`. **USB and Bluetooth report layouts differ** (DualSense enhanced BT: report id 0x31, 2-byte prefix + CRC32; DS4 BT: report id 0x11; short/simple reports use DS4-style button offsets). XInput covers DSX Xbox mode and real Xbox pads; Back/View maps to GameHQ's Share action (standard XInput has no Create/Share button) and the Guide button maps to PS via `XInputGetStateEx` (xinput1_4 ordinal 100) when available. WinMM (`joyGetPosEx`) is the last-resort fallback for virtual/DirectInput controllers that do not appear through Raw Input or XInput; it picks the button mapping from the joystick's vendor/product id (`joyGetDevCaps`) — Sony pads use the DirectInput Sony order (Square/Cross/Circle/Triangle = 0–3, Share = 8, Options = 9, PS = 12), everything else the Xbox order (A/B/X/Y = 0–3, Back = 6, Start = 7).

**HidHide caveat:** DSX installs HidHide to hide the physical pad from other apps. If HidHide is set to hide devices and GameHQ.exe is not on its application whitelist, Raw Input sees nothing even though the pad works in games — add GameHQ.exe in *HidHide Configuration Client → Applications* for native DualSense handling. The WinMM fallback usually still works meanwhile.

### Detection & hot-plug (robustness rules)

- **Per-device state, keyed by Raw Input handle.** Every supported Sony/DS4 pad is tracked with its own layout, button bitmask, stick hysteresis, and last-report timestamp. Handles change on every reconnect, so a fresh handle is simply a new tracked device. This prevents two pads reporting at once (real DualSense + DSX virtual DS4) from corrupting each other's edge detection.
- **One active device per backend, selected by real control activity.** Physical Sony hardware outranks virtual DualSense and virtual DS4 devices immediately. Otherwise a non-active device takes over only after the previous device has stopped producing button/stick changes for >1 s. Continuous idle reports cannot pin a virtual device forever. If the active pad is removed, the best recent candidate takes over immediately; only when no candidate exists does the 1.5 s debounce run.
- **Arrivals probe one device, not the world.** `WM_INPUT_DEVICE_CHANGE` (via `RIDEV_DEVNOTIFY`) carries the device handle; only that handle is queried. A debounced (400 ms) reconciliation pass syncs against the enumerated device list to prune handles Windows no longer lists — removal messages can be lost during heavy re-enumeration (DSX re-creating its virtual pad).
- **Every WM_INPUT is read header-first, and each device is classified once.** See [Flood hardening](#flood-hardening-high-polling-rate-pads) below.
- **Xbox-type pads belong to XInput only.** HID collections whose interface path contains `IG_` are XInput devices; Raw Input ignores them so one physical pad can never drive two backends.
- **Empty XInput slots are never hot-polled** (documented pitfall: `XInputGetState` on an empty slot can stall for milliseconds). Connected slots poll at 33 ms; empty slots are probed only on the Raw Input backend's debounced device-topology hint plus a 3 s safety-net timer. WinMM works the same way: 50 ms polling while connected, arrival scans only on topology hints plus a 2 s safety net — and the discovery sweep itself runs on a background worker (0.7.6; it was measured blocking the GUI thread for 161 ms).
- **Exactly one backend routes input, governed by a takeover contract** (`ControllerArbitration::backendMayTakeOver`, unit-tested in `tst_controllerarbitration`). Sony HID > XInput > WinMM chooses only the initial or disconnect fallback; after that the active backend keeps the role while it is connected and has produced a control within `BackendTakeoverSilenceMs` (1 s). Any cross-backend event within `BackendDuplicateWindowMs` (100 ms) of the active backend's last control is a suspect mirror and never takes over — **regardless of whether the canonical control id matches**, because remappers translate ids between APIs (letting different ids through double-acted one physical press). Between the two thresholds a real event is not promoted — but it is not thrown away either, because that would lose real input. The backend becomes a **pending candidate** and its first press is **held, not dropped** (`ControllerArbitration::heldPressSurvives` / `candidateMayConfirm`, `InputEngine::holdCandidatePress` / `resolvePendingCandidate`). This matters because XInput and WinMM report state *changes*: a user who presses once and lets go produces exactly one event, so a scheme that waited for a second one would lose that press for good. After `BackendCandidateConfirmMs` (250 ms) the held press is either delivered — the active backend never spoke, so this was a real switch — or discarded, because the active backend answered and the press was its mirror. A release arriving while the press is held is remembered rather than forwarded and replayed immediately behind it, so a **tap stays a tap and never becomes a hold** (`tst_bindingeditor::replayedPressAndReleasePairStaysATap`). A second candidate control before the window closes promotes immediately, delivering the held press first, in the order pressed. Without this, toggling DSX between Sony and Xbox mode cost close to a second of dropped presses. Two further exceptions move the role sooner: the active backend disconnecting (`updateActiveBackend()` re-picks by priority), and a higher-priority backend whose device fingerprint equals the active one's (the same physical pad over a better path — Sony Raw Input and WinMM both report `VID:PID`; XInput's slot fingerprint deliberately never matches). Every takeover lands in the diagnostics ring with its reason. Switching or losing the active backend cancels held navigation repeat and Share tap/hold state, and XInput/WinMM synthesize release events before disconnect.
- **Settings shows live status**: `input.controllerStatus` (green/grey dot row in the input-test card) reports the active backend independently of the last-input line.
- **The left stick doubles as the D-pad, and the rule lives in one place.** `input/StickNav.h` maps a stick's raw x/y onto `Dpad*` bits for every backend; each one only supplies an `AxisConfig`. Deadzone values stay per-backend on purpose — they are tuned against each pad's raw range and are not interchangeable: DualSense `center 128, deadzone 60, return 30`, XInput `center 0, deadzone 12000`, WinMM `center 32767, deadzone 16000`. What is shared is the structure, and it encodes the three traps: **Y polarity is not universal** (DualSense/WinMM report positive = down, XInput positive = up), the two directions on an axis are **mutually exclusive**, and **hysteresis is opt-in** (`returnZone < deadzone` keeps an axis active until the stick returns well inside center; set `returnZone == deadzone` and the rule collapses to a plain threshold). Only the DualSense backend runs hysteresis today — XInput and WinMM never had it and still don't.
- **The right stick scrolls like a mouse wheel (vertical only).** `StickNav::verticalBits` maps the right stick's Y axis onto the `RStickUp`/`RStickDown` bits (`gamepad.stick_right_up`/`_down`) using the same `AxisConfig` and hysteresis rules as the left stick. Default-bound to the repeating desktop actions `desktop.scroll_up`/`desktop.scroll_down`, which emit `desktopScroll(direction)`; QML routes it to whichever surface is showing — gallery grid, Settings options page, assignment editor dialog, Help, full release notes — moving only the viewport, never the selection. Decoded by the Sony and XInput backends; WinMM deliberately does not decode it (which of Z/R/U/V the right stick lands on is not standardized across generic pads, and a wrong guess would turn a trigger into phantom scrolling). GameInput keeps its right-thumbstick direction flags routed nowhere while standard controls remain shadow-only, but `StandardControlMap` can map them onto the same ids at Stage C re-enablement.
- **Because the right stick leaves focus behind, directions re-enter from the viewport.** A long scroll can push the focused control far off screen, and stepping from it would scroll the page straight back. `SettingsPage.focusInViewport()` tests whether the focused control still overlaps the visible band; when it does not, `SettingsView.enterFromViewport()` hands the step to `PadNav.viewportTarget()`, which picks the topmost on-screen focusable for Down and the bottommost for Up (nearest control outside the band when a tall text block leaves none inside). `focusOptions()` uses the same entry point, so entering the options panel from the category list also lands on what the user is looking at — on an unscrolled page that is still the first control. Left keeps its edge meaning and backs out to the categories.

### Flood hardening (high-polling-rate pads)

Raw Input is registered with `RIDEV_INPUTSINK`, so **every** report from every
joystick/gamepad/multi-axis device on the machine arrives as a `WM_INPUT` on
the GUI thread — including pads GameHQ does not drive. Controllers advertising
8000 Hz polling are sold today, and each one produces 8000 messages a second.
The backend therefore spends as little as possible on a message before it knows
whose it is:

1. **Header first.** `RID_HEADER` copies a fixed 24-byte struct onto the stack
   and names the device. No size probe, no buffer, no allocation.
2. **Cached verdict.** A per-handle negative cache (`KnownIgnored`) answers in
   one hash lookup. Nothing else happens on that path: no OS query, no
   allocation, no log line.
3. **Classification, at most twice per handle.** `RIDI_DEVICEINFO` decides on
   VID/PID and usage alone; the interface path (`RIDI_DEVICENAME`, the `IG_`
   XInput check) is only queried for hardware that already looks like a
   supported pad.
4. **Payload last.** `RID_INPUT` is read only for a tracked pad, into a buffer
   the backend reuses, so even a supported pad does not allocate per report.

Cache rules, in order of how much damage getting them wrong would do:

- **A failed query is not a verdict.** `RIDI_DEVICEINFO`/`RIDI_DEVICENAME` can
  fail transiently (a device mid-enumeration). That is never cached as ignored;
  the handle stays unclassified and is retried on the next report.
- **Handle values are reused.** Windows hands out the same `HANDLE` value to a
  different device after a replug, so cached verdicts are keyed strictly by live
  handle and dropped on **every** `WM_INPUT_DEVICE_CHANGE` (arrival *and*
  removal) and on every reconciliation pass that no longer lists the handle.
  Tracked pads are deliberately *not* dropped on arrival — that would reset a
  live pad's active role.
- **The negative cache never holds a supported pad.** Only a definite answer
  from the OS puts a handle in it.

`WM_INPUT` also follows the documented cleanup contract: a foreground event
(`RIM_INPUT`) is passed to `DefWindowProc` after processing, while a sink event
(`RIM_INPUTSINK`) returns 0.

**Rates are counted, never logged per event.** `InputRateMonitor` aggregates
events per handle; the backend samples every 5 s and logs a line only when a
device starts streaming, changes rate materially, or stops. An 8 kHz pad shows
up as one line, not 8000 a second.

All of this is testable because every OS call goes through the injectable
`RawInputApi` seam (`readHeader` / `readPayload` / `describeDevice` /
`devicePath` / `enumerateDevices` / `scanHiddenPads`). Production uses
`RawInputApi::createSystem()`; `tests/tst_rawinputflood.cpp` substitutes a fake
that counts calls and drives the real `DualSenseDevice` at 1000/4000/8000 Hz —
real `HRAWINPUT` values cannot be fabricated, which is why the seam exists.

## Tap vs Hold (Share)

```txt
button down  → start timer
released < threshold        → TAP action (screenshot)
held ≥ threshold (def. 2 s) → HOLD action (save replay), mark consumed
release after consumed      → nothing
```

Threshold options: 1.0 / 1.5 / 2.0 / 3.0 s / custom. Implemented in the binding runtime's gesture handling (`BindingRuntime`).

**Frame grab while a clip is focused.** In the Playback scope (a clip focused in the overlay or the desktop lightbox), a **Share tap** is bound to `playback.frame_grab` instead of the global screenshot. It grabs the exact frame currently shown on the video surface — paused or mid-playback — and saves it as a screenshot for the clip's game through the normal screenshot pipeline. Keyboard equivalent: **S** (Playback scope only, so it never collides with the global `Ctrl+Shift+S`). Share **hold** is unbound in Playback scope, so it still falls through to the global save-replay action.

The substitution is **declared, not inferred**. `ContextOverrideCatalog` (`src/input/ContextOverrideCatalog.h`) holds the explicit list of contextual actions that replace a Global one, and it currently contains exactly one entry: `playback.frame_grab` shadows `global.screenshot` on a **single tap**. `BindingResolver::matching()` unions Global bindings with the active contextual ones and drops only the pairs this catalog names. There is deliberately **no** generic "a contextual scope shadows Global" rule — that would silently swallow overlaps the user created themselves, which the binding editor is supposed to surface and classify. Because the entry is scoped to a single tap, Share **hold** (save replay) and Share **double-tap** (overlay toggle) stay live during playback, and Guide keeps toggling the overlay in every scope.

## Binding patterns (trigger + gesture model)

`src/input/BindingPattern.h` holds the model a binding is expressed in. A binding is a **trigger** (what is pressed) plus a **gesture** (how it is pressed):

| | Kinds | Fields |
|---|---|---|
| `TriggerSpec` | `Single`, `OrderedChord` | ordered list of canonical control ids |
| `GestureSpec` | `Press`, `Tap`, `Hold` | `tapCount` (1–3), `holdMs` (0 = use the configured default) |

**Serialization.** A single trigger serializes to its control id verbatim — `gamepad.capture`, `Ctrl+Shift+S`, `gamepad.button.7` — so every value written before patterns existed parses back to exactly what it meant and nothing on disk is rewritten. An ordered chord serializes to `chord:v1:<first>><second>`, e.g. `chord:v1:gamepad.capture>gamepad.guide`. The whole `chord:` prefix is a reserved namespace: a serialization version this build does not know (`chord:v2:…`) **fails closed** rather than being approximated. Order is significant — the first control is held, the second one is pressed.

**Validation** happens at the parse boundary and is mandatory:

- `Single` — exactly one control, and it may not sit in the reserved `chord:` namespace.
- `OrderedChord` — exactly two **different** controls, both canonical (`ControlId::isCanonical`), so one saved chord means the same thing on Sony HID, XInput and WinMM. Chords are controller-only and support the `Press` gesture only in version 1.
- `Press` — `tapCount` 1, `holdMs` 0. `Tap` — `tapCount` 1–3, `holdMs` 0. `Hold` — `tapCount` 1, `holdMs` ≥ 0.

`GestureSpec` still speaks the persisted activation tokens: an empty string and `press` mean Press, `tap` means Tap ×1, `double_tap` means Tap ×2, `hold` means Hold. Tap ×3 deliberately has **no** legacy spelling — `hasLegacyActivation()` returns false for it, so nothing can quietly downgrade a triple tap to a double one before the `tap_count` column exists.

**Persistence.** `binding_overrides` stores the trigger in `trigger_code` and the gesture in `activation` + `tap_count` + `hold_ms` (schema v4, see [database.md](database.md)). A Hold row that stores `0` means "use the configured hold duration" — the built-in hold defaults do exactly that, so **Settings → Input → hold threshold** is the single place that number lives instead of being copied into every default row. `desktop.bulk_toggle` keeps its own explicit 1000 ms because that one is a per-binding duration, not the global one.

**Where it bites.** `BindingResolver::reload()` validates every `binding_overrides` row it reads. A row that cannot form a valid pattern — an unknown chord version, the same control twice, a `press` carrying a hold duration, a value corrupted outside the app — is **skipped**, so the action falls back to its default instead of firing something nobody assigned. Each skip is logged (`Bindings: skipping stored override …`) and recorded through `InputDiagnostics::noteRejectedBinding()`, which surfaces it in the copied diagnostics under *Rejected binding rows*. Cleared slots (`unbound`, no trigger) are validated on the gesture half only, so the gesture-inheritance sentinels below survive untouched.

## Pattern recognition (runtime)

`src/input/InputPatternRecognizer.{h,cpp}` is the whole timing state machine: tap counting, hold timing, chord candidates, constituent suppression and cancellation. `BindingRuntime` keeps only the two halves that need the binding table — telling the recognizer what exists on a control, and turning a recognized pattern into actions. The split is deliberate: dispatch bugs read as *the wrong thing happened*, timing bugs read as *sometimes nothing happens*, and only the second kind needs a clock you control. Behind the `TriggerFacts` seam the recognizer is testable with no device, database or app (`tst_inputpattern`, 34 cases).

**Snapshot and generation.** At the first physical press of a pattern the recognizer snapshots the device profile, the scope pair, the effective facts for that control, and the timing values. A pending pattern carries a generation counter; a backend switch, a disconnect, a binding reload, a timing change or a scope change bumps it and cancels whatever was pending. A delayed action can therefore never fire into a context the user has already left — first tap in the gallery, overlay opens, second tap: the gallery action does not arrive late.

**Exact-count taps.** Three taps fire the ×3 binding **only** — never ×1, then ×2, then ×3 on the way up. A lower count waits out `input.multi_tap_interval_ms` **only** when a higher count is actually bound on that control; otherwise it fires on the release edge exactly as a single tap always has. A count nobody bound fires nothing: with ×1 and ×3 bound, two taps mean neither. A hold consumes the pending tap sequence, and hold time is measured from *that* press — holding the second tap of a double tap is a hold, not a slow tap.

**Ordered chords.** A control that starts a chord opens a bounded candidate instead of acting; a configured partner pressed within `input.chord_window_ms` completes it and consumes both components. Several chords can share a first control (View+Guide, View+X) — the second control selects. Everything about the fallback is anchored to the **original physical press**: a hold threshold that elapsed inside the window fires immediately at timeout, a hold longer than the window still fires at its own threshold, and letting go of the first control early replays press+release at once so a quick tap stays a tap and can never become a hold. Constituents are replayed at most once. A repeated press of a control whose candidate is already open is ignored, so a mirrored backend (physical DualSense + DSX virtual pad) cannot double-fire.

## Binding relations (conflict policy)

`BindingRelation` (`src/input/BindingRelation.h`) is the single source of truth for how any two bindings relate. The binding editor and the runtime both read it, so a warning in Settings can never disagree with what the buttons actually do. It replaced `BindingEditorModel::scopesConflict()`, which was literally `left == right`: that flagged legitimate same-scope tap/hold sharing as fatal and stayed silent on real Global-vs-context collisions.

`classify()` walks a **frozen decision order** and returns on the first match. Reordering it changes user-visible verdicts.

1. Different device group, different trigger, or an unbound row → `None`.
2. Scopes that are never active together → `None`.
3. A pair declared in `ContextOverrideCatalog` → `ContextOverride`.
4. Same action + trigger + activation + device group + profile → `Redundant`.
5. Overlapping scopes with compatible gestures → `SharedGesture`.
6. Overlapping scopes with a `press` gesture involved, or the same gesture on different actions → `HardConflict`.
7. Otherwise → `None`.

**Scope overlap.** Global is always live, so it overlaps everything, including when the runtime represents an absent contextual fallback as Global. Two *different* contextual scopes never overlap: once the primary scope binds an exact trigger, it owns that trigger across press, tap and hold gestures; the contextual fallback is consulted only when the primary has no binding for that trigger. Playback-over-Desktop and Playback-over-Overlay are therefore priority chains, not collisions. This is why D-pad Left can ship as both `playback.seek_back` and `desktop.navigate_left`, while Cross cannot pause a clip and then leak through to Desktop Confirm or Bulk Select.

**Chords and tap counts.** The precondition is *triggers share any control*, not *same trigger code*. Appended rules, in order: the same chord twice is a `HardConflict`; two chords sharing only a first control are `None` (the second control selects); a single-button binding on a chord's **first** control is a `HardConflict` when it acts immediately — a plain press, or an action flagged `repeats` in the catalog such as d-pad navigation or L1/R1 paging — and otherwise `SharedGesture`; a binding on the **second** control is `SharedGesture`.

**Two notice tiers** (`BindingRelation::Notice`, produced by the shared policy, never by ad-hoc editor logic). *Higher-tap-count delay*: this action waits because the same button also carries a higher tap count. *Chord-start delay*: this action waits because the button starts a combination. `noticeText()` takes the live `input.chord_window_ms` and `input.multi_tap_interval_ms`, so the number the user reads is the number the runtime waits.

**Gesture compatibility.** Tap and hold coexist on one button, and so do two taps that need a different number of taps (×1 with ×2, ×2 with ×3), because the runtime separates them in time. Two holds never do — the shorter threshold always fires first — and anything involving `press` does not either: press resolves on the down edge, before a timed gesture is known.

**Redundant requires an identical trigger.** The same action reached from both `Ctrl+Shift+S` and `F12` is a deliberate second slot, not redundancy.

`BindingRuntime::relations(group, profile)` precompiles every non-`None` pair and caches it until the next `reload()`. Nothing on the per-event path calls it — gesture dispatch works off the resolver, which already applies the override catalog.

### Editor notices

Only `HardConflict` blocks. It opens `BindingConflictDialog.qml`, a dedicated three-action modal (Replace / Choose another / Cancel) rather than an extra button bolted onto the app-wide `ConfirmDialog`. The other tiers save the binding and explain it through `bindingEditor.relationKind` / `relationNotice` / `validationError`.

These notices have their own property channel on purpose. `input.controllerWarning` is reserved for HidHide/cloaked-pad state; routing a routine key conflict through it would let a binding notice overwrite the one warning a user cannot diagnose on their own.

Three further kinds cover the ways a binding can fail, and they are deliberately **not** the same kind — conflating them would let a hotkey refusal masquerade as evidence that the controller path works:

- `unsupported_input` — the controller/backend cannot expose this control ("Button not reported"). This is the GameSir case, raised through `BindingEditorModel::reportUnsupportedInput()`. It is implemented and covered by a controlled test fixture, but **no production backend emits it yet**; only a GameInput-class backend will know a control exists and cannot be delivered.
- `hotkey_unavailable` — Windows or another application owns the chord ("Shortcut already taken"), raised when the `RegisterHotKey` claim is refused. Nothing is written and the previous shortcut stays live.
- `persistence_error` — the write itself failed ("Could not save this binding"); the OS registration and the displaced rows are rolled back first.

All three populate `validationError` so QML can style them as errors without parsing the message.

When a new binding relates to several existing ones at once, the notice shown is ranked `ContextOverride` > `Redundant` > `SharedGesture`, not taken from whichever hash bucket came first.

### Slot gesture inheritance

A slot's gesture (activation + hold time) belongs to the slot, not to the trigger sitting in it. Capturing into an empty slot and clearing a bound one both resolve the gesture through `BindingResolver::inheritedGesture()`, in a fixed order: the slot's own override row (unbound rows included), the shipped default for that exact slot, another live slot of the same action, any default of the same action, and only then plain `press`. Clearing writes the resolved gesture onto the unbound row, so tap/hold/double-tap survive a clear-and-rebind round trip. The controller capture prompt appends the gesture being assigned ("… · Tap"), and keyboard/mouse captures remain plain presses. Before this, an empty-slot capture defaulted to `press`, which the decision order above rightly treats as clashing with every timed gesture on the same trigger — so valid assignments (a second Screenshot button on a pad that multiplexes Share) were blocked by conflicts that only existed because of the guess. Pre-0.7.3 cleared rows carry a `press`/`0` sentinel; that combination on an unbound row is treated as "no information" and falls through to the defaults.

## Default mapping

See [product-spec.md §6](product-spec.md#6-controller-mapping-default). PS button is frequently intercepted (Steam, DS4Windows, Game Bar) → treat as optional; fallback overlay toggle: **Share double-tap** and `Ctrl+Shift+G`. PS carries a tap/hold pair like Share: **tap** toggles the overlay (fires on release — deliberately tap, not press, so the hold cannot also open the overlay on the way down), **2 s hold** fires `global.toggle_desktop` ("Show / Hide GameHQ Window", bindable in Settings → Input): `App::toggleDesktopWindow()` summons the desktop window with real OS focus via the overlay's `ForegroundAcquirer` AttachThreadInput bypass, remembering the window the foreground was taken from; a second hold while the GameHQ window is focused hides it and hands the foreground back. When the overlay is open during a summon it steps aside without restoring focus to the game (`OverlayManager::hideForDesktopHandoff()`), and its remembered game window becomes the return address instead.

## Bindings

Built-in defaults are merged with sparse rows from `binding_overrides`. `BindingResolver` applies group-wide or device-fingerprint overrides, and `BindingRuntime` resolves press, tap (by count) and hold gestures with playback → overlay/desktop → global context precedence. Controller codes are position-based, so the same assignment follows the physical button position across PlayStation, Xbox, Nintendo, and generic pads. The legacy `bindings` table is retained only for database compatibility.

## Overlay routing

`InputEngine` owns the routing gates. Overlay open ⇒ events go to QML navigation and are *not* forwarded anywhere else. Overlay closed ⇒ global triggers (Share tap/hold, PS/toggle) remain available, while desktop-gallery navigation is allowed only when the main GameHQ window is focused **and** the real Win32 foreground window belongs to the GameHQ process. This second foreground-process check is required because RawInput uses `RIDEV_INPUTSINK`, so pad reports still arrive while a game has focus. GameHQ never blocks the pad for the game itself — isolation relies on the game losing focus (see [overlay.md](overlay.md)).

## Stable XInput identity

Device-specific binding profiles need to follow the *pad*, but XInput only exposes slot numbers, so historical device rows were fingerprinted `xinput.slotN` — two different pads that ever share a slot silently inherit each other's overrides. `ControllerIdentity::resolveXInputFingerprint` (`src/input/ControllerIdentity.h`, unit-tested) fixes this where it can be done honestly: the Sony Raw Input backend tracks the VID/PID of every XInput-class (`IG_`) HID collection it ignores, and when exactly **one** distinct device and exactly **one** connected slot exist, `InputEngine::updateXInputIdentity()` keys the XInput profile on the real hardware identity (`vvvv:pppp`). Any ambiguity — no `IG_` device visible (HidHide), several devices, several slots — keeps the legacy slot fingerprint, and the profile name says so: "per-slot profile — model cannot be identified". Never guess.

Migration is explicit, never silent: existing `xinput.slotN` rows stay live for the correlated pad through a resolver **profile alias** (precedence: group-wide < aliased slot rows < device-specific rows), so upgrades change nothing; Settings → Input offers "Adopt per-slot bindings", which *copies* slot rows to the stable identity without touching the originals and never clobbers a row already saved for that pad. Slot rows are never broadened to all controllers. The GameInput `APP_LOCAL_DEVICE_ID` (see `docs/design/gameinput-spike.md`) is the designed future replacement for the correlation heuristic.

## Backend arbitration & mirror suppression (0.7.4)

One physical pad often reaches GameHQ through two APIs at once: Windows exposes every XInput pad through the legacy WinMM joystick surface, and remappers (DSX, reWASD) feed Raw Input, XInput and WinMM simultaneously. `ControllerArbitration` (`src/input/ControllerArbitration.h`) keeps one **active backend** and treats any cross-backend event within `BackendDuplicateWindowMs` (100 ms) of the active backend's last control as a suspect mirror. Two defenses (0.7.4, pinned by `tst_controllerarbitration` including a production-sequence regression that walks the exact `onControlPressed` → `resolvePendingCandidate` branch order):

- An event born **inside** the mirror window is dropped outright in `InputEngine::onControlPressed` — it is never buffered as a pending candidate. Buffering it was the 0.7.3 "one tap moves two rows" bug: the mirror trails the real press by a few ms, the active backend then goes silent (the user only tapped once), and the held press was replayed ~250 ms later as a second action.
- `heldPressSurvives` rejects a held press whose birth was inside the mirror window even if a future caller buffers one (defense in depth; overflow-safe against the "active never spoke" sentinel).
`WinMMDevice` deliberately does **not** filter out the WinMM view of XInput pads (an interim filter written during 0.7.4 development was removed before release): VID:PID identifies a *model*, not a physical endpoint, so model-level suppression could hide a legacy-only pad that shares its VID:PID with an XInput one — and the arbitration drop above already neutralizes the mirrored stream. Endpoint-strong correlation (PnP/container id) is the only acceptable basis for a future filter.

Genuine failover is preserved: a candidate press outside the window opens a pending run and is delivered after `BackendCandidateConfirmMs` (250 ms) if the active backend stays silent, so a real backend switch never loses its first tap.

## Background focus policy (GameInput, 0.7.4)

GameHQ normally runs *behind* the focused game, and GameInput's default focus policy delivers input — including the system Guide/Share buttons — only to the focused process. `GameInputWrapper::start()` therefore calls `IGameInputApi::applyBackgroundFocusPolicy()` after `initialize()` and **before** any callback registration (`SetFocusPolicy(EnableBackgroundInput | EnableBackgroundGuideButton | EnableBackgroundShareButton)` — all three flags, because `EnableBackgroundInput` does not cover the system buttons). Never `ExclusiveForeground*`: an overlay observes, it must not steal the game's controller. Order is contract-tested in `tst_gameinputwrapper`.

**Controller-to-keyboard mappers (JoyXoff etc.).** A mapper that turns pad input into mouse/keyboard events is a second *logical* source GameHQ cannot dedup against: the pad arrives natively and the synthesized arrow keys arrive through Qt. Users of such tools should add GameHQ to the mapper's ignored-applications list; GameHQ deliberately does not blanket-ignore injected input, which would break Steam Input, accessibility tools and intentional macros.

## Cross-provider integration (t25, 0.7.3)

`ProviderIntegration` (`src/input/ProviderIntegration.h`) is the single seam every real input provider reports into: one `PhysicalControllerRegistry` (physical identity, lifecycle, capabilities) and one `CapabilityEventRouter` (press-cycle ownership + cross-provider edge dedup) shared by Sony Raw Input, GameInput, XInput, WinMM and the selective Raw HID fallback. `InputEngine` owns it; `GameInputRouter` receives it via `setProviderIntegration()` before `start()`.

**Correlation.** Legacy providers carry no GameInput `APP_LOCAL_DEVICE_ID`, so they correlate through the topology root — the lowercase `vvvv:pppp` fingerprint both sides can produce. A correlation is accepted only when it identifies exactly **one** logical controller and the candidate has no live attachment from the same provider (a provider observes each physical device separately, so a same-provider match is the identical twin, not the same pad). When a strong (GameInput) observation merges into a controller a legacy provider created first, the logical ID is upgraded to the deterministic strong hash so persisted per-controller state (binding profiles, extra-button layouts) survives provider arrival order.

**Share/Guide dedup (press-cycle ownership, 0.7.5).** Legacy `Capture`/`Guide` edges route through `ProviderIntegration::routeLegacySystemEdge` (fail-open if the attachment was never observed); GameInput system buttons route through the same shared router. On a merged controller the router models one physical press as one *cycle*: the first accepted press owns the control for that cycle; a mirrored press from another correlated provider joins the cycle as a participant but stays `duplicate` — it never transfers ownership and never emits a second action; a release from **any** participant closes the cycle exactly once; a release with no open cycle is explicitly ignored, so a late mirror release can never become a spurious action. Ownership only moves when the owning provider detached mid-hold — the stuck state is closed with a safe release and the working source opens a fresh cycle. (0.7.4 instead transferred a held control to the preferred mirror, which rejected the owner's release and left Share/PS logically held forever — every later press was swallowed as a duplicate.) Exactly one edge per physical press, proven by `tst_capabilityeventrouter`, `tst_providerintegration` and `tst_gameinputrouter`, including 20-cycle repeated Share/PS handoff regressions. If a legacy provider is connected but **not** correlated onto a GameInput controller (two identical pads, or a remapper reporting a different identity), GameInput Share/Guide edges are withheld (`shadowedSystemEdgeCount`) because dedup cannot be guaranteed; a release of an already-held control always passes so nothing sticks. When GameInput turns Off, shuts down or fails its session it detaches from the shared registry so the legacy path immediately owns Share/Guide again. GameInput edges execute bindings under the controller's topology-root profile, so "this controller" assignments fire regardless of the delivering provider.

**Standard controls** remain shadow-only in GameInput (Stage C): standard-control dedup across providers is not yet proven end to end. The complete v3 flag map (`src/gameinput/StandardControlMap.h`, incl. L3/R3, C/Z, paddles; right-stick directions deliberately unmapped) is defined and tested for that re-enablement. Extra buttons whose device-reported label names a standard control are blanked from `Extra Button N` enumeration (`ExtraButtonCatalog::isStandardControlLabel`) so one physical button can't carry two identities.

## Selective Raw HID fallback (t26, 0.7.3)

For a dedicated button no backend exposes (some GameSir/8BitDo extras), the Raw Input backend runs a production fallback on *ignored* gamepad-class HID handles: `WM_INPUT` payload → `RawInputApi::buttonUsages` (RIDI_PREPARSEDDATA + `HidP_GetUsages`, `hid.dll`) → usage press/release transitions → `SelectiveRawHidFallback` (bound-control filter) → canonical `gamepad.raw.<device-hash>.usage.<page>.<usage>` control → `ProviderIntegration::routeRawHidEdge` (correlates onto the pad's logical controller by `vvvv:pppp`) → binding runtime. The idle guarantee is preserved: with no bound raw-HID control and no probe open the ignored path costs two branches (zero allocation, pinned by `tst_rawinputflood`); an unbound device with bindings elsewhere settles ineligibility once per handle. A device that disappears mid-press gets a synthesized release. Binding capture for these controls currently happens while the 3-second probe window is open.

## Layout reconfirmation (extra buttons)

When a controller's extra-button layout signature changes (firmware update, mode switch), old bindings stop routing and Settings → Input shows **Controller button layout changed** with two actions: **Review buttons** (runs the 3-second probe) and **Confirm current layout** (`InputEngine::confirmModernControllerLayout` → `GameInputRouter::confirmLayouts` → `ExtraButtonCatalog::confirm`). Confirmation clears `needs_reconfirmation` and extra buttons resume with the new layout — there is no permanent silent state.

## Diagnostics

`InputDiagnostics` (`src/input/InputDiagnostics.h`) is the process-wide sink behind the input section of the copied diagnostic summary (Settings → Advanced). It records: the active backend and a timestamped ring of backend switches, every device classification (`tracked`/`ignored` + reason), aggregated event rates, the last logical controls with their source backend, overlay foreground acquisition results, HidHide cloak status, and a "previous session ended unexpectedly" flag backed by a `session.marker` file only an orderly shutdown removes. Everything is bounded (fixed rings), and device paths are reduced to VID/PID plus a short hash — no serials, instance ids, usernames, or local paths leave the machine.

**Button probe.** "Identify a controller button" (Settings → Input) opens a 3-second window in which raw button changes are summarized — including inputs GameHQ normally ignores. During the window the Raw Input backend payload-reads *ignored* handles too, but only ones presenting a Joystick/Gamepad/MultiAxis collection (keyboards, mice and vendor collections are never captured), with one eligibility query per handle and a read budget (`ProbeReadBudget`); XInput reports raw `wButtons` diffs. The budget reads **every** report up to 10 000 a second, so 1, 4 and 8 kHz pads are covered report for report and a 20 ms tap cannot land in a gap — the probe is explicitly requested, lasts three seconds and stores only changed-bit summaries, so completeness is worth its cost (≤30 000 reads for the standard window). Above 10 000 reports a second — several flooding devices at once — it switches to *evenly strided* sampling, every Nth report with N recomputed each 100 ms slice from the previous slice's rate. What it never does is spend an allowance at the start of a slice and go blind for the rest: that is precisely how a short press disappears. Any skipped report is reported in the summary, so a probe that sampled never reads as one that saw everything. `tst_rawinputflood` walks a 20 ms press through every phase of a slice, at 8 kHz and at a 40 kHz flood, and requires a read inside the press each time. Only changed byte positions and XOR bits are recorded, never full reports. When the window closes, the ignored-device fast path returns to exactly one header read + one hash lookup per event (`tst_rawinputflood` pins this). This is the GameSir probe: it ships to every user instead of a side tool.

## Main App Select Mode

In the main app gallery, Select mode uses the standard batch-action mapping: **Cross** toggles the focused capture, **Triangle** selects/deselects all visible captures, **Square** opens the delete confirmation, and **Circle** exits Select mode or cancels the confirmation. The destructive delete is always confirmed through `ConfirmDialog`.

## Mouse extra buttons (lazy global hook, 0.7.6)

Mouse Back/Forward/Middle (`mouse.button4`/`mouse.button5`/`mouse.middle`) are observed through a global `WH_MOUSE_LL` hook (`MouseHookDevice`) that passes every event through unmodified. Two rules keep it harmless (GitHub stutter report — an always-on hook owned by the GUI thread made every main-thread stall a system-wide mouse stall):

- **Lazy activation** (`MouseMonitorPolicy`, `InputEngine::syncMouseMonitoring`): the hook is installed only while at least one `mouse`-group binding exists, or while the binding editor is capturing on the mouse group (a first mouse binding must be observable before any exists). Synced on start, on every binding reload and on every editor state change; users without mouse bindings never get a hook at all. Stopping the hook logically releases any still-held mouse button, so a binding removed mid-hold cannot leave a hold gesture armed.
- **Dedicated hook thread**: Windows delivers every `WH_MOUSE_LL` callback synchronously to the installing thread and the system pointer waits on it, so the hook lives on its own worker thread that does nothing but sleep in `GetMessage()`. Normal thread priority — the thread is idle whenever the hook is. `start()`/`stop()` are idempotent (`tst_mousehooklazy`, including a 25× rapid-cycle case).
- **Generation guard**: every hook event is stamped with the hook lifetime (`MouseHookDevice::generation()`, bumped on every start AND stop). A press queued cross-thread can arrive after the hook stopped — or stopped and restarted, which an `isRunning()` check would wrongly accept — and its release was never captured; InputEngine drops any press whose generation is not current (`tst_mousehooklazy::staleQueuedPressAcrossRestartIsRejected`). Releases process regardless of generation: releasing an unpressed control is a safe no-op, dropping a real one risks a stuck control.

Diagnostics for the remaining suspects ship alongside: `PerfTrace::reportSlow` logs any XInput poll/rescan or WinMM poll pass above 2 ms (rate-limited, `Perf:` prefix), and `EventLoopStallMonitor` logs GUI event-loop stalls ≥ 50 ms the same way, so one user log shows whether hitches, slow backend calls and main-thread stalls line up. The WinMM discovery sweep itself no longer runs on the GUI thread at all (measured at 161 ms; see `WinMMDevice::scanSlots`) — it executes on a short-lived worker with an in-flight guard, and only the result is marshalled back to the owning thread.

## Keyboard fallback (global hotkeys, `RegisterHotKey`)

`Ctrl+Shift+S` screenshot · `Ctrl+Shift+E` save replay · `Ctrl+Shift+G` overlay toggle. The replay buffer itself has no hotkey — it auto-arms while a game is focused (`replay.auto`, Settings → Replay).

**Rebinding is transactional.** OS registration and the database write commit together or both roll back:

1. Remember the chord currently registered for the action/slot.
2. `RegisterHotKey` the new chord **first**. On refusal, show *"This shortcut is already used by Windows or another application."* and abort — the old chord stays live and nothing is written.
3. Persist the displaced rows, then the new row.
4. If any write fails, undo the rows that landed, hand the OS registration back to the previous chord, and show an error.
5. Only a fully committed change refreshes the editor.

Previously the row was written first and `InputEngine` reconciled the hotkey layer afterwards with its return value discarded, so a chord Windows had refused was still saved and displayed as a working shortcut. `HotkeyManager::applyBindingSlot()` already kept the old registration on failure; the missing halves were the write-failure rollback and surfacing the result. Both seams — `BindingEditorModel::setHotkeyApply()` and `setPersistRow()` — are injectable, so `tst_bindingeditor` can force either half to fail without touching a real database or the real keyboard.

Only **keyboard** bindings on **Global**-scope actions with a `press` activation own a Win32 hotkey; controller and mouse edits skip the OS step entirely.

## Hidden pads (HidHide cloak detection, 0.6.3)

Remapping tools (DSX, DS4Windows, reWASD) ship the **Nefarius HidHide** kernel
filter, whose job is hiding the physical pad from every application except the
whitelisted remapper. The cloak lives in the driver, so it can outlast the
remapper's own session — a pad then exists in Device Manager but is invisible
to Raw Input, DirectInput, `joy.cpl`, Steam, and GameHQ alike. No user-mode
enumeration strategy can bypass it, so GameHQ detects and explains it instead:

- **Cross-check.** Every debounced device reconciliation compares present
  `HID\VID_xxxx&PID_xxxx` devnodes from the PnP tree (`CM_Get_Device_ID_ListW`)
  against the interfaces `GetRawInputDeviceList` returns. A supported Sony/DS4
  ID present in PnP but absent from Raw Input means a filter driver is cloaking
  it (`HidCloakMonitor::scan`, reached from `DualSenseDevice::reconcileDevices`
  through `RawInputApi::scanHiddenPads`).
  054C:0ECC is excluded — on PlayStation Link hardware that PID carries only
  vendor-defined collections and cannot be cross-checked meaningfully.
- **Surfacing.** `input.controllerWarning` (Settings → Input, "Controller
  hidden" row) names the pad and the cause; the log gets a matching warning.
- **One-click fix.** When the HidHide class filter is registered
  (`UpperFilters` on the HID class GUID), `input.fixHiddenController()`
  relaunches `GameHQ.exe --hidhide-allow-self` elevated (one UAC prompt). The
  helper appends the exe's NT image path to HidHide's application whitelist
  via the documented `\.\HidHide` control device (IOCTLs 2048/2049, device
  type 32769) and exits; the main instance polls the helper, reports the
  outcome through the same warning row, and rescans. Nothing is installed or
  removed and the remapper keeps working. Changes may require replugging the
  pad — HidHide applies cloak decisions when a device (re)connects.
- **Collection filter.** `probeDevice` requires a Joystick/Gamepad/MultiAxis
  usage before tracking a supported VID/PID; vendor-defined collections on
  Sony hardware (PS Link adapter) are ignored instead of being logged as
  phantom "tracked DualSense" devices.
