import QtQuick
import GameHQ
import "../components"

SettingsPage {
    //% "General"
    pageTitle: qsTrId("gamehq.settings.general.title")
    //% "Appearance, startup, and window behavior."
    pageDescription: qsTrId("gamehq.settings.general.description")

    SettingsSection {
        //% "Language"
        eyebrow: qsTrId("gamehq.settings.language.eyebrow")
        //% "Interface language"
        title: qsTrId("gamehq.settings.language.title")
        //% "Choose the language GameHQ uses."
        description: qsTrId("gamehq.settings.language.description")

        SettingsRow {
            //% "Display language"
            label: qsTrId("gamehq.settings.language.label")
            //% "Changes apply immediately throughout GameHQ."
            description: qsTrId("gamehq.settings.language.row_description")

            SettingsCombo {
                id: languageCombo
                objectName: "languageSelector"
                defaultValue: languageManager.requestedLanguage
                options: [{
                    //% "System language"
                    label: qsTrId("gamehq.settings.language.system"),
                    value: "system"
                }].concat(languageManager.availableLanguages.map(function (locale) {
                    return { label: locale.nativeName, value: locale.tag }
                }))
                onValueCommitted: function(value) {
                    languageManager.requestedLanguage = value
                    // The setter is synchronous. Refreshing here also restores
                    // the previous selection when a replacement catalog fails.
                    languageCombo.refresh()
                }
            }
        }
    }

    SettingsSection {
        //% "Personalization"
        eyebrow: qsTrId("gamehq.settings.general.personalization.eyebrow")
        //% "Look and feel"
        title: qsTrId("gamehq.settings.general.personalization.title")
        //% "Choose the visual style and in-game overlay strength."
        description: qsTrId("gamehq.settings.general.personalization.description")
        SettingsRow {
            id: themeRow
            //% "Theme"
            label: qsTrId("gamehq.settings.general.theme.label")
            // Track the picker rather than the live skin: this should describe
            // what is selected in the combo, which is the same thing, but the
            // intent is the selection, not whatever is currently painting.
            description: {
                const match = Theme.availableSkins.filter(function (s) {
                    return s.key === Theme.activeSkin
                })
                if (match.length)
                    return match[0].blurb
                //% "Choose how %1 looks."
                return qsTrId("gamehq.settings.general.theme.fallback").arg(Brand.name)
            }
            SettingsCombo {
                configKey: "theme.active_skin"
                defaultValue: "obsidian"
                options: Theme.availableSkins.map(function (s) {
                    return { label: s.label, value: s.key }
                })
            }
        }
        SettingsRow {
            //% "Overlay dimming"
            label: qsTrId("gamehq.settings.general.overlay_dimming.label")
            //% "How strongly the in-game overlay darkens the game behind it. 100% is the theme's own dimming; lower values keep more of the game visible."
            description: qsTrId("gamehq.settings.general.overlay_dimming.description")
            SettingsSlider {
                configKey: "theme.overlay_scrim_strength"
                defaultValue: 100
                from: 25
                to: 150
                stepSize: 5
            }
        }
    }

    SettingsSection {
        //% "Startup"
        eyebrow: qsTrId("gamehq.settings.general.startup.eyebrow")
        //% "How %1 starts"
        title: qsTrId("gamehq.settings.general.startup.title").arg(Brand.name)
        //% "Choose whether GameHQ starts when you sign in to Windows and opens quietly."
        description: qsTrId("gamehq.settings.general.startup.description")
        SettingsRow {
            //% "Start with Windows"
            label: qsTrId("gamehq.settings.general.startup.windows.label")
            //% "Register %1 for the current Windows user; no administrator access is required."
            description: qsTrId("gamehq.settings.general.startup.windows.description").arg(Brand.name)
            SettingsToggle { configKey: "startup.enabled"; defaultValue: false }
        }
        SettingsRow {
            //% "Launch minimized"
            label: qsTrId("gamehq.settings.general.startup.minimized.label")
            //% "Launch directly in the system tray without opening the main window."
            description: qsTrId("gamehq.settings.general.startup.minimized.description")
            SettingsToggle { configKey: "startup.minimized"; defaultValue: false }
        }
    }

    SettingsSection {
        //% "Desktop"
        eyebrow: qsTrId("gamehq.settings.general.desktop.eyebrow")
        //% "Window and tray behavior"
        title: qsTrId("gamehq.settings.general.desktop.title")
        //% "Choose what happens when the main window is minimized or closed."
        description: qsTrId("gamehq.settings.general.desktop.description")
        SettingsRow {
            //% "Minimize to tray"
            label: qsTrId("gamehq.settings.general.desktop.minimize_to_tray.label")
            //% "Minimizing the window sends it straight to the tray instead of the taskbar."
            description: qsTrId("gamehq.settings.general.desktop.minimize_to_tray.description")
            SettingsToggle { configKey: "tray.minimize_to_tray"; defaultValue: false }
        }
        SettingsRow {
            //% "Close to tray"
            label: qsTrId("gamehq.settings.general.desktop.close_to_tray.label")
            //% "Keep capture and replay services running; when disabled, Close exits %1."
            description: qsTrId("gamehq.settings.general.desktop.close_to_tray.description").arg(Brand.name)
            SettingsToggle { configKey: "tray.close_to_tray"; defaultValue: true }
        }
    }
}
