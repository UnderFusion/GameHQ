import QtQuick
import QtQuick.Controls.Basic as QC
import GameHQ
import "../helpers/PadNav.js" as PadNav

// Name entry for the mapping-preset library (cpo-p06): create, rename and
// duplicate all take a display name, and the id is what every assignment keeps
// pointing at, so this dialog never carries identity - only the label.
//
// Pad contract (see SettingsPage.padOverlay): Cross accepts the prefilled
// suggestion, Circle cancels, Left/Right moves between the field and the
// buttons. A keyboard user edits the field and presses Enter.
Item {
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
    signal canceled()

    visible: false
    opacity: 0

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
    function close() { visible = false }
    function padStep(direction) { padHorizontal(direction) }
    function padHorizontal(direction) {
        const active = Window.window ? Window.window.activeFocusItem : null
        if (!active || !PadNav.isInside(active, root)) {
            saveButton.forceActiveFocus()
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
        if (active === nameField) {
            root.accepted(nameField.text)
            root.close()
            return
        }
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
        width: Math.min(parent.width - Theme.s32, Theme.s48 * 8)
        implicitHeight: card.implicitHeight + Theme.s24 * 2
        radius: Theme.radiusM
        color: Theme.surface
        border.width: Theme.borderWidth
        border.color: Theme.borderLight

        Column {
            id: card
            anchors.fill: parent
            anchors.margins: Theme.s24
            spacing: Theme.s16

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
                    border.width: nameField.activeFocus ? Theme.borderWidth + 1 : 1
                    border.color: nameField.activeFocus ? Theme.focusRing : Theme.borderLight
                }
                Keys.onReturnPressed: { root.accepted(nameField.text); root.close() }
                Keys.onEnterPressed: { root.accepted(nameField.text); root.close() }
            }
            Row {
                anchors.right: parent.right
                spacing: Theme.s12
                AccentButton {
                    label: root.cancelLabel
                    onClicked: { root.canceled(); root.close() }
                }
                AccentButton {
                    id: saveButton
                    primary: true
                    label: root.confirmLabel
                    onClicked: { root.accepted(nameField.text); root.close() }
                }
            }
        }
    }
}
