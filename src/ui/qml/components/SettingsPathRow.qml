import QtQuick
import GameHQ

SettingsRow {
    id: root
    property string path: ""
    property bool showChange: false
    property bool showOpen: true
    property bool showReset: false
    signal changeRequested()
    signal openRequested()
    signal resetRequested()

    icon: "\u25A4"
    description: path
    controlWidth: Theme.s48 * 5

    TextLink {
        visible: root.showReset
        //% "Use default"
        label: qsTrId("gamehq.common.action.use_default")
        onClicked: root.resetRequested()
    }
    AccentButton {
        visible: root.showChange
        //% "Change"
        label: qsTrId("gamehq.common.action.change")
        quiet: true
        onClicked: root.changeRequested()
    }
    AccentButton {
        visible: root.showOpen
        //% "Open"
        label: qsTrId("gamehq.common.action.open")
        quiet: true
        onClicked: root.openRequested()
    }
}
