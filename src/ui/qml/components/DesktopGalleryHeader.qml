import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import GameHQ

RowLayout {
    id: root

    property string titleText: ""
    property bool bulkMode: false
    property int bulkCount: 0
    property bool bulkAllSelected: false

    signal bulkEnterRequested()
    signal bulkSelectAllRequested()
    signal bulkDeleteRequested()
    signal bulkExitRequested()

    Layout.fillWidth: true

    Text {
        text: {
            if (!root.bulkMode)
                return root.titleText
            //% "%n selected"
            return qsTrId("gamehq.gallery.selection.count", root.bulkCount)
        }
        color: Theme.text
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontDisplay
        font.weight: Font.Light
        Layout.fillWidth: true
    }

    AccentButton {
        visible: !root.bulkMode
        quiet: true
        icon: "\u2611"
        //% "Bulk select"
        label: qsTrId("gamehq.gallery.action.bulk_select")
        onClicked: root.bulkEnterRequested()
    }

    RowLayout {
        visible: root.bulkMode
        spacing: Theme.s8

        AccentButton {
            quiet: true
            icon: root.bulkAllSelected ? "\u2610" : "\u2611"
            label: {
                if (root.bulkAllSelected) {
                    //% "Deselect all"
                    return qsTrId("gamehq.action.deselect_all")
                }
                //% "Select all"
                return qsTrId("gamehq.action.select_all")
            }
            onClicked: root.bulkSelectAllRequested()
        }

        AccentButton {
            quiet: true
            icon: "\uE74D"
            iconFontFamily: "Segoe Fluent Icons"
            iconColor: Theme.danger
            labelColor: Theme.danger
            borderColor: Theme.danger
            quietIdleBorderColor: Theme.dangerQuietBorder
            quietTopColor: Theme.dangerQuietTop
            quietBottomColor: Theme.dangerQuietBottom
            //% "Delete"
            label: qsTrId("gamehq.action.delete")
            opacity: root.bulkCount === 0 ? 0.45 : 1.0
            enabled: root.bulkCount > 0
            onClicked: root.bulkDeleteRequested()
        }

        AccentButton {
            quiet: true
            icon: "\u2713"
            borderColor: Theme.success
            quietIdleBorderColor: Theme.successQuietBorder
            quietTopColor: Theme.successQuietTop
            quietBottomColor: Theme.successQuietBottom
            //% "Done"
            label: qsTrId("gamehq.action.done")
            onClicked: root.bulkExitRequested()
        }
    }
}
