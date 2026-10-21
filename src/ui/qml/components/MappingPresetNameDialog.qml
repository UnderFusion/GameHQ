import QtQuick
import QtQuick.Controls.Basic as QC
import GameHQ

// Name entry for the mapping-preset library (cpo-p06): create, rename and
// duplicate all take a display name, and the id is what every assignment keeps
// pointing at, so this dialog never carries identity - only the label.
//
// Pad contract (see SettingsPage.padOverlay and DialogShell): Cross accepts the
// prefilled suggestion, Circle cancels, Left/Right moves between the field and
// the buttons. A keyboard user edits the field and presses Enter.
DialogShell {
    id: root

    property string title: ""
    property string message: ""
    // The prefilled name: always a unique, collision-free suggestion, so a
    // gamepad user can accept it without typing anything.
    property string suggestedName: ""
    //% "Name"
    property string fieldLabel: qsTrId("gamehq.settings.presets.name_label")
    //% "Cancel"
    property string cancelLabel: qsTrId("gamehq.action.cancel")
    //% "Save"
    property string confirmLabel: qsTrId("gamehq.action.save")
    signal accepted(string name)

    landingItem: saveButton
    cardWidth: Math.min(width - Theme.s32, Theme.s48 * 8)
    cardRadius: Theme.radiusM
    cardBorderColor: Theme.borderLight

    function open() {
        nameField.text = root.suggestedName
        visible = true
        Qt.callLater(function() {
            if (!root.visible)
                return
            nameField.forceActiveFocus()
            nameField.selectAll()
        })
    }
    function accept() {
        root.accepted(nameField.text)
        root.close()
    }

    Text {
        width: parent.width
        text: root.title
        color: Theme.text
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontTitle
        wrapMode: Text.WordWrap
    }
    Text {
        width: parent.width
        visible: root.message.length > 0
        text: root.message
        color: Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontBody
        wrapMode: Text.WordWrap
    }
    Text {
        width: parent.width
        text: root.fieldLabel
        color: Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontCaption
    }
    QC.TextField {
        id: nameField
        width: parent.width
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontBody
        color: Theme.text
        selectionColor: Theme.accent
        selectedTextColor: Theme.textOnAccent
        background: Rectangle {
            radius: Theme.radiusM
            color: Theme.bg1
            border.width: nameField.activeFocus ? Theme.borderWidth + 1 : Theme.borderWidth
            border.color: nameField.activeFocus ? Theme.focusRing : Theme.borderLight
        }
        // Enter on the keyboard and Cross on the pad (DialogShell.padConfirm)
        // both accept the field.
        onAccepted: root.accept()
    }
    Row {
        anchors.right: parent.right
        spacing: Theme.s12
        AccentButton {
            label: root.cancelLabel
            onClicked: root.cancel()
        }
        AccentButton {
            id: saveButton
            primary: true
            label: root.confirmLabel
            onClicked: root.accept()
        }
    }
}
