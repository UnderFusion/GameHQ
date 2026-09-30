import QtQuick
import QtQuick.Controls.Basic
import GameHQ

// Three-state sidebar mode control shared by the desktop sidebar and the
// overlay sidebar. One activation cycles auto -> expanded -> collapsed -> auto.
// The icon is a panel outline whose leading strip shows the mode, so it reads
// the same under RTL (anchors mirror with the layout) instead of relying on a
// direction arrow. In the compact rail only the icon remains; the label moves
// into the tooltip.
Rectangle {
    id: root

    property string mode: "expanded"
    property bool compact: false
    // Controller/keyboard cursor, mirroring SidebarItem.sidebarHovered.
    property bool padHovered: false
    readonly property alias hovered: mouse.containsMouse
    signal clicked()

    //% "Sidebar: Auto"
    readonly property string autoLabel: qsTrId("gamehq.navigation.sidebar_mode.auto")
    //% "Sidebar: Expanded"
    readonly property string expandedLabel: qsTrId("gamehq.navigation.sidebar_mode.expanded")
    //% "Sidebar: Collapsed"
    readonly property string collapsedLabel: qsTrId("gamehq.navigation.sidebar_mode.collapsed")
    readonly property string label: root.mode === "expanded" ? root.expandedLabel
                                  : root.mode === "collapsed" ? root.collapsedLabel
                                  : root.autoLabel

    function nextMode(current) {
        return current === "expanded" ? "auto"
             : current === "auto" ? "collapsed"
             : "expanded"
    }

    implicitWidth: 200
    implicitHeight: Theme.s32
    radius: Theme.radiusS
    color: root.padHovered ? Theme.surfaceAlt
         : mouse.containsMouse || root.activeFocus ? Theme.hoverTint
         : "transparent"

    activeFocusOnTab: true
    Keys.onReturnPressed: root.clicked()
    Keys.onSpacePressed: root.clicked()
    Accessible.role: Accessible.Button
    Accessible.name: root.label

    Row {
        id: contentRow
        anchors.verticalCenter: parent.verticalCenter
        anchors.left: parent.left
        anchors.leftMargin: root.compact ? Math.max(0, (root.width - icon.width) / 2) : Theme.s8
        spacing: Theme.s8

        Item {
            id: icon
            width: Theme.s24
            height: Theme.s24
            anchors.verticalCenter: parent.verticalCenter

            Rectangle {
                id: frame
                anchors.centerIn: parent
                width: Theme.s16
                height: Theme.s12
                radius: Theme.s4 / 2
                color: "transparent"
                border.width: Math.max(1, Theme.borderWidth)
                border.color: root.padHovered || mouse.containsMouse ? Theme.text : Theme.textMuted
            }

            // Leading strip: narrow = collapsed, wide = expanded, dimmed = auto.
            Rectangle {
                anchors.left: frame.left
                anchors.top: frame.top
                anchors.bottom: frame.bottom
                anchors.margins: Math.max(1, Theme.borderWidth) + 1
                width: root.mode === "expanded" ? frame.width / 2 - 1 : Theme.s4 / 2
                radius: 1
                color: Theme.accent
                opacity: root.mode === "auto" ? 0.55 : 1
            }
        }

        Text {
            visible: !root.compact
            text: root.label
            width: Math.max(0, root.width - icon.width - contentRow.spacing - Theme.s8 * 2)
            elide: Text.ElideRight
            color: root.padHovered || mouse.containsMouse ? Theme.text : Theme.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontBody
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: parent.radius
        color: "transparent"
        border.width: (root.activeFocus || root.padHovered) ? 2 : 0
        border.color: Theme.accent
    }

    ToolTip.text: root.label
    ToolTip.visible: root.compact && mouse.containsMouse
    ToolTip.delay: 400

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }
}
