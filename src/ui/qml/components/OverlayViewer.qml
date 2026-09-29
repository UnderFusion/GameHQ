import QtQuick
import QtMultimedia
import GameHQ

// Full-screen capture viewer inside the overlay window. Cross on a screenshot
// opens it; L1/R1 step through every capture (screenshots AND clips, clips
// play here full screen); Circle closes back to the strip. An Item layer, not
// a second top-level window like Lightbox.qml: the overlay never takes focus,
// and a separate window would fight its foreground/no-activate handling.
Item {
    id: root

    property var galleryModel
    property bool usingGamepad: true

    readonly property bool open: root.index >= 0
    property int index: -1
    property int _modelRevision: 0

    readonly property var current: {
        root._modelRevision
        return (root.index >= 0 && root.galleryModel && root.index < root.galleryModel.rowCount())
            ? root.galleryModel.get(root.index) : ({})
    }
    readonly property bool currentIsVideo: root.current.captureType === "video"
    readonly property int seekStepMs: playerControls.seekStepMs

    // Same double-buffer rule as Lightbox: a clip paints its thumbnail until
    // the player renders the first frame over it, so stepping never blanks.
    readonly property url _targetUrl: {
        if (!root.current.fileUrl)
            return ""
        if (root.currentIsVideo)
            return root.current.thumbnail ? "file:///" + root.current.thumbnail : ""
        return root.current.fileUrl
    }

    // Emitted with the row the viewer ended on, so the strip can follow it.
    signal closed(int lastIndex)

    visible: root.open

    function openAt(row) {
        if (!root.galleryModel || row < 0 || row >= root.galleryModel.rowCount())
            return
        root.index = row
        mediaStage.committedUrl = root._targetUrl
    }
    function close() {
        if (!root.open)
            return
        const last = root.index
        mediaStage.player.stop()
        root.index = -1
        mediaStage.committedUrl = ""
        root.closed(last)
    }
    // Clamped like the overlay strip: no wrap from the newest to the oldest.
    function step(delta) {
        const count = root.galleryModel ? root.galleryModel.rowCount() : 0
        if (count <= 0)
            return
        const next = Math.max(0, Math.min(count - 1, root.index + delta))
        if (next === root.index)
            return
        root.index = next
        sounds.play("nav_tick")
    }
    function seekVideo(deltaMs) {
        const player = mediaStage.player
        if (!root.currentIsVideo || player.duration <= 0)
            return
        playerControls.revealControls()
        player.position = Math.max(0, Math.min(player.duration, player.position + deltaMs))
    }
    function toggleVideoPlayback() {
        if (!root.currentIsVideo)
            return
        playerControls.revealControls()
        if (mediaStage.player.playbackState === MediaPlayer.PlayingState) {
            mediaStage.player.pause()
            playerControls.showPulse(false)
        } else {
            mediaStage.player.play()
            playerControls.showPulse(true)
        }
        sounds.play("confirm")
    }
    function saveCurrentFrame() {
        if (!root.currentIsVideo)
            return
        app.saveVideoFrame(mediaStage.videoSink, root.current.gameName || "")
    }
    function revealControls() {
        if (root.currentIsVideo)
            playerControls.revealControls()
    }

    Connections {
        target: root.galleryModel
        function onModelReset() { root._modelRevision += 1; root._clampIndex() }
        function onRowsInserted() { root._modelRevision += 1 }
        function onRowsRemoved() { root._modelRevision += 1; root._clampIndex() }
        function onRowsMoved() { root._modelRevision += 1 }
        function onDataChanged() { root._modelRevision += 1 }
    }
    // A delete (or filter reset) under the open viewer keeps it on a valid
    // row, and closes it once nothing is left to show.
    function _clampIndex() {
        if (!root.open)
            return
        const count = root.galleryModel ? root.galleryModel.rowCount() : 0
        if (count <= 0)
            root.close()
        else if (root.index >= count)
            root.index = count - 1
    }

    // Opaque enough to hide the strip behind it; a click on bare background
    // closes like Circle does.
    Rectangle {
        anchors.fill: parent
        color: Theme.lightboxScrim
        MouseArea { anchors.fill: parent; onClicked: root.close() }
    }

    Item {
        id: stage
        anchors.fill: parent
        anchors.margins: Theme.s48

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: (mouse) => {
                if (mouse.button === Qt.RightButton)
                    root.close()
            }
        }

        MediaStage {
            id: mediaStage
            anchors.fill: parent
            targetUrl: root._targetUrl
            stillVisible: true
            videoSource: (root.currentIsVideo && root.current.fileUrl) ? root.current.fileUrl : ""
            videoVisible: root.currentIsVideo
            stopOnEmptySource: true
            onPlaybackStarted: playerControls.showPulse(true)
            onPlaybackEnded: playerControls.revealControls()
        }

        PlayerControls {
            id: playerControls
            anchors.fill: parent
            active: root.open && root.currentIsVideo
            player: mediaStage.player
            onPlayPauseRequested: root.toggleVideoPlayback()
            onSeekRequested: (deltaMs) => root.seekVideo(deltaMs)
            onSurfaceRightClicked: root.close()
        }

        Text {
            anchors.bottom: parent.bottom
            anchors.bottomMargin: -Theme.s32
            anchors.horizontalCenter: parent.horizontalCenter
            //% "%1 · %2"
            text: qsTrId("gamehq.gallery.capture_caption")
                .arg(root.current.gameName || "")
                .arg(root.current.dateText || "")
            color: Theme.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontCaption
        }
    }

    // Side pills: which button flips captures. Hidden at the ends of the list.
    component StepPill: Rectangle {
        id: pill
        property string label
        property int direction: 1
        property bool available: true
        width: pillText.implicitWidth + Theme.s24
        height: Theme.s48
        radius: Theme.radiusPill
        color: Theme.text
        visible: root.open && pill.available
        opacity: pillMouse.containsMouse ? 1.0 : 0.92
        Text {
            id: pillText
            anchors.centerIn: parent
            text: pill.label
            color: Theme.bg0
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontTitle
            font.bold: true
        }
        MouseArea {
            id: pillMouse
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            hoverEnabled: true
            onClicked: root.step(pill.direction)
        }
        Behavior on opacity { NumberAnimation { duration: Theme.durFast } }
    }

    StepPill {
        anchors.left: parent.left
        anchors.leftMargin: Theme.s12
        anchors.verticalCenter: parent.verticalCenter
        label: root.usingGamepad ? "L1" : "←"
        direction: -1
        available: root.index > 0
    }
    StepPill {
        anchors.right: parent.right
        anchors.rightMargin: Theme.s12
        anchors.verticalCenter: parent.verticalCenter
        label: root.usingGamepad ? "R1" : "→"
        direction: 1
        available: {
            root._modelRevision
            return root.galleryModel && root.index < root.galleryModel.rowCount() - 1
        }
    }

    // Close pill (top-right): a Circle glyph on the pad path, an X for mouse.
    Rectangle {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: Theme.s12
        anchors.rightMargin: Theme.s12
        width: Theme.s48
        height: Theme.s48
        radius: Theme.radiusPill
        color: Theme.text
        visible: root.open
        opacity: closeMouse.containsMouse ? 1.0 : 0.92

        Rectangle {
            visible: root.usingGamepad
            anchors.centerIn: parent
            width: Theme.s24
            height: Theme.s24
            radius: Theme.radiusPill
            color: "transparent"
            border.width: Theme.s4
            border.color: Theme.bg0
        }
        Repeater {
            model: root.usingGamepad ? 0 : 2
            Rectangle {
                required property int index
                width: Theme.s24
                height: Theme.s4
                radius: Theme.radiusPill
                color: Theme.bg0
                anchors.centerIn: parent
                rotation: index === 0 ? 45 : -45
            }
        }
        MouseArea {
            id: closeMouse
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            hoverEnabled: true
            onClicked: root.close()
        }
        Behavior on opacity { NumberAnimation { duration: Theme.durFast } }
    }
}
