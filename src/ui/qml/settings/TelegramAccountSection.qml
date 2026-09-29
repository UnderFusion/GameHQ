import QtQuick
import QtQuick.Controls.Basic as QC
import QtQuick.Layouts
import GameHQ
import "../components"

// Telegram account setup for Share (t23). One step at a time: developer
// credentials, then Connect, then whatever Telegram asks for (phone, code,
// two-step password). The phone number, code and password are typed into
// GameHQ once, handed straight to Telegram's library and never stored or
// logged. Disconnecting is a separate explicit action on the provider row.
SettingsSection {
    id: root

    readonly property string accountState: telegramAccount.state
    readonly property bool busy: accountState === "starting" || accountState === "logging_out"

    function errorText(code) {
        switch (code) {
        case "":
            return ""
        case "no_credentials":
            //% "Enter the API ID and API hash first."
            return qsTrId("gamehq.settings.telegram.error.no_credentials")
        case "runtime_not_installed":
        case "runtime_unpinned":
            //% "The optional Telegram component is not installed in this GameHQ."
            return qsTrId("gamehq.settings.telegram.error.no_runtime")
        case "runtime_hash_mismatch":
        case "runtime_incompatible":
        case "runtime_load_failed":
            //% "The Telegram component is not the version GameHQ expects, so it was not loaded."
            return qsTrId("gamehq.settings.telegram.error.bad_runtime")
        case "phone_invalid":
            //% "Telegram does not accept that phone number. Use the international format, for example +48 123 456 789."
            return qsTrId("gamehq.settings.telegram.error.phone_invalid")
        case "code_invalid":
            //% "That code is not correct."
            return qsTrId("gamehq.settings.telegram.error.code_invalid")
        case "code_expired":
            //% "That code has expired. Cancel and start again to get a new one."
            return qsTrId("gamehq.settings.telegram.error.code_expired")
        case "password_invalid":
            //% "That password is not correct."
            return qsTrId("gamehq.settings.telegram.error.password_invalid")
        case "rate_limited":
            //% "Telegram asks you to wait before trying again."
            return qsTrId("gamehq.settings.telegram.error.rate_limited")
        case "credentials_rejected":
            //% "Telegram rejected the API ID or API hash."
            return qsTrId("gamehq.settings.telegram.error.credentials_rejected")
        case "unsupported_auth":
            //% "This account needs a sign-in step GameHQ does not support. Use Telegram Desktop sharing instead."
            return qsTrId("gamehq.settings.telegram.error.unsupported_auth")
        case "invalid_api_id":
            //% "The API ID is a number."
            return qsTrId("gamehq.settings.telegram.error.invalid_api_id")
        case "invalid_api_hash":
            //% "The API hash is 32 letters and digits."
            return qsTrId("gamehq.settings.telegram.error.invalid_api_hash")
        }
        //% "Couldn't connect to Telegram. Check your connection and try again."
        return qsTrId("gamehq.settings.telegram.error.generic")
    }

    property string formError: ""

    //% "Telegram account"
    title: qsTrId("gamehq.settings.telegram.title")
    //% "Send captures from GameHQ straight to your Telegram contacts. GameHQ shows no chats or messages and sends nothing you did not pick. This signs in to your whole Telegram account, so only connect an account you are comfortable with."
    description: qsTrId("gamehq.settings.telegram.description")

    // Step 1: the developer credentials of this GameHQ build.
    ColumnLayout {
        visible: !telegramAccount.hasCredentials
        Layout.fillWidth: true
        spacing: Theme.s8

        Text {
            //% "Create an API ID and API hash for GameHQ at my.telegram.org (API development tools), then enter them here. They are kept in Windows Credential Manager, never in GameHQ's settings."
            text: qsTrId("gamehq.settings.telegram.credentials_help")
            color: Theme.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        TelegramField {
            id: apiIdField
            //% "API ID"
            placeholderText: qsTrId("gamehq.settings.telegram.api_id")
        }
        TelegramField {
            id: apiHashField
            secret: true
            //% "API hash"
            placeholderText: qsTrId("gamehq.settings.telegram.api_hash")
        }
        AccentButton {
            //% "Save"
            label: qsTrId("gamehq.action.save")
            onClicked: {
                root.formError = telegramAccount.setCredentials(apiIdField.text, apiHashField.text)
                if (root.formError === "") {
                    apiIdField.text = ""
                    apiHashField.text = ""
                    sounds.play("confirm")
                }
            }
        }
    }

    // Step 2: connect.
    RowLayout {
        visible: telegramAccount.hasCredentials && (root.accountState === "disconnected" || root.accountState === "error")
        Layout.fillWidth: true
        spacing: Theme.s8
        AccentButton {
            //% "Connect Telegram"
            label: qsTrId("gamehq.settings.telegram.connect")
            onClicked: {
                root.formError = ""
                telegramAccount.connectAccount()
            }
        }
        AccentButton {
            quiet: true
            //% "Forget API ID and hash"
            label: qsTrId("gamehq.settings.telegram.forget")
            onClicked: telegramAccount.forgetCredentials()
        }
    }

    Text {
        visible: root.busy
        //% "Working…"
        text: qsTrId("gamehq.settings.telegram.working")
        color: Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontCaption
    }

    // Step 3: whatever Telegram asks for next.
    ColumnLayout {
        visible: root.accountState === "wait_phone" || root.accountState === "wait_code" || root.accountState === "wait_password"
        Layout.fillWidth: true
        spacing: Theme.s8

        Text {
            text: root.accountState === "wait_phone"
                  //% "Enter the phone number of your Telegram account."
                  ? qsTrId("gamehq.settings.telegram.ask_phone")
                  : root.accountState === "wait_code"
                    //% "Telegram sent you a code. Enter it here."
                    ? qsTrId("gamehq.settings.telegram.ask_code")
                    //% "Enter your Telegram two-step verification password."
                    : qsTrId("gamehq.settings.telegram.ask_password")
            color: Theme.text
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontBody
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        TelegramField {
            id: answerField
            secret: root.accountState === "wait_password"
            onAccepted: root.submit()
        }
        RowLayout {
            spacing: Theme.s8
            AccentButton {
                //% "Continue"
                label: qsTrId("gamehq.settings.telegram.continue")
                onClicked: root.submit()
            }
            AccentButton {
                quiet: true
                //% "Cancel"
                label: qsTrId("gamehq.action.cancel")
                onClicked: {
                    answerField.text = ""
                    telegramAccount.cancelLogin()
                }
            }
        }
    }

    function submit() {
        const value = answerField.text
        answerField.text = ""
        if (accountState === "wait_phone") telegramAccount.submitPhone(value)
        else if (accountState === "wait_code") telegramAccount.submitCode(value)
        else if (accountState === "wait_password") telegramAccount.submitPassword(value)
    }

    Text {
        readonly property string code: root.formError !== "" ? root.formError : telegramAccount.errorCode
        visible: code !== ""
        text: root.errorText(code)
        color: Theme.danger
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontCaption
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }

    Text {
        visible: root.accountState === "connected"
        //% "Connected. To sign out and delete GameHQ's saved Telegram session, use Disconnect in the list above."
        text: qsTrId("gamehq.settings.telegram.connected_help")
        color: Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontCaption
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }

    component TelegramField: QC.TextField {
        property bool secret: false
        Layout.fillWidth: true
        echoMode: secret ? TextInput.Password : TextInput.Normal
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontBody
        color: Theme.text
        placeholderTextColor: Theme.textMuted
        selectionColor: Theme.accent
        selectedTextColor: Theme.textOnAccent
        background: Rectangle {
            radius: Theme.radiusM
            color: Theme.bg1
            border.width: parent.activeFocus ? Theme.borderWidth + 1 : 1
            border.color: parent.activeFocus ? Theme.focusRing : Theme.borderLight
        }
    }
}
