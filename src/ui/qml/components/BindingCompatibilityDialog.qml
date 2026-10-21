import QtQuick
import GameHQ

// Non-destructive alternative to BindingConflictDialog. Both assignments stay:
// an existing editable Press becomes Single tap, then the requested timed
// gesture is added atomically.
//
// Pad routing while modal comes from DialogShell: directions move between the
// three buttons, Cross fires the focused one, Circle or Esc cancels.
DialogShell {
    id: root

    //% "Make these assignments compatible?"
    property string title: qsTrId("gamehq.input.compatibility.title")
    property string message: ""
    //% "Convert & add"
    property string convertLabel: qsTrId("gamehq.input.compatibility.convert")
    //% "Choose another"
    property string retryLabel: qsTrId("gamehq.input.choose_another")
    //% "Cancel"
    property string cancelLabel: qsTrId("gamehq.action.cancel")

    signal converted()
    signal retried()

    landingItem: cancelButton
    escapeCancels: true
    cardWidth: Math.min(width - Theme.s32, Theme.dialogWidth)
    cardBorderColor: Theme.warning

    function open() { root.present() }

    Text {
        width: parent.width
        text: root.title
        color: Theme.warning
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
            label: root.retryLabel
            onClicked: { root.close(); root.retried() }
        }
        AccentButton {
            primary: true
            label: root.convertLabel
            onClicked: { root.converted(); root.close() }
        }
    }
}
