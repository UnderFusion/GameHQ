import QtQuick
import QtQuick.Layouts
import GameHQ
import "../components"

// Settings > Sharing (t21): every Share switch in one place. The provider list
// comes from shareService, so a provider added later (Telegram Integrated...)
// shows up here with no QML change. Turning a provider off only hides it in
// Share; its credentials, session and saved destinations stay until the user
// disconnects or removes them.
SettingsPage {
    id: root
    //% "Sharing"
    pageTitle: qsTrId("gamehq.settings.sharing.title")
    //% "Choose where captures can be shared. Nothing is ever sent without you picking a capture and a destination."
    pageDescription: qsTrId("gamehq.settings.sharing.description")

    property var providers: shareService.providerSettings()
    function refresh() { root.providers = shareService.providerSettings() }

    function authText(auth) {
        switch (auth) {
        case "connected":
            //% "Connected"
            return qsTrId("gamehq.settings.sharing.auth.connected")
        case "connecting":
            //% "Connecting…"
            return qsTrId("gamehq.settings.sharing.auth.connecting")
        case "error":
            //% "Connection problem"
            return qsTrId("gamehq.settings.sharing.auth.error")
        }
        //% "Not connected"
        return qsTrId("gamehq.settings.sharing.auth.disconnected")
    }

    Connections {
        target: shareService
        function onProvidersChanged() { root.refresh() }
        function onEnablementChanged() { root.refresh() }
    }

    SettingsSection {
        //% "Sharing"
        title: qsTrId("gamehq.settings.sharing.master.title")
        SettingsRow {
            //% "Enable Sharing"
            label: qsTrId("gamehq.settings.sharing.master.label")
            //% "Turns the Share option on or off everywhere: gallery, lightbox and overlay."
            description: qsTrId("gamehq.settings.sharing.master.description")
            showDivider: false
            SettingsToggle {
                configKey: "share.enabled"
                defaultValue: true
            }
        }
    }

    SettingsSection {
        visible: root.providers.length > 0
        opacity: shareService.sharingEnabled ? 1 : 0.5
        //% "Destinations"
        title: qsTrId("gamehq.settings.sharing.providers.title")
        //% "Turning a destination off only hides it. Saved accounts and channels are kept until you disconnect or remove them."
        description: qsTrId("gamehq.settings.sharing.providers.description")

        Repeater {
            model: root.providers
            delegate: ColumnLayout {
                id: entry
                required property var modelData
                required property int index
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                spacing: 0

                SettingsRow {
                    Layout.fillWidth: true
                    label: entry.modelData.name
                    description: entry.modelData.requiresAccount
                                 ? root.authText(entry.modelData.auth)
                                 : (entry.modelData.available ? entry.modelData.privacy
                                                              : entry.modelData.reason)
                    showDivider: entry.modelData.requiresAccount || entry.index < root.providers.length - 1
                    SettingsToggle {
                        enabled: shareService.sharingEnabled
                        configKey: "share.provider." + entry.modelData.id + ".enabled"
                        defaultValue: true
                    }
                }
                SettingsRow {
                    Layout.fillWidth: true
                    visible: entry.modelData.requiresAccount && entry.modelData.auth !== "disconnected"
                    //% "Disconnect and remove the saved session"
                    label: qsTrId("gamehq.settings.sharing.disconnect.label")
                    //% "Signs GameHQ out and deletes its local session. Turning the destination off does not do this."
                    description: qsTrId("gamehq.settings.sharing.disconnect.description")
                    compact: true
                    showDivider: entry.index < root.providers.length - 1
                    AccentButton {
                        //% "Disconnect"
                        label: qsTrId("gamehq.settings.sharing.disconnect.action")
                        onClicked: {
                            shareService.disconnectProvider(entry.modelData.id)
                            sounds.play("confirm")
                        }
                    }
                }
            }
        }
    }

    TelegramAccountSection { }

    ShareDestinationsSection { }

    SettingsSection {
        //% "Share add-ons"
        title: qsTrId("gamehq.share.addons.title")
        SettingsRow {
            //% "Allow add-ons from other programs"
            label: qsTrId("gamehq.share.addons.label")
            //% "Other programs running as you on this PC can then offer Share destinations and receive the captures you choose to share. Off by default. Restart GameHQ to apply."
            description: qsTrId("gamehq.share.addons.description")
            showDivider: false
            SettingsToggle {
                configKey: "share.external_providers"
                defaultValue: false
            }
        }
    }
}
