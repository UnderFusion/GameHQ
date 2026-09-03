# Localization

Playnite-native localization dictionaries belong here. The plugin now has a
settings UI, menu items, dialogs, validation, status text, and diagnostic labels
to localize, so canonical follow-up `t1 / LANG-04H` owns that implementation.

The plugin must consume Playnite's active UI locale and resource mechanism. It
must not load GameHQ's Qt `.qm` catalogs. A complete English dictionary remains
the final fallback and must be packaged inside the `.pext` with every enabled
locale dictionary.
