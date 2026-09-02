import QtQuick
import QtQuick.Window
import QtMultimedia
import GameHQ
import "."

// Full-screen image viewer, styled after OverlayWindow.qml: a separate
// top-level frameless window (NOT the main app window resized) that covers
// the whole monitor with a dimmed scrim and shows the current capture at
// ~80% of that screen. Left/Right cycles through the gallery model, Esc —
// or a click on the dimmed area — closes. Opened from a single left click
// on a CaptureTile (see Main.qml).
Window {
    id: root
    visible: false
    flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    color: "transparent"
    //% "%1 Viewer"
    title: qsTrId("gamehq.gallery.viewer.window_title").arg(Brand.name)

    property var galleryModel
    property var parentWindow
    // Keep a real native owner relationship. Besides stacking the viewer over
    // GameHQ, Windows can then return activation to the desktop window when
    // this top-level child disappears.
    transientParent: root.parentWindow
    property int index: -1
    // Tracks which input source last navigated the gallery so the side
    // hint pills can render the matching label (L1/R1 for gamepad,
    // ←/→ for keyboard/mouse). Set from padStep() and Keys handlers.
    property bool usingGamepad: false
    readonly property var current: root.index >= 0 && root.galleryModel
        ? root.galleryModel.get(root.index) : ({})

    // The stage's double buffer lives in MediaStage.qml; `_targetUrl` is this
    // window's rule for it. A clip decodes its THUMBNAIL, which the still layer
    // keeps painting until the player renders its first frame over the top —
    // without it the stage blanks on every clip while the media loads, which
    // reads as a flicker when stepping quickly through a mixed gallery.
    readonly property url _targetUrl: {
        if (!root.current.fileUrl)
            return ""
        if (root.current.captureType === "video")
            return root.current.thumbnail ? "file:///" + root.current.thumbnail : ""
        return root.current.fileUrl
    }

    screen: root.parentWindow ? root.parentWindow.screen : undefined

    signal closed()

    function openAt(row) {
        root.index = row
        // Commit the opening capture immediately so the first paint isn't
        // a blank stage waiting for the async loader. Subsequent steps
        // leave the committed still alone — the loader updates it when Ready.
        mediaStage.committedUrl = root._targetUrl
        root.showFullScreen()
        root.requestActivate()
        content.forceActiveFocus()
    }
    function close() {
        mediaStage.player.stop()
        root.visible = false
        root.index = -1
        mediaStage.committedUrl = ""
        root.closed()
    }
    function step(delta) {
        const count = root.galleryModel ? root.galleryModel.rowCount() : 0
        if (count <= 0)
            return
        root.index = (root.index + delta + count) % count
    }
    function clickStep(delta) {
        root.usingGamepad = false
        root.step(delta)
    }
    function seekVideo(deltaMs) {
        const player = mediaStage.player
        if (!player || player.duration <= 0)
            return
        playerControls.revealControls()
        player.position = Math.max(0, Math.min(player.duration, player.position + deltaMs))
    }
    // Save the frame currently shown by the focused clip as a screenshot,
    // attributed to the clip's game. No-op unless the current item is a video.
    function saveCurrentFrame() {
        if (root.current.captureType !== "video")
            return
        app.saveVideoFrame(mediaStage.videoSink, root.current.gameName || "")
    }
    function toggleVideoPlayback() {
        if (root.current.captureType !== "video")
            return
        playerControls.revealControls()
        if (mediaStage.player.playbackState === MediaPlayer.PlayingState) {
            mediaStage.player.pause()
            playerControls.showPulse(false)
        } else {
            mediaStage.player.play()
            playerControls.showPulse(true)
        }
    }
    function padStep(direction) {
        // L1/R1 from the pad: step through every capture (images AND
        // videos) so the bumper acts like a "next/prev item" shortcut,
        // regardless of what's currently shown. Videos keep their seek
        // bar reachable via d-pad left/right (see padNavigate).
        root.usingGamepad = true
        root.step(direction)
    }
    function padNavigate(direction) {
        if (root.current.captureType === "video") {
            root.seekVideo(direction * playerControls.seekStepMs)
        } else {
            root.usingGamepad = true
            root.step(direction)
        }
    }
    function padConfirm() {
        if (root.current.captureType === "video")
            root.toggleVideoPlayback()
    }
    function padReveal() {
        if (root.current.captureType === "video")
            playerControls.revealControls()
    }

    Item {
        id: content
        anchors.fill: parent
        focus: true

        Keys.onPressed: (event) => {
            if (root.current.captureType === "video")
                playerControls.revealControls()
            root.usingGamepad = false
            event.accepted = input.handleKeyPressed(event.key, event.modifiers,
                                                    event.isAutoRepeat)
        }
        Keys.onReleased: (event) => {
            event.accepted = input.handleKeyReleased(event.key, event.modifiers)
        }

        // Dimmed background — click anywhere outside the image to close.
        Rectangle {
            anchors.fill: parent
            color: Theme.lightboxScrim
            MouseArea { anchors.fill: parent; onClicked: root.close() }
        }

        Item {
            id: stage
            width: content.width * 0.8
            height: content.height * 0.8
            anchors.centerIn: parent

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                onClicked: (mouse) => {
                    if (mouse.button === Qt.RightButton)
                        root.close()
                }
            }   // swallow left clicks so the scrim doesn't close

            // Still/clip stage. The still layer stays visible for clips too:
            // it paints the clip's thumbnail and the video surface above it
            // covers that once the player has a frame, so the stage never
            // blanks between captures. `_targetUrl` is only empty when there is
            // genuinely nothing to paint (no capture, or a clip with no
            // thumbnail), which is exactly when clearing is correct.
            MediaStage {
                id: mediaStage
                anchors.fill: parent
                targetUrl: root._targetUrl
                stillVisible: true
                videoSource: (root.current.captureType === "video" && root.current.fileUrl)
                    ? root.current.fileUrl : ""
                videoVisible: root.current.captureType === "video"
                stopOnEmptySource: true
                onPlaybackStarted: playerControls.showPulse(true)
                onPlaybackEnded: playerControls.revealControls()
            }

            PlayerControls {
                id: playerControls
                anchors.fill: parent
                active: root.current.captureType === "video"
                player: mediaStage.player
                onPlayPauseRequested: root.toggleVideoPlayback()
                onSeekRequested: (deltaMs) => root.seekVideo(deltaMs)
                onSurfaceRightClicked: root.close()
            }

            Text {
                anchors.bottom: parent.bottom
                anchors.bottomMargin: -Theme.s24
                anchors.horizontalCenter: parent.horizontalCenter
                //% "%1 · %2"
                text: qsTrId("gamehq.gallery.capture_caption")
                    .arg(root.current.gameName || "")
                    .arg(root.current.dateText || "")
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontCaption
            }

            // Side hint pills — show which key navigates between captures.
            // Visible for any loaded item (image or video), positioned at
            // the left/right edges of the preview, vertically centered.
            // Label adapts to the last input source: "L1"/"R1" after a pad
            // press, "←"/"→" after a keyboard arrow so the hint matches
            // whatever the user is actually using.
            Rectangle {
                id: closePill
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.topMargin: -Theme.s32
                anchors.rightMargin: -Theme.s32
                width: Theme.s48
                height: Theme.s48
                radius: Theme.radiusPill
                color: Theme.text
                visible: root.index >= 0
                opacity: closeMouse.containsMouse ? 1.0 : 0.92

                Rectangle {
                    width: Theme.s24
                    height: Theme.s4
                    radius: Theme.radiusPill
                    color: Theme.bg0
                    anchors.centerIn: parent
                    rotation: 45
                }
                Rectangle {
                    width: Theme.s24
                    height: Theme.s4
                    radius: Theme.radiusPill
                    color: Theme.bg0
                    anchors.centerIn: parent
                    rotation: -45
                }
                MouseArea {
                    id: closeMouse
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton
                    cursorShape: Qt.PointingHandCursor
                    hoverEnabled: true
                    onClicked: root.close()
                }
                Behavior on opacity { NumberAnimation { duration: Theme.durFast } }
            }

            Rectangle {
                id: pillLeft
                anchors.left: parent.left
                anchors.leftMargin: -Theme.s32
                anchors.verticalCenter: parent.verticalCenter
                width: hintTextL.implicitWidth + Theme.s24
                height: Theme.s48
                radius: Theme.radiusPill
                color: Theme.text
                visible: root.index >= 0
                opacity: leftMouse.containsMouse ? 1.0 : 0.92
                Text {
                    id: hintTextL
                    anchors.centerIn: parent
                    text: root.usingGamepad ? "L1" : "\u2190"
                    color: Theme.bg0
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontTitle
                    font.bold: true
                }
                MouseArea {
                    id: leftMouse
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton
                    cursorShape: Qt.PointingHandCursor
                    hoverEnabled: true
                    onClicked: root.clickStep(-1)
                }
                Behavior on opacity { NumberAnimation { duration: Theme.durFast } }
            }

            Rectangle {
                id: pillRight
                anchors.right: parent.right
                anchors.rightMargin: -Theme.s32
                anchors.verticalCenter: parent.verticalCenter
                width: hintTextR.implicitWidth + Theme.s24
                height: Theme.s48
                radius: Theme.radiusPill
                color: Theme.text
                visible: root.index >= 0
                opacity: rightMouse.containsMouse ? 1.0 : 0.92
                Text {
                    id: hintTextR
                    anchors.centerIn: parent
                    text: root.usingGamepad ? "R1" : "\u2192"
                    color: Theme.bg0
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontTitle
                    font.bold: true
                }
                MouseArea {
                    id: rightMouse
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton
                    cursorShape: Qt.PointingHandCursor
                    hoverEnabled: true
                    onClicked: root.clickStep(1)
                }
                Behavior on opacity { NumberAnimation { duration: Theme.durFast } }
            }
        }
    }
}
