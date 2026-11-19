import QtQuick
import GameHQ
import "../helpers/UpdateUi.js" as UpdateUi

// Thin download/install progress track shared by the update banner and the
// About dialog, so both surfaces show the same state. Hidden unless the
// updater is downloading or installing; the install phase has no byte count,
// so it fills the track completely.
Rectangle {
    id: root

    readonly property bool downloading: updates.stateName === "Downloading"
    readonly property bool installing: UpdateUi.installing(updates.stateName)

    visible: root.downloading || root.installing
    implicitHeight: Theme.updateProgressHeight
    radius: Theme.updateProgressHeight / 2
    color: Theme.hoverTint

    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        radius: parent.radius
        color: Theme.accent
        width: parent.width * (root.installing
                               ? 1 : Math.max(0, Math.min(100, updates.progress)) / 100)

        Behavior on width {
            NumberAnimation { duration: Theme.durNormal; easing.type: Easing.OutCubic }
        }
    }
}
