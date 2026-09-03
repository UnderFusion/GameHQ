import QtQuick
import GameHQ

// Reusable toast card for the bottom-right notification stack (docs/notifications.md).
// All visual values come from Theme. Fixed width (the delegate sets root.width);
// rises + fades in, auto-dismisses after `lifespan` ms, emits dismissed() on exit.
Item {
    id: root

    property string title
    property string body: ""
    property string imageUrl: ""
    property date when                // formatted on demand so live locale changes update the toast
    property bool isVideo: false      // show the play badge over the thumbnail
    property string kind: "info"      // success | info | error
    property int lifespan: 3600
    signal dismissed()

    implicitHeight: card.height
    height: implicitHeight
    // width is set by the delegate (stack width).

    function accentColor() {
        if (kind === "success") return Theme.success
        if (kind === "error")   return Theme.danger
        return Theme.accent
    }

    Rectangle {
        id: card
        width: root.width
        height: bodyRow.height + Theme.s16 * 2
        radius: Theme.radiusM
        topRightRadius: 0          // flat right edge so the accent bar sits flush
        bottomRightRadius: 0
        color: Theme.surface
        border.width: 1
        border.color: Theme.stroke
        clip: true

        Rectangle {                       // right accent bar (kind colour)
            width: 4
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.topMargin: card.border.width      // sit inside the border, no overrun
            anchors.bottomMargin: card.border.width
            color: root.accentColor()
        }

        Row {
            id: bodyRow
            x: Theme.s16
            width: card.width - Theme.s16 * 2
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.s16

            Rectangle {                   // thumbnail — locked to 16:9 (screen ratio)
                id: thumb
                visible: root.imageUrl !== ""
                width: visible ? 124 : 0     // ~10% shorter card (thumbnail drives height)
                height: Math.round(width * 9 / 16)
                radius: Theme.radiusS
                color: Theme.surfaceAlt
                border.width: 1
                border.color: Theme.borderLight
                clip: true
                anchors.verticalCenter: parent.verticalCenter
                Image {
                    anchors.fill: parent
                    source: root.imageUrl
                    fillMode: Image.PreserveAspectCrop
                    sourceSize: Qt.size(384, 216)   // 16:9 downscale on decode
                    asynchronous: true
                    cache: false
                }

                // Same circle+▶ badge as the gallery tiles, so a clip
                // thumbnail reads as a clip here too.
                VideoBadge {
                    anchors.centerIn: parent
                    visible: root.isVideo
                    diameter: Math.min(thumb.width, thumb.height) * 0.45
                }
            }

            Column {
                anchors.verticalCenter: parent.verticalCenter
                width: bodyRow.width - (thumb.visible ? thumb.width + bodyRow.spacing : 0)
                spacing: 2
                Text {
                    width: parent.width
                    text: root.title
                    color: Theme.text
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontTitle
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    visible: root.body !== ""
                    text: root.body
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontBody
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    visible: !isNaN(root.when.getTime()) && root.when.getTime() > 0
                    text: {
                        languageManager.translationRevision
                        return languageManager.formatDateTime(root.when)
                    }
                    color: Theme.textFaint
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontCaption
                    elide: Text.ElideRight
                }
            }
        }
    }

    // Entry: rise + fade. A Translate transform avoids fighting the Column
    // positioner (which owns root.y) and never clips against the window edge.
    opacity: 0
    transform: Translate { id: rise; y: 16 }
    Component.onCompleted: enter.start()
    ParallelAnimation {
        id: enter
        NumberAnimation { target: root; property: "opacity"; from: 0; to: 1; duration: Theme.durNormal; easing.type: Easing.OutCubic }
        NumberAnimation { target: rise; property: "y"; from: 16; to: 0; duration: Theme.durNormal; easing.type: Easing.OutCubic }
    }

    // Exit: fade out, then tell the parent to drop us.
    SequentialAnimation {
        id: exit
        NumberAnimation { target: root; property: "opacity"; to: 0; duration: Theme.durFast; easing.type: Easing.InCubic }
        ScriptAction { script: root.dismissed() }
    }

    Timer {
        interval: root.lifespan
        running: true
        repeat: false
        onTriggered: exit.start()
    }
}
