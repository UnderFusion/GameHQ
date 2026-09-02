import QtQuick
import GameHQ
import "../helpers/PadNav.js" as PadNav

// Non-destructive alternative to BindingConflictDialog. Both assignments stay:
// an existing editable Press becomes Single tap, then the requested timed
// gesture is added atomically.
FocusScope {
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
    signal canceled()

    visible: false
    opacity: 0

    function open() {
        visible = true
        Qt.callLater(function() { cancelButton.forceActiveFocus() })
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

    Keys.onEscapePressed: function(event) {
        root.canceled()
        root.close()
        event.accepted = true
    }

    Behavior on opacity { NumberAnimation { duration: Theme.durFast } }
    states: State {
        when: root.visible
        PropertyChanges { target: root; opacity: 1 }
    }

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
        width: Math.min(parent.width - Theme.s32, Theme.dialogWidth)
        height: content.implicitHeight + Theme.s24 * 2
        radius: Theme.radiusL
        color: Theme.surface
        border.width: Theme.borderWidth
        border.color: Theme.warning

        MouseArea { anchors.fill: parent }

        Column {
            id: content
            x: Theme.s24
            y: Theme.s24
            width: parent.width - Theme.s24 * 2
            spacing: Theme.s16

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
                    onClicked: { root.canceled(); root.close() }
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
    }
}
