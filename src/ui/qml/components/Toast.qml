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
    property string kind: "info"      // success | info | warning | error
    readonly property bool alert: kind === "warning" || kind === "error"
    property int lifespan: pending ? Theme.toastPendingLifespan
                         : alert ? Theme.toastWarningLifespan : Theme.toastLifespan
    property bool pending: false
    // closable: the mouse moved recently, so offer an X. paused: the pointer is
    // over the stack, so hold the card until it leaves.
    property bool closable: false
    property bool paused: false
    onPausedChanged: {
        if (!ready || exit.running) return
        if (paused) lifetime.stop()
        else lifetime.restart()
    }
    property int contentRevision: 0
    property bool ready: false
    onContentRevisionChanged: {
        if (!ready) return
        exit.stop()
        enter.stop()
        opacity = 1
        rise.y = 0
        lifetime.restart()
    }
    signal dismissed()

    implicitHeight: card.height
    height: implicitHeight
    // width is set by the delegate (stack width).

    function accentColor() {
        if (kind === "success") return Theme.success
        if (kind === "error")   return Theme.danger
        if (kind === "warning") return Theme.warning
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

        Rectangle {                       // close button, mouse only
            id: closeButton
            width: Theme.s24
            height: Theme.s24
            radius: width / 2
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.topMargin: Theme.s8
            anchors.rightMargin: Theme.s8 + 4          // clear of the accent bar
            z: 1
            visible: opacity > 0
            opacity: root.closable ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: Theme.durFast } }
            color: closeArea.containsMouse ? Theme.surfaceAlt : "transparent"
            border.width: closeArea.containsMouse ? 1 : 0
            border.color: Theme.stroke
            Accessible.role: Accessible.Button
            //% "Close notification"
            Accessible.name: qsTrId("gamehq.notifications.close")
            Text {
                anchors.centerIn: parent
                text: "✕"
                color: closeArea.containsMouse ? Theme.text : Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontCaption
            }
            MouseArea {
                id: closeArea
                anchors.fill: parent
                enabled: root.closable
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (exit.running) return
                    lifetime.stop()
                    exit.start()
                }
            }
        }

        Row {
            id: bodyRow
            x: Theme.s16
            width: card.width - Theme.s16 * 2
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.s16

            Rectangle {                   // caution badge for warnings/errors
                id: badge
                visible: root.alert && root.imageUrl === ""
                width: visible ? Theme.toastWarningGlyph : 0
                height: width
                radius: width / 2
                color: Qt.rgba(root.accentColor().r, root.accentColor().g, root.accentColor().b, 0.16)
                border.width: 2
                border.color: root.accentColor()
                anchors.verticalCenter: parent.verticalCenter
                Text {
                    anchors.centerIn: parent
                    text: "!"
                    color: root.accentColor()
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontDisplay
                    font.weight: Font.Bold
                }
            }

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
                       - (badge.visible ? badge.width + bodyRow.spacing : 0)
                spacing: 2
                Text {
                    // Leave room for the close button when it shows.
                    width: parent.width - (root.closable ? Theme.s24 : 0)
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
                    // Warnings explain what to do, so let them wrap instead of
                    // cutting the advice off after one line.
                    wrapMode: root.alert ? Text.WordWrap : Text.NoWrap
                    maximumLineCount: root.alert ? 4 : 1
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
    Component.onCompleted: { ready = true; enter.start() }
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
        id: lifetime
        interval: root.lifespan
        running: true
        repeat: false
        onTriggered: exit.start()
    }
}
