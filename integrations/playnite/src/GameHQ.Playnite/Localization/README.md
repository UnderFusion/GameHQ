# Localization

Playnite 10 selects extension localization from package-relative XAML resource
dictionaries. `en_US.xaml` is the complete base/fallback dictionary, XAML views
use `DynamicResource`, and C# resolves keys through `ResourceProvider.GetString`.
`PluginLocalization` falls back to the packaged English dictionary if the host
lookup fails or returns an unresolved key. No GameHQ Qt catalog is loaded.

`locale-map.json` currently maps the twelve implemented plugin locales to Playnite's
underscore filenames. In particular, `zh-Hans` maps to `zh_CN` and `zh-Hant` maps to
`zh_TW`. Playnite owns the active language; the plugin has no language setting.

The owner-approved launch portfolio now also requires `th-TH`, `es-419`, `uk-UA`, and
`it-IT`. They remain explicit unfinished work in `LANG-04H`; that item cannot close until
all sixteen package-local dictionaries, mappings, fallbacks, and tests pass. The static
`extension.yaml` name and add-on
marketplace copy remain protected/independently authored metadata because the
Playnite 10 manifest schema does not resolve extension resource keys.
