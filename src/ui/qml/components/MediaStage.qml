import QtQuick
import QtMultimedia
import GameHQ

// Double-buffered still/clip stage shared by Lightbox.qml and OverlayPreview.qml.
//
// The double buffer: `loader` async-decodes `targetUrl` off the UI thread and
// promotes it to `committedUrl` — the source of the visible `still` — only once
// it reaches Image.Ready. The previously committed still stays painted until
// then, so stepping between captures never flashes an empty stage.
//
// Both callers paint a clip's thumbnail on the still layer and let the video
// surface — which sits above it — cover that once the player has a frame, so the
// stage never blanks between captures. They still differ in WHEN the video takes
// over (the Lightbox plays a clip as soon as it is selected, the overlay waits
// for playback to be focused), so those rules are set by the caller; this
// component owns what is identical — the decode-then-promote handoff and the
// player/end-of-media wiring.
Item {
    id: root

    // What the hidden loader decodes. "" means "nothing to decode".
    property url targetUrl: ""
    // Whether the committed still paints (callers hide it behind a clip).
    property bool stillVisible: true
    // What the player plays. "" means "no clip selected".
    property url videoSource: ""
    property bool videoVisible: false

    // Stop the player when its source clears. The overlay leaves the player
    // alone instead and re-sources it when playback is focused.
    property bool stopOnEmptySource: false

    // The already-decoded still on screen. Writable so a caller can commit a
    // capture eagerly (Lightbox.openAt) or drop it on close.
    property url committedUrl: ""

    // True once the current clip source has advanced past its first frame.
    property bool _videoStarted: false

    readonly property alias player: clipPlayer
    // The video surface's sink holds the live frame — callers hand this to
    // AppController.saveVideoFrame to grab a still of the clip on screen.
    readonly property alias videoSink: videoSurface.videoSink
    readonly property real sourceWidth: still.implicitWidth
    readonly property real sourceHeight: still.implicitHeight

    // A freshly decoded still was just promoted.
    signal committed(url source)
    // targetUrl went empty and the stage has nothing left to paint.
    signal cleared()
    // Playback lifecycle — callers drive their own PlayerControls from these.
    signal playbackStarted()
    signal playbackEnded()

    Image {
        id: still
        anchors.fill: parent
        // Stays up under a starting clip until the player has produced its
        // first frame; hiding it earlier flashed an empty stage on play.
        visible: root.stillVisible || (root.videoVisible && !root._videoStarted)
        source: root.committedUrl
        fillMode: Image.PreserveAspectFit
        cache: true
    }

    Image {
        id: loader
        anchors.fill: parent
        visible: false
        source: root.targetUrl
        asynchronous: true
        cache: true
        // An empty targetUrl means there is genuinely nothing to paint, so the
        // committed still goes with it. Callers keep the stage populated across
        // a capture step by pointing targetUrl at something decodable (a clip
        // points at its thumbnail) — never by leaving it empty.
        onSourceChanged: {
            if (source.toString() === "") {
                root.committedUrl = ""
                root.cleared()
            }
        }
        onStatusChanged: {
            if (status === Image.Ready && source.toString() !== "") {
                root.committedUrl = source
                root.committed(source)
            }
        }
    }

    // QML Image can't decode video — clips play through the player rather than
    // failing to decode the raw .mp4 as a still frame.
    MediaPlayer {
        id: clipPlayer
        source: root.videoSource
        audioOutput: AudioOutput {}
        videoOutput: videoSurface
        onPositionChanged: {
            if (position > 0)
                root._videoStarted = true
        }
        onSourceChanged: {
            root._videoStarted = false
            if (source.toString() !== "") {
                play()
                root.playbackStarted()
            } else if (root.stopOnEmptySource) {
                stop()
            }
        }
        onMediaStatusChanged: {
            if (mediaStatus === MediaPlayer.EndOfMedia) {
                pause()
                if (duration > 0)
                    position = duration
                root.playbackEnded()
            }
        }
    }

    VideoOutput {
        id: videoSurface
        anchors.fill: parent
        visible: root.videoVisible
        fillMode: VideoOutput.PreserveAspectFit
    }
}
