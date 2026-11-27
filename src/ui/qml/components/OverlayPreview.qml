import QtQuick
import QtQuick.Effects
import QtMultimedia
import GameHQ

Item {
    id: root

    property var galleryModel
    property int currentIndex: 0
    property bool videoFocused: false
    property bool suspended: false

    readonly property int displayedIndex: _displayedIndex
    // galleryModel.get() is an imperative call, so the binding has no way to
    // know the row moved under it — touching _modelRevision (same rule as
    // _targetUrl below) re-reads the record whenever the model changes. A
    // fresh capture is PREPENDED, so without this the record for row 0 stays
    // the previous capture: X refuses to play a just-saved clip and a
    // just-saved screenshot inherits the old clip's play badge.
    readonly property var displayedRecord: {
        root._modelRevision
        return (root._displayedIndex >= 0 && root.galleryModel && root.galleryModel.rowCount() > 0)
            ? root.galleryModel.get(root._displayedIndex) : ({})
    }
    readonly property bool displayedIsVideo: displayedRecord.captureType === "video"
    readonly property bool isPlaying: mediaStage.player.playbackState === MediaPlayer.PlayingState
    readonly property bool canSeek: mediaStage.player.duration > 0
    readonly property int seekStepMs: playerControls.seekStepMs

    property int _displayedIndex: currentIndex
    property int _modelRevision: 0

    // This surface's rule for the shared stage's hidden loader: a clip decodes
    // its THUMBNAIL, so the still layer keeps painting that frame until
    // videoFocused hands the stage over to the video surface.
    readonly property url _targetUrl: {
        if (root.suspended)
            return ""
        root._modelRevision
        const rec = (root.currentIndex >= 0 && root.galleryModel && root.galleryModel.rowCount() > 0)
            ? root.galleryModel.get(root.currentIndex) : ({})
        if (!rec || rec.captureType === undefined) return ""
        if (rec.captureType === "video")
            return rec.thumbnail !== "" ? "file:///" + rec.thumbnail : ""
        return rec.fileUrl
    }

    signal playPauseRequested()
    signal seekRequested(int deltaMs)
    signal backRequested()

    function stopPlayback() {
        mediaStage.player.stop()
    }

    // Save the frame currently shown by the focused clip as a screenshot,
    // attributed to the clip's game. No-op unless a video is actually focused.
    function saveCurrentFrame() {
        if (!(root.videoFocused && root.displayedIsVideo))
            return
        app.saveVideoFrame(mediaStage.videoSink, root.displayedRecord.gameName || "")
    }

    function revealControls() {
        playerControls.revealControls()
    }

    function seekBy(deltaMs) {
        const player = mediaStage.player
        if (player.duration <= 0)
            return
        revealControls()
        player.position = Math.max(0, Math.min(player.duration, player.position + deltaMs))
    }

    function playVideo() {
        mediaStage.player.play()
        playerControls.showPulse(true)
    }

    function pauseVideo() {
        mediaStage.player.pause()
        playerControls.showPulse(false)
    }

    Connections {
        target: root.galleryModel
        function onModelReset() { root._modelRevision += 1 }
        function onRowsInserted() { root._modelRevision += 1 }
        // Removal/move shifts rows exactly like an insert does, so the
        // record bindings have to be re-read for those too.
        function onRowsRemoved() { root._modelRevision += 1 }
        function onRowsMoved() { root._modelRevision += 1 }
        function onDataChanged() { root._modelRevision += 1 }
    }

    readonly property real fitScale: {
        const iw = mediaStage.sourceWidth
        const ih = mediaStage.sourceHeight
        if (iw <= 0 || ih <= 0 || root.width <= 0 || root.height <= 0)
            return 0
        return Math.min(root.width / iw, root.height / ih)
    }
    readonly property real paintedWidth: mediaStage.sourceWidth * root.fitScale
    readonly property real paintedHeight: mediaStage.sourceHeight * root.fitScale

    Rectangle {
        id: previewFrame
        anchors.left: parent.left
        anchors.top: parent.top
        width: root.fitScale > 0 ? root.paintedWidth : root.width
        height: root.fitScale > 0 ? root.paintedHeight : root.height
        radius: Theme.radiusL
        color: "transparent"
        border.width: root.videoFocused ? 2 : 1
        border.color: root.videoFocused ? Theme.accent : Theme.stroke

        MouseArea { anchors.fill: parent }

        Rectangle {
            anchors.fill: parent
            radius: parent.radius
            color: "transparent"
            border.width: 1
            border.color: Theme.focusGlow
            opacity: root.videoFocused ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: Theme.durFast } }
        }

        Item {
            id: previewContent
            anchors.fill: parent
            anchors.margins: previewFrame.border.width
            layer.enabled: true
            layer.effect: MultiEffect {
                maskEnabled: true
                maskSource: previewMask
            }

            // Still/clip stage — the committed frame is tracked against the
            // requested one (_displayedIndex) so the rest of the overlay reads
            // what is actually painted, not what was last asked for.
            MediaStage {
                id: mediaStage
                anchors.fill: parent
                targetUrl: root._targetUrl
                stillVisible: !(root.videoFocused && root.displayedIsVideo)
                videoSource: (root.videoFocused && root.displayedIsVideo
                              && root.displayedRecord.fileUrl)
                    ? root.displayedRecord.fileUrl : ""
                videoVisible: root.videoFocused && root.displayedIsVideo
                onCommitted: root._displayedIndex = root.currentIndex
                onCleared: root._displayedIndex = -1
                onPlaybackStarted: playerControls.showPulse(true)
                onPlaybackEnded: playerControls.revealControls()
            }
        }

        PlayerControls {
            id: playerControls
            anchors.fill: previewFrame
            active: root.videoFocused && root.displayedIsVideo
            surfaceEnabled: root.displayedIsVideo
            player: mediaStage.player
            onPlayPauseRequested: root.playPauseRequested()
            onSeekRequested: (deltaMs) => root.seekRequested(deltaMs)
            onSurfaceRightClicked: root.backRequested()
        }

        Rectangle {
            id: previewMask
            anchors.fill: previewContent
            radius: Math.max(0, previewFrame.radius - previewFrame.border.width)
            visible: false
            layer.enabled: true
        }

        VideoBadge {
            anchors.centerIn: previewFrame
            visible: root.displayedIsVideo && !root.videoFocused
            diameter: Math.min(previewFrame.width, previewFrame.height) * 0.16
        }
    }
}
