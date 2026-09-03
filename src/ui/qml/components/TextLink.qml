import QtQuick
import QtQuick.Layouts
import GameHQ

Item {
    id: root

    property string label
    property string suffix: ""
    property color suffixColor: Theme.accent
    property int suffixFontSize: Theme.fontBody
    property bool selected: false
    signal clicked()

    function mirroredSuffix(value) {
        if (languageManager.layoutDirection !== Qt.RightToLeft)
            return value
        return value.replace(/›/g, "‹").replace(/→/g, "←")
    }

    implicitWidth: linkRow.implicitWidth
    implicitHeight: 30
    Layout.preferredWidth: implicitWidth
    Layout.preferredHeight: implicitHeight
    activeFocusOnTab: true

    Keys.onReturnPressed: root.clicked()
    Keys.onSpacePressed: root.clicked()

    Row {
        id: linkRow
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.s8

        Text {
            text: root.label
            color: root.selected || mouse.containsMouse || root.activeFocus
                   ? Theme.accent : Theme.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontBody
            font.underline: root.selected || mouse.containsMouse
        }

        Text {
            visible: root.suffix !== ""
            text: root.mirroredSuffix(root.suffix)
            color: root.suffixColor
            font.family: Theme.fontFamily
            font.pixelSize: root.suffixFontSize
        }
    }

    Rectangle {
        visible: root.activeFocus || root.selected
        anchors.left: linkRow.left
        anchors.right: linkRow.right
        anchors.bottom: parent.bottom
        height: 2
        color: Theme.accent
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }
}
