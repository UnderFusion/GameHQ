import QtQuick
import GameHQ

Text {
    id: root

    property bool usingGamepad: false
    property bool menuOpen: false
    property bool videoFocused: false

    width: Math.min(parent.width, implicitWidth)
    leftPadding: Theme.s16
    rightPadding: Theme.s16
    topPadding: Theme.s8
    bottomPadding: Theme.s8
    wrapMode: Text.WordWrap
    horizontalAlignment: Text.AlignHCenter
    anchors.bottom: parent.bottom
    anchors.horizontalCenter: parent.horizontalCenter

    Rectangle {
        anchors.fill: parent
        z: -1
        radius: Theme.radiusM
        // Share the existing overlay dimming slider; keep the hint text opaque.
        color: Qt.rgba(Theme.panelTint.r, Theme.panelTint.g, Theme.panelTint.b,
                       Math.min(0.95, Theme.panelTint.a * Theme.overlayScrimStrength / 100))
        border.width: 1
        border.color: Theme.stroke
    }

    text: {
        const pad = root.usingGamepad
        if (root.menuOpen) {
            if (pad) {
                //% "D-pad Up/Down — choose | Cross — confirm | Circle — close menu"
                return qsTrId("gamehq.overlay.hint.menu.gamepad")
            }
            //% "Up/Down — choose | Enter — confirm | Esc/Backspace — close menu"
            return qsTrId("gamehq.overlay.hint.menu.keyboard")
        }
        if (root.videoFocused) {
            if (pad) {
                //% "D-pad Left/Right — scrub | Cross — play/pause | Circle — back to captures"
                return qsTrId("gamehq.overlay.hint.video.gamepad")
            }
            //% "Left/Right — scrub clip | Enter — play/pause | Esc/Backspace — back to captures"
            return qsTrId("gamehq.overlay.hint.video.keyboard")
        }
        if (pad) {
            //% "L1/R1 — captures | D-pad Up/Down — categories/games | Cross — open | Triangle — favorite | Square — menu | Circle — back to game"
            return qsTrId("gamehq.overlay.hint.browse.gamepad")
        }
        // The overlay has no keyboard capture switch: left/right is
        // seek-only there (and a no-op without a focused clip), so the
        // browse hint does not promise it.
        //% "Up/Down — categories/games | Enter — open | F — favorite | M — menu | Esc — back to game"
        return qsTrId("gamehq.overlay.hint.browse.keyboard")
    }
    color: Theme.textMuted
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontCaption
}
