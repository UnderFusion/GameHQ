# Feedback-wave beta - wired DualSense checklist (cpo-h01)

Purpose: confirm controller stability and mapping presets on a **real wired
DualSense** against the feedback-wave beta package. Fill every case with
PASS / FAIL / UNRUN plus a short observation. PASS requires a direct
observation - never infer a case from another one or from the automated gate.

Identity: verify the ZIP SHA-256 against the adjacent `.sha256` file and copy
the version, source commit and package value from `beta-manifest.json`.

## Report header

- Tester, date:
- Windows version (winver):
- DualSense model (standard / Edge) and firmware:
- Connection: USB (port), nothing wireless attached? yes/no:
- HidHide / DSX / ViGEm / Steam Input or any remapper active? (list):
- Games used:

## Prerequisites

1. If DSX + HidHide is installed with hiding active, add `GameHQ.exe` from the
   beta `app\` folder to the HidHide application whitelist first, otherwise
   Raw Input sees nothing (documented caveat).
2. Start the beta, open **Settings > Input**. Keep **Modern controller
   support** at its default (Auto) and note the runtime status line.

## Cases

1. **Identity and provider.** With the pad connected by USB, run the 3-second
   Controller Probe in Settings > Input and press every button once, including
   Share/Create and PS/Guide. Record: which controls were recognized, which
   were not, and the provider shown for the pad (Raw Input / GameInput /
   XInput / WinMM) with any switch reason. Expected: one pad listed with a
   stable identity during the session; no provider churn while idle and
   playing.
2. **Share button behaviour.** In-game (or in the gallery): tap Share once ->
   one screenshot appears in the gallery; hold Share -> one replay clip is
   saved. Expected: exactly one item per gesture, no double fire, visible
   feedback promptly.
3. **PS/Guide.** Tap PS/Guide once. Expected: the overlay opens/closes
   cleanly (detailed overlay behaviour is cpo-h02). Also note honestly: the
   game still sees the press by design - only GameHQ's own routing is under
   test.
4. **One press, one action.** In Settings > Input, bind one explicit action
   (for example "Save replay clip") to a single control, then use tap / hold
   and (if the editor offers them) double/triple tap. Expected: exactly the
   intended action fires per gesture; no stray second action; no action stuck
   on release.
5. **Mapping presets on the wired pad.** In Settings > Input > Mapping
   presets: create a named preset, change one row to something observable,
   and assign it to the game you are about to play. Expected: the assignment
   applies in that game session and applies immediately (no restart).
   Switching the game assignment back to "Follow fallback (no preset)"
   removes that game-specific preset assignment and makes the game follow the
   device/controller fallback mappings; if no higher fallback mapping applies,
   built-in defaults are used. Record which rows applied and where they did
   not.
6. **Preset deletion safety (spot check).** Duplicate a preset, assign the
   copy, then delete it through the delete dialog. Expected: every
   assignment/reference that used the deleted preset is moved to the
   replacement preset selected in the dialog; no assignment is left broken or
   silently missing, and bindings still work afterwards.
7. **Disconnect stability.** Hold a bound button and unplug the USB cable;
   reconnect. Expected: no action remains stuck, the same pad keeps its saved
   profile, and another press works immediately. Repeat once, also plugging
   into a different USB port - record whether the identity/profile follows the
   pad (strong-identity behaviour) and report either outcome honestly.

## Evidence to return

- **Copy controller compatibility report** (Settings > Input) - paste it with
  the case results.
- **Copy diagnostic summary** (Settings > Advanced) - it contains the
  provider/routing facts and the preset assignment state used above.
- `gamehq-data\logs\gamehq.log` from the beta folder - attach only if a case
  failed or something looked wrong.
- The screenshot and clip produced in case 2 (attach the files).
- A screenshot of Settings > Input showing the pad's provider line as used in
  case 1.

## Outcome rules

- Any double fire, stuck action, lost profile for the same wired pad, or
  provider churn without a real hardware event = FAIL for that case.
- A control that the probe shows as unknown is a reportable observation, not
  automatically a FAIL - list it.
- Mark UNRUN with a reason when hardware or a game did not allow the case
  (for example no spare USB port), and never guess a result.
