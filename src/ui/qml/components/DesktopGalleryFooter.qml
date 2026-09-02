import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import GameHQ

RowLayout {
    id: root

    property bool hasCaptures: false
    property bool sidebarFocused: false
    property bool bulkMode: false
    property bool usingGamepad: false
    property int zoomLevel: 280

    signal zoomInRequested()
    signal zoomOutRequested()
    signal zoomMoved(int value)

    visible: root.hasCaptures
    Layout.fillWidth: true
    Layout.topMargin: Theme.s4
    spacing: Theme.s8

    Text {
        Layout.fillWidth: true
        Layout.alignment: Qt.AlignVCenter | Qt.AlignLeft
        text: {
            if (root.sidebarFocused) {
                if (root.usingGamepad) {
                    //% "D-pad ↑↓ — pick | Cross — select | R1 — back to grid"
                    return qsTrId("gamehq.gallery.hint.sidebar.gamepad")
                }
                //% "↑↓ — pick | Enter — select | Esc — back to grid"
                return qsTrId("gamehq.gallery.hint.sidebar.keyboard")
            }
            if (root.bulkMode) {
                if (root.usingGamepad) {
                    //% "Cross — select | Triangle — all | Square — delete | Circle — done"
                    return qsTrId("gamehq.gallery.hint.bulk.gamepad")
                }
                //% "Enter/Space — select | Ctrl+A — all | Delete — delete | Esc — done"
                return qsTrId("gamehq.gallery.hint.bulk.keyboard")
            }
            if (root.usingGamepad) {
                //% "Cross — open | Triangle — favorite | Square — menu | L1 — sidebar | PS — overlay"
                return qsTrId("gamehq.gallery.hint.browse.gamepad")
            }
            //% "Enter — open | F — favorite | E — show in folder | Ctrl+Shift+G — overlay"
            return qsTrId("gamehq.gallery.hint.browse.keyboard")
        }
        color: Theme.textFaint
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontCaption
    }

    Rectangle {
        Layout.preferredWidth: 28
        Layout.preferredHeight: 28
        radius: Theme.radiusS
        color: zoomOutMouse.containsMouse ? Theme.surfaceAlt : "transparent"
        border.width: 1
        border.color: Theme.stroke

        Text {
            anchors.centerIn: parent
            text: "\u2212"
            color: Theme.text
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontH3
            font.weight: Font.DemiBold
        }

        MouseArea {
            id: zoomOutMouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: root.zoomOutRequested()
        }
    }

    Slider {
        id: zoomSlider
        Layout.preferredWidth: 140
        from: 160
        to: 480
        stepSize: 20
        value: root.zoomLevel
        onMoved: root.zoomMoved(Math.round(value))

        background: Rectangle {
            x: zoomSlider.leftPadding + zoomSlider.availableWidth / 2 - width / 2
            y: zoomSlider.topPadding + zoomSlider.availableHeight / 2 - height / 2
            implicitWidth: 140
            implicitHeight: 4
            width: zoomSlider.availableWidth
            height: 4
            radius: 2
            color: Theme.surfaceAlt
        }

        handle: Rectangle {
            x: zoomSlider.leftPadding + zoomSlider.visualPosition * (zoomSlider.availableWidth - width)
            y: zoomSlider.topPadding + zoomSlider.availableHeight / 2 - height / 2
            implicitWidth: 14
            implicitHeight: 14
            radius: 7
            color: zoomSlider.pressed ? Theme.accent1 : Theme.accent
            border.width: 1
            border.color: Theme.borderLight
        }
    }

    Rectangle {
        Layout.preferredWidth: 28
        Layout.preferredHeight: 28
        radius: Theme.radiusS
        color: zoomInMouse.containsMouse ? Theme.surfaceAlt : "transparent"
        border.width: 1
        border.color: Theme.stroke

        Text {
            anchors.centerIn: parent
            text: "+"
            color: Theme.text
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontH3
            font.weight: Font.DemiBold
        }

        MouseArea {
            id: zoomInMouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: root.zoomInRequested()
        }
    }
}
