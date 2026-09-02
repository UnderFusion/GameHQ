import QtQuick
import GameHQ

Column {
    id: root

    signal addFolderRequested()

    anchors.centerIn: parent
    spacing: Theme.s12

    Text {
        text: "\u25a6"
        color: Theme.textFaint
        font.pixelSize: Theme.fontHero
        anchors.horizontalCenter: parent.horizontalCenter
    }

    Text {
        //% "No captures yet — add a folder to watch."
        text: qsTrId("gamehq.gallery.empty.description")
        color: Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontBody
    }

    AccentButton {
        anchors.horizontalCenter: parent.horizontalCenter
        primary: true
        //% "Add folder…"
        label: qsTrId("gamehq.gallery.action.add_folder")
        onClicked: root.addFolderRequested()
    }
}
