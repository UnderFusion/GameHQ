# Contextual translation quality

GameHQ translations convey the English message's meaning and UI function, not a sequence
of isolated words. Stable IDs, developer context, source locations, neighboring strings,
surface types, the protected glossary, and the locale style guide are all normative input.

## Required decisions

- Never use blind word-for-word translation or force English word order onto another
  language.
- Preserve the source role: labels stay concise labels, buttons stay actions, tooltips stay
  explanations, warnings and errors stay natural sentences, and headings stay scannable.
- Select terminology by feature context. `Input`, `binding`, `capture`, `frame`, `focus`,
  `slot`, `buffer`, `clip`, `overlay`, `replay`, and `gallery` do not have one universal
  translation.
- Preserve protected brands, products, APIs, protocols, commands, filenames, configuration
  keys, and other identifiers. Translate the surrounding UI naturally.
- Treat placeholder, plural, markup, accelerator, source-hash, and resource checks as
  structural gates only. Passing them does not prove linguistic quality.

## Polish calibration sample

`i18n/quality/polish-calibration.json` records the representative audit against the actual
QML and C++ locations. It covers actions, labels, explanations, warnings, feature names,
and input-assignment state. The sample deliberately includes both accepted translations
and corrections so the rule is not "always replace the English term."

High-signal decisions include:

- GameHQ's in-game `Overlay` is `nakładka`, not the literal `powierzchnia`.
- A replay `buffer` stores recent gameplay; it is not a playback buffer.
- Controller `capture` in the binding dialog means listening for a control, not recording
  media.
- Window `focus` describes which application is active, not mental concentration.
- A button uses an action form such as `Przywróć sterowanie`; a noun phrase such as
  `Przywrócenie wejścia` changes the control's role.
- `GameInput`, `XInput`, `DualSense`, `HDR`, and `FPS` remain protected technical names.

This calibration does not retranslate the application. Its decisions refine the policy
used by subsequent selective translation and required per-locale linguistic QA.
