import QtQuick
import QtMultimedia
import GameHQ

Item {
    id: root

    property var player
    property bool active: false
    property bool surfaceEnabled: active
    property bool controlsShown: false
    readonly property bool controlsVisible: active && controlsShown
    property int seekStepMs: Theme.playerSeekStepMs
    property bool pulsePlaying: false

    signal playPauseRequested()
    signal seekRequested(int deltaMs)
    signal surfaceRightClicked()

    onActiveChanged: {
        if (active)
            revealControls()
        else {
            controlsShown = false
            autoHideTimer.stop()
        }
    }

    function isPlaying() {
        return player && player.playbackState === MediaPlayer.PlayingState
    }

    function revealControls() {
        if (!active)
            return
        controlsShown = true
        autoHideTimer.restart()
    }

    function showPulse(playing) {
        pulsePlaying = playing
        pulseAnim.restart()
    }

    function formatTime(ms) {
        languageManager.translationRevision
        return languageManager.formatDuration(ms || 0)
    }

    readonly property real progress: {
        if (!player || player.duration <= 0)
            return 0
        return Math.max(0, Math.min(1, player.position / player.duration))
    }

    Timer {
        id: autoHideTimer
        interval: Theme.playerControlsAutoHideMs
        repeat: false
        onTriggered: root.controlsShown = false
    }

    MouseArea {
        anchors.fill: parent
        enabled: root.surfaceEnabled
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onPositionChanged: root.revealControls()
        onPressed: (mouse) => {
            root.revealControls()
        }
        onClicked: (mouse) => {
            root.revealControls()
            if (mouse.button === Qt.RightButton)
                root.surfaceRightClicked()
            else
                root.playPauseRequested()
        }
    }

    Rectangle {
        id: centerPulse
        anchors.centerIn: parent
        width: Math.min(parent.width, parent.height) * 0.18
        height: width
        radius: width / 2
        color: Theme.pulseFill
        border.width: Math.max(1, Math.round(Theme.s4 / 2))
        border.color: Theme.borderLight
        opacity: 0
        visible: opacity > 0
        z: 4

        PlayIcon {
            anchors.centerIn: parent
            visible: root.pulsePlaying
            width: parent.width * 0.56
            height: width
        }

        Row {
            anchors.centerIn: parent
            visible: !root.pulsePlaying
            spacing: parent.width * 0.12

            Repeater {
                model: 2
                Rectangle {
                    width: centerPulse.width * 0.11
                    height: centerPulse.height * 0.44
                    radius: width / 2
                    color: Theme.text
                }
            }
        }

        SequentialAnimation {
            id: pulseAnim
            NumberAnimation { target: centerPulse; property: "opacity"; to: 1; duration: Theme.durFast }
            PauseAnimation { duration: Theme.playerHudHoldMs }
            NumberAnimation { target: centerPulse; property: "opacity"; to: 0; duration: Theme.durSlow; easing.type: Easing.OutCubic }
        }
    }

    Rectangle {
        id: controlsPanel
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.s24
        width: Math.min(parent.width - Theme.s48, Theme.playerControlsMaxWidth)
        height: controlsColumn.implicitHeight + Theme.s16 * 2
        radius: Theme.radiusM
        color: "transparent"
        border.width: 0
        opacity: root.controlsVisible ? 1 : 0
        visible: opacity > 0
        z: 3

        Behavior on opacity { NumberAnimation { duration: Theme.durFast; easing.type: Easing.OutCubic } }

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            onPositionChanged: root.revealControls()
            onPressed: root.revealControls()
        }

        Column {
            id: controlsColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: Theme.s16
            anchors.rightMargin: Theme.s16
            spacing: Theme.s12

            Rectangle {
                id: progressTrack
                width: parent.width
                height: Theme.s8
                radius: height / 2
                color: Theme.surfaceAlt
                border.width: Math.max(1, Math.round(Theme.s4 / 2))
                border.color: Theme.stroke

                Rectangle {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: Math.max(parent.height, parent.width * root.progress)
                    radius: parent.radius
                    color: Theme.text
                }

                Rectangle {
                    width: Theme.s16
                    height: width
                    radius: width / 2
                    color: Theme.text
                    anchors.verticalCenter: parent.verticalCenter
                    x: Math.max(0, Math.min(progressTrack.width - width, progressTrack.width * root.progress - width / 2))
                }

                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    onPressed: (mouse) => {
                        root.revealControls()
                        root.seekToRatio(mouse.x / width)
                    }
                    onPositionChanged: (mouse) => {
                        root.revealControls()
                        if (pressed)
                            root.seekToRatio(mouse.x / width)
                    }
                }
            }

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: Theme.s16

                PlayerButton {
                    text: "<<"
                    onClicked: {
                        root.revealControls()
                        root.seekRequested(-root.seekStepMs)
                    }
                }

                PlayerButton {
                    text: ""
                    playIcon: !root.isPlaying()
                    pauseIcon: root.isPlaying()
                    fontSize: Theme.fontTitle
                    onClicked: {
                        root.revealControls()
                        root.playPauseRequested()
                    }
                }

                PlayerButton {
                    text: ">>"
                    onClicked: {
                        root.revealControls()
                        root.seekRequested(root.seekStepMs)
                    }
                }
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                //% "%1 / %2"
                text: qsTrId("gamehq.player.position_duration")
                        .arg(root.formatTime(root.player ? root.player.position : 0))
                        .arg(root.formatTime(root.player ? root.player.duration : 0))
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontCaption
            }
        }
    }

    function seekToRatio(ratio) {
        if (!player || player.duration <= 0)
            return
        const clamped = Math.max(0, Math.min(1, ratio))
        player.position = Math.round(player.duration * clamped)
    }

    component PlayerButton: Rectangle {
        id: button
        property alias text: label.text
        property int fontSize: Theme.fontH3
        property bool playIcon: false
        property bool pauseIcon: false
        signal clicked()

        width: Theme.playerButtonSize
        height: width
        radius: width / 2
        color: mouse.containsMouse ? Theme.surfaceAlt : "transparent"
        border.width: Math.max(1, Math.round(Theme.s4 / 2))
        border.color: Theme.borderLight

        Text {
            id: label
            anchors.centerIn: parent
            visible: !button.pauseIcon && !button.playIcon
            color: Theme.text
            font.family: Theme.fontFamily
            font.pixelSize: button.fontSize
            font.weight: Font.DemiBold
        }

        PlayIcon {
            anchors.centerIn: parent
            visible: button.playIcon
            width: Theme.s24 + Theme.s4
            height: width
        }

        Row {
            anchors.centerIn: parent
            visible: button.pauseIcon
            spacing: Theme.s8 - Theme.s4 / 2

            Repeater {
                model: 2
                Rectangle {
                    width: Theme.s8 - Theme.s4 / 2
                    height: Theme.s24 - Theme.s4
                    radius: Theme.s4
                    color: Theme.text
                }
            }
        }

        MouseArea {
            id: mouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: button.clicked()
        }
    }

    component PlayIcon: Canvas {
        // The Lightbox Window is created `visible: false` and shown later,
        // so the Canvas's first paint can be skipped and never retrigger on
        // its own. Force a repaint whenever size/completion/visibility fires.
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onVisibleChanged: requestPaint()
        Component.onCompleted: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)
            ctx.fillStyle = Theme.text
            ctx.beginPath()
            ctx.moveTo(width * 0.30, height * 0.18)
            ctx.lineTo(width * 0.82, height * 0.50)
            ctx.lineTo(width * 0.30, height * 0.82)
            ctx.closePath()
            ctx.fill()
        }
    }
}
