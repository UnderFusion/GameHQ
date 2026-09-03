import QtQuick
import QtQuick.Layouts
import GameHQ

Rectangle {
    id: root
    property string icon: "\u2197"
    property string label: ""
    property string description: ""
    property bool showDivider: true
    signal clicked()

    Layout.fillWidth: true
    implicitHeight: 54
    activeFocusOnTab: true
    radius: Theme.radiusS
    color: mouse.containsMouse || activeFocus ? Theme.surfaceHover : "transparent"
    border.width: activeFocus ? 1 : 0
    border.color: Theme.focusRing
    Keys.onReturnPressed: clicked()
    Keys.onSpacePressed: clicked()

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.s8
        anchors.rightMargin: Theme.s8
        spacing: Theme.s12
        Text {
            text: root.icon
            color: Theme.accent
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontBody
            Layout.preferredWidth: Theme.s24
            horizontalAlignment: Text.AlignHCenter
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            Text {
                text: root.label
                color: Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontBody
                font.weight: Font.Medium
                Layout.fillWidth: true
            }
            Text {
                visible: root.description.length > 0
                text: root.description
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontCaption
                elide: Text.ElideMiddle
                Layout.fillWidth: true
            }
        }
        Text {
            text: languageManager.layoutDirection === Qt.RightToLeft ? "\u2039" : "\u203A"
            color: Theme.textFaint
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontH3
        }
    }

    Rectangle {
        visible: root.showDivider
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: Theme.divider
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }
}
