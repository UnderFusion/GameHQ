# Localization

Playnite 10 selects extension localization from package-relative XAML resource
dictionaries. `en_US.xaml` is the complete base/fallback dictionary, XAML views
use `DynamicResource`, and C# resolves keys through `ResourceProvider.GetString`.
`PluginLocalization` falls back to the packaged English dictionary if the host
lookup fails or returns an unresolved key. No GameHQ Qt catalog is loaded.

`locale-map.json` maps all sixteen GameHQ launch locales to Playnite's underscore
filenames. In particular, `zh-Hans` maps to `zh_CN`, `zh-Hant` maps to `zh_TW`, and
the Latin American `es-419` portfolio entry maps to the Playnite-compatible `es_MX`
resource name. Playnite owns the active language; the plugin has no language setting.

Playnite 10 currently exposes neither Thai nor Latin American Spanish as host UI
choices. Their complete `th_TH` and `es_MX` dictionaries are packaged so the native
loader can use them when the host exposes those locales. Until then, Playnite's normal
base-language behavior and `PluginLocalization` safely resolve unavailable resources
or keys through the package-local `en_US` dictionary. Italian and Ukrainian use the
host's existing `it_IT` and `uk_UA` locale names.

The static `extension.yaml` name, add-on marketplace description, installer changelog,
and packaged Markdown help remain protected or independently authored English metadata
because Playnite 10 does not resolve localization keys in those schemas. They are not
interactive plugin runtime text.
