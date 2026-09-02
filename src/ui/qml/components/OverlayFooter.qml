import QtQuick
import GameHQ

Text {
    id: root

    property bool usingGamepad: false
    property bool menuOpen: false
    property bool videoFocused: false

    anchors.bottom: parent.bottom
    anchors.horizontalCenter: parent.horizontalCenter

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
        //% "Left/Right — captures | Up/Down — categories/games | Enter — open | F — favorite | M — menu | Esc — back to game"
        return qsTrId("gamehq.overlay.hint.browse.keyboard")
    }
    color: Theme.textMuted
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontCaption
}
