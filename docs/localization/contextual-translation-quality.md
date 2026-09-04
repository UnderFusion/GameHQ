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
- `overlay` is `叠加层`, `gallery` is `图库` and a `media gallery` is `媒体图库`, while the
  Settings `library` category is `媒体库`; `sidebar` is `侧边栏` and `tray` is `托盘`. The
  gallery and library terms are never interchanged.
- A saved screenshot or video item is `捕获内容`; the act of capturing is `捕获`. In binding
  and input-assignment surfaces capture is always `捕获手柄输入`, so it cannot be read as
  capturing media from the controller.
- `binding` is `绑定`, `assignment` is `分配`, `slot` is `槽位`, `gesture` is `手势`, and
  `combination` is `组合`. Gesture names form one series: `press` is `按下`, `tap` is `单击`,
  `double tap` is `双击`, `triple tap` is `三击`, and `hold` is `长按`.
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

A targeted recovery pass after the first review corrected nine further units. The About
description said a screenshot was controller-friendly instead of stating that the product
supports controller operation, and it collapsed `media gallery` into the Library term. The
binding dialog's capture notice did not say what was being captured. Both `Toggle Favorite`
labels read as switching a favorites view beside surface toggles such as `切换侧边栏`, so
they now use the explicit `收藏 / 取消收藏` action. The `Tap` gesture was named `轻触` while
its siblings were `双击` and `三击` and the compatibility dialog quoted `单击`, so the whole
series was unified on `单击`.

## Russian calibration decisions

`i18n/quality/reviews/ru-RU.json` records the complete contextual pass over the Russian
catalog and its auxiliary surfaces. The machine draft was grammatical in places but carried
subtitle artefacts, second-person dialogue and reversed actions, so the review fixed
terminology and register as well as phrasing.

The binding decisions are:

- `replay` is `повтор`, `buffer` is `буфер`, `clip` is `клип`, `recording` is `запись`.
- A saved screenshot or video item is `запись`; the act of capturing is `захват`. Binding
  detection never reuses the media term.
- `overlay` is `оверлей`, `gallery` is `галерея`, `library` is `библиотека`, `sidebar` is
  `боковая панель`, and `tray` is `трей`.
- `binding` is `привязка`, `assignment` is `назначение`, `slot` is `слот`, `gesture` is
  `жест`, `hold` is `удержание`, `tap` is `касание`, and `press` is `нажатие`.
- `controller` is `геймпад` throughout the product, matching Russian gaming convention
  rather than the literal `контроллер`.
- Update security vocabulary is fixed: `manifest` is `манифест`, `checksum` is
  `контрольная сумма`, `signature` is `подпись`, `staging` is `подготовка`, `transaction`
  is `транзакция`, and `trust state` is `состояние доверия`.
- Portable-import vocabulary is fixed: `portable` is `портативный`, `staged` is
  `подготовленный`, `journal` is `журнал`, and `instance` is `экземпляр`.
- Key and button names stay untranslated. DualSense face buttons are `Крест`, `Круг`,
  `Квадрат`, and `Треугольник`.

Buttons and action-catalog descriptions use the Russian infinitive; errors and warnings stay
impersonal complete sentences. All three plural forms are checked against the numerus source
rather than assuming a single form.

## European Spanish calibration decisions

`i18n/quality/reviews/es-ES.json` records the complete contextual pass over the European
Spanish catalog and its auxiliary surfaces. The machine draft mixed literal translations,
subtitle fragments, missing clauses, translated key names, and Latin-American or unrelated
word senses, so the review corrected terminology, register, and UI roles.

The binding decisions are:

- `controller` is `mando`; an input `assignment` or `binding` is `asignación`, its `slot` is
  `ranura`, and binding capture means detecting a control rather than recording media.
- Gesture names preserve timing: `Press` is `Pulsación`, release-based `Tap` is `Pulsación
  breve`, `Double Tap` and `Triple Tap` are `Pulsación doble` and `Pulsación triple`, and
  `Hold` is `Mantener pulsado`.
- `replay` is `repetición`, its rolling `buffer` is `búfer de repetición continuo`, a saved
  short video is a `clip`, recording is `grabación`, and ordinary playback is `reproducción`.
- A saved media item is a `captura`, a screenshot is a `captura de pantalla`, and a video
  frame is a `fotograma`. `gallery` is `galería`, `library` is `biblioteca`, `overlay` is
  `superposición`, and the system tray is `bandeja`.
- Update security uses `manifiesto`, `suma de comprobación`, `firma`, `preparación`,
  `transacción`, `estado de confianza`, and `unidad` for a filesystem volume.
- Portable import uses `portátil`, `preparado`, `registro`, `cadena`, `raíz`, `origen`, and
  `instancia`; the `portable:/` scheme stays literal.

The locale uses neutral Spain Spanish, informal address, concise infinitive action labels,
natural word order, and Spanish punctuation and agreement. Shortcut key names, protected
technical identifiers, paths, commands, placeholders, and version strings remain unchanged.

## Brazilian Portuguese calibration decisions

`i18n/quality/reviews/pt-BR.json` records the complete contextual pass over the Brazilian
Portuguese catalog and its auxiliary surfaces. The machine draft mixed European Portuguese,
subtitle fragments, missing clauses, translated key names, and unrelated literal senses.

