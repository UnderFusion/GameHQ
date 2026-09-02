import QtQuick
import GameHQ
import "../helpers/PadNav.js" as PadNav

// Modal shown only for a HardConflict: the trigger the user just pressed is
// already bound to a different action in a context that is live at the same
// time, so keeping both would fire two actions from one press.
//
// Deliberately not a ConfirmDialog with an extra button. ConfirmDialog is the
// app-wide two-choice modal; growing a third action onto it for this one case
// would push a binding-editor concern into every delete and reset prompt.
// "Choose another" is the whole point here — the user almost never wants to
// silently drop the assignment they already had.
Item {
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
    signal canceled()

    visible: false
    opacity: 0

    function open() {
        visible = true
        // Land on Cancel: keeping the existing binding is the safe default.
        Qt.callLater(function() { if (root.visible) cancelButton.forceActiveFocus() })
    }
    function close() { visible = false }

    // Pad routing while modal (see SettingsPage.padOverlay): directions move
    // between the three buttons, Cross fires the focused one, Circle cancels.
    function padStep(direction) { padHorizontal(direction) }
    function padHorizontal(direction) {
        const active = Window.window ? Window.window.activeFocusItem : null
        if (!active || !PadNav.isInside(active, root)) {
            cancelButton.forceActiveFocus()
            sounds.play("nav_tick")
            return
        }
        const target = PadNav.horizontalTarget(root, active, direction)
        if (target) {
            target.forceActiveFocus()
            sounds.play("nav_tick")
        }
    }
    function padConfirm() {
        const active = Window.window ? Window.window.activeFocusItem : null
        if (active && PadNav.isInside(active, root) && active.clicked)
            active.clicked()
    }
    function padBack() {
        root.canceled()
        root.close()
    }

    Behavior on opacity { NumberAnimation { duration: Theme.durFast } }
    states: State {
        when: root.visible
        PropertyChanges { target: root; opacity: 1 }
    }

    // Clicking the scrim keeps the existing binding — the safe default.
    Rectangle {
        anchors.fill: parent
        color: Theme.scrim
        MouseArea {
            anchors.fill: parent
            onClicked: { root.canceled(); root.close() }
        }
    }

    Rectangle {
        anchors.centerIn: parent
        width: Theme.dialogWidth
        height: col.implicitHeight + Theme.s24 * 2
        radius: Theme.radiusL
        color: Theme.surface
        border.width: 1
        border.color: Theme.danger

        MouseArea { anchors.fill: parent }   // swallow clicks so the scrim does not cancel

        Column {
            id: col
            x: Theme.s24
            y: Theme.s24
            width: parent.width - Theme.s24 * 2
            spacing: Theme.s16

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
                    onClicked: { root.canceled(); root.close() }
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
    }
}
