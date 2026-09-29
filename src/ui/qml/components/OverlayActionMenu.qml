import QtQuick
import GameHQ

Item {
    id: root

    property bool open: false
    property int currentIndex: 0
    // The desktop and the overlay share this menu but do not offer the same
    // actions — bulk selection is a desktop-only mode — so the caller owns the
    // list. Each entry is { id, label }: callers act on the stable id, never
    // on the position, so adding or reordering entries cannot retarget one.
    property var entries: [
        //% "Share"
        { id: "share", label: qsTrId("gamehq.gallery.action.share") },
        //% "Show in folder"
        { id: "show_in_folder", label: qsTrId("gamehq.gallery.action.show_in_folder") },
        //% "Delete"
        { id: "delete", label: qsTrId("gamehq.action.delete") }
    ]

    signal closeRequested()
    signal itemHovered(int index)
    signal actionConfirmed(string actionId)

    // Id of the highlighted entry ("" when none), for pad confirm.
    function currentActionId() {
        return root.currentIndex >= 0 && root.currentIndex < root.entries.length
            ? root.entries[root.currentIndex].id : ""
    }

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
                        text: modelData.label
                        color: root.currentIndex === index ? Theme.text : Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontBody
                    }

                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        onEntered: root.itemHovered(index)
                        onClicked: {
                            root.itemHovered(index)
                            root.actionConfirmed(modelData.id)
                        }
                    }
                }
            }
        }
    }
}
