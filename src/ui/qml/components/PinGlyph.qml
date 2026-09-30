import QtQuick
import GameHQ

// Flat push-pin drawn from shapes (no icon font): a head, a collar and a
// needle, tilted like a pin stuck into the board. Filled when pinned,
// outlined otherwise.
Item {
    id: root
    property bool filled: false
    property color color: Theme.textMuted
    readonly property real stroke: Math.max(1, Theme.borderWidth) * 1.5

    implicitWidth: Theme.s16
    implicitHeight: Theme.s16

    Item {
        id: pin
        width: root.width * 0.5
        height: root.height
        anchors.centerIn: parent
        rotation: 45

        Rectangle {                     // head
            id: head
            width: parent.width
            height: parent.height * 0.42
            anchors.top: parent.top
            anchors.horizontalCenter: parent.horizontalCenter
            radius: width * 0.3
            color: root.filled ? root.color : "transparent"
            border.width: root.stroke
            border.color: root.color
        }
        Rectangle {                     // collar
            width: parent.width * 1.5
            height: root.stroke * 1.4
            anchors.top: head.bottom
            anchors.horizontalCenter: parent.horizontalCenter
            radius: height / 2
            color: root.color
        }
        Rectangle {                     // needle
            width: root.stroke
            anchors.top: head.bottom
            anchors.bottom: parent.bottom
            anchors.horizontalCenter: parent.horizontalCenter
            radius: width / 2
            color: root.color
        }
    }
}
