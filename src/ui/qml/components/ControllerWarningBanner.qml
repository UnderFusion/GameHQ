import QtQuick
import QtQuick.Layouts
import GameHQ

// Full-width desktop notice for a controller GameHQ cannot read (HidHide or
// another HID filter cloaking the pad). Settings > Input keeps the same card;
// this one sits above the whole window so the cause and the one-click fix are
// seen without opening Settings. Mouse-only on purpose: while it is shown the
// pad itself is usually the thing that cannot reach GameHQ.
Rectangle {
    id: root

    // The message the user dismissed; a different warning (e.g. the fix
    // result) brings the banner back.
    property string dismissedText: ""
    property bool suppressed: false
    signal settingsRequested()

    readonly property bool active: input.controllerWarning.length > 0
                                   && input.controllerWarning !== root.dismissedText
                                   && !root.suppressed

    visible: height > 0
    height: active ? implicitHeight : 0
    implicitHeight: content.implicitHeight + Theme.s12 * 2
    color: Theme.warningSoft
    radius: Theme.radiusM
    border.width: Theme.borderWidth
    border.color: Qt.rgba(Theme.warning.r, Theme.warning.g, Theme.warning.b, 0.45)
    clip: true

    Behavior on height {
        NumberAnimation { duration: Theme.durNormal; easing.type: Easing.OutCubic }
    }

    RowLayout {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.s12
        spacing: Theme.s12

        Rectangle {
            Layout.preferredWidth: 32
            Layout.preferredHeight: 32
            Layout.alignment: Qt.AlignVCenter
            radius: width / 2
            color: Qt.rgba(Theme.warning.r, Theme.warning.g, Theme.warning.b, 0.18)

            Text {
                anchors.centerIn: parent
                text: "!"
                color: Theme.warning
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontH3
                font.weight: Font.Bold
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: Theme.s4

            Text {
                Layout.fillWidth: true
                //% "Controller hidden"
                text: qsTrId("gamehq.settings.input.hidden.title")
                textFormat: Text.PlainText
                color: Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Text {
                Layout.fillWidth: true
                text: input.controllerWarning
                textFormat: Text.PlainText
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.WordWrap
                maximumLineCount: 3
                elide: Text.ElideRight
            }
        }

        AccentButton {
            Layout.alignment: Qt.AlignVCenter
            visible: input.controllerFixAvailable
            //% "Fix automatically"
            label: qsTrId("gamehq.settings.input.hidden.fix")
            primary: true
            onClicked: input.fixHiddenController()
        }
        AccentButton {
            Layout.alignment: Qt.AlignVCenter
            //% "Settings"
            label: qsTrId("gamehq.navigation.settings")
            quiet: true
            onClicked: root.settingsRequested()
        }
        AccentButton {
            Layout.alignment: Qt.AlignVCenter
            //% "Not now"
            label: qsTrId("gamehq.action.not_now")
            quiet: true
            onClicked: root.dismissedText = input.controllerWarning
        }
    }
}
