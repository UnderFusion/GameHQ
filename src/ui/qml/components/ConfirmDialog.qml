import QtQuick
import GameHQ
import "../helpers/PadNav.js" as PadNav

// Reusable modal confirmation dialog (scrim + centred card + Cancel/confirm).
// Styled from Theme. open()/close(); emits confirmed() or canceled().
Item {
    id: root

    //% "Are you sure?"
    property string title: qsTrId("gamehq.dialog.confirm.default_title")
    property string message: ""
    //% "Cancel"
    property string cancelLabel: qsTrId("gamehq.action.cancel")
    //% "Delete"
    property string confirmLabel: qsTrId("gamehq.action.delete")
    signal confirmed()
    signal canceled()

    visible: false
    opacity: 0

    function open() {
        visible = true
        // Cancel is the safe pad landing spot; Cross must not destroy anything
        // until the user deliberately moves to the confirm button.
        Qt.callLater(function() { if (root.visible) cancelButton.forceActiveFocus() })
    }
    function close() { visible = false }

    // Pad routing while modal (see SettingsPage.padOverlay): Left/Right and
    // Up/Down move between the two buttons, Cross fires the focused one,
    // Circle cancels.
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

    // Scrim — clicking outside the card cancels.
    Rectangle {
        anchors.fill: parent
        color: Theme.scrim
        MouseArea {
            anchors.fill: parent
            onClicked: { root.canceled(); root.close() }
        }
    }

    Rectangle {
        id: dialog
        anchors.centerIn: parent
        width: 400
        height: col.implicitHeight + Theme.s24 * 2
        radius: Theme.radiusL
        color: Theme.surface
        border.width: 1
        border.color: Theme.stroke

        MouseArea { anchors.fill: parent }   // swallow clicks so the scrim doesn't cancel

        Column {
            id: col
            x: Theme.s24
            y: Theme.s24
            width: parent.width - Theme.s24 * 2
            spacing: Theme.s16

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
                    onClicked: { root.canceled(); root.close() }
                }
                AccentButton {
                    primary: true
                    label: root.confirmLabel
                    onClicked: { root.confirmed(); root.close() }
                }
            }
        }
    }
}
