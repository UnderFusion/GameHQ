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

## Simplified Chinese calibration decisions

`i18n/quality/reviews/zh-Hans.json` records the complete contextual pass over the
Simplified Chinese catalog and its auxiliary surfaces. The machine draft was structurally
valid but not usable prose, so the review fixed terminology as well as phrasing.

The binding decisions are:

- `replay` is `回放`, never `重播`; `buffer` is `缓冲区`, `clip` is `片段`, `recording` is `录制`.
- `overlay` is `叠加层`, `gallery` is `图库`, the media `library` is `媒体库`, `sidebar` is
  `侧边栏`, and `tray` is `托盘`.
- A saved screenshot or video item is `捕获内容`; the act of capturing is `捕获`. Binding
  detection never reuses the media term.
- `binding` is `绑定`, `assignment` is `分配`, `slot` is `槽位`, `gesture` is `手势`,
  `hold` is `长按`, `tap` is `轻触`, and `combination` is `组合`.
- `controller` is `手柄` throughout the product, matching Mainland gaming convention rather
  than the literal `控制器`.
- Update security vocabulary is fixed: `manifest` is `清单`, `checksum` is `校验和`,
  `signature` is `签名`, `staging` is `暂存`, `transaction` is `事务`, and `trust state` is
  `信任状态`.
- Portable-import vocabulary is fixed: `portable` is `便携`, `staged` is `暂存`, `journal` is
  `日志`, `schema` is `架构`, and `folder` is `文件夹` rather than `文件`.
- DualSense face buttons are `叉键`, `圆圈键`, `方块键`, and `三角键`. `Share`, `View`,
  `Guide`, `PS`, `GameInput`, `XInput`, `DualSense`, `HDR`, and `FPS` stay untranslated.

Sentences use full-width Chinese punctuation; placeholders, key names, hint separators, and
the `portable:/` scheme keep their exact source form.
