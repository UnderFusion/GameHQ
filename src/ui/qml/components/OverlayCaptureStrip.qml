import QtQuick
import GameHQ

Rectangle {
    id: root

    property alias currentIndex: strip.currentIndex
    property var model
    property bool usingGamepad: false
    property bool videoFocused: false

    // Mouse path for the per-tile hover icons. The strip owns no capture
    // actions itself — it moves the selection to the tile that was clicked
    // (so the preview follows the mouse exactly like it follows the pad) and
    // hands the row to the host, which already owns delete/folder/favourite.
    signal deleteRequested(int index)
    signal openFolderRequested(int index)
    signal favoriteToggleRequested(int index)

    function decrementCurrentIndex() { strip.decrementCurrentIndex() }
    function incrementCurrentIndex() { strip.incrementCurrentIndex() }

    height: 132 + Theme.s16 * 2
    radius: Theme.radiusL
    color: Theme.panelTint
    border.width: root.videoFocused ? 1 : 2
    border.color: root.videoFocused ? Theme.stroke : Theme.accent

    MouseArea { anchors.fill: parent }

    Rectangle {
        anchors.fill: parent
        radius: parent.radius
        color: "transparent"
        border.width: 1
        border.color: Theme.focusGlow
        opacity: root.videoFocused ? 0 : 1
        Behavior on opacity { NumberAnimation { duration: Theme.durFast } }
    }

    Rectangle {
        id: stripPillLeft
        anchors.left: parent.left
        anchors.leftMargin: -Theme.s12
        anchors.verticalCenter: parent.verticalCenter
        width: stripPillTextL.implicitWidth + Theme.s8
        height: Theme.s16
        radius: Theme.radiusPill
        color: Theme.text
        opacity: 0.92
        // The pill labels the L1/R1 capture switch, so it is shown
        // only while a pad is in use: the overlay has no arrow-key route.
        visible: root.usingGamepad

        Text {
            id: stripPillTextL
            anchors.centerIn: parent
            text: root.usingGamepad ? "L1" : "\u2190"
            color: Theme.bg0
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontCaption
            font.bold: true
        }

        Behavior on opacity { NumberAnimation { duration: Theme.durFast } }
    }

    Rectangle {
        id: stripPillRight
        anchors.right: parent.right
        anchors.rightMargin: -Theme.s12
        anchors.verticalCenter: parent.verticalCenter
        width: stripPillTextR.implicitWidth + Theme.s8
        height: Theme.s16
        radius: Theme.radiusPill
        color: Theme.text
        opacity: 0.92
        visible: root.usingGamepad

        Text {
            id: stripPillTextR
            anchors.centerIn: parent
            text: root.usingGamepad ? "R1" : "\u2192"
            color: Theme.bg0
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontCaption
            font.bold: true
        }

        Behavior on opacity { NumberAnimation { duration: Theme.durFast } }
    }

    ListView {
        id: strip
        anchors.top: parent.top
        anchors.topMargin: Theme.s16
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.s16
        anchors.left: parent.left
        anchors.leftMargin: Theme.s8
        anchors.right: parent.right
        anchors.rightMargin: Theme.s8
        clip: true
        orientation: ListView.Horizontal
        spacing: 0
        model: root.model
        currentIndex: 0
        onCurrentIndexChanged: sounds.play("nav_tick")
        highlightMoveDuration: Theme.durFast

        delegate: Item {
            width: 180 + Theme.s16
            height: strip.height

            CaptureTile {
                anchors.fill: parent
                anchors.leftMargin: Theme.s8
                anchors.rightMargin: Theme.s8
                anchors.topMargin: Theme.s4
                thumbnail: model.thumbnail
                captureType: model.captureType
                gameName: model.gameName
                dateText: model.dateText
                favorite: model.favorite
                selected: strip.currentIndex === index
                onActivated: strip.currentIndex = index
                onRequestDelete: {
                    strip.currentIndex = index
                    root.deleteRequested(index)
                }
                onRequestOpenFolder: {
                    strip.currentIndex = index
                    root.openFolderRequested(index)
                }
                onToggleFavoriteRequested: {
                    strip.currentIndex = index
                    root.favoriteToggleRequested(index)
                }
            }
        }
    }
}
