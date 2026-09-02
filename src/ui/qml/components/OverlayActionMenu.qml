import QtQuick
import GameHQ

Item {
    id: root

    property bool open: false
    property int currentIndex: 0
    // The desktop and the overlay share this menu but do not offer the same
    // actions — bulk selection is a desktop-only mode — so the caller owns the
    // list. Index order is the caller's contract with its own confirm handler.
    property var entries: [
        //% "Show in folder"
        qsTrId("gamehq.gallery.action.show_in_folder"),
        //% "Delete"
        qsTrId("gamehq.action.delete")
    ]

    signal closeRequested()
    signal itemHovered(int index)
    signal itemConfirmed(int index)

    anchors.fill: parent
    visible: root.open

    Rectangle {
        anchors.fill: parent
        color: Theme.scrim
        MouseArea {
            anchors.fill: parent
            onClicked: root.closeRequested()
        }
    }

    Rectangle {
        anchors.centerIn: parent
        width: 320
        height: menuColumn.implicitHeight + Theme.s24 * 2
        radius: Theme.radiusL
        color: Theme.surface
        border.width: 1
        border.color: Theme.stroke

        MouseArea { anchors.fill: parent }

        Column {
            id: menuColumn
            x: Theme.s24
            y: Theme.s24
            width: parent.width - Theme.s24 * 2
            spacing: Theme.s8

            Text {
            //% "Capture actions"
            text: qsTrId("gamehq.overlay.capture_actions")
                color: Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontH3
                font.weight: Font.DemiBold
            }

            Repeater {
                model: root.entries
                delegate: Rectangle {
                    width: menuColumn.width
                    height: 40
                    radius: Theme.radiusS
                    color: root.currentIndex === index ? Theme.surfaceAlt : "transparent"

                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: Theme.s12
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData
                        color: root.currentIndex === index ? Theme.text : Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontBody
                    }

                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        onEntered: root.itemHovered(index)
                        onClicked: root.itemConfirmed(index)
                    }
                }
            }
        }
    }
}
