import QtQuick
import QtQuick.Layouts
import GameHQ

Rectangle {
    id: root

    //% "Close"
    Accessible.name: qsTrId("gamehq.common.action.close")
    Accessible.role: Accessible.Button

    signal clicked()

    implicitWidth: 44
    implicitHeight: 44
    Layout.preferredWidth: implicitWidth
    Layout.preferredHeight: implicitHeight
    radius: Theme.radiusS
    color: mouse.containsMouse || root.activeFocus ? Theme.hoverTint : "transparent"
    border.width: 0

    activeFocusOnTab: true
    Keys.onReturnPressed: root.clicked()
    Keys.onSpacePressed: root.clicked()

    Text {
        anchors.centerIn: parent
        text: "X"
        color: mouse.containsMouse || root.activeFocus ? Theme.accent : Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: 28
        font.weight: Font.Light
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }
}