The binding decisions are:

- `controller` is `controle`; an input `assignment` is `atribuição`, its `slot` is `posição`,
  and input capture is `detecção de entrada`, never media capture.
- Gesture timing stays explicit: immediate `Press` is `Pressionar`, release-based `Tap` is
  `Pressionamento breve`, `Double Tap` and `Triple Tap` are `Pressionamento duplo` and
  `Pressionamento triplo`, and `Hold` is `Manter pressionado`; a completed hold suppresses the
  short-press action.
- `Replay` names the feature, its rolling buffer is `buffer contínuo`, a saved video is a
  `clipe`, recording is `gravação`, and ordinary playback is `reprodução`.
- A saved media item is `captura`, a screenshot is `captura de tela`, a frame is `quadro`,
  `gallery` is `galeria`, `library` is `biblioteca`, `overlay` is `sobreposição`, and the
  system tray is `bandeja`.
- Update security uses `manifesto`, `soma de verificação`, `assinatura`, `preparação`,
  `transação`, `estado de confiança`, and `unidade`.
- Portable import uses `portátil`, `preparado`, `registro`, `cadeia de caracteres`, `raiz`,
  `origem`, `instância`, `banco de dados`, and `chave estrangeira`; `portable:/` stays literal.

The locale uses neutral Brazilian Portuguese, concise infinitive actions, `computador`,
`arquivo`, and `pasta`, and rejects European forms such as `ficheiro`, `ecrã`, and `ordenador`.
Protected identifiers, shortcut keys, button names, paths, placeholders, and versions remain
unchanged.

## German calibration decisions

`i18n/quality/reviews/de-DE.json` records the complete contextual review of the German
application catalog and auxiliary surfaces. The draft was structurally valid but contained
literal compounds, conversational fragments, incorrect technical senses, and translated key
names.

The binding decisions are:

- `Controller` is retained; an input assignment is a `Belegung`, its position is a `Slot`,
  and input capture is `Controller-Eingabe erfassen`, never media capture.
- Gesture timing remains explicit: immediate `Press` is `Drücken`, release-based `Tap` is
  `Kurz drücken`, double and triple tap are `Doppelt drücken` and `Dreifach drücken`, and
  `Hold` is `Gedrückt halten`; a completed hold prevents the short-press action.
- `Replay` names the feature, its rolling store is the `Replay-Puffer`, a saved video is a
  `Clip`, recording is `Aufnahme`, and ordinary playback is `Wiedergabe`.
- A saved media item is an `Aufnahme`, a screenshot is a `Screenshot`, and a video frame is
  an `Einzelbild`. `Gallery` is `Galerie`, `Library` is `Bibliothek`, `Overlay` remains
  `Overlay`, and the system tray is the `Infobereich`.
- Update security uses `Manifest`, `Prüfsumme`, `Signatur`, `Vorbereitung`, `Transaktion`,
  `Vertrauensstatus`, and `Laufwerk`; database foreign keys are `Fremdschlüssel`.
- Portable import uses `portabel`, `vorbereitet`, `Journal`, `Zeichenkette`, `Stammordner`,
  `Quelle`, and `Instanz`; the `portable:/` scheme remains literal.

German nouns are capitalized, action labels use concise verbs, explanatory text uses neutral
formal or impersonal phrasing, and compounds are formed naturally. Protected identifiers,
shortcut keys, button names, paths, placeholders, and versions remain unchanged.

## Japanese calibration decisions

`i18n/quality/reviews/ja-JP.json` records the complete contextual review of the Japanese
application catalog and auxiliary surfaces. The draft was structurally valid but contained
literal word senses, subtitle fragments, incorrect controls, and translated key names.

The binding decisions are:

- `controller` is `コントローラー`; input assignment is `割り当て`, its position is a
  `スロット`, and input capture means detecting `入力`, never recording media.
- Gesture timing remains explicit: immediate `Press` is `押す`, release-based `Tap` is
  `短押し`, double and triple press are `2回押し` and `3回押し`, and `Hold` is `長押し`;
  a completed hold prevents the short-press action.
- `リプレイ` names the feature, its rolling store is the `リプレイバッファ`, a saved video
  is a `クリップ`, recording is `録画`, and ordinary playback is `再生`.
- A saved media item is a `キャプチャ`, a screenshot is a `スクリーンショット`, and a
  video frame is a `フレーム`. `gallery` is `ギャラリー`, `library` is `ライブラリ`,
  `overlay` is `オーバーレイ`, and the system tray is `通知領域`.
- Update security uses `マニフェスト`, `チェックサム`, `署名`, `準備`, `トランザクション`,
  and `信頼状態`; database foreign keys are `外部キー`.
- Portable import uses `ポータブル`, `準備済み`, `ジャーナル`, `文字列`, `ルート`,
  `インポート元`, and `インスタンス`; the `portable:/` scheme remains literal.

Controls use concise labels without unnecessary polite endings, while explanatory prose uses
consistent polite Japanese. Protected identifiers, shortcut keys, button names, paths,
placeholders, and versions remain unchanged.
