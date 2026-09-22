# Feedback-wave beta - borderless overlay checklist (cpo-h02)

Purpose: confirm the overlay behaviour in **real borderless games, including
the reported title**, against the feedback-wave beta package. Fill every case
with PASS / FAIL / UNRUN plus a short observation. PASS requires a direct
observation with screenshots where the case asks for them.

Identity: verify the ZIP SHA-256 against the adjacent `.sha256` file and copy
the version, source commit and package value from `beta-manifest.json`.

Scope: borderless-windowed (and windowed) games. Exclusive fullscreen is not
guaranteed by design - do not report exclusive-fullscreen results as a FAIL,
just note what happened.

Read this before judging case 3: since the controller-isolation work the overlay
asks GameInput for exclusive input while it is interactive, and that policy binds
**other GameInput clients**. A title reading the pad through XInput, Raw Input, a
virtual pad or Steam Input is outside it. `docs/overlay.md` ("Controller
isolation: what is verified, and what is not") lists what is measured and what is
not. A game that reacts to overlay navigation is therefore a **finding about that
title's input path** - record it, do not fail it - while a *double* GameHQ action
from one press is a defect.

## Report header

- Tester, date:
- Windows version (winver):
- GPU and driver version:
- Monitors (count/resolution):
- Games and display modes used:
- Was the reported title (The First Berserker: Khazan) available? yes/no:
- Controller(s) and connection (wired DualSense / XInput pad / other, USB or wireless):
- Remapper or virtual-pad tools running (Steam Input on/off, DSX, JoyXoff, ...) - name them:
- GameHQ build identity from `beta-manifest.json` (version, source commit, package SHA-256):

## Cases

1. **Open above the game.** In a borderless game: PS/Guide tap -> overlay appears
   above the running game and the game stays **visible and not minimised** behind
   it. Close it; then repeat with `Ctrl+Shift+G` and with the Share double-tap.
   Expected: it opens and closes above the game every time, with no alt-tab
   needed, no desktop window showing and the game never minimised.
2. **The overlay takes the foreground.** While the overlay is open the overlay is
   the active window: its own focus indicator clears, keyboard navigation and
   `Esc` work, and the game keeps running/animating behind it. Expected: the
   overlay owns the foreground, the game is not minimised, and nothing is
   injected into the game. If the overlay says it could not get the foreground,
   record that - it is a legitimate outcome, not a FAIL.
3. **Navigation does not act in the game.** With a game in the foreground and the
   overlay open, use the D-pad, the left stick and the face buttons in the
   overlay. Record for the title: did the overlay act on the press (it should),
   and did the **game** act on the same press (yes/no + what it did). Attach the
   **Copy diagnostic summary** (Settings > Advanced): its open record states the
   isolation level (`isolation=...`) for that open. FAIL only if one press
   produces two GameHQ actions.
4. **Close with a control held.** Repeat three times: hold a face button, then a
   trigger, then a stick direction pushed outside the deadzone, and close the
   overlay while it is still held (PS/Guide tap and Circle). Release only after
   the overlay is gone. Record whether the game acted on the held control.
   Expected: no leaked action - GameHQ waits, briefly, for the controller to be
   at rest before handing it back. Attach the diagnostic summary: the close line
   carries the receipt (`neutral-handoff=passed duration_ms=...`, `=timeout ...`,
   `=released-elsewhere ...` or `=not-engaged (...)`). A `timeout` is an honest
   outcome to report, not a reason to hide the case.
5. **Foreground returns to the game.** After closing, the game is the foreground
   window again, it keeps running, and input goes straight back to it without a
   click or an alt-tab.
6. **The controller still works after close.** In the game: a bound control acts,
   and the GameHQ capture hotkey still saves a capture. Repeat after a close that
   reported `neutral-handoff=timeout`.
7. **External foreground changes are not stolen.** With the overlay open, change
   the foreground yourself - Alt-Tab to another application, or click the
   desktop - then close the overlay. Expected: GameHQ hides and does **not** pull
   the foreground back to the game or to itself; whatever you chose stays in
   front. Record what happened, with the diagnostic summary.
8. **GameHQ exit leaves the controller usable.** Quit GameHQ normally while a
   game is running, then use the controller in the game: it must work
   immediately, with no stuck direction, no stuck button and no restart needed.
   (A separate, still-open question is what an *abnormally killed* GameHQ leaves
   behind; note it if you try it, but case 8 is about a normal quit.)
9. **Natural pause-on-focus-loss (metadata only).** For each title, record
   whether the game paused on its own when the overlay took the foreground:
   yes/no + what it looked like. Never a FAIL.
10. **Opening gesture, reported separately.** Note what happens in the game when
    you press PS/Guide to *open* the overlay (for example a platform overlay or a
    menu appearing). This is a separate question from case 3 and is never a FAIL
    of the overlay - just record it per title.
11. **Repeated cycles.** Open and close the overlay 10 times in one game session.
    Expected: everything above still holds at the end, no degradation, no stuck
    input, no accumulating window or lag.
12. **The reported title (Khazan).** If available: play the reported title in
    borderless windowed and report explicitly - overlay above the game? game
    minimised? overlay navigation affecting the game? close with a held control
    affecting the game? focus after close? freeze or black screen? natural pause
    behaviour? Attach screenshots (and a short video if possible) plus the
    diagnostic summary taken after case 4 and after this case. If the title is
    not available, mark UNRUN and say so - do not substitute it silently.
13. **One more borderless title.** At least one additional borderless title, with
    the same short rows as case 12. Names and results only, no guessing: two
    titles are worth more than one title measured twice.

## Evidence to return

- Screenshots of the overlay **above the game** for each title/mode used, plus
  one of the game continuing after close.
- **Copy diagnostic summary** (Settings > Advanced) taken after case 3, after
  case 4, after case 7 and after case 12 - it carries overlay visibility,
  foreground facts, the isolation classification, the focus-policy timeline, the
  handoff receipt and the show/hide timeline.
- `gamehq-data\logs\gamehq.log` from the beta folder - attach on FAIL or any
  unexpected freeze/black screen.
- The completed case table with per-case PASS / FAIL / UNRUN and the observed
  input-path facts for every title that reacted to overlay navigation.

## Outcome rules

- Overlay not appearing above a borderless game, the game being minimised or
  restarted by the overlay, a freeze/black screen, GameHQ firing two of its own
  actions for one press, the foreground being stolen back after you deliberately
  moved it elsewhere, or the controller being dead after a normal GameHQ exit =
  FAIL for that case, with the log attached.
- A held control leaking into the game at the moment of close while the record
  says `neutral-handoff=passed` = FAIL, and attach the log: the receipt and the
  observation disagree, which is exactly what this case exists to catch.
- A game acting on presses aimed at the overlay when Steam Input or a virtual-pad
  tool was translating them: record it and the configuration; it is a scoped
  finding, not a defect of the overlay - unless the tool was off and the title is
  a plain GameInput client, in which case say so explicitly.
- The game pausing naturally, and the opening gesture being seen by the game, are
  **not** FAILs - they are the metadata this checklist asks for.
- Mark UNRUN with a reason when a title, a mode or a controller was unavailable;
  never guess, and never upgrade an observation you did not make.
