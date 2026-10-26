import QtQuick
import GameHQ

// Reusable modal confirmation dialog (scrim + centred card + Cancel/confirm).
// Styled from Theme. open()/close(); emits confirmed() or canceled().
// Pad routing while modal comes from DialogShell: Left/Right and Up/Down move
// between the two buttons, Cross fires the focused one, Circle cancels.
DialogShell {
    id: root

    //% "Are you sure?"
    property string title: qsTrId("gamehq.dialog.confirm.default_title")
    property string message: ""
    //% "Cancel"
    property string cancelLabel: qsTrId("gamehq.action.cancel")
    //% "Delete"
    property string confirmLabel: qsTrId("gamehq.action.delete")
    signal confirmed()

    // Cancel is the safe pad landing spot; Cross must not destroy anything
    // until the user deliberately moves to the confirm button.
    landingItem: cancelButton
    cardWidth: Theme.confirmDialogWidth

    function open() { root.present() }

    Text {
        width: parent.width
        text: root.title
        color: Theme.text
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontTitle
        font.weight: Font.DemiBold
        wrapMode: Text.WordWrap
    }
    Text {
        width: parent.width
        visible: root.message !== ""
        text: root.message
        color: Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontBody
        wrapMode: Text.WordWrap
    }
    Row {
        anchors.right: parent.right
        spacing: Theme.s12
        AccentButton {
            id: cancelButton
            label: root.cancelLabel
            onClicked: root.cancel()
        }
        AccentButton {
            primary: true
            label: root.confirmLabel
            onClicked: { root.confirmed(); root.close() }
        }
    }
}
