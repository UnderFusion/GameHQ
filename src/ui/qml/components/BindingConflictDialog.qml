import QtQuick
import GameHQ

// Modal shown only for a HardConflict: the trigger the user just pressed is
// already bound to a different action in a context that is live at the same
// time, so keeping both would fire two actions from one press.
//
// Deliberately not a ConfirmDialog with an extra button. ConfirmDialog is the
// app-wide two-choice modal; growing a third action onto it for this one case
// would push a binding-editor concern into every delete and reset prompt.
// "Choose another" is the whole point here — the user almost never wants to
// silently drop the assignment they already had.
//
// Pad routing while modal comes from DialogShell: directions move between the
// three buttons, Cross fires the focused one, Circle (or the scrim) cancels,
// which keeps the existing binding — the safe default.
DialogShell {
    id: root

    //% "These assignments conflict"
    property string title: qsTrId("gamehq.input.conflict.title")
    property string message: ""
    //% "Replace"
    property string replaceLabel: qsTrId("gamehq.action.replace")
    //% "Choose another"
    property string retryLabel: qsTrId("gamehq.input.choose_another")
    //% "Cancel"
    property string cancelLabel: qsTrId("gamehq.action.cancel")

    signal replaced()
    signal retried()

    // Land on Cancel: keeping the existing binding is the safe default.
    landingItem: cancelButton
    cardBorderColor: Theme.danger

    function open() { root.present() }

    Text {
        width: parent.width
        text: root.title
        color: Theme.danger
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
            label: root.replaceLabel
            onClicked: { root.replaced(); root.close() }
        }
    }
}
