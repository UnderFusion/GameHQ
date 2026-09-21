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

## Report header

- Tester, date:
- Windows version (winver):
- GPU and driver version:
- Monitors (count/resolution):
- Games and display modes used:
- Was the reported title (The First Berserker: Khazan) available? yes/no:

## Cases

1. **Open and close.** In a borderless game: PS/Guide tap -> overlay appears
   above the running game. Close it; then repeat with `Ctrl+Shift+G` and with
   Share double-tap. Expected: it opens and closes above the game every time,
   with no alt-tab needed and no desktop window showing.
2. **Foreground preservation.** While the overlay is open, the game must stay
   the OS foreground app: it keeps running/animating, the taskbar does not
   activate, and after closing, input goes straight back to the game.
   Expected: the game never lost focus. Attach the **Copy diagnostic summary**
   (Settings > Advanced) evidence: the overlay facts include the OS-observed
   "game foreground preserved on show" value and the show/hide timeline.
3. **Input escape routes.** With the game holding focus, close the overlay
   using each route that applies: PS/Guide tap, Circle (back/close), Share
   double-tap, `Ctrl+Shift+G`. Expected: every route closes it. Note honestly:
   `Esc`/`Backspace` only work when the overlay itself holds focus - that is
   by design; if you try them, record what actually happened.
4. **Honest input behaviour.** While the overlay is visible, the game keeps
   receiving the same controller presses (GameHQ never suppresses or injects
   input). Observe and report what the game did with a press also bound to an
   overlay action, and confirm the overlay warned about focus if it does.
   Expected: GameHQ fires exactly one of its own actions per press; the game
   still sees its input. A GameHQ double-fire is a FAIL.
5. **Borderless transitions.** With the overlay open: toggle the game
   borderless/fullscreen (for example Alt+Enter) -> the overlay follows the
   replacement window of the same process and stays above it. Minimize the
   game -> the overlay hides. Click the desktop -> the overlay hides. Restore
   and reopen -> everything works again. Record each step separately.
6. **The reported title (Khazan).** If available: play the reported title in
   borderless windowed, open the overlay (PS/Guide or `Ctrl+Shift+G`), browse
   the gallery (nav, seek a clip), close, and continue playing. Report
   explicitly: overlay above the game? freeze or black screen? focus kept?
   input to the game after close? Attach screenshots (and a short video if
   possible). If the title is not available, mark UNRUN and say so - do not
   substitute it silently; still list every other borderless game you tested.

## Evidence to return

- Screenshots of the overlay **above the game** for each title/mode used,
  plus one of the game continuing after close.
- **Copy diagnostic summary** (Settings > Advanced) taken after case 2 and
  after case 6 - it carries overlay visibility, foreground facts, and the
  show/hide timeline.
- `gamehq-data\logs\gamehq.log` from the beta folder - attach on FAIL or any
  unexpected freeze/black screen.
- The completed case table with per-case PASS / FAIL / UNRUN.

## Outcome rules

- Overlay not appearing above a borderless game, focus being stolen from the
  game, freeze/black screen, or GameHQ double-firing one press = FAIL for that
  case, with the log attached.
- The game reacting to presses while the overlay is open is expected honest
  behaviour, not a FAIL.
- Mark UNRUN with a reason when a title or mode was unavailable; never guess.
