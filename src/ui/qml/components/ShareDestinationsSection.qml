import QtQuick
import QtQuick.Controls.Basic as QC
import QtQuick.Layouts
import GameHQ

// Settings for Share destinations the user configures themselves (Discord
// channel webhooks today). Generic on purpose: it lists every provider that
// reports saved destinations through `shareService` and shows nothing when
// there is none, so a new provider of that kind needs no QML here. The secret
// (webhook link) is typed once, goes straight to the provider and is never
// shown again or handed back.
SettingsSection {
    id: root

    property var providers: shareService.savedDestinationProviders()

    function refresh() {
        root.providers = shareService.savedDestinationProviders()
    }

    function errorText(code) {
        switch (code) {
        case "invalid_name":
            //% "Enter a short name for this channel."
            return qsTrId("gamehq.share.settings.error.invalid_name")
        case "invalid_secret":
            //% "That is not a Discord webhook link."
            return qsTrId("gamehq.share.settings.error.invalid_secret")
        case "too_many":
            //% "You have reached the limit of saved channels."
            return qsTrId("gamehq.share.settings.error.too_many")
        }
        //% "Couldn't save the channel. Try again."
        return qsTrId("gamehq.share.settings.error.storage_failed")
    }

    visible: providers.length > 0
    //% "Share destinations"
    title: qsTrId("gamehq.share.settings.title")
    //% "Channels that Share can post to. Webhook links are kept in Windows Credential Manager, not in GameHQ's settings."
    description: qsTrId("gamehq.share.settings.description")

    Connections {
        target: shareService
        function onProvidersChanged() { root.refresh() }
    }

    Repeater {
        model: root.providers
        delegate: ColumnLayout {
            id: provider
            required property var modelData
            property string formError: ""

            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: Theme.s8

            Text {
                text: provider.modelData.name
                color: Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
                Layout.fillWidth: true
            }
            Text {
                visible: provider.modelData.privacy.length > 0
                text: provider.modelData.privacy
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            Repeater {
                model: provider.modelData.destinations
                delegate: SettingsRow {
                    required property var modelData
                    label: modelData.name
                    compact: true
                    AccentButton {
                        // Pinned destinations are listed first in Share.
                        label: modelData.pinned
                               //% "Unpin"
                               ? qsTrId("gamehq.share.settings.unpin")
                               //% "Pin"
                               : qsTrId("gamehq.share.settings.pin")
                        onClicked: {
                            shareService.setSavedDestinationPinned(
                                provider.modelData.id, modelData.id, !modelData.pinned)
                            sounds.play("confirm")
                        }
                    }
                    AccentButton {
                        //% "Remove"
                        label: qsTrId("gamehq.action.remove")
                        onClicked: {
                            shareService.removeSavedDestination(provider.modelData.id, modelData.id)
                            sounds.play("confirm")
                        }
                    }
                }
            }

            Text {
                //% "Name"
                text: qsTrId("gamehq.settings.presets.name_label")
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontCaption
            }
            QC.TextField {
                id: nameField
                Layout.fillWidth: true
                maximumLength: 48
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontBody
                color: Theme.text
                selectionColor: Theme.accent
                selectedTextColor: Theme.textOnAccent
                background: Rectangle {
                    radius: Theme.radiusM
                    color: Theme.bg1
                    border.width: nameField.activeFocus ? Theme.borderWidth + 1 : 1
                    border.color: nameField.activeFocus ? Theme.focusRing : Theme.borderLight
                }
            }
            Text {
                //% "Webhook link"
                text: qsTrId("gamehq.share.settings.webhook_label")
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontCaption
            }
            QC.TextField {
                id: secretField
                Layout.fillWidth: true
                echoMode: TextInput.Password   // a credential: not shown on screen
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontBody
                color: Theme.text
                selectionColor: Theme.accent
                selectedTextColor: Theme.textOnAccent
                background: Rectangle {
                    radius: Theme.radiusM
                    color: Theme.bg1
                    border.width: secretField.activeFocus ? Theme.borderWidth + 1 : 1
                    border.color: secretField.activeFocus ? Theme.focusRing : Theme.borderLight
                }
                Keys.onReturnPressed: addButton.clicked()
                Keys.onEnterPressed: addButton.clicked()
            }
            Text {
                visible: provider.formError.length > 0
                text: provider.formError
                color: Theme.danger
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            AccentButton {
                id: addButton
                primary: true
                //% "Save"
                label: qsTrId("gamehq.action.save")
                onClicked: {
                    const code = shareService.addSavedDestination(
                        provider.modelData.id, nameField.text, secretField.text)
                    provider.formError = code.length > 0 ? root.errorText(code) : ""
                    if (code.length === 0) {
                        nameField.text = ""
                        secretField.text = ""   // the link is not kept in the UI
                    }
                    sounds.play(code.length > 0 ? "error" : "confirm")
                }
            }
        }
    }
}
